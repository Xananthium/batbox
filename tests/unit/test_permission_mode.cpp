// tests/unit/test_permission_mode.cpp
//
// doctest suite for batbox::permissions::PermissionMode helpers.
//
// Covers:
//   - round-trip parse/to_string for all four modes
//   - cycle_next covers all four and wraps correctly
//   - mode_from_string rejects garbage input
//   - deprecated Nuclear aliases parse successfully
//   - AcceptEdits aliases parse successfully
//   - banner_text is non-empty only for Nuclear
//   - requires_banner is true only for Nuclear
//
// Build (standalone):
//   c++ -std=c++20 -Iinclude \
//       -I<path-to-doctest-include> \
//       tests/unit/test_permission_mode.cpp \
//       src/permissions/PermissionMode.cpp \
//       -o /tmp/test_pmmode && /tmp/test_pmmode

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/permissions/PermissionMode.hpp>

#include <array>
#include <string>

using namespace batbox::permissions;

// ---------------------------------------------------------------------------
// to_string
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionMode — to_string") {

    TEST_CASE("Default → \"default\"") {
        CHECK(to_string(PermissionMode::Default) == "default");
    }

    TEST_CASE("Plan → \"plan\"") {
        CHECK(to_string(PermissionMode::Plan) == "plan");
    }

    TEST_CASE("AcceptEdits → \"acceptedits\"") {
        CHECK(to_string(PermissionMode::AcceptEdits) == "acceptedits");
    }

    TEST_CASE("Nuclear → \"nuclear\"") {
        CHECK(to_string(PermissionMode::Nuclear) == "nuclear");
    }
}

// ---------------------------------------------------------------------------
// mode_from_string — canonical names (round-trip)
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionMode — mode_from_string canonical round-trip") {

    TEST_CASE("\"default\" → Default") {
        auto r = mode_from_string("default");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::Default);
    }

    TEST_CASE("\"plan\" → Plan") {
        auto r = mode_from_string("plan");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::Plan);
    }

    TEST_CASE("\"acceptedits\" → AcceptEdits") {
        auto r = mode_from_string("acceptedits");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("\"nuclear\" → Nuclear") {
        auto r = mode_from_string("nuclear");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::Nuclear);
    }

    TEST_CASE("all four modes round-trip via to_string → mode_from_string") {
        const std::array<PermissionMode, 4> all_modes = {
            PermissionMode::Default,
            PermissionMode::Plan,
            PermissionMode::AcceptEdits,
            PermissionMode::Nuclear,
        };
        for (auto mode : all_modes) {
            auto canonical = to_string(mode);
            auto r = mode_from_string(canonical);
            REQUIRE_MESSAGE(r.has_value(), "round-trip failed for mode: " << std::string(canonical));
            CHECK(r.value() == mode);
        }
    }

    TEST_CASE("parsing is case-insensitive: \"DEFAULT\", \"NUCLEAR\"") {
        auto r1 = mode_from_string("DEFAULT");
        REQUIRE(r1.has_value());
        CHECK(r1.value() == PermissionMode::Default);

        auto r2 = mode_from_string("NUCLEAR");
        REQUIRE(r2.has_value());
        CHECK(r2.value() == PermissionMode::Nuclear);

        auto r3 = mode_from_string("AcceptEdits");
        REQUIRE(r3.has_value());
        CHECK(r3.value() == PermissionMode::AcceptEdits);
    }
}

// ---------------------------------------------------------------------------
// mode_from_string — aliases
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionMode — mode_from_string aliases") {

    TEST_CASE("\"accept-edits\" → AcceptEdits") {
        auto r = mode_from_string("accept-edits");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("\"accept_edits\" → AcceptEdits") {
        auto r = mode_from_string("accept_edits");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::AcceptEdits);
    }

    TEST_CASE("\"skip-permissions\" → Nuclear (deprecated alias)") {
        auto r = mode_from_string("skip-permissions");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::Nuclear);
    }

    TEST_CASE("\"dangerously-skip-permissions\" → Nuclear (deprecated alias)") {
        auto r = mode_from_string("dangerously-skip-permissions");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::Nuclear);
    }

    TEST_CASE("\"skip_permissions\" → Nuclear (deprecated alias)") {
        auto r = mode_from_string("skip_permissions");
        REQUIRE(r.has_value());
        CHECK(r.value() == PermissionMode::Nuclear);
    }
}

// ---------------------------------------------------------------------------
// mode_from_string — rejection of garbage
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionMode — mode_from_string rejects unknown input") {

    TEST_CASE("empty string → error") {
        auto r = mode_from_string("");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("\"garbage\" → error") {
        auto r = mode_from_string("garbage");
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("garbage") != std::string::npos);
    }

    TEST_CASE("\"dangerously_allow_all\" → error (not a known alias)") {
        auto r = mode_from_string("dangerously_allow_all");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("\"nuke\" → error (partial match not accepted)") {
        auto r = mode_from_string("nuke");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("\"plans\" → error (extra char not accepted)") {
        auto r = mode_from_string("plans");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("whitespace around name → error (no trimming)") {
        auto r = mode_from_string(" nuclear ");
        CHECK_FALSE(r.has_value());
    }
}

// ---------------------------------------------------------------------------
// cycle_next
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionMode — cycle_next") {

    TEST_CASE("Default → Plan") {
        CHECK(cycle_next(PermissionMode::Default) == PermissionMode::Plan);
    }

    TEST_CASE("Plan → AcceptEdits") {
        CHECK(cycle_next(PermissionMode::Plan) == PermissionMode::AcceptEdits);
    }

    TEST_CASE("AcceptEdits → Nuclear") {
        CHECK(cycle_next(PermissionMode::AcceptEdits) == PermissionMode::Nuclear);
    }

    TEST_CASE("Nuclear → Default (wraps)") {
        CHECK(cycle_next(PermissionMode::Nuclear) == PermissionMode::Default);
    }

    TEST_CASE("full cycle visits all four modes exactly once before returning to start") {
        const std::array<PermissionMode, 4> expected = {
            PermissionMode::Plan,
            PermissionMode::AcceptEdits,
            PermissionMode::Nuclear,
            PermissionMode::Default,
        };
        PermissionMode m = PermissionMode::Default;
        for (std::size_t i = 0; i < 4; ++i) {
            m = cycle_next(m);
            CHECK(m == expected[i]);
        }
        // After a full cycle we must be back at Default
        CHECK(m == PermissionMode::Default);
    }

    TEST_CASE("two full cycles are stable (idempotent)") {
        PermissionMode m = PermissionMode::Default;
        for (int round = 0; round < 2; ++round) {
            m = cycle_next(m); CHECK(m == PermissionMode::Plan);
            m = cycle_next(m); CHECK(m == PermissionMode::AcceptEdits);
            m = cycle_next(m); CHECK(m == PermissionMode::Nuclear);
            m = cycle_next(m); CHECK(m == PermissionMode::Default);
        }
    }
}

// ---------------------------------------------------------------------------
// requires_banner / banner_text
// ---------------------------------------------------------------------------
TEST_SUITE("PermissionMode — banner helpers") {

    TEST_CASE("requires_banner is true only for Nuclear") {
        CHECK_FALSE(requires_banner(PermissionMode::Default));
        CHECK_FALSE(requires_banner(PermissionMode::Plan));
        CHECK_FALSE(requires_banner(PermissionMode::AcceptEdits));
        CHECK(requires_banner(PermissionMode::Nuclear));
    }

    TEST_CASE("banner_text is non-empty only for Nuclear") {
        CHECK(banner_text(PermissionMode::Default).empty());
        CHECK(banner_text(PermissionMode::Plan).empty());
        CHECK(banner_text(PermissionMode::AcceptEdits).empty());
        CHECK_FALSE(banner_text(PermissionMode::Nuclear).empty());
    }

    TEST_CASE("Nuclear banner_text contains \"NUCLEAR\"") {
        const auto text = banner_text(PermissionMode::Nuclear);
        CHECK_FALSE(text.empty());
        CHECK(text.find("NUCLEAR") != std::string_view::npos);
    }

    TEST_CASE("Nuclear banner_text contains \"PERMISSIONS\"") {
        const auto text = banner_text(PermissionMode::Nuclear);
        CHECK(text.find("PERMISSIONS") != std::string_view::npos);
    }

    TEST_CASE("requires_banner and banner_text agree: non-empty iff requires_banner") {
        const std::array<PermissionMode, 4> all_modes = {
            PermissionMode::Default,
            PermissionMode::Plan,
            PermissionMode::AcceptEdits,
            PermissionMode::Nuclear,
        };
        for (auto mode : all_modes) {
            const bool has_banner = requires_banner(mode);
            const bool text_nonempty = !banner_text(mode).empty();
            CHECK_MESSAGE(has_banner == text_nonempty,
                "mismatch for mode: " << std::string(to_string(mode)));
        }
    }
}
