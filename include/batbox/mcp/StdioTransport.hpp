// include/batbox/mcp/StdioTransport.hpp
// ---------------------------------------------------------------------------
// StdioTransport — MCP transport over stdin/stdout of a child process.
//
// Design (per ned-cpp.md §2.C8):
//   posix_spawn the server process with stdin/stdout piped and a new process
//   group (POSIX_SPAWN_SETPGROUP) so the entire subtree can be killed cleanly.
//
//   Reader thread:
//     Calls read() on the child stdout fd in a loop, feeds raw bytes into a
//     FrameReader, and for each complete JSON-RPC message:
//       - JsonRpcResponse → locates the matching promise in pending_map_ and
//         resolves it with Ok(result) or Err("<code>: <message>").
//       - JsonRpcNotification → invokes the on_notification handler if set.
//       - JsonRpcRequest from server → currently ignored (not used by MCP).
//     Reader thread exits when the child pipe is closed (EOF) or stop() fires.
//
//   Writer:
//     Serialises outgoing JSON, prepends Content-Length framing, and writes
//     to the child stdin pipe.  A mutex guards the fd so concurrent request()
//     and notify() calls do not interleave bytes.
//
//   Shutdown sequence (stop()):
//     1. Send notifications/exit notification (best-effort).
//     2. Wait up to 500 ms for child to exit voluntarily.
//     3. SIGTERM to process group.
//     4. Wait up to 2 s.
//     5. SIGKILL to process group.
//     6. waitpid() to reap zombie; join reader thread.
//     7. Cancel all pending request() calls → Err("transport stopped").
//
// Thread safety:
//   request() and notify() are safe to call concurrently from multiple threads.
//   on_notification() must be registered before start().
//
// Build standalone integration test (from repo root):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_mcp_stdio.cpp \
//       src/mcp/StdioTransport.cpp src/mcp/JsonRpc.cpp src/mcp/McpFraming.cpp \
//       src/core/CancelToken.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_mcp_stdio && /tmp/test_mcp_stdio
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// POSIX headers — stdio transport is POSIX-only
#include <sys/types.h>

namespace batbox::mcp {

// ============================================================================
// StdioTransport — posix_spawn + Content-Length framing over stdin/stdout
// ============================================================================

/// Concrete MCP transport that launches a server process and communicates
/// via the LSP Content-Length framing protocol over the process's stdin/stdout.
///
/// Usage:
///   StdioTransport t("npx", {"-y", "@modelcontextprotocol/server-filesystem", "/"});
///   t.on_notification([](auto method, auto params) { ... });
///   auto [src, ct] = CancelToken::make_root();
///   REQUIRE(t.start(std::move(ct)));
///   auto res = t.request("initialize", params, CancelToken{});
///   t.stop();
class StdioTransport final : public IMcpTransport {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a StdioTransport that will spawn @p command with @p args.
    ///
    /// @param command  Absolute or PATH-resolved executable path (e.g. "python3").
    /// @param args     Arguments passed to the command (argv[1..]).
    /// @param env      Extra environment variables in "KEY=VALUE" form.  These
    ///                 are appended to the parent process's environment.  An
    ///                 empty vector means "inherit parent environment unchanged".
    explicit StdioTransport(std::string              command,
                            std::vector<std::string> args = {},
                            std::vector<std::string> env  = {});

    ~StdioTransport() override;

    // Non-copyable, non-movable (mutex + thread members prevent move).
    StdioTransport(const StdioTransport&)            = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;
    StdioTransport(StdioTransport&&)                 = delete;
    StdioTransport& operator=(StdioTransport&&)      = delete;

    // -------------------------------------------------------------------------
    // IMcpTransport interface
    // -------------------------------------------------------------------------

    /// Spawn the child process, open pipes, and start the reader thread.
    ///
    /// Idempotent: if already healthy() returns Ok immediately.
    /// @param ct  Fires if the caller wants to abort the start attempt.
    [[nodiscard]] Result<void> start(CancelToken ct) override;

    /// Gracefully shut down: send notifications/exit → SIGTERM → SIGKILL → reap.
    ///
    /// All pending request() calls are resolved with Err("transport stopped").
    /// Idempotent: calling on an already-stopped transport is a no-op.
    void stop() override;

    /// Returns true when the child process is alive and the reader thread is
    /// running.  Thread-safe atomic read.
    [[nodiscard]] bool healthy() const override;

    /// Send a JSON-RPC 2.0 request and block until the response arrives.
    ///
    /// Assigns the next id, serialises, sends, then blocks on a std::future.
    /// Returns:
    ///   Ok(result)                    — successful response
    ///   Err("<code>: <message>")      — JSON-RPC error response
    ///   Err("cancelled")              — ct fired before response arrived
    ///   Err("transport stopped")      — stop() called while waiting
    ///   Err("transport disconnected") — child exited unexpectedly
    ///   Err("transport not started")  — start() was never called
    [[nodiscard]] Result<Json> request(std::string method,
                                       Json        params,
                                       CancelToken ct) override;

    /// Send a JSON-RPC 2.0 notification (fire-and-forget).
    ///
    /// Returns Ok when bytes are written to the pipe, Err if transport stopped.
    [[nodiscard]] Result<void> notify(std::string method, Json params) override;

    /// Register the inbound-notification callback.
    ///
    /// Must be called before start(). Replaces any previously registered handler.
    /// The callback is invoked on the reader thread; do not call stop() from it.
    void on_notification(
        std::function<void(std::string method, Json params)> handler) override;

private:
    // -------------------------------------------------------------------------
    // Configuration (immutable after construction)
    // -------------------------------------------------------------------------
    std::string              command_;
    std::vector<std::string> args_;
    std::vector<std::string> extra_env_;

    // -------------------------------------------------------------------------
    // Child process state
    // -------------------------------------------------------------------------
    pid_t child_pid_{-1};   ///< PID of the spawned child, -1 when not running
    int   stdin_fd_{-1};    ///< Write end of the child's stdin pipe (owned by us)
    int   stdout_fd_{-1};   ///< Read end of the child's stdout pipe (owned by us)

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------
    std::atomic<bool> healthy_{false};  ///< True when child is alive + reader running
    std::atomic<bool> stopping_{false}; ///< True once stop() has been called

    // -------------------------------------------------------------------------
    // Reader thread
    // -------------------------------------------------------------------------
    std::thread reader_thread_;

    // -------------------------------------------------------------------------
    // Writer mutex
    // -------------------------------------------------------------------------
    std::mutex write_mutex_; ///< Serialises all writes to stdin_fd_

    // -------------------------------------------------------------------------
    // Pending request map
    // -------------------------------------------------------------------------

    /// Per-request state stored while waiting for a response.
    struct PendingRequest {
        std::promise<Result<Json>> promise;
    };

    std::mutex pending_mutex_;
    std::unordered_map<int64_t, PendingRequest> pending_map_;

    // -------------------------------------------------------------------------
    // Notification handler
    // -------------------------------------------------------------------------
    std::function<void(std::string, Json)> notification_handler_;

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    /// Body of the reader thread.
    void reader_loop();

    /// Write framed bytes to child stdin (mutex held by caller via write_mutex_).
    /// Returns false on write error.
    [[nodiscard]] bool write_framed(const std::string& json_text);

    /// Resolve all pending promises with the given error (called when transport dies).
    void cancel_all_pending(const std::string& reason);

    /// Close a file descriptor and set it to -1.
    static void close_fd(int& fd) noexcept;
};

} // namespace batbox::mcp
