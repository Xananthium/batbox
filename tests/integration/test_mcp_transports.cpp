// tests/integration/test_mcp_transports.cpp
// ---------------------------------------------------------------------------
// CPP T.4 — MCP-transports integration test: all 4 transports together.
//
// Strategy:
//   Spins up all 4 fixture servers simultaneously (stdio, http, sse, ws),
//   registers them in a McpServerRegistry, and exercises McpClient against
//   all 4 in a single test run.
//
// Acceptance criteria:
//   AC1. All 4 fixture servers boot (healthy state reached in parallel)
//   AC2. McpClient orchestrates all 4 with parallel initialize
//   AC3. Each transport's tools/list returns its declared tools
//   AC4. tools/call dispatch routes to the correct transport
//   AC5. One transport crash: others remain healthy; crashed one transitions
//        to unhealthy
//   AC6. Test runs on Linux + macOS
//
// Fixture scripts:
//   fake_mcp_stdio.py   — stdio/Content-Length  (echo_tool)
//   fake_mcp_http.py    — HTTP POST JSON-RPC     (http_tool)
//   fake_mcp_sse.py     — MCP-over-SSE           (sse_tool)
//   fake_mcp_ws.py      — JSON-RPC over WS       (ws_tool)
//
// BATBOX_FIXTURE_DIR is injected by CMake as a compile-time string macro.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/mcp/McpClient.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/mcp/StdioTransport.hpp>
#include <batbox/mcp/HttpTransport.hpp>
#include <batbox/mcp/SseTransport.hpp>
#include <batbox/mcp/WsTransport.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::mcp;

// ---------------------------------------------------------------------------
// Compile-time fixture directory (injected by CMake).
// ---------------------------------------------------------------------------
#ifndef BATBOX_FIXTURE_DIR
#  define BATBOX_FIXTURE_DIR "tests/fixtures"
#endif

// ---------------------------------------------------------------------------
// Fixture path helpers
// ---------------------------------------------------------------------------

static std::string fixture_dir() {
    // Try the compile-time path first.
    if (fs::exists(BATBOX_FIXTURE_DIR)) {
        return std::string(BATBOX_FIXTURE_DIR);
    }
    // Walk up from cwd to find the repo root.
    fs::path dir = fs::current_path();
    for (int d = 0; d < 10; ++d) {
        fs::path candidate = dir / "tests" / "fixtures";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return std::string(BATBOX_FIXTURE_DIR);
}

static std::string fixture_path(const std::string& name) {
    return fixture_dir() + "/" + name;
}

// ---------------------------------------------------------------------------
// FakeNetServer RAII
//
// Spawns a Python script, captures "READY <port>" from its stdout, then
// provides the port to tests.  SIGTERM on destruction.
// ---------------------------------------------------------------------------
struct FakeNetServer {
    pid_t pid{-1};
    int   port{0};
    FILE* out{nullptr};

    /// Start the script with optional extra args.
    bool start(const std::string& script,
               const std::vector<std::string>& extra_args = {}) {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]);
            ::close(pipefd[1]);
            return false;
        }

        if (pid == 0) {
            // Child: wire stdout to the pipe write end.
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            // Suppress fixture stderr to keep test output clean.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }

            std::vector<const char*> argv;
            argv.push_back("python3");
            argv.push_back(script.c_str());
            for (const auto& a : extra_args) argv.push_back(a.c_str());
            argv.push_back(nullptr);
            ::execvp("python3", const_cast<char* const*>(argv.data()));
            ::_exit(127);
        }

        // Parent: read "READY <port>".
        ::close(pipefd[1]);
        out = ::fdopen(pipefd[0], "r");
        if (!out) { ::kill(pid, SIGTERM); pid = -1; return false; }

        char line[256] = {};
        if (!::fgets(line, sizeof(line), out)) { stop(); return false; }
        if (::sscanf(line, "READY %d", &port) != 1) { stop(); return false; }
        return port > 0;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            // Wait up to 3 s for graceful exit.
            for (int i = 0; i < 30; ++i) {
                if (::waitpid(pid, &status, WNOHANG) == pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            pid = -1;
        }
        if (out) { ::fclose(out); out = nullptr; }
        port = 0;
    }

    ~FakeNetServer() { stop(); }
};

// ---------------------------------------------------------------------------
// Helper: make a non-cancelling token.
// ---------------------------------------------------------------------------
static CancelToken never_cancel() {
    static CancelSource src;
    return src.token();
}

// ---------------------------------------------------------------------------
// Helper: build URLs.
// ---------------------------------------------------------------------------
static std::string http_url(int port)  { return "http://127.0.0.1:" + std::to_string(port); }
static std::string sse_url(int port)   { return "http://127.0.0.1:" + std::to_string(port) + "/sse"; }
static std::string ws_url(int port)    { return "ws://127.0.0.1:"   + std::to_string(port); }

// ---------------------------------------------------------------------------
// Wait helper: poll healthy() until true or timeout.
// ---------------------------------------------------------------------------
static bool wait_healthy(const IMcpTransport& t, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (t.healthy()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return t.healthy();
}

// ===========================================================================
// AC1 — All 4 fixture servers boot and transports reach healthy state.
// ===========================================================================

TEST_CASE("AC1: all 4 fixture servers boot") {
    const std::string dir = fixture_dir();

    // --- Stdio transport (no port — spawned directly by StdioTransport) ---
    {
        const std::string script = dir + "/fake_mcp_stdio.py";
        REQUIRE(fs::exists(script));
        StdioTransport stdio_t("python3", {script});
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(stdio_t.start(std::move(ct)).has_value());
        CHECK(stdio_t.healthy());
        stdio_t.stop();
    }

    // --- HTTP transport ---
    {
        FakeNetServer http_srv;
        REQUIRE(http_srv.start(dir + "/fake_mcp_http.py"));
        HttpTransport http_t(http_url(http_srv.port));
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(http_t.start(std::move(ct)).has_value());
        CHECK(http_t.healthy());
        http_t.stop();
    }

    // --- SSE transport ---
    {
        FakeNetServer sse_srv;
        REQUIRE(sse_srv.start(dir + "/fake_mcp_sse.py"));
        SseTransport sse_t(sse_url(sse_srv.port));
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(sse_t.start(std::move(ct)).has_value());
        CHECK(sse_t.healthy());
        sse_t.stop();
    }

    // --- WebSocket transport ---
    {
        FakeNetServer ws_srv;
        REQUIRE(ws_srv.start(dir + "/fake_mcp_ws.py"));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        WsTransport ws_t(ws_url(ws_srv.port));
        auto [src, ct] = CancelToken::make_root();
        REQUIRE(ws_t.start(std::move(ct)).has_value());
        CHECK(ws_t.healthy());
        ws_t.stop();
    }
}

// ===========================================================================
// AC2 — McpClient orchestrates all 4 transports with parallel initialize.
// ===========================================================================

TEST_CASE("AC2: McpClient parallel initialize across all 4 transports") {
    const std::string dir = fixture_dir();

    // Boot network fixture servers.
    FakeNetServer http_srv, sse_srv, ws_srv;
    REQUIRE(http_srv.start(dir + "/fake_mcp_http.py"));
    REQUIRE(sse_srv.start(dir + "/fake_mcp_sse.py"));
    REQUIRE(ws_srv.start(dir + "/fake_mcp_ws.py"));
    // Brief pause for WS server event loop to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Build registry with all 4 transports.
    McpServerRegistry reg;

    auto stdio_up = std::make_unique<StdioTransport>(
        "python3",
        std::vector<std::string>{dir + "/fake_mcp_stdio.py"});
    auto http_up  = std::make_unique<HttpTransport>(http_url(http_srv.port));
    auto sse_up   = std::make_unique<SseTransport>(sse_url(sse_srv.port));
    auto ws_up    = std::make_unique<WsTransport>(ws_url(ws_srv.port));

    reg.add_transport("stdio", std::move(stdio_up));
    reg.add_transport("http",  std::move(http_up));
    reg.add_transport("sse",   std::move(sse_up));
    reg.add_transport("ws",    std::move(ws_up));

    // start_all brings all transports to healthy state in parallel.
    {
        auto [src, ct] = CancelToken::make_root();
        auto errs = reg.start_all(src.token());
        CHECK(errs.empty());
    }

    // All transports should be healthy.
    CHECK(reg.get("stdio") != nullptr);
    CHECK(reg.get("http")  != nullptr);
    CHECK(reg.get("sse")   != nullptr);
    CHECK(reg.get("ws")    != nullptr);

    CHECK(reg.get("stdio")->healthy());
    CHECK(reg.get("http")->healthy());
    CHECK(reg.get("sse")->healthy());
    CHECK(reg.get("ws")->healthy());

    // McpClient runs initialize handshake on all 4 in parallel.
    McpClient client(reg);
    {
        auto r = client.initialize_all(never_cancel());
        REQUIRE(r.has_value());
    }

    // All 4 servers should be in initialized_servers().
    auto inited = client.initialized_servers();
    CHECK(inited.size() == 4);
    bool has_stdio = false, has_http = false, has_sse = false, has_ws = false;
    for (const auto& s : inited) {
        if (s == "stdio") has_stdio = true;
        if (s == "http")  has_http  = true;
        if (s == "sse")   has_sse   = true;
        if (s == "ws")    has_ws    = true;
    }
    CHECK(has_stdio);
    CHECK(has_http);
    CHECK(has_sse);
    CHECK(has_ws);

    reg.stop_all();
}

// ===========================================================================
// AC3 — Each transport's tools/list returns its declared tools.
// ===========================================================================

TEST_CASE("AC3: tools/list returns declared tools per transport") {
    const std::string dir = fixture_dir();

    FakeNetServer http_srv, sse_srv, ws_srv;
    REQUIRE(http_srv.start(dir + "/fake_mcp_http.py"));
    REQUIRE(sse_srv.start(dir + "/fake_mcp_sse.py"));
    REQUIRE(ws_srv.start(dir + "/fake_mcp_ws.py"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    McpServerRegistry reg;
    reg.add_transport("stdio", std::make_unique<StdioTransport>(
        "python3", std::vector<std::string>{dir + "/fake_mcp_stdio.py"}));
    reg.add_transport("http",  std::make_unique<HttpTransport>(http_url(http_srv.port)));
    reg.add_transport("sse",   std::make_unique<SseTransport>(sse_url(sse_srv.port)));
    reg.add_transport("ws",    std::make_unique<WsTransport>(ws_url(ws_srv.port)));

    {
        auto [src, ct] = CancelToken::make_root();
        auto errs = reg.start_all(src.token());
        CHECK(errs.empty());
    }

    McpClient client(reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // --- Stdio: fake_mcp_stdio.py advertises echo_tool ---
    {
        auto r = client.tools_list("stdio", never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("tools"));
        const auto& tools = (*r)["tools"];
        REQUIRE(tools.is_array());
        REQUIRE(!tools.empty());
        bool found = false;
        for (const auto& t : tools) {
            if (t["name"].get<std::string>() == "echo_tool") found = true;
        }
        CHECK(found);
    }

    // --- HTTP: fake_mcp_http.py advertises http_tool ---
    {
        auto r = client.tools_list("http", never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("tools"));
        const auto& tools = (*r)["tools"];
        REQUIRE(tools.is_array());
        bool found = false;
        for (const auto& t : tools) {
            if (t["name"].get<std::string>() == "http_tool") found = true;
        }
        CHECK(found);
    }

    // --- SSE: fake_mcp_sse.py advertises sse_tool ---
    {
        auto r = client.tools_list("sse", never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("tools"));
        const auto& tools = (*r)["tools"];
        REQUIRE(tools.is_array());
        bool found = false;
        for (const auto& t : tools) {
            if (t["name"].get<std::string>() == "sse_tool") found = true;
        }
        CHECK(found);
    }

    // --- WS: fake_mcp_ws.py advertises ws_tool ---
    {
        auto r = client.tools_list("ws", never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("tools"));
        const auto& tools = (*r)["tools"];
        REQUIRE(tools.is_array());
        bool found = false;
        for (const auto& t : tools) {
            if (t["name"].get<std::string>() == "ws_tool") found = true;
        }
        CHECK(found);
    }

    reg.stop_all();
}

// ===========================================================================
// AC4 — tools/call dispatch routes to the correct transport.
// ===========================================================================

TEST_CASE("AC4: tools/call routed to correct transport") {
    const std::string dir = fixture_dir();

    FakeNetServer http_srv, sse_srv, ws_srv;
    REQUIRE(http_srv.start(dir + "/fake_mcp_http.py"));
    REQUIRE(sse_srv.start(dir + "/fake_mcp_sse.py"));
    REQUIRE(ws_srv.start(dir + "/fake_mcp_ws.py"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    McpServerRegistry reg;
    reg.add_transport("stdio", std::make_unique<StdioTransport>(
        "python3", std::vector<std::string>{dir + "/fake_mcp_stdio.py"}));
    reg.add_transport("http",  std::make_unique<HttpTransport>(http_url(http_srv.port)));
    reg.add_transport("sse",   std::make_unique<SseTransport>(sse_url(sse_srv.port)));
    reg.add_transport("ws",    std::make_unique<WsTransport>(ws_url(ws_srv.port)));

    {
        auto [src, ct] = CancelToken::make_root();
        auto errs = reg.start_all(src.token());
        CHECK(errs.empty());
    }

    McpClient client(reg);
    REQUIRE(client.initialize_all(never_cancel()).has_value());

    // --- Stdio: call echo_tool, expect the message echoed back ---
    {
        Json args = {{"message", "hello-from-stdio"}};
        auto r = client.tools_call("stdio", "echo_tool", args, never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("content"));
        const auto& content = (*r)["content"];
        REQUIRE(content.is_array());
        REQUIRE(!content.empty());
        CHECK(content[0]["text"].get<std::string>() == "hello-from-stdio");
    }

    // --- HTTP: call http_tool ---
    {
        Json args = {{"message", "hello-from-http"}};
        auto r = client.tools_call("http", "http_tool", args, never_cancel());
        REQUIRE(r.has_value());
        // Fixture returns content array.
        REQUIRE((*r).contains("content"));
        const auto& content = (*r)["content"];
        REQUIRE(content.is_array());
        REQUIRE(!content.empty());
        CHECK(content[0]["text"].get<std::string>() == "hello-from-http");
    }

    // --- SSE: call sse_tool ---
    {
        Json args = {{"message", "hello-from-sse"}};
        auto r = client.tools_call("sse", "sse_tool", args, never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("content"));
        const auto& content = (*r)["content"];
        REQUIRE(content.is_array());
        REQUIRE(!content.empty());
        CHECK(content[0]["text"].get<std::string>() == "hello-from-sse");
    }

    // --- WS: call ws_tool ---
    {
        Json args = {{"message", "hello-from-ws"}};
        auto r = client.tools_call("ws", "ws_tool", args, never_cancel());
        REQUIRE(r.has_value());
        REQUIRE((*r).contains("content"));
        const auto& content = (*r)["content"];
        REQUIRE(content.is_array());
        REQUIRE(!content.empty());
        CHECK(content[0]["text"].get<std::string>() == "hello-from-ws");
    }

    reg.stop_all();
}

// ===========================================================================
// AC5 — One transport crash: others remain healthy; crashed one goes unhealthy.
// ===========================================================================

TEST_CASE("AC5: crash isolation — one server crash leaves others healthy") {
    const std::string dir = fixture_dir();

    FakeNetServer http_srv, sse_srv, ws_srv;
    REQUIRE(http_srv.start(dir + "/fake_mcp_http.py"));
    REQUIRE(sse_srv.start(dir + "/fake_mcp_sse.py"));
    REQUIRE(ws_srv.start(dir + "/fake_mcp_ws.py"));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    McpServerRegistry reg;
    reg.add_transport("stdio", std::make_unique<StdioTransport>(
        "python3", std::vector<std::string>{dir + "/fake_mcp_stdio.py"}));
    reg.add_transport("http",  std::make_unique<HttpTransport>(http_url(http_srv.port)));
    reg.add_transport("sse",   std::make_unique<SseTransport>(sse_url(sse_srv.port)));
    reg.add_transport("ws",    std::make_unique<WsTransport>(ws_url(ws_srv.port)));

    {
        auto [src, ct] = CancelToken::make_root();
        auto errs = reg.start_all(src.token());
        CHECK(errs.empty());
    }

    // All should be healthy before crash.
    CHECK(reg.get("stdio")->healthy());
    CHECK(reg.get("http")->healthy());
    CHECK(reg.get("sse")->healthy());
    CHECK(reg.get("ws")->healthy());

    // Crash the SSE server by killing the fixture process.
    sse_srv.stop();

    // Give the SSE transport time to detect the connection drop.
    bool sse_unhealthy = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!reg.get("sse")->healthy()) {
            sse_unhealthy = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // SSE transport must have transitioned to unhealthy.
    CHECK(sse_unhealthy);

    // All other transports must remain healthy.
    CHECK(reg.get("stdio")->healthy());
    CHECK(reg.get("http")->healthy());
    CHECK(reg.get("ws")->healthy());

    // Verify remaining transports still respond.
    {
        auto [src, ct] = CancelToken::make_root();
        auto r = reg.get("stdio")->request("tools/list", Json::object(), std::move(ct));
        CHECK(r.has_value());
    }
    {
        auto [src, ct] = CancelToken::make_root();
        auto r = reg.get("http")->request("tools/list", Json::object(), std::move(ct));
        CHECK(r.has_value());
    }
    {
        auto [src, ct] = CancelToken::make_root();
        auto r = reg.get("ws")->request("ping", Json::object(), std::move(ct));
        CHECK(r.has_value());
    }

    reg.stop_all();
}
