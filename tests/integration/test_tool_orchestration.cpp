// tests/integration/test_tool_orchestration.cpp
// =============================================================================
// doctest integration tests for batbox::conversation::ToolCallOrchestrator
// (CPP 3.4).
//
// Acceptance criteria:
//   [AC1] Single tool call: accumulate → dispatch → Tool message returned.
//   [AC2] Multiple parallel calls: all dispatched; one Tool message per call.
//   [AC3] User denial: Tool message with is_error=true, body="user denied tool call".
//   [AC4] Malformed args: Tool message with parse error in body; other calls intact.
//   [AC5] Integration test passes.
//
// Additional cases covered:
//   - Cancellation before dispatch: stops iteration; already-dispatched msgs returned.
//   - Cancellation after permission: stops before next tool dispatch.
//   - Progress callback fires with correct tool name and args.
//   - Dispatch error (unknown tool) → Tool message with error body.
//   - persist_rule from Allow decision → store.add_allow_rule called.
//   - ToolResult::is_error propagated to Message::is_error.
//   - call_id and tool_name are correctly wired into Tool messages.
//   - Empty accumulator → dispatch_all returns empty vector.
//   - Permission Deny → no dispatch call made.
//
// Build standalone (no CMake, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_tool_orchestration.cpp \
//       src/conversation/ToolCallOrchestrator.cpp \
//       src/conversation/Message.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/inference/ChatRequest.cpp \
//       src/tools/ToolRegistry.cpp \
//       src/permissions/PermissionGate.cpp \
//       src/permissions/PermissionMode.cpp \
//       src/permissions/PermissionRule.cpp \
//       src/permissions/PatternMatcher.cpp \
//       src/permissions/PermissionStore.cpp \
//       src/config/SettingsLoader.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Json.cpp \
//       src/core/Paths.cpp src/core/CancelToken.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_orch && /tmp/test_orch
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/ToolCallOrchestrator.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Uuid.hpp>
#include <batbox/inference/ChatResponse.hpp>
#include <batbox/inference/ToolCallAccumulator.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/tools/ITool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using batbox::CancelSource;
using batbox::conversation::Message;
using batbox::conversation::Role;
using batbox::conversation::ToolCallOrchestrator;
using batbox::inference::ToolCallDelta;
using batbox::Json;
using batbox::permissions::Decision;
using batbox::permissions::PermissionGate;
using batbox::permissions::PermissionMode;
using batbox::permissions::PermissionStore;
using batbox::permissions::PermissionRule;
using batbox::tools::ITool;
using batbox::tools::ToolContext;
using batbox::tools::ToolRegistry;
using batbox::tools::ToolResult;

// =============================================================================
// Helpers
// =============================================================================

/// Build a ToolCallDelta — helper analogous to the one in test_tool_call_accumulator.
static ToolCallDelta make_delta(int                         index,
                                std::optional<std::string>  id,
                                std::optional<std::string>  name,
                                std::optional<std::string>  args_fragment)
{
    ToolCallDelta d;
    d.index              = index;
    d.id                 = std::move(id);
    d.name               = std::move(name);
    d.arguments_fragment = std::move(args_fragment);
    return d;
}

/// Build a complete single-chunk delta (id + name + full args in one shot).
static ToolCallDelta one_shot_delta(int                index,
                                    const std::string& id,
                                    const std::string& name,
                                    const std::string& args_json)
{
    return make_delta(index, id, name, args_json);
}

// =============================================================================
// Mock tool — returns whatever body was configured at construction.
// =============================================================================
class MockTool : public ITool {
public:
    explicit MockTool(std::string tool_name,
                      ToolResult  result,
                      bool        read_only = false)
        : name_(std::move(tool_name))
        , result_(std::move(result))
        , read_only_(read_only)
    {}

    [[nodiscard]] std::string_view name()        const override { return name_; }
    [[nodiscard]] std::string_view description() const override { return "mock tool"; }
    [[nodiscard]] Json             schema_json() const override {
        return Json{{"name", name_}, {"description", "mock"}, {"parameters", Json::object()}};
    }
    [[nodiscard]] ToolResult run(const Json& /*args*/, ToolContext& /*ctx*/) override {
        return result_;
    }
    [[nodiscard]] bool is_read_only()         const override { return read_only_; }
    [[nodiscard]] bool requires_confirmation() const override { return !read_only_; }

private:
    std::string name_;
    ToolResult  result_;
    bool        read_only_;
};

// =============================================================================
// Fixture — builds a minimal wired-up test harness.
// =============================================================================
struct Fixture {
    // Temporary directory for the PermissionStore's settings.json backing file.
    fs::path tmp_dir;

    std::shared_ptr<PermissionStore>     store;
    std::unique_ptr<PermissionGate>      gate;
    ToolRegistry                         registry;
    CancelSource                         cancel_src;

    ToolContext ctx;

    // Convenience: the allow-all prompt callback.
    PermissionGate::PromptFn always_allow = [](std::string_view, const Json&) {
        return Decision::allow();
    };

    // Convenience: the deny-all prompt callback.
    PermissionGate::PromptFn always_deny = [](std::string_view, const Json&) {
        return Decision::deny();
    };

    explicit Fixture(PermissionMode mode = PermissionMode::Nuclear,
                     PermissionGate::PromptFn prompt = nullptr)
    {
        tmp_dir = fs::temp_directory_path() / ("test_orch_" + batbox::uuid_v4());
        fs::create_directories(tmp_dir);

        store = std::make_shared<PermissionStore>(
            tmp_dir / "settings.json");

        if (!prompt) {
            // Nuclear mode: gate always allows without prompting.
            prompt = [](std::string_view, const Json&) { return Decision::allow(); };
        }

        gate = std::make_unique<PermissionGate>(store, mode, std::move(prompt));

        ctx.cwd          = tmp_dir;
        ctx.mode         = mode;
        ctx.session_id   = batbox::uuid_v4();
        ctx.agent_id     = "";
        ctx.cancel_token = cancel_src.token();
    }

    ~Fixture() {
        // Best-effort cleanup.
        std::error_code ec;
        fs::remove_all(tmp_dir, ec);
    }

    /// Register a mock tool that returns the given ToolResult.
    void register_mock(const std::string& name,
                       ToolResult         result,
                       bool               read_only = false)
    {
        registry.register_tool(
            std::make_unique<MockTool>(name, std::move(result), read_only));
    }

    /// Construct an orchestrator backed by this fixture.
    /// progress_cb is forwarded if provided.
    ToolCallOrchestrator make_orchestrator(
            ToolCallOrchestrator::ProgressFn progress_cb = nullptr)
    {
        return ToolCallOrchestrator(registry, *gate, std::move(progress_cb));
    }
};

// =============================================================================
// AC1 — Single tool call: accumulate → dispatch → Tool message returned.
// =============================================================================
TEST_SUITE("ToolCallOrchestrator [AC1] single tool call") {

    TEST_CASE("accumulate + dispatch_all returns one Tool message with correct fields") {
        Fixture f;
        f.register_mock("get_weather", ToolResult::ok(R"({"temp":22,"unit":"C"})"));

        auto orch = f.make_orchestrator();

        // Simulate streaming: id + name on first chunk, two arg fragments.
        orch.accumulate(make_delta(0, "call_wx_01", "get_weather", std::string("{\"loc")));
        orch.accumulate(make_delta(0, std::nullopt, std::nullopt, std::string("ation\":\"Paris\"}")));

        auto msgs = orch.dispatch_all(f.ctx);

        REQUIRE(msgs.size() == 1);
        const Message& m = msgs[0];
        CHECK(m.role == Role::Tool);
        CHECK(m.tool_call_id.has_value());
        CHECK(m.tool_call_id.value() == "call_wx_01");
        CHECK(m.tool_name.has_value());
        CHECK(m.tool_name.value() == "get_weather");
        CHECK(!m.id.empty());  // UUID auto-assigned
        // Successful result: is_error absent or false.
        CHECK((!m.is_error.has_value() || m.is_error.value() == false));
        CHECK(m.content == R"({"temp":22,"unit":"C"})");
    }

    TEST_CASE("one-shot delta (full args in single chunk) dispatches correctly") {
        Fixture f;
        f.register_mock("calculator", ToolResult::ok("42"));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "call_calc", "calculator", R"({"expr":"6*7"})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].content == "42");
        CHECK(msgs[0].tool_name.value() == "calculator");
        CHECK(msgs[0].tool_call_id.value() == "call_calc");
    }

    TEST_CASE("tool returning is_error=true propagates to Message::is_error") {
        Fixture f;
        f.register_mock("failing_tool", ToolResult::error("file not found: /tmp/ghost"));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "call_fail", "failing_tool", R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].is_error.has_value());
        CHECK(msgs[0].is_error.value() == true);
        CHECK(msgs[0].content == "file not found: /tmp/ghost");
    }

    TEST_CASE("empty accumulator returns empty vector") {
        Fixture f;
        auto orch = f.make_orchestrator();
        auto msgs = orch.dispatch_all(f.ctx);
        CHECK(msgs.empty());
    }
}

// =============================================================================
// AC2 — Multiple parallel calls: one Tool message per call, all dispatched.
// =============================================================================
TEST_SUITE("ToolCallOrchestrator [AC2] multiple parallel calls") {

    TEST_CASE("two independent calls produce two Tool messages in accumulation order") {
        Fixture f;
        f.register_mock("tool_a", ToolResult::ok("result_a"));
        f.register_mock("tool_b", ToolResult::ok("result_b"));

        auto orch = f.make_orchestrator();

        // Interleaved streaming of two parallel calls (index 0 and index 1).
        orch.accumulate(make_delta(0, "call_aaa", "tool_a", std::string("{}")));
        orch.accumulate(make_delta(1, "call_bbb", "tool_b", std::string("{}")));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 2);

        // Index 0 should come first (ascending index order from ToolCallAccumulator).
        CHECK(msgs[0].tool_name.value() == "tool_a");
        CHECK(msgs[0].tool_call_id.value() == "call_aaa");
        CHECK(msgs[0].content == "result_a");

        CHECK(msgs[1].tool_name.value() == "tool_b");
        CHECK(msgs[1].tool_call_id.value() == "call_bbb");
        CHECK(msgs[1].content == "result_b");
    }

    TEST_CASE("three parallel calls all dispatched; each has distinct id") {
        Fixture f;
        f.register_mock("read_file", ToolResult::ok("file content"));
        f.register_mock("list_dir",  ToolResult::ok("dir listing"));
        f.register_mock("get_cwd",   ToolResult::ok("/home/user"));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "c0", "read_file", R"({"path":"/a"})"));
        orch.accumulate(one_shot_delta(1, "c1", "list_dir",  R"({"path":"/b"})"));
        orch.accumulate(one_shot_delta(2, "c2", "get_cwd",   R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 3);

        // Each message must have a non-empty unique Message::id.
        CHECK(msgs[0].id != msgs[1].id);
        CHECK(msgs[1].id != msgs[2].id);
        CHECK(msgs[0].id != msgs[2].id);

        CHECK(msgs[0].tool_call_id.value() == "c0");
        CHECK(msgs[1].tool_call_id.value() == "c1");
        CHECK(msgs[2].tool_call_id.value() == "c2");
    }
}

// =============================================================================
// AC3 — User denial: Tool message with is_error=true, body="user denied tool call".
// =============================================================================
TEST_SUITE("ToolCallOrchestrator [AC3] user denial") {

    TEST_CASE("denied call produces is_error Tool message, no dispatch") {
        // Use Default mode with a deny-all prompt.
        bool dispatch_called = false;

        // Custom mock that sets a flag if run() is ever called.
        class SentinelTool : public ITool {
        public:
            bool& flag_;
            explicit SentinelTool(bool& flag) : flag_(flag) {}
            std::string_view name()        const override { return "sentinel"; }
            std::string_view description() const override { return "sentinel"; }
            Json schema_json() const override {
                return Json{{"name","sentinel"},{"description","s"},
                            {"parameters",Json::object()}};
            }
            ToolResult run(const Json&, ToolContext&) override {
                flag_ = true;
                return ToolResult::ok("should not reach");
            }
        };

        Fixture f(PermissionMode::Default,
                  [](std::string_view, const Json&) { return Decision::deny(); });
        f.registry.register_tool(std::make_unique<SentinelTool>(dispatch_called));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "call_s", "sentinel", R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].is_error.has_value());
        CHECK(msgs[0].is_error.value() == true);
        CHECK(msgs[0].content == "user denied tool call");
        CHECK(!dispatch_called);  // tool must NOT have been called
    }

    TEST_CASE("second call denied while first allowed — only first dispatched") {
        int call_count = 0;

        class CountingTool : public ITool {
        public:
            int& count_;
            std::string name_;
            explicit CountingTool(std::string name, int& c) : count_(c), name_(std::move(name)) {}
            std::string_view name()        const override { return name_; }
            std::string_view description() const override { return "counting"; }
            Json schema_json() const override {
                return Json{{"name",name_},{"description","c"},{"parameters",Json::object()}};
            }
            ToolResult run(const Json&, ToolContext&) override {
                ++count_;
                return ToolResult::ok("ok");
            }
        };

        // Allow first call, deny second.
        bool first_call = true;
        Fixture f(PermissionMode::Default,
                  [&first_call](std::string_view, const Json&) -> Decision {
                      if (first_call) {
                          first_call = false;
                          return Decision::allow();
                      }
                      return Decision::deny();
                  });

        f.registry.register_tool(std::make_unique<CountingTool>("tool_a", call_count));
        f.registry.register_tool(std::make_unique<CountingTool>("tool_b", call_count));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "c_a", "tool_a", R"({})"));
        orch.accumulate(one_shot_delta(1, "c_b", "tool_b", R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 2);

        CHECK(msgs[0].content == "ok");                    // tool_a ran
        CHECK(!msgs[0].is_error.has_value() ||
              !msgs[0].is_error.value());                  // no error

        CHECK(msgs[1].content == "user denied tool call"); // tool_b denied
        CHECK(msgs[1].is_error.has_value());
        CHECK(msgs[1].is_error.value() == true);
        CHECK(call_count == 1);                            // only one dispatch
    }
}

// =============================================================================
// AC4 — Malformed args: parse error Tool message; other calls unaffected.
// =============================================================================
TEST_SUITE("ToolCallOrchestrator [AC4] malformed args") {

    TEST_CASE("single malformed call produces error message, no dispatch") {
        Fixture f;
        // Tool registered but must NOT be called.
        bool dispatched = false;
        class NeverTool : public ITool {
        public:
            bool& flag_;
            NeverTool(bool& f) : flag_(f) {}
            std::string_view name() const override { return "never"; }
            std::string_view description() const override { return "never"; }
            Json schema_json() const override {
                return Json{{"name","never"},{"description","n"},{"parameters",Json::object()}};
            }
            ToolResult run(const Json&, ToolContext&) override {
                flag_ = true;
                return ToolResult::ok("should not reach");
            }
        };
        f.registry.register_tool(std::make_unique<NeverTool>(dispatched));

        auto orch = f.make_orchestrator();
        // Feed malformed JSON: stream never closes the brace.
        orch.accumulate(make_delta(0, "call_bad", "never", std::string("{\"broken\": ")));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].is_error.has_value());
        CHECK(msgs[0].is_error.value() == true);
        // body must contain "parse error"
        CHECK(msgs[0].content.find("parse error") != std::string::npos);
        CHECK(!dispatched);
    }

    TEST_CASE("malformed first call; valid second call still dispatched") {
        Fixture f;
        f.register_mock("good_tool", ToolResult::ok("good result"));

        auto orch = f.make_orchestrator();
        // index 0: malformed args
        orch.accumulate(make_delta(0, "call_bad", "bad_tool", std::string("{INVALID")));
        // index 1: valid call to a registered tool
        orch.accumulate(one_shot_delta(1, "call_good", "good_tool", R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 2);

        // First message: parse error (index 0)
        CHECK(msgs[0].is_error.has_value());
        CHECK(msgs[0].is_error.value() == true);
        CHECK(msgs[0].content.find("parse error") != std::string::npos);

        // Second message: successful dispatch (index 1)
        CHECK(msgs[1].content == "good result");
        CHECK(!msgs[1].is_error.has_value() || !msgs[1].is_error.value());
    }
}

// =============================================================================
// AC5 — Cancellation: stops iteration; already-dispatched messages returned.
// =============================================================================
TEST_SUITE("ToolCallOrchestrator [AC5] cancellation") {

    TEST_CASE("token cancelled before dispatch_all: no messages produced") {
        Fixture f;
        f.register_mock("some_tool", ToolResult::ok("result"));

        f.cancel_src.request_stop();  // cancel before dispatch

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "call_x", "some_tool", R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        // No messages because the very first tool was cancelled before dispatch.
        CHECK(msgs.empty());
    }

    TEST_CASE("token cancelled between two calls: first result included, second skipped") {
        Fixture f;
        int dispatch_count = 0;
        CancelSource& cancel_ref = f.cancel_src;

        // Tool A: dispatch succeeds, then immediately cancel.
        class CancelAfterTool : public ITool {
        public:
            int& count_;
            CancelSource& src_;
            CancelAfterTool(int& c, CancelSource& s) : count_(c), src_(s) {}
            std::string_view name() const override { return "cancel_after"; }
            std::string_view description() const override { return "cancel_after"; }
            Json schema_json() const override {
                return Json{{"name","cancel_after"},{"description","ca"},
                            {"parameters",Json::object()}};
            }
            ToolResult run(const Json&, ToolContext&) override {
                ++count_;
                src_.request_stop();  // cancel on first dispatch
                return ToolResult::ok("first result");
            }
        };
        class NeverTool2 : public ITool {
        public:
            int& count_;
            NeverTool2(int& c) : count_(c) {}
            std::string_view name() const override { return "second_tool"; }
            std::string_view description() const override { return "second_tool"; }
            Json schema_json() const override {
                return Json{{"name","second_tool"},{"description","st"},
                            {"parameters",Json::object()}};
            }
            ToolResult run(const Json&, ToolContext&) override {
                ++count_;
                return ToolResult::ok("second result");
            }
        };

        f.registry.register_tool(std::make_unique<CancelAfterTool>(dispatch_count, cancel_ref));
        f.registry.register_tool(std::make_unique<NeverTool2>(dispatch_count));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "c0", "cancel_after", R"({})"));
        orch.accumulate(one_shot_delta(1, "c1", "second_tool",  R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        // First call completed before cancellation was noticed at the second iteration.
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].content == "first result");
        CHECK(dispatch_count == 1);  // second_tool never ran
    }
}

// =============================================================================
// Additional: dispatch error (unknown tool name)
// =============================================================================
TEST_SUITE("ToolCallOrchestrator — dispatch error (unknown tool)") {

    TEST_CASE("unknown tool name produces error Tool message") {
        Fixture f;
        // No tools registered.

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "call_unk", "no_such_tool", R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].is_error.has_value());
        CHECK(msgs[0].is_error.value() == true);
        CHECK(!msgs[0].content.empty());
        CHECK(msgs[0].tool_call_id.value() == "call_unk");
    }
}

// =============================================================================
// Additional: progress callback fires correctly
// =============================================================================
TEST_SUITE("ToolCallOrchestrator — progress callback") {

    TEST_CASE("progress_cb called with tool name and args before each dispatch") {
        Fixture f;
        f.register_mock("weather_tool", ToolResult::ok("sunny"));
        f.register_mock("time_tool",    ToolResult::ok("12:00"));

        std::vector<std::string> progress_names;
        auto orch = f.make_orchestrator(
            [&progress_names](std::string_view name, const Json& /*args*/) {
                progress_names.emplace_back(name);
            });

        orch.accumulate(one_shot_delta(0, "c0", "weather_tool", R"({})"));
        orch.accumulate(one_shot_delta(1, "c1", "time_tool",    R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 2);

        REQUIRE(progress_names.size() == 2);
        CHECK(progress_names[0] == "weather_tool");
        CHECK(progress_names[1] == "time_tool");
    }

    TEST_CASE("progress_cb NOT called for denied or parse-error calls") {
        Fixture f;
        // Deny-all gate.
        Fixture f2(PermissionMode::Default,
                   [](std::string_view, const Json&) { return Decision::deny(); });

        int progress_count = 0;
        auto orch = ToolCallOrchestrator(
            f2.registry, *f2.gate,
            [&progress_count](std::string_view, const Json&) { ++progress_count; });

        orch.accumulate(one_shot_delta(0, "c0", "some_tool", R"({})"));
        orch.dispatch_all(f2.ctx);

        CHECK(progress_count == 0);  // denied before progress callback fires
    }
}

// =============================================================================
// Additional: persist_rule wired through to PermissionStore
// =============================================================================
TEST_SUITE("ToolCallOrchestrator — persist_rule from gate") {

    TEST_CASE("allow_with_rule decision persists rule to store") {
        // Use Default mode; prompt returns allow_with_rule.
        Fixture f(PermissionMode::Default,
                  [](std::string_view tool, const Json&) {
                      return Decision::allow_with_rule(std::string(tool) + "(**)");
                  });
        f.register_mock("my_tool", ToolResult::ok("ok"));

        auto orch = f.make_orchestrator();
        orch.accumulate(one_shot_delta(0, "c0", "my_tool", R"({})"));
        auto msgs = orch.dispatch_all(f.ctx);

        REQUIRE(msgs.size() == 1);
        CHECK(!msgs[0].is_error.has_value() || !msgs[0].is_error.value());

        // The rule should now appear in the store's in-memory allow list.
        const auto& rules = f.store->allow_rules();
        bool found = false;
        for (const auto& pattern : rules) {
            if (pattern == "my_tool(**)") { found = true; break; }
        }
        CHECK(found);
    }
}

