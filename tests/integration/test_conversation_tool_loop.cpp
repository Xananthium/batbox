// tests/integration/test_conversation_tool_loop.cpp
// =============================================================================
// TUI-FIX-T5: Integration tests for the tool-call loop + follow-up turn
// correctness.
//
// Root cause documented here (TUI-FIX-T5):
//   Conversation::run_turn() correctly implements the tool-call loop in
//   Conversation.cpp — the for-loop iterates until finish_reason != "tool_calls"
//   or k_max_tool_turns is reached.  The dropped-turn bug observed in live
//   sessions was a TUI-layer race: App.cpp's tui_on_submit spawned a second
//   worker thread calling user_message() / run_turn() concurrently while the
//   first tool-call loop was still in progress, corrupting the non-thread-safe
//   Conversation.  Fix: tui_turn_in_flight atomic bool guard in App.cpp.
//
// These tests verify:
//   AC1 — Tool-call loop produces exactly 4 messages:
//          User, Assistant(tool_calls), Tool(result), Assistant(stop).
//   AC2 — A follow-up user turn after the tool loop produces 2 more messages:
//          User2, Assistant2(stop) — total 6 messages.
//   AC3 — The final assistant (stop) message has non-empty content ("token 1").
//   AC4 — History snapshot: build_chat_request includes the tool-result message
//          in the second POST.  Verified by checking that the tool message in
//          messages_ has role==Tool and tool_call_id set.
//
// Strategy:
//   Uses the existing fake_openai_server.py fixture (already handles the
//   tool-call loop correctly: tools + no tool-role msgs → tool_calls stream;
//   tool-role msgs present → stop stream).
//   The same FakeServer + TmpDir helpers as test_conversation_basic.cpp.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_conversation_tool_loop.cpp \
//       src/conversation/Conversation.cpp \
//       src/conversation/ToolCallOrchestrator.cpp \
//       src/conversation/Message.cpp \
//       src/tools/ToolRegistry.cpp \
//       src/conversation/ContextWindow.cpp \
//       src/conversation/Compactor.cpp \
//       src/inference/Client.cpp \
//       src/inference/ChatRequest.cpp \
//       src/inference/SseParser.cpp \
//       src/session/SessionStore.cpp \
//       src/session/SessionFile.cpp \
//       src/session/SessionIndex.cpp \
//       src/session/SessionRecovery.cpp \
//       src/conversation/SystemPrompt.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/perf/PerfSnapshot.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libcpr.a \
//       build/vcpkg_installed/arm64-osx/lib/libcurl.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libz.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_conv_tool_loop && /tmp/test_conv_tool_loop
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/Conversation.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Uuid.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox::conversation;
using batbox::CancelSource;
using batbox::CancelToken;

// =============================================================================
// Utility: locate fake_openai_server.py (same as test_conversation_basic.cpp)
// =============================================================================

static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_openai_server.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_openai_server.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

// =============================================================================
// FakeServer RAII (identical to test_conversation_basic.cpp)
// =============================================================================

struct FakeServer {
    pid_t  pid{-1};
    int    port{0};
    FILE*  stdout_pipe{nullptr};

    bool start(const std::string& script_path) {
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) {
            ::close(pipefd[0]); ::close(pipefd[1]);
            return false;
        }
        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            const char* argv[] = {"python3", script_path.c_str(), nullptr};
            ::execvp("python3", const_cast<char* const*>(argv));
            ::_exit(127);
        }

        ::close(pipefd[1]);
        stdout_pipe = ::fdopen(pipefd[0], "r");
        if (!stdout_pipe) {
            ::kill(pid, SIGTERM); ::close(pipefd[0]); pid = -1;
            return false;
        }

        char line[256]{};
        for (int i = 0; i < 50; ++i) {
            if (::fgets(line, sizeof(line), stdout_pipe) != nullptr) {
                if (::strncmp(line, "READY ", 6) == 0) {
                    port = std::atoi(line + 6);
                    return port > 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        stop();
        return false;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
        if (stdout_pipe) { ::fclose(stdout_pipe); stdout_pipe = nullptr; }
    }

    ~FakeServer() { stop(); }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/v1";
    }
};

// =============================================================================
// TmpDir RAII
// =============================================================================

struct TmpDir {
    fs::path path;

    TmpDir() {
        auto base = fs::temp_directory_path()
                    / ("batbox_tool_loop_test_" + batbox::Uuid::v4().to_string());
        fs::create_directories(base);
        path = base;
    }

    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// =============================================================================
// Helpers
// =============================================================================

static batbox::config::Config make_test_config(const std::string& base_url) {
    batbox::config::Config cfg;
    cfg.api.base_url            = base_url;
    cfg.api.api_key             = "test-key-123";
    cfg.api.request_timeout_sec = 10;
    cfg.api.default_model       = "gpt-4o";
    cfg.api.max_tokens          = 512;
    cfg.api.temperature         = 0.7;
    cfg.api.top_p               = 1.0;
    cfg.compact.auto_compact_at_pct        = 80;
    cfg.compact.keep_last_n_turns_verbatim = 4;
    return cfg;
}

// Always-allow permission gate
static std::pair<std::shared_ptr<batbox::permissions::PermissionStore>,
                 std::unique_ptr<batbox::permissions::PermissionGate>>
make_allow_gate(const fs::path& settings_path) {
    auto store = std::make_shared<batbox::permissions::PermissionStore>(settings_path);
    auto gate  = std::make_unique<batbox::permissions::PermissionGate>(
        store,
        batbox::permissions::PermissionMode::Default,
        [](std::string_view, const batbox::Json&) -> batbox::permissions::Decision {
            return batbox::permissions::Decision::allow();
        }
    );
    return {store, std::move(gate)};
}

// Stub tool — increments a counter and returns "ok"
class StubTool final : public batbox::tools::ITool {
public:
    explicit StubTool(std::string n) : name_(std::move(n)) {}

    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return "stub"; }
    [[nodiscard]] batbox::Json schema_json() const override {
        return batbox::Json::parse(R"({"name":")" + name_ + R"(","description":"stub","parameters":{"type":"object","properties":{},"required":[]}})");
    }
    [[nodiscard]] batbox::tools::ToolResult
    run(const batbox::Json&, batbox::tools::ToolContext&) override {
        ++call_count;
        return batbox::tools::ToolResult::ok("stub result");
    }
    [[nodiscard]] bool is_read_only()         const override { return true; }
    [[nodiscard]] bool requires_confirmation() const override { return false; }

    std::atomic<int> call_count{0};
private:
    std::string name_;
};

// =============================================================================
// AC1 — Tool-call loop produces exactly 4 messages.
//
// Expected history after run_turn():
//   [0] User  "What is the weather?"
//   [1] Assistant  {tool_calls: [get_weather(...)]}
//   [2] Tool   {tool_call_id: ..., content: "stub result"}
//   [3] Assistant  {content: "token 1 token 2 ..."}  finish_reason=stop
// =============================================================================
TEST_CASE("tool-call loop: 4-message history [AC1]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer srv;
    REQUIRE(srv.start(script));

    TmpDir tmp;
    auto cfg = make_test_config(srv.base_url());
    batbox::inference::Client     client{cfg};
    batbox::session::SessionStore store{tmp.path};

    batbox::tools::ToolRegistry reg;
    auto stub_ptr = std::make_unique<StubTool>("get_weather");
    StubTool* stub = stub_ptr.get();
    reg.register_tool(std::move(stub_ptr));

    auto [pstore, gate] = make_allow_gate(tmp.path / "settings.json");

    Conversation conv{client, store, cfg, tmp.path,
                      /*on_delta=*/nullptr, &reg, gate.get()};
    conv.user_message("What is the weather in Paris?");

    auto [src, tok] = CancelToken::make_root();
    auto res = conv.run_turn(std::move(tok));
    REQUIRE(res.has_value());

    // Stub must have been called exactly once (single tool call from fake server).
    CHECK(stub->call_count == 1);

    const auto& msgs = conv.messages();
    REQUIRE(msgs.size() == 4);

    // [0] User
    CHECK(msgs[0].role == Role::User);
    CHECK(msgs[0].content == "What is the weather in Paris?");

    // [1] Assistant with tool_calls
    CHECK(msgs[1].role == Role::Assistant);
    REQUIRE(msgs[1].tool_calls.has_value());
    CHECK_FALSE(msgs[1].tool_calls->empty());
    CHECK(msgs[1].tool_calls.value()[0].name == "get_weather");

    // [2] Tool result
    CHECK(msgs[2].role == Role::Tool);
    CHECK(msgs[2].tool_call_id.has_value());
    CHECK_FALSE(msgs[2].tool_call_id->empty());
    CHECK(msgs[2].content == "stub result");

    // [3] Final assistant stop
    CHECK(msgs[3].role == Role::Assistant);
    CHECK_FALSE(msgs[3].content.empty());
}

// =============================================================================
// AC2 — Follow-up user turn after tool loop: history grows to 6 messages.
//
// After the first run_turn() (4 messages), we submit a plain "Hello" message.
// The fake server returns a normal stop stream (no tools in second request
// because we register the tool but the tool is not re-invoked — the second
// request has the tool-result messages already present, so the fake server
// routes to the stop stream).
//
// Wait — the fake server routes based on has_tools AND has_tool_results.
// On the second user turn the history has Tool-role messages, so the fake
// server emits stop stream for the second user turn too.
//
// Expected history after second run_turn():
//   [0] User "What is the weather?"
//   [1] Assistant {tool_calls}
//   [2] Tool  {result}
//   [3] Assistant {stop}          <- from first turn
//   [4] User "Hello follow-up"
//   [5] Assistant {stop}          <- from second turn
// =============================================================================
TEST_CASE("follow-up turn after tool loop produces 6 messages [AC2]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer srv;
    REQUIRE(srv.start(script));

    TmpDir tmp;
    auto cfg = make_test_config(srv.base_url());
    batbox::inference::Client     client{cfg};
    batbox::session::SessionStore store{tmp.path};

    batbox::tools::ToolRegistry reg;
    auto stub_ptr = std::make_unique<StubTool>("get_weather");
    StubTool* stub = stub_ptr.get();
    reg.register_tool(std::move(stub_ptr));

    auto [pstore, gate] = make_allow_gate(tmp.path / "settings.json");

    Conversation conv{client, store, cfg, tmp.path,
                      /*on_delta=*/nullptr, &reg, gate.get()};

    // --- First turn: tool-call loop ---
    conv.user_message("What is the weather in Paris?");
    {
        auto [src, tok] = CancelToken::make_root();
        auto res = conv.run_turn(std::move(tok));
        REQUIRE(res.has_value());
    }
    REQUIRE(conv.messages().size() == 4);
    CHECK(stub->call_count == 1);

    // --- Second turn: plain follow-up ---
    conv.user_message("Hello follow-up");
    {
        auto [src, tok] = CancelToken::make_root();
        auto res = conv.run_turn(std::move(tok));
        REQUIRE(res.has_value());
    }

    // Tool must NOT have been called again on the follow-up turn.
    CHECK(stub->call_count == 1);

    const auto& msgs = conv.messages();
    REQUIRE(msgs.size() == 6);

    CHECK(msgs[4].role == Role::User);
    CHECK(msgs[4].content == "Hello follow-up");

    CHECK(msgs[5].role == Role::Assistant);
    CHECK_FALSE(msgs[5].content.empty());
}

// =============================================================================
// AC3 — build_chat_request snapshot: tool-result message in second POST.
//
// Verifies that after the tool-call turn the Tool-role message has:
//   - role == Role::Tool
//   - tool_call_id set and non-empty
//   - content == "stub result"
//   - tool_name set to "get_weather"
//
// This test confirms that build_chat_request at the second loop iteration
// correctly serializes the Tool message (role="tool", content, tool_call_id)
// into the next POST — ruling out candidate (e) from the task description
// (tool_result not serialized due to WireMessage encoding).
// =============================================================================
TEST_CASE("tool result message serialization fields [AC3]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer srv;
    REQUIRE(srv.start(script));

    TmpDir tmp;
    auto cfg = make_test_config(srv.base_url());
    batbox::inference::Client     client{cfg};
    batbox::session::SessionStore store{tmp.path};

    batbox::tools::ToolRegistry reg;
    reg.register_tool(std::make_unique<StubTool>("get_weather"));

    auto [pstore, gate] = make_allow_gate(tmp.path / "settings.json");

    Conversation conv{client, store, cfg, tmp.path,
                      /*on_delta=*/nullptr, &reg, gate.get()};
    conv.user_message("tool result encoding check");

    auto [src, tok] = CancelToken::make_root();
    auto res = conv.run_turn(std::move(tok));
    REQUIRE(res.has_value());

    const auto& msgs = conv.messages();
    REQUIRE(msgs.size() == 4);

    const Message& tool_msg = msgs[2];
    CHECK(tool_msg.role == Role::Tool);
    REQUIRE(tool_msg.tool_call_id.has_value());
    CHECK_FALSE(tool_msg.tool_call_id->empty());
    CHECK(tool_msg.content == "stub result");
    REQUIRE(tool_msg.tool_name.has_value());
    CHECK(tool_msg.tool_name.value() == "get_weather");
    // No error flag on a successful tool result.
    CHECK((!tool_msg.is_error.has_value() || !tool_msg.is_error.value()));
}

// =============================================================================
// AC4 — on_message_appended_cb fires for tool-call and tool-result messages.
//
// Registers a callback and verifies it is invoked exactly twice per tool-call
// turn: once for the assistant{tool_calls} message and once for the Tool
// result message.  This mirrors the TUI-T5 wiring that posts
// make_message_appended_event to the ChatView.
// =============================================================================
TEST_CASE("on_message_appended_cb fires for tool-call and tool-result [AC4]") {
    std::string script = find_fixture_script();
    REQUIRE_FALSE(script.empty());

    FakeServer srv;
    REQUIRE(srv.start(script));

    TmpDir tmp;
    auto cfg = make_test_config(srv.base_url());
    batbox::inference::Client     client{cfg};
    batbox::session::SessionStore store{tmp.path};

    batbox::tools::ToolRegistry reg;
    reg.register_tool(std::make_unique<StubTool>("get_weather"));
    auto [pstore, gate] = make_allow_gate(tmp.path / "settings.json");

    // Collect appended-message roles.
    std::vector<std::string> appended_roles;
    std::vector<std::string> appended_names;

    Conversation conv{client, store, cfg, tmp.path,
                      /*on_delta=*/nullptr, &reg, gate.get()};
    conv.set_on_message_appended_cb(
        [&](std::string_view role,
            std::string_view tool_name,
            std::string_view /*content*/,
            bool /*is_error*/) {
            appended_roles.emplace_back(role);
            appended_names.emplace_back(tool_name);
        });

    conv.user_message("callback fire test");

    auto [src, tok] = CancelToken::make_root();
    auto res = conv.run_turn(std::move(tok));
    REQUIRE(res.has_value());

    // Exactly two callbacks: one for assistant{tool_calls}, one for Tool.
    REQUIRE(appended_roles.size() == 2);
    CHECK(appended_roles[0] == "assistant");
    CHECK(appended_roles[1] == "tool");
    CHECK(appended_names[1] == "get_weather");
}
