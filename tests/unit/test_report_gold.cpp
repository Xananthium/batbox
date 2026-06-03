// tests/unit/test_report_gold.cpp
// =============================================================================
// doctest suite for batbox::tools::ReportGoldTool (S4, DIS-980).
//
// Covers AC2: the report_gold ITool exists with the answer / confidence? /
// follow_up_ok? schema; schema shape + parse() + that run() surfaces the
// structured result.
//
// Build standalone (no CMake, from repo root; x64-linux triplet):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_report_gold.cpp \
//       src/tools/ReportGoldTool.cpp \
//       src/core/Json.cpp src/core/CancelToken.cpp \
//       src/permissions/PermissionMode.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       -o /tmp/test_report_gold && /tmp/test_report_gold
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/ReportGoldTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

using batbox::CancelSource;
using batbox::Json;
using batbox::tools::ReportGold;
using batbox::tools::ReportGoldTool;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;

namespace {
struct Ctx {
    CancelSource src;
    ToolContext  ctx;
    Ctx() { ctx.cancel_token = src.token(); }
};
} // namespace

// =============================================================================
// Schema + identity [AC2]
// =============================================================================

TEST_CASE("identity + permission hints [AC2]") {
    ReportGoldTool t;
    CHECK(t.name() == "report_gold");
    CHECK(t.is_read_only());            // declares a result; mutates nothing
    CHECK_FALSE(t.requires_confirmation());
}

TEST_CASE("schema shape: answer required, optional confidence/follow_up_ok [AC2]") {
    ReportGoldTool t;
    Json s = t.schema_json();
    CHECK(s.at("name") == "report_gold");
    REQUIRE(s.contains("parameters"));
    const Json& p = s.at("parameters");
    CHECK(p.at("type") == "object");

    REQUIRE(p.contains("properties"));
    const Json& props = p.at("properties");
    CHECK(props.contains("answer"));
    CHECK(props.at("answer").at("type") == "string");
    CHECK(props.contains("confidence"));
    CHECK(props.at("confidence").at("type") == "number");
    CHECK(props.contains("follow_up_ok"));
    CHECK(props.at("follow_up_ok").at("type") == "boolean");

    REQUIRE(p.contains("required"));
    const Json& req = p.at("required");
    REQUIRE(req.is_array());
    bool answer_required = false;
    for (const auto& r : req) if (r == "answer") answer_required = true;
    CHECK(answer_required);
}

// =============================================================================
// parse() [AC2]
// =============================================================================

TEST_CASE("parse: answer only → confidence/follow_up_ok absent [AC2]") {
    auto g = ReportGoldTool::parse(Json{{"answer", "the gold"}});
    REQUIRE(g.has_value());
    CHECK(g->answer == "the gold");
    CHECK_FALSE(g->confidence.has_value());
    CHECK_FALSE(g->follow_up_ok.has_value());
}

TEST_CASE("parse: all fields present [AC2]") {
    auto g = ReportGoldTool::parse(Json{
        {"answer", "the gold"}, {"confidence", 0.8}, {"follow_up_ok", true}});
    REQUIRE(g.has_value());
    CHECK(g->answer == "the gold");
    REQUIRE(g->confidence.has_value());
    CHECK(g->confidence.value() == doctest::Approx(0.8));
    REQUIRE(g->follow_up_ok.has_value());
    CHECK(g->follow_up_ok.value() == true);
}

TEST_CASE("parse: missing answer → nullopt [AC2]") {
    CHECK_FALSE(ReportGoldTool::parse(Json{{"confidence", 0.5}}).has_value());
}

TEST_CASE("parse: empty answer → nullopt [AC2]") {
    CHECK_FALSE(ReportGoldTool::parse(Json{{"answer", ""}}).has_value());
}

TEST_CASE("parse: non-string answer → nullopt [AC2]") {
    CHECK_FALSE(ReportGoldTool::parse(Json{{"answer", 42}}).has_value());
}

TEST_CASE("parse: non-object args → nullopt [AC2]") {
    CHECK_FALSE(ReportGoldTool::parse(Json::array({1, 2, 3})).has_value());
    CHECK_FALSE(ReportGoldTool::parse(Json("a bare string")).has_value());
}

TEST_CASE("parse: wrong-typed optionals are ignored, not fatal [AC2]") {
    auto g = ReportGoldTool::parse(Json{
        {"answer", "ok"}, {"confidence", "high"}, {"follow_up_ok", "yes"}});
    REQUIRE(g.has_value());
    CHECK(g->answer == "ok");
    CHECK_FALSE(g->confidence.has_value());     // "high" is not a number → ignored
    CHECK_FALSE(g->follow_up_ok.has_value());   // "yes" is not a bool → ignored
}

// =============================================================================
// run() surfaces the structured result [AC2]
// =============================================================================

TEST_CASE("run: valid args → ok result, body==answer, structured payload [AC2]") {
    ReportGoldTool t;
    Ctx c;
    ToolResult r = t.run(Json{
        {"answer", "GOLD"}, {"confidence", 0.7}, {"follow_up_ok", false}}, c.ctx);
    CHECK_FALSE(r.is_error);
    CHECK(r.body == "GOLD");
    REQUIRE(r.structured_payload.has_value());
    const Json& p = *r.structured_payload;
    CHECK(p.at("answer") == "GOLD");
    CHECK(p.at("confidence") == doctest::Approx(0.7));
    CHECK(p.at("follow_up_ok") == false);
}

TEST_CASE("run: invalid args → error result [AC2]") {
    ReportGoldTool t;
    Ctx c;
    ToolResult r = t.run(Json{{"confidence", 0.5}}, c.ctx);  // no answer
    CHECK(r.is_error);
}
