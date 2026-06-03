// tests/unit/test_threshold_decider.cpp
// =============================================================================
// doctest suite for batbox::tools::ThresholdEngulfDecider (S1, DIS-980).
//
// Covers AC1: engulfs iff result.body strictly exceeds the configured threshold;
// cheap + side-effect-free; tested at / above / below the boundary and for the
// error-result and small-result cases.
//
// Build standalone (no CMake, from repo root; x64-linux triplet):
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_threshold_decider.cpp \
//       src/tools/ThresholdEngulfDecider.cpp \
//       src/core/Json.cpp src/core/CancelToken.cpp \
//       src/permissions/PermissionMode.cpp \
//       build/vcpkg_installed/x64-linux/lib/libsimdjson.a \
//       build/vcpkg_installed/x64-linux/lib/libspdlog.a \
//       build/vcpkg_installed/x64-linux/lib/libfmt.a \
//       -o /tmp/test_threshold_decider && /tmp/test_threshold_decider
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/tools/ThresholdEngulfDecider.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>

using batbox::CancelSource;
using batbox::Json;
using batbox::tools::ThresholdEngulfDecider;
using batbox::tools::ToolContext;
using batbox::tools::ToolResult;

namespace {

// Minimal valid ToolContext (cancel token from a live source).
struct Ctx {
    CancelSource src;
    ToolContext  ctx;
    Ctx() { ctx.cancel_token = src.token(); }
};

ToolResult ok_body(std::size_t n) {
    return ToolResult::ok(std::string(n, 'x'));
}

} // namespace

TEST_CASE("threshold accessor reflects construction") {
    ThresholdEngulfDecider d{1000};
    CHECK(d.threshold() == 1000);
}

TEST_CASE("below threshold → does not engulf [AC1]") {
    ThresholdEngulfDecider d{100};
    Ctx c;
    CHECK_FALSE(d.should_engulf("read_file", Json::object(), ok_body(99), c.ctx));
}

TEST_CASE("exactly at threshold → does not engulf (strictly-greater) [AC1]") {
    ThresholdEngulfDecider d{100};
    Ctx c;
    CHECK_FALSE(d.should_engulf("read_file", Json::object(), ok_body(100), c.ctx));
}

TEST_CASE("one byte above threshold → engulfs [AC1]") {
    ThresholdEngulfDecider d{100};
    Ctx c;
    CHECK(d.should_engulf("read_file", Json::object(), ok_body(101), c.ctx));
}

TEST_CASE("far above threshold → engulfs [AC1]") {
    ThresholdEngulfDecider d{100};
    Ctx c;
    CHECK(d.should_engulf("grep", Json::object(), ok_body(100000), c.ctx));
}

TEST_CASE("error result is never engulfed even when huge [AC1]") {
    ThresholdEngulfDecider d{100};
    Ctx c;
    ToolResult err = ToolResult::error(std::string(100000, 'e'));
    CHECK_FALSE(d.should_engulf("bash", Json::object(), err, c.ctx));
}

TEST_CASE("small successful result is never engulfed [AC1]") {
    ThresholdEngulfDecider d{200000};
    Ctx c;
    CHECK_FALSE(d.should_engulf("read_file", Json::object(), ToolResult::ok("tiny"), c.ctx));
}

TEST_CASE("size is the trigger, not tool identity [AC1]") {
    // A read-only tool's huge output still engulfs: the cost is the bytes
    // entering the parent context, regardless of which tool produced them.
    ThresholdEngulfDecider d{10};
    Ctx c;
    CHECK(d.should_engulf("read_file", Json::object(), ok_body(11), c.ctx));
    CHECK(d.should_engulf("glob",      Json::object(), ok_body(11), c.ctx));
}

TEST_CASE("decider is side-effect-free: repeated calls are stable [AC1]") {
    ThresholdEngulfDecider d{50};
    Ctx c;
    ToolResult r = ok_body(51);
    bool first = d.should_engulf("x", Json::object(), r, c.ctx);
    for (int i = 0; i < 5; ++i) {
        CHECK(d.should_engulf("x", Json::object(), r, c.ctx) == first);
    }
    CHECK(first);
    // The result is not mutated by the (const) decision.
    CHECK(r.body.size() == 51);
}
