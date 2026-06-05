// tests/integration/test_envelope_no_bypass.cpp
// =============================================================================
// doctest integration tests for the S7 universal subagent-dispatch seam
// (DIS-979): batbox::tools::ToolSubagentEnvelope wired into ToolRegistry,
// exercised through the full ToolCallOrchestrator path.
//
// The decisive invariant: there is NO un-wrapped path from an accumulated
// tool_call to a Tool message — every dispatched tool result (native AND MCP)
// flows through the envelope exactly once.
//
// Acceptance coverage:
//   [AC2] No un-wrapped path + MCP parity:
//         - A sentinel distiller (replaces every body) installed on the registry
//           envelope rewrites the body of BOTH a native tool and an MCP-shaped
//           tool when driven end-to-end through the orchestrator.  If any path
//           bypassed the envelope, the original body would leak through.
//         - A counting spy distiller observes EXACTLY ONE pass per dispatched
//           call; pre-run rejections (unknown tool, plan-mode block) produce
//           ZERO envelope passes (they short-circuit before run()).
//         - static_assert proves the live McpTool IS-A ITool, so it is resolved
//           and dispatched through the identical ToolRegistry::dispatch path.
//   [AC3] No regression: with the default (pass-through) envelope, results are
//         byte-identical to pre-S7.
//   [AC4] Swappable hooks: the same seam transforms results once a non-default
//         distiller is installed — proving S1+S4 fill the hooks without touching
//         the wiring.
//
// Build standalone (no CMake, from repo root; x64-linux triplet):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/x64-linux/include \
//       tests/integration/test_envelope_no_bypass.cpp \
//       src/conversation/ToolCallOrchestrator.cpp \
//       src/conversation/Message.cpp \
//       src/inference/ToolCallAccumulator.cpp \
//       src/inference/ChatRequest.cpp \
//       src/tools/ToolRegistry.cpp \
//       src/tools/ToolSubagentEnvelope.cpp \
//       src/permissions/PermissionGate.cpp \
//       src/permissions/PermissionMode.cpp \
//       src/permissions/PermissionRule.cpp \
//       src/permissions/PatternMatcher.cpp \
//       src/permissions/PermissionStore.cpp \
//       src/config/SettingsLoader.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Json.cpp \
//       src/core/Paths.cpp src/core/CancelToken.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       -o /tmp/test_envelope_no_bypass && /tmp/test_envelope_no_bypass
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
#include <batbox/tools/McpTool.hpp>          // for the IS-A ITool static_assert (header only)
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/tools/ToolSubagentEnvelope.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
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
using batbox::tools::IEngulfDecider;
using batbox::tools::IResultDistiller;
using batbox::tools::ITool;
using batbox::tools::McpTool;
using batbox::tools::ToolContext;
using batbox::tools::ToolRegistry;
using batbox::tools::ToolResult;

// =============================================================================
// Compile-time MCP parity proof
// =============================================================================
// The live McpTool is a final subclass of ITool.  ToolRegistry::dispatch() has
// exactly one signature — it resolves every call to an ITool* by name and only
// ever invokes ITool base-class virtuals.  There is no McpTool-specific dispatch
// overload and no separate MCP path.  Therefore an McpTool, being an ITool,
// traverses the envelope identically to any native tool.  (We exercise this
// behaviorally below with an MCP-shaped ITool; constructing the live McpTool in
// a scoped build would require linking McpServerRegistry + 4 transport TUs,
// which are out of this feature's scope.)
static_assert(std::is_base_of_v<ITool, McpTool>,
              "McpTool must be an ITool so it is dispatched through the same "
              "ToolRegistry::dispatch -> ToolSubagentEnvelope seam as native tools");

// =============================================================================
// Test doubles
// =============================================================================

// A native-shaped tool returning a fixed body.
class NativeTool final : public ITool {
public:
    NativeTool(std::string name, std::string body)
        : name_(std::move(name)), body_(std::move(body)) {}
    std::string_view name()        const override { return name_; }
    std::string_view description() const override { return "native tool"; }
    Json schema_json() const override {
        return Json{{"name", name_}, {"description", "native"}, {"parameters", Json::object()}};
    }
    ToolResult run(const Json&, ToolContext&) override { return ToolResult::ok(body_); }
    bool is_read_only()          const override { return false; }
    bool requires_confirmation() const override { return true;  }
private:
    std::string name_, body_;
};

// An MCP-shaped tool: mirrors batbox::tools::McpTool's ITool surface exactly
// (name "MCP", not read-only, requires confirmation) without the transport
// machinery.  Stands in for the live McpTool in the scoped build; the
// static_assert above pins the real type to the same dispatch path.
class McpShapedTool final : public ITool {
public:
    explicit McpShapedTool(std::string body) : body_(std::move(body)) {}
    std::string_view name()        const override { return "MCP"; }
    std::string_view description() const override { return "mcp-shaped proxy"; }
    Json schema_json() const override {
        return Json{{"name", "MCP"}, {"description", "mcp"}, {"parameters", Json::object()}};
    }
    ToolResult run(const Json&, ToolContext&) override { return ToolResult::ok(body_); }
    bool is_read_only()          const override { return false; }  // same as McpTool
    bool requires_confirmation() const override { return true;  }  // same as McpTool
private:
    std::string body_;
};

// Distiller that rewrites every body to a sentinel and counts invocations.
class SpyDistiller final : public IResultDistiller {
public:
    SpyDistiller(std::string sentinel, int* counter)
        : sentinel_(std::move(sentinel)), counter_(counter) {}
    ToolResult distill(std::string_view, const Json&,
                       ToolResult result, ToolContext&) const override {
        ++(*counter_);
        result.body = sentinel_;
        return result;
    }
private:
    std::string sentinel_;
    int*        counter_;
};

// Decider that always engulfs (turns the seam "on" for testing).
class AlwaysEngulf final : public IEngulfDecider {
public:
    bool should_engulf(std::string_view, const Json&,
                       const ToolResult&, const ToolContext&) const override { return true; }
};

// =============================================================================
// Fixture (mirrors test_tool_orchestration.cpp's harness).
// =============================================================================
struct Fixture {
    fs::path                         tmp_dir;
    std::shared_ptr<PermissionStore> store;
    std::unique_ptr<PermissionGate>  gate;
    ToolRegistry                     registry;
    CancelSource                     cancel_src;
    ToolContext                      ctx;

    explicit Fixture(PermissionMode mode = PermissionMode::Nuclear) {
        tmp_dir = fs::temp_directory_path() / ("test_env_" + batbox::uuid_v4());
        fs::create_directories(tmp_dir);
        store = std::make_shared<PermissionStore>(tmp_dir / "settings.json");
        gate  = std::make_unique<PermissionGate>(
            store, mode,
            [](std::string_view, const Json&) { return Decision::allow(); });
        ctx.cwd          = tmp_dir;
        ctx.mode         = mode;
        ctx.session_id   = batbox::uuid_v4();
        ctx.cancel_token = cancel_src.token();
    }
    ~Fixture() { std::error_code ec; fs::remove_all(tmp_dir, ec); }

    ToolCallOrchestrator make_orch() { return ToolCallOrchestrator(registry, *gate); }
};

static ToolCallDelta one_shot(int index, const std::string& id,
                              const std::string& name, const std::string& args) {
    ToolCallDelta d;
    d.index = index; d.id = id; d.name = name; d.arguments_fragment = args;
    return d;
}

// =============================================================================
// AC2 — no un-wrapped path; native AND MCP-shaped both traverse the envelope
// =============================================================================
TEST_SUITE("S7 [AC2] no un-wrapped path (end-to-end via orchestrator)") {

    TEST_CASE("sentinel distiller rewrites BOTH native and MCP-shaped results") {
        Fixture f;
        f.registry.register_tool(std::make_unique<NativeTool>("grep", "RAW_NATIVE"));
        f.registry.register_tool(std::make_unique<McpShapedTool>("RAW_MCP"));

        // Turn the seam on with a sentinel distiller.  If ANY dispatch path
        // bypassed the envelope, the raw body would survive to the Tool message.
        int distill_count = 0;
        f.registry.envelope().set_decider(std::make_shared<AlwaysEngulf>());
        f.registry.envelope().set_distiller(
            std::make_shared<SpyDistiller>("<<DISTILLED>>", &distill_count));

        auto orch = f.make_orch();
        orch.accumulate(one_shot(0, "c_native", "grep", R"({})"));
        orch.accumulate(one_shot(1, "c_mcp",    "MCP",  R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 2);

        // Both bodies were rewritten by the envelope → neither bypassed it.
        CHECK(msgs[0].tool_name.value() == "grep");
        CHECK(msgs[0].content == "<<DISTILLED>>");
        CHECK(msgs[1].tool_name.value() == "MCP");
        CHECK(msgs[1].content == "<<DISTILLED>>");

        // Exactly one envelope pass per dispatched call.
        CHECK(distill_count == 2);
    }

    TEST_CASE("spy observes one pass per dispatched call; pre-run rejections do NOT traverse") {
        // The two pre-run gates inside ToolRegistry::dispatch — allowed_tools and
        // unknown-tool — reject BEFORE run() and therefore must NOT traverse the
        // envelope.  Nuclear mode keeps the permission gate out of the way so the
        // only gates exercised are the registry's own pre-run gates.
        Fixture f;  // Nuclear
        f.registry.register_tool(std::make_unique<NativeTool>("runme",   "x")); // runs
        f.registry.register_tool(std::make_unique<NativeTool>("blocked", "y")); // not allowed
        f.ctx.allowed_tools = std::vector<std::string>{"runme"};  // excludes "blocked"

        int distill_count = 0;
        f.registry.envelope().set_decider(std::make_shared<AlwaysEngulf>());
        f.registry.envelope().set_distiller(
            std::make_shared<SpyDistiller>("S", &distill_count));

        auto orch = f.make_orch();
        orch.accumulate(one_shot(0, "c_run", "runme",   R"({})")); // run() invoked   → 1 pass
        orch.accumulate(one_shot(1, "c_blk", "blocked", R"({})")); // allowed_tools   → 0 pass
        orch.accumulate(one_shot(2, "c_unk", "ghost",   R"({})")); // unknown tool    → 0 pass

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 3);

        // Only "runme" actually ran → exactly one envelope pass.
        CHECK(distill_count == 1);

        // The dispatched result was distilled.
        CHECK(msgs[0].content == "S");
        // The two pre-run rejections are error messages, untouched by the seam.
        CHECK(msgs[1].is_error.value() == true);
        CHECK(msgs[1].content != "S");
        CHECK(msgs[2].is_error.value() == true);
        CHECK(msgs[2].content != "S");
    }
}

// =============================================================================
// AC3 — no regression: default envelope is byte-identical pass-through
// =============================================================================
TEST_SUITE("S7 [AC3] default envelope = byte-identical") {

    TEST_CASE("default (pass-through) registry envelope leaves results unchanged") {
        Fixture f;  // registry envelope is default-constructed = pass-through
        f.registry.register_tool(std::make_unique<NativeTool>("grep", "RAW_NATIVE"));
        f.registry.register_tool(std::make_unique<McpShapedTool>("RAW_MCP"));

        auto orch = f.make_orch();
        orch.accumulate(one_shot(0, "c0", "grep", R"({})"));
        orch.accumulate(one_shot(1, "c1", "MCP",  R"({})"));

        auto msgs = orch.dispatch_all(f.ctx);
        REQUIRE(msgs.size() == 2);
        CHECK(msgs[0].content == "RAW_NATIVE");  // unchanged
        CHECK(msgs[1].content == "RAW_MCP");     // unchanged
        CHECK((!msgs[0].is_error.has_value() || !msgs[0].is_error.value()));
        CHECK((!msgs[1].is_error.has_value() || !msgs[1].is_error.value()));
    }

    TEST_CASE("registry.dispatch directly also routes through the default envelope") {
        // Even bypassing the orchestrator and calling dispatch() raw, the result
        // flows through the (default, no-op) envelope — there is no rawer path.
        Fixture f;
        f.registry.register_tool(std::make_unique<NativeTool>("t", "BODY"));
        auto r = f.registry.dispatch("t", Json::object(), f.ctx);
        REQUIRE(r.has_value());
        CHECK(r.value().body == "BODY");
    }
}

// =============================================================================
// AC4 — hooks swappable through the registry envelope (S1+S4 plug-in proof)
// =============================================================================
TEST_SUITE("S7 [AC4] swappable hooks via registry.envelope()") {

    TEST_CASE("installing a non-default distiller transforms the dispatched result") {
        Fixture f;
        f.registry.register_tool(std::make_unique<NativeTool>("t", "ORIGINAL"));

        // Before: default pass-through.
        {
            auto r = f.registry.dispatch("t", Json::object(), f.ctx);
            REQUIRE(r.has_value());
            CHECK(r.value().body == "ORIGINAL");
        }

        // After: swap hooks — the seam is unchanged, only the hooks differ.
        int n = 0;
        f.registry.envelope().set_decider(std::make_shared<AlwaysEngulf>());
        f.registry.envelope().set_distiller(std::make_shared<SpyDistiller>("GOLD", &n));
        {
            auto r = f.registry.dispatch("t", Json::object(), f.ctx);
            REQUIRE(r.has_value());
            CHECK(r.value().body == "GOLD");
            CHECK(n == 1);
        }
    }
}
