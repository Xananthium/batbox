// =============================================================================
// tests/unit/test_uuid.cpp — Unit tests for batbox::Uuid
//
// Build + run (standalone, no CMake needed):
//   c++ -std=c++20 -Iinclude -I<path-to-doctest> \
//       tests/unit/test_uuid.cpp src/core/Uuid.cpp \
//       -o /tmp/test_uuid && /tmp/test_uuid
//
// Or with the vcpkg-installed doctest:
//   c++ -std=c++20 -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_uuid.cpp src/core/Uuid.cpp \
//       -o /tmp/test_uuid && /tmp/test_uuid
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/core/Uuid.hpp"

#include <set>
#include <string>
#include <unordered_set>

using batbox::Uuid;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

// Validate the 36-character canonical form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
static bool valid_canonical_format(const std::string& s) {
    if (s.size() != 36) return false;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    // Check all hex positions
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!is_hex_char(s[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Basic format
// ---------------------------------------------------------------------------
TEST_CASE("Uuid::v4 produces valid 36-char canonical format") {
    const Uuid u = Uuid::v4();
    const std::string s = u.to_string();
    CHECK(valid_canonical_format(s));
}

TEST_CASE("Uuid::v4 string is lowercase") {
    for (int n = 0; n < 20; ++n) {
        std::string s = Uuid::v4().to_string();
        for (int i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            // must not be uppercase
            bool is_upper = (s[i] >= 'A' && s[i] <= 'F'); CHECK_FALSE(is_upper);
        }
    }
}

// ---------------------------------------------------------------------------
// RFC 4122 version bits
// ---------------------------------------------------------------------------
TEST_CASE("Uuid::v4 has version 4 in byte[6] high nibble") {
    // byte[6] high nibble must be '4', i.e. string position 14 == '4'
    for (int n = 0; n < 50; ++n) {
        std::string s = Uuid::v4().to_string();
        CHECK(s[14] == '4');
    }
}

TEST_CASE("Uuid::v4 version byte raw check") {
    for (int n = 0; n < 50; ++n) {
        Uuid u = Uuid::v4();
        // byte[6] top nibble must be 0x4
        CHECK((u.bytes[6] >> 4) == 0x4u);
    }
}

// ---------------------------------------------------------------------------
// RFC 4122 variant bits
// ---------------------------------------------------------------------------
TEST_CASE("Uuid::v4 has RFC 4122 variant bits in byte[8]") {
    // byte[8] must have top two bits == 0b10, so (byte >> 6) == 2
    for (int n = 0; n < 50; ++n) {
        Uuid u = Uuid::v4();
        CHECK((u.bytes[8] >> 6) == 0x2u);
    }
}

TEST_CASE("Uuid::v4 variant character at string position 19 is 8/9/a/b") {
    // string[19] is the first hex char of byte[8]; with variant=10xx the
    // high nibble is 0x8..0xb, giving characters '8','9','a','b'
    for (int n = 0; n < 100; ++n) {
        std::string s = Uuid::v4().to_string();
        char c = s[19];
        CHECK((c == '8' || c == '9' || c == 'a' || c == 'b'));
    }
}

// ---------------------------------------------------------------------------
// Uniqueness
// ---------------------------------------------------------------------------
TEST_CASE("Two consecutive Uuid::v4 calls produce different UUIDs") {
    for (int n = 0; n < 20; ++n) {
        Uuid a = Uuid::v4();
        Uuid b = Uuid::v4();
        CHECK_FALSE(a == b);
    }
}

TEST_CASE("1000 generated UUIDs are all unique") {
    constexpr int N = 1000;
    std::set<std::string> seen;
    for (int i = 0; i < N; ++i) {
        std::string s = Uuid::v4().to_string();
        CHECK(valid_canonical_format(s));
        CHECK(seen.find(s) == seen.end());
        seen.insert(s);
    }
    CHECK(static_cast<int>(seen.size()) == N);
}

// ---------------------------------------------------------------------------
// Round-trip parse / to_string
// ---------------------------------------------------------------------------
TEST_CASE("Uuid round-trip: to_string then parse recovers the same UUID") {
    for (int n = 0; n < 20; ++n) {
        Uuid original = Uuid::v4();
        std::string s = original.to_string();
        auto parsed = Uuid::parse(s);
        REQUIRE(parsed.has_value());
        CHECK(parsed.value() == original);
    }
}

TEST_CASE("Uuid::parse accepts uppercase hex") {
    // Build a valid UUID string with uppercase hex
    std::string s = Uuid::v4().to_string();
    for (char& c : s) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 32);
    }
    auto result = Uuid::parse(s);
    REQUIRE(result.has_value());
    CHECK(valid_canonical_format(result->to_string()));
}

TEST_CASE("Uuid::parse round-trip: known fixed UUID string") {
    const std::string known = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    auto result = Uuid::parse(known);
    REQUIRE(result.has_value());
    CHECK(result->to_string() == known);
}

// ---------------------------------------------------------------------------
// Parse rejects malformed input
// ---------------------------------------------------------------------------
TEST_CASE("Uuid::parse rejects empty string") {
    CHECK_FALSE(Uuid::parse("").has_value());
}

TEST_CASE("Uuid::parse rejects wrong length") {
    CHECK_FALSE(Uuid::parse("f47ac10b-58cc-4372-a567-0e02b2c3d47").has_value());   // 35 chars
    CHECK_FALSE(Uuid::parse("f47ac10b-58cc-4372-a567-0e02b2c3d4790").has_value()); // 37 chars
}

TEST_CASE("Uuid::parse rejects missing dashes") {
    CHECK_FALSE(Uuid::parse("f47ac10b58cc4372a5670e02b2c3d479").has_value()); // 32 chars, no dashes
    CHECK_FALSE(Uuid::parse("f47ac10b+58cc-4372-a567-0e02b2c3d479").has_value());
}

TEST_CASE("Uuid::parse rejects dashes in wrong positions") {
    // Swap positions of one dash
    CHECK_FALSE(Uuid::parse("f47ac10-b58cc-4372-a567-0e02b2c3d479").has_value());
}

TEST_CASE("Uuid::parse rejects invalid hex characters") {
    CHECK_FALSE(Uuid::parse("g47ac10b-58cc-4372-a567-0e02b2c3d479").has_value()); // 'g' invalid
    CHECK_FALSE(Uuid::parse("f47ac10b-58cc-4372-a567-0e02b2c3d47z").has_value()); // 'z' invalid
}

TEST_CASE("Uuid::parse rejects UUID with spaces") {
    CHECK_FALSE(Uuid::parse("f47ac10b-58cc-4372-a567-0e02b2c3d47 ").has_value());
}

// ---------------------------------------------------------------------------
// is_nil
// ---------------------------------------------------------------------------
TEST_CASE("Uuid::nil() is_nil() returns true") {
    CHECK(Uuid::nil().is_nil());
}

TEST_CASE("Uuid::v4() is_nil() returns false") {
    for (int n = 0; n < 20; ++n) {
        CHECK_FALSE(Uuid::v4().is_nil());
    }
}

TEST_CASE("Parse nil UUID string") {
    const std::string nil_str = "00000000-0000-0000-0000-000000000000";
    auto result = Uuid::parse(nil_str);
    REQUIRE(result.has_value());
    CHECK(result->is_nil());
    CHECK(result->to_string() == nil_str);
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------
TEST_CASE("operator< provides strict weak ordering") {
    Uuid a = Uuid::v4();
    Uuid b = Uuid::v4();
    if (a == b) {
        // Astronomically unlikely but technically possible
        CHECK_FALSE(a < b);
        CHECK_FALSE(b < a);
    } else if (a < b) {
        CHECK_FALSE(b < a);
    } else {
        CHECK(b < a);
    }
}

// ---------------------------------------------------------------------------
// std::hash
// ---------------------------------------------------------------------------
TEST_CASE("std::hash<Uuid> works in unordered_set") {
    std::unordered_set<Uuid> us;
    for (int i = 0; i < 100; ++i) {
        us.insert(Uuid::v4());
    }
    CHECK(us.size() == 100u);
}

TEST_CASE("std::hash<Uuid> nil and v4 hash differently (probabilistic)") {
    std::hash<Uuid> hasher;
    Uuid nil = Uuid::nil();
    Uuid generated = Uuid::v4();
    // These could theoretically collide but won't in practice
    CHECK(hasher(nil) != hasher(generated));
}
