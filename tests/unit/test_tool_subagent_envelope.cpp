// tests/unit/test_tool_subagent_envelope.cpp
// =============================================================================
// doctest suite for batbox::tools::ToolSubagentEnvelope (S7, DIS-979).
//
// Covers the envelope abstraction in isolation:
//   [AC1] Default construction = pass-through (decider false → identity).
//   [AC4] Swappable hooks: a non-default distiller transforms the result; the
//         decision hook gates whether the distiller runs at all.
//   - Decider==false ⇒ distiller NEVER invoked (even a transforming one).
//   - Decider==true  ⇒ distiller invoked exactly once per process() call.
//   - Null hooks fall back to defaults (never-null invariant).
//   - set_decider/set_distiller swap hooks; null is ignored (keeps current).
//
// Build standalone (no CMake, from repo root; x64-linux triplet):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_tool_subagent_envelope.cpp \
//       src/tools/ToolSubagentEnvelope.cpp \
//       src/core/Json.cpp src/core/CancelToken.cpp \
//       src/permissions/PermissionMode.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       -o /tmp/test_envelope && /tmp/test_envelope
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/tools/ToolSubagentEnvelope.hpp>

#include <memory>
#include <string>

using batbox::CancelSource;
using batbox::Json;
using batbox::tools::IEngulfDecider;
using batbox::tools::IResultDistiller;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;
using batbox::tools::ToolSubagentEnvelope;

// =============================================================================
// Test doubles
// =============================================================================

// Decider whose verdict is fixed at construction.
class FixedDecider final : public IEngulfDecider {
public:
    explicit FixedDecider(bool verdict, int* counter = nullptr)
        : verdict_(verdict), counter_(counter) {}
    [[nodiscard]] bool should_engulf(std::string_view, const Json&,
                                     const ToolResult&, const ToolContext&) const override {
        if (counter_) ++(*counter_);
        return verdict_;
    }
private:
    bool verdict_;
    int* counter_;
};

// Distiller that replaces the body with a sentinel and counts invocations.
class SentinelDistiller final : public IResultDistiller {
public:
    explicit SentinelDistiller(std::string sentinel, int* counter = nullptr)
        : sentinel_(std::move(sentinel)), counter_(counter) {}
    [[nodiscard]] ToolResult distill(std::string_view, const Json&,
                                     ToolResult result, ToolContext&) const override {
        if (counter_) ++(*counter_);
        result.body = sentinel_;
        return result;
    }
private:
    std::string sentinel_;
    int*        counter_;
};

// Minimal valid ToolContext (cancel token from a live source).
struct Ctx {
    CancelSource src;
    ToolContext  ctx;
    Ctx() { ctx.cancel_token = src.token(); }
};

// =============================================================================
// AC1 — default construction is pure pass-through
// =============================================================================
TEST_SUITE("ToolSubagentEnvelope [AC1] default pass-through") {

    TEST_CASE("default envelope returns the result byte-identical") {
        ToolSubagentEnvelope env;  // PassThroughDecider + IdentityDistiller
        Ctx c;
        ToolResult in = ToolResult::ok(R"({"temp":22})");
        ToolResult out = env.process("get_weather", Json::object(), in, c.ctx);
        CHECK(out == in);
        CHECK(out.body == R"({"temp":22})");
        CHECK(out.is_error == false);
    }

    TEST_CASE("default envelope preserves is_error and structured payload") {
        ToolSubagentEnvelope env;
        Ctx c;
        ToolResult in = ToolResult::error("boom", Json{{"code", 7}});
        ToolResult out = env.process("t", Json::object(), in, c.ctx);
        CHECK(out == in);
        CHECK(out.is_error == true);
        REQUIRE(out.structured_payload.has_value());
        CHECK(out.structured_payload->at("code").get<int>() == 7);
    }
}

// =============================================================================
// Decision hook gates the distiller
// =============================================================================
TEST_SUITE("ToolSubagentEnvelope — decision gates distiller") {

    TEST_CASE("decider==false: transforming distiller is NEVER invoked") {
        int decide_calls = 0, distill_calls = 0;
        ToolSubagentEnvelope env(
            std::make_shared<FixedDecider>(false, &decide_calls),
            std::make_shared<SentinelDistiller>("<<DISTILLED>>", &distill_calls));
        Ctx c;
        ToolResult out = env.process("t", Json::object(), ToolResult::ok("raw"), c.ctx);
        CHECK(out.body == "raw");          // untouched
        CHECK(decide_calls == 1);          // decider consulted once
        CHECK(distill_calls == 0);         // distiller never ran
    }

    TEST_CASE("decider==true: distiller runs exactly once and transforms") {
        int decide_calls = 0, distill_calls = 0;
        ToolSubagentEnvelope env(
            std::make_shared<FixedDecider>(true, &decide_calls),
            std::make_shared<SentinelDistiller>("<<DISTILLED>>", &distill_calls));
        Ctx c;
        ToolResult out = env.process("t", Json::object(), ToolResult::ok("raw"), c.ctx);
        CHECK(out.body == "<<DISTILLED>>"); // transformed
        CHECK(decide_calls == 1);
        CHECK(distill_calls == 1);          // exactly one pass
    }
}

// =============================================================================
// AC4 — hooks are swappable at runtime
// =============================================================================
TEST_SUITE("ToolSubagentEnvelope [AC4] swappable hooks") {

    TEST_CASE("set_decider + set_distiller flips a default envelope to active") {
        ToolSubagentEnvelope env;  // starts as pass-through
        Ctx c;

        // Default: pass-through.
        CHECK(env.process("t", Json::object(), ToolResult::ok("raw"), c.ctx).body == "raw");

        // Swap in active hooks (S1+S4 will do exactly this).
        env.set_decider(std::make_shared<FixedDecider>(true));
        env.set_distiller(std::make_shared<SentinelDistiller>("GOLD"));

        CHECK(env.process("t", Json::object(), ToolResult::ok("raw"), c.ctx).body == "GOLD");
    }

    TEST_CASE("null hooks are ignored — never-null invariant holds") {
        int distill_calls = 0;
        ToolSubagentEnvelope env(
            std::make_shared<FixedDecider>(true),
            std::make_shared<SentinelDistiller>("KEEP", &distill_calls));
        Ctx c;

        // Attempt to clear the distiller with null — must be ignored.
        env.set_distiller(nullptr);
        env.set_decider(nullptr);

        ToolResult out = env.process("t", Json::object(), ToolResult::ok("raw"), c.ctx);
        CHECK(out.body == "KEEP");      // original distiller still installed
        CHECK(distill_calls == 1);
    }

    TEST_CASE("null hooks at construction fall back to defaults (pass-through)") {
        ToolSubagentEnvelope env(nullptr, nullptr);  // both null → defaults
        Ctx c;
        ToolResult out = env.process("t", Json::object(), ToolResult::ok("raw"), c.ctx);
        CHECK(out.body == "raw");  // PassThroughDecider ⇒ identity
    }
}
