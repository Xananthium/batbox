// src/mcp/StdioTransport.cpp
// ---------------------------------------------------------------------------
// StdioTransport — posix_spawn + Content-Length framing over stdin/stdout.
// See include/batbox/mcp/StdioTransport.hpp for design notes.
// ---------------------------------------------------------------------------

#include <batbox/mcp/StdioTransport.hpp>
#include <batbox/mcp/McpFraming.hpp>
#include <batbox/mcp/JsonRpc.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// POSIX / macOS headers
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// The C standard library exposes environ on both macOS and Linux.
extern char** environ;

namespace batbox::mcp {

// ============================================================================
// Construction / Destruction
// ============================================================================

StdioTransport::StdioTransport(std::string              command,
                               std::vector<std::string> args,
                               std::vector<std::string> env)
    : command_(std::move(command))
    , args_(std::move(args))
    , extra_env_(std::move(env))
{}

StdioTransport::~StdioTransport() {
    // Best-effort shutdown — stop() is idempotent.
    stop();
}

// ============================================================================
// on_notification — must be called before start()
// ============================================================================

void StdioTransport::on_notification(
    std::function<void(std::string method, Json params)> handler)
{
    notification_handler_ = std::move(handler);
}

// ============================================================================
// healthy
// ============================================================================

bool StdioTransport::healthy() const {
    return healthy_.load(std::memory_order_acquire);
}

// ============================================================================
// start
// ============================================================================

Result<void> StdioTransport::start(CancelToken ct) {
    // Idempotent: already running.
    if (healthy()) {
        return {};
    }
    if (stopping_.load(std::memory_order_acquire)) {
        return Err(std::string("transport is stopping"));
    }

    // -----------------------------------------------------------------------
    // Build pipe pair: child_stdin_pipe and child_stdout_pipe
    //   child_stdin_pipe[0]  → child reads  (dup2 to STDIN_FILENO in child)
    //   child_stdin_pipe[1]  → parent writes (our stdin_fd_)
    //   child_stdout_pipe[0] → parent reads  (our stdout_fd_)
    //   child_stdout_pipe[1] → child writes  (dup2 to STDOUT_FILENO in child)
    // -----------------------------------------------------------------------
    int child_stdin_pipe[2]  = {-1, -1};
    int child_stdout_pipe[2] = {-1, -1};

    if (::pipe(child_stdin_pipe) != 0) {
        return Err(std::string("pipe() failed for stdin: ") + ::strerror(errno));
    }
    if (::pipe(child_stdout_pipe) != 0) {
        ::close(child_stdin_pipe[0]);
        ::close(child_stdin_pipe[1]);
        return Err(std::string("pipe() failed for stdout: ") + ::strerror(errno));
    }

    // -----------------------------------------------------------------------
    // Build argv (null-terminated pointer array)
    // -----------------------------------------------------------------------
    std::vector<const char*> argv;
    argv.push_back(command_.c_str());
    for (const auto& a : args_) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    // -----------------------------------------------------------------------
    // Build envp: parent environment + extra env entries
    // -----------------------------------------------------------------------
    std::vector<std::string> env_strings;
    for (char** p = environ; p && *p; ++p) {
        env_strings.emplace_back(*p);
    }
    for (const auto& e : extra_env_) {
        // Override any existing entry with the same key
        auto eq = e.find('=');
        if (eq != std::string::npos) {
            std::string key = e.substr(0, eq + 1); // "KEY="
            auto it = std::find_if(env_strings.begin(), env_strings.end(),
                [&key](const std::string& s) {
                    return s.size() >= key.size() &&
                           s.compare(0, key.size(), key) == 0;
                });
            if (it != env_strings.end()) {
                *it = e;
            } else {
                env_strings.push_back(e);
            }
        } else {
            env_strings.push_back(e);
        }
    }
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(const_cast<char*>(s.c_str()));
    }
    envp.push_back(nullptr);

    // -----------------------------------------------------------------------
    // Configure posix_spawn file actions
    // -----------------------------------------------------------------------
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    // Child stdin ← child_stdin_pipe[0]
    posix_spawn_file_actions_adddup2(&file_actions, child_stdin_pipe[0],  STDIN_FILENO);
    // Child stdout ← child_stdout_pipe[1]
    posix_spawn_file_actions_adddup2(&file_actions, child_stdout_pipe[1], STDOUT_FILENO);

    // Close the parent-side ends in the child (they were duplicated above)
    posix_spawn_file_actions_addclose(&file_actions, child_stdin_pipe[1]);
    posix_spawn_file_actions_addclose(&file_actions, child_stdout_pipe[0]);

    // Also close the original pipe ends after dup2 so child doesn't have extras
    posix_spawn_file_actions_addclose(&file_actions, child_stdin_pipe[0]);
    posix_spawn_file_actions_addclose(&file_actions, child_stdout_pipe[1]);

    // -----------------------------------------------------------------------
    // Configure posix_spawn attributes: new process group (setpgid)
    // -----------------------------------------------------------------------
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0); // pgid = child pid (set by kernel)

    // -----------------------------------------------------------------------
    // Spawn the child
    // -----------------------------------------------------------------------
    pid_t pid = -1;
    int rc = ::posix_spawnp(&pid,
                           command_.c_str(),
                           &file_actions,
                           &attr,
                           const_cast<char* const*>(argv.data()),
                           envp.data());

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        // Close all pipe fds before returning error
        ::close(child_stdin_pipe[0]);
        ::close(child_stdin_pipe[1]);
        ::close(child_stdout_pipe[0]);
        ::close(child_stdout_pipe[1]);
        return Err(std::string("posix_spawnp failed: ") + ::strerror(rc));
    }

    // -----------------------------------------------------------------------
    // Parent: close the child-side ends of the pipes
    // -----------------------------------------------------------------------
    ::close(child_stdin_pipe[0]);   // child reads from this end
    ::close(child_stdout_pipe[1]);  // child writes to this end

    child_pid_  = pid;
    stdin_fd_   = child_stdin_pipe[1];   // we write to child stdin
    stdout_fd_  = child_stdout_pipe[0];  // we read from child stdout

    // Check if caller cancelled during spawn setup
    if (ct.is_cancelled()) {
        // Kill the just-spawned child and clean up
        ::kill(-child_pid_, SIGKILL);
        ::waitpid(child_pid_, nullptr, 0);
        close_fd(stdin_fd_);
        close_fd(stdout_fd_);
        child_pid_ = -1;
        return Err(std::string("cancelled"));
    }

    // -----------------------------------------------------------------------
    // Mark healthy and start reader thread
    // -----------------------------------------------------------------------
    healthy_.store(true, std::memory_order_release);
    reader_thread_ = std::thread(&StdioTransport::reader_loop, this);

    return {};
}

// ============================================================================
// stop
// ============================================================================

void StdioTransport::stop() {
    // Idempotent: if already stopped, nothing to do
    bool already_stopping = stopping_.exchange(true, std::memory_order_acq_rel);
    if (already_stopping) {
        return;
    }

    // Mark unhealthy immediately so new request() calls see transport stopped
    healthy_.store(false, std::memory_order_release);

    // Cancel all pending requests
    cancel_all_pending("transport stopped");

    // -----------------------------------------------------------------------
    // Step 1: Send notifications/exit (best-effort, may fail if pipe broken)
    // -----------------------------------------------------------------------
    if (stdin_fd_ >= 0) {
        Json exit_notif = make_notification("notifications/exit", Json(nullptr));
        std::string framed = frame_message(exit_notif.dump());
        // Best-effort write, ignore errors (pipe may already be broken)
        {
            std::lock_guard<std::mutex> lk(write_mutex_);
            ::write(stdin_fd_, framed.data(), framed.size());
        }
    }

    // -----------------------------------------------------------------------
    // Step 2: Close our write end of stdin pipe
    // This signals EOF to the child's stdin
    // -----------------------------------------------------------------------
    close_fd(stdin_fd_);

    if (child_pid_ > 0) {
        // -------------------------------------------------------------------
        // Step 3: Wait up to 500 ms for voluntary exit
        // -------------------------------------------------------------------
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        bool exited = false;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
            if (r == child_pid_) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // -------------------------------------------------------------------
        // Step 4: SIGTERM to process group
        // -------------------------------------------------------------------
        if (!exited) {
            ::kill(-child_pid_, SIGTERM);

            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (std::chrono::steady_clock::now() < deadline) {
                int status = 0;
                pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
                if (r == child_pid_) {
                    exited = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        // -------------------------------------------------------------------
        // Step 5: SIGKILL to process group
        // -------------------------------------------------------------------
        if (!exited) {
            ::kill(-child_pid_, SIGKILL);
            // Block until the zombie is reaped
            ::waitpid(child_pid_, nullptr, 0);
        }

        child_pid_ = -1;
    }

    // -----------------------------------------------------------------------
    // Step 6: Close stdout fd (reader thread will hit EOF and exit)
    // Join the reader thread
    // -----------------------------------------------------------------------
    close_fd(stdout_fd_);

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

// ============================================================================
// request
// ============================================================================

Result<Json> StdioTransport::request(std::string method, Json params, CancelToken ct) {
    if (!healthy()) {
        return Err(std::string("transport not started"));
    }
    if (ct.is_cancelled()) {
        return Err(std::string("cancelled"));
    }

    // Assign id and build JSON-RPC request envelope
    int64_t id = next_id();
    Json req_json = make_request(id, method, std::move(params));
    std::string framed = frame_message(req_json.dump());

    // Register the pending request before sending (avoid race where response
    // arrives before we register the promise)
    std::promise<Result<Json>> promise;
    std::future<Result<Json>>  future = promise.get_future();
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_map_.emplace(id, PendingRequest{std::move(promise)});
    }

    // Send the request
    bool written = false;
    {
        std::lock_guard<std::mutex> lk(write_mutex_);
        written = write_framed(framed);
    }

    if (!written) {
        // Remove the pending entry and return error
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_map_.erase(id);
        return Err(std::string("transport disconnected"));
    }

    // -----------------------------------------------------------------------
    // Wait for response, checking cancellation with a polling loop
    // -----------------------------------------------------------------------
    while (true) {
        // Poll future with 50ms timeout
        auto status = future.wait_for(std::chrono::milliseconds(50));

        if (status == std::future_status::ready) {
            return future.get();
        }

        // Check cancellation
        if (ct.is_cancelled()) {
            // Remove pending entry and return cancelled
            {
                std::lock_guard<std::mutex> lk(pending_mutex_);
                pending_map_.erase(id);
            }
            return Err(std::string("cancelled"));
        }

        // Check if transport died while we were waiting
        if (!healthy() && !stopping_.load(std::memory_order_acquire)) {
            // Transport disconnected unexpectedly — the reader loop will have
            // called cancel_all_pending which resolves our future
            auto status2 = future.wait_for(std::chrono::milliseconds(100));
            if (status2 == std::future_status::ready) {
                return future.get();
            }
            // Safety fallback
            {
                std::lock_guard<std::mutex> lk(pending_mutex_);
                pending_map_.erase(id);
            }
            return Err(std::string("transport disconnected"));
        }
    }
}

// ============================================================================
// notify
// ============================================================================

Result<void> StdioTransport::notify(std::string method, Json params) {
    if (!healthy()) {
        return Err(std::string("transport not started"));
    }

    Json notif_json = make_notification(method, std::move(params));
    std::string framed = frame_message(notif_json.dump());

    std::lock_guard<std::mutex> lk(write_mutex_);
    if (!write_framed(framed)) {
        return Err(std::string("transport disconnected"));
    }
    return {};
}

// ============================================================================
// Private: reader_loop
// ============================================================================

void StdioTransport::reader_loop() {
    FrameReader frame_reader;
    constexpr std::size_t kBufSize = 65536;
    std::vector<char> buf(kBufSize);

    while (true) {
        // Read a chunk from the child's stdout
        ssize_t n = ::read(stdout_fd_, buf.data(), buf.size());

        if (n < 0) {
            if (errno == EINTR) {
                continue; // retry on signal interrupt
            }
            // Read error — transport broken
            break;
        }
        if (n == 0) {
            // EOF — child closed its stdout (process exited)
            break;
        }

        // Feed raw bytes into the framing decoder
        auto messages = frame_reader.feed(std::string_view(buf.data(), static_cast<std::size_t>(n)));

        for (auto& msg_body : messages) {
            // Parse the JSON body
            Json j;
            try {
                j = Json::parse(msg_body);
            } catch (...) {
                // Malformed JSON from server — skip
                continue;
            }

            // Dispatch based on message type
            auto parsed = parse_message(j);
            if (!parsed.has_value()) {
                // Unrecognised message shape — skip
                continue;
            }

            std::visit([this](auto&& msg) {
                using T = std::decay_t<decltype(msg)>;

                if constexpr (std::is_same_v<T, JsonRpcResponse>) {
                    // Match by id and resolve the pending promise
                    int64_t resp_id = 0;
                    if (std::holds_alternative<int64_t>(msg.id)) {
                        resp_id = std::get<int64_t>(msg.id);
                    }
                    // String ids are not used by StdioTransport (we always use int64_t)

                    std::lock_guard<std::mutex> lk(pending_mutex_);
                    auto it = pending_map_.find(resp_id);
                    if (it != pending_map_.end()) {
                        if (msg.error.has_value()) {
                            std::string errmsg = std::to_string(msg.error->code)
                                                 + ": " + msg.error->message;
                            it->second.promise.set_value(Err(errmsg));
                        } else if (msg.result.has_value()) {
                            it->second.promise.set_value(*msg.result);
                        } else {
                            it->second.promise.set_value(Err(
                                std::string("invalid response: no result or error")));
                        }
                        pending_map_.erase(it);
                    }
                    // If no matching pending request, this is an unsolicited response — ignore

                } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
                    // Fire the notification callback if registered
                    if (notification_handler_) {
                        notification_handler_(msg.method, msg.params);
                    }

                } else {
                    // JsonRpcRequest from server — not used in current MCP client
                    // (server-initiated requests are an extension; ignore for now)
                    (void)msg;
                }
            }, parsed.value());
        }
    }

    // -----------------------------------------------------------------------
    // Reader loop has exited — child is gone.
    // Mark unhealthy and cancel all outstanding requests.
    // -----------------------------------------------------------------------
    healthy_.store(false, std::memory_order_release);
    cancel_all_pending("transport disconnected");
}

// ============================================================================
// Private: write_framed
// Must be called with write_mutex_ held.
// ============================================================================

bool StdioTransport::write_framed(const std::string& text) {
    if (stdin_fd_ < 0) return false;

    const char* data = text.data();
    std::size_t remaining = text.size();

    while (remaining > 0) {
        ssize_t written = ::write(stdin_fd_, data, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false; // pipe broken
        }
        data      += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

// ============================================================================
// Private: cancel_all_pending
// ============================================================================

void StdioTransport::cancel_all_pending(const std::string& reason) {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    for (auto& [id, pending] : pending_map_) {
        try {
            pending.promise.set_value(Err(std::string(reason)));
        } catch (const std::future_error&) {
            // Promise already satisfied — skip
        }
    }
    pending_map_.clear();
}

// ============================================================================
// Private: close_fd
// ============================================================================

void StdioTransport::close_fd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace batbox::mcp
