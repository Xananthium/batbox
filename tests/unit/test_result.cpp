// tests/unit/test_result.cpp
//
// doctest suite for batbox::Result<T,E>.
// Covers both the C++20 polyfill and the C++23 std::expected alias path;
// the same tests compile and pass on either because the API surface is identical.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/Result.hpp>

#include <memory>
#include <string>
#include <utility>

using namespace batbox;

// ============================================================================
// TEST SUITE 1: Construction and basic observers
// ============================================================================
TEST_SUITE("Result — construction & observers") {

    TEST_CASE("ok value from int literal (implicit)") {
        Result<int> r = 42;
        CHECK(r.has_value());
        CHECK(static_cast<bool>(r));
        CHECK(r.value() == 42);
    }

    TEST_CASE("ok value from string (implicit)") {
        Result<std::string> r = std::string("hello");
        CHECK(r.has_value());
        CHECK(r.value() == "hello");
    }

    TEST_CASE("error construction via make_unexpected") {
        Result<int, std::string> r = make_unexpected(std::string("bad thing"));
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(static_cast<bool>(r));
        CHECK(r.error() == "bad thing");
    }

    TEST_CASE("error construction via batbox::Err()") {
        Result<int, std::string> r = Err(std::string("oops"));
        CHECK_FALSE(r.has_value());
        CHECK(r.error() == "oops");
    }

    TEST_CASE("error construction via Unexpected directly") {
        Result<int, std::string> r{Unexpected<std::string>("direct")};
        CHECK_FALSE(r.has_value());
        CHECK(r.error() == "direct");
    }

    TEST_CASE("value_or returns value when ok") {
        Result<int> r = 7;
        CHECK(r.value_or(99) == 7);
    }

    TEST_CASE("value_or returns default when error") {
        Result<int, std::string> r = Err(std::string("nope"));
        CHECK(r.value_or(99) == 99);
    }

    TEST_CASE("operator* and operator->") {
        struct Point { int x; int y; };
        Result<Point> r{Point{3, 4}};
        CHECK((*r).x == 3);
        CHECK(r->y == 4);
    }

    TEST_CASE("default error type is std::string") {
        Result<int> r = Err(std::string("default E"));
        CHECK_FALSE(r.has_value());
        CHECK(r.error() == "default E");
    }
}

// ============================================================================
// TEST SUITE 2: Copy and move semantics
// ============================================================================
TEST_SUITE("Result — copy and move semantics") {

    TEST_CASE("copy-construct ok") {
        Result<std::string> a = std::string("abc");
        Result<std::string> b = a;
        CHECK(b.has_value());
        CHECK(b.value() == "abc");
        CHECK(a.value() == "abc");
    }

    TEST_CASE("copy-construct error") {
        Result<int, std::string> a = Err(std::string("err"));
        Result<int, std::string> b = a;
        CHECK_FALSE(b.has_value());
        CHECK(b.error() == "err");
    }

    TEST_CASE("move-construct ok") {
        Result<std::string> a = std::string("move me");
        Result<std::string> b = std::move(a);
        CHECK(b.has_value());
        CHECK(b.value() == "move me");
    }

    TEST_CASE("move-construct error") {
        Result<int, std::string> a = Err(std::string("move err"));
        Result<int, std::string> b = std::move(a);
        CHECK_FALSE(b.has_value());
        CHECK(b.error() == "move err");
    }

    TEST_CASE("copy-assign ok to error") {
        Result<int, std::string> a = 5;
        Result<int, std::string> b = Err(std::string("x"));
        b = a;
        CHECK(b.has_value());
        CHECK(b.value() == 5);
    }

    TEST_CASE("move-assign error to ok") {
        Result<int, std::string> a = Err(std::string("moved"));
        Result<int, std::string> b = 10;
        b = std::move(a);
        CHECK_FALSE(b.has_value());
        CHECK(b.error() == "moved");
    }

    TEST_CASE("unique_ptr value is move-only") {
        Result<std::unique_ptr<int>, std::string> r{std::make_unique<int>(42)};
        CHECK(r.has_value());
        auto ptr = std::move(r).value();
        CHECK(*ptr == 42);
    }
}

// ============================================================================
// TEST SUITE 3: and_then
// ============================================================================
TEST_SUITE("Result — and_then") {

    TEST_CASE("and_then chains ok through two steps") {
        Result<int> r = 5;
        auto result = r
            .and_then([](int v) -> Result<int> { return v * 2; })
            .and_then([](int v) -> Result<int> { return v + 1; });
        CHECK(result.has_value());
        CHECK(result.value() == 11);
    }

    TEST_CASE("and_then short-circuits on error") {
        Result<int, std::string> r = Err(std::string("stop"));
        bool called = false;
        auto result = r.and_then([&](int) -> Result<int, std::string> {
            called = true;
            return 99;
        });
        CHECK_FALSE(called);
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == "stop");
    }

    TEST_CASE("and_then can change value type") {
        Result<int, std::string> r = 10;
        auto result = r.and_then([](int v) -> Result<std::string, std::string> {
            return std::to_string(v);
        });
        CHECK(result.has_value());
        CHECK(result.value() == "10");
    }

    TEST_CASE("and_then can produce an error") {
        Result<int, std::string> r = -1;
        auto result = r.and_then([](int v) -> Result<int, std::string> {
            if (v < 0) return Err(std::string("negative"));
            return v;
        });
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == "negative");
    }
}

// ============================================================================
// TEST SUITE 4: or_else
// ============================================================================
TEST_SUITE("Result — or_else") {

    TEST_CASE("or_else not called when ok") {
        Result<int, std::string> r = 42;
        bool called = false;
        auto result = r.or_else([&](const std::string&) -> Result<int, std::string> {
            called = true;
            return 0;
        });
        CHECK_FALSE(called);
        CHECK(result.has_value());
        CHECK(result.value() == 42);
    }

    TEST_CASE("or_else called on error and recovers") {
        Result<int, std::string> r = Err(std::string("recoverable"));
        auto result = r.or_else([](const std::string&) -> Result<int, std::string> {
            return 99;
        });
        CHECK(result.has_value());
        CHECK(result.value() == 99);
    }

    TEST_CASE("or_else can change error type") {
        Result<int, std::string> r = Err(std::string("old error"));
        auto result = r.or_else([](const std::string& e) -> Result<int, int> {
            return Err(static_cast<int>(e.size()));
        });
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == static_cast<int>(std::string("old error").size()));
    }
}

// ============================================================================
// TEST SUITE 5: transform
// ============================================================================
TEST_SUITE("Result — transform") {

    TEST_CASE("transform applies function to ok value") {
        Result<int> r = 3;
        auto result = r.transform([](int v) { return v * v; });
        CHECK(result.has_value());
        CHECK(result.value() == 9);
    }

    TEST_CASE("transform skips function on error") {
        Result<int, std::string> r = Err(std::string("skip"));
        bool called = false;
        auto result = r.transform([&](int v) { called = true; return v; });
        CHECK_FALSE(called);
        CHECK_FALSE(result.has_value());
        CHECK(result.error() == "skip");
    }

    TEST_CASE("transform chains") {
        Result<int, std::string> r = 2;
        auto result = r
            .transform([](int v) { return v + 1; })
            .transform([](int v) { return v * 4; });
        CHECK(result.has_value());
        CHECK(result.value() == 12);
    }

    TEST_CASE("transform can change type") {
        Result<int, std::string> r = 7;
        auto result = r.transform([](int v) -> std::string {
            return "v=" + std::to_string(v);
        });
        CHECK(result.has_value());
        CHECK(result.value() == "v=7");
    }
}

// ============================================================================
// TEST SUITE 6: Result<void, E> specialization
// ============================================================================
TEST_SUITE("Result<void,E> — void specialization") {

    TEST_CASE("default construct is ok") {
        Result<void, std::string> r;
        CHECK(r.has_value());
        CHECK(static_cast<bool>(r));
    }

    TEST_CASE("construct from Unexpected error") {
        Result<void, std::string> r = Err(std::string("void error"));
        CHECK_FALSE(r.has_value());
        CHECK(r.error() == "void error");
    }

    TEST_CASE("copy ok") {
        Result<void, std::string> a;
        Result<void, std::string> b = a;
        CHECK(b.has_value());
    }

    TEST_CASE("copy error") {
        Result<void, std::string> a = Err(std::string("ve"));
        Result<void, std::string> b = a;
        CHECK_FALSE(b.has_value());
        CHECK(b.error() == "ve");
    }

    TEST_CASE("move ok") {
        Result<void, std::string> a;
        Result<void, std::string> b = std::move(a);
        CHECK(b.has_value());
    }

    TEST_CASE("move error") {
        Result<void, std::string> a = Err(std::string("vm"));
        Result<void, std::string> b = std::move(a);
        CHECK_FALSE(b.has_value());
        CHECK(b.error() == "vm");
    }

    TEST_CASE("and_then on void ok calls callable") {
        Result<void, std::string> r;
        bool called = false;
        auto result = r.and_then([&]() -> Result<void, std::string> {
            called = true;
            return {};
        });
        CHECK(called);
        CHECK(result.has_value());
    }

    TEST_CASE("and_then on void error skips callable") {
        Result<void, std::string> r = Err(std::string("void-err"));
        bool called = false;
        auto result = r.and_then([&]() -> Result<void, std::string> {
            called = true;
            return {};
        });
        CHECK_FALSE(called);
        CHECK_FALSE(result.has_value());
    }

    TEST_CASE("or_else on void error recovers") {
        Result<void, std::string> r = Err(std::string("recover"));
        auto result = r.or_else([](const std::string&) -> Result<void, std::string> {
            return {};
        });
        CHECK(result.has_value());
    }

    TEST_CASE("transform on void ok produces value") {
        Result<void, std::string> r;
        auto result = r.transform([]() -> int { return 42; });
        CHECK(result.has_value());
        CHECK(result.value() == 42);
    }

    TEST_CASE("chain: void ok through and_then to transformed value") {
        Result<void, std::string> start;
        auto r = start
            .and_then([]() -> Result<int, std::string> { return 10; })
            .transform([](int v) { return v * 3; });
        CHECK(r.has_value());
        CHECK(r.value() == 30);
    }
}

// ============================================================================
// TEST SUITE 7: Real-world chaining example
// ============================================================================
TEST_SUITE("Result — real-world chaining") {

    static Result<int, std::string> parse_int(const std::string& s) {
        try {
            return std::stoi(s);
        } catch (...) {
            return Err(std::string("not a number: ") + s);
        }
    }

    static Result<double, std::string> compute(int n) {
        if (n <= 0) return Err(std::string("must be positive"));
        return static_cast<double>(n) * 1.5;
    }

    TEST_CASE("happy path chain") {
        auto r = parse_int("8")
            .and_then([](int n) { return compute(n); })
            .transform([](double d) { return d + 0.5; });
        CHECK(r.has_value());
        CHECK(r.value() == doctest::Approx(12.5));
    }

    TEST_CASE("chain stops at first error") {
        auto r = parse_int("abc")
            .and_then([](int n) { return compute(n); })
            .transform([](double d) { return d + 0.5; });
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("not a number") != std::string::npos);
    }

    TEST_CASE("or_else recovery then continuing chain") {
        auto r = parse_int("bad")
            .or_else([](const std::string&) -> Result<int, std::string> {
                return 0;
            })
            .and_then([](int n) { return compute(n); });
        CHECK_FALSE(r.has_value());
        CHECK(r.error() == "must be positive");
    }
}

// ============================================================================
// TEST SUITE 8: Compile-time feature path
// ============================================================================
TEST_SUITE("Result — compile-time feature path") {

    TEST_CASE("BATBOX_HAS_STD_EXPECTED macro is 0 or 1") {
        [[maybe_unused]] int v = BATBOX_HAS_STD_EXPECTED;
        CHECK((v == 0 || v == 1));
    }

#if BATBOX_HAS_STD_EXPECTED
    TEST_CASE("C++23 path: Result is std::expected, std::unexpected works") {
        Result<int, std::string> r = std::unexpected(std::string("std-expected path"));
        CHECK_FALSE(r.has_value());
        CHECK(r.error() == "std-expected path");
    }
#else
    TEST_CASE("C++20 path: polyfill Result is usable") {
        Result<int, std::string> r = 99;
        CHECK(r.has_value());
        CHECK(r.value() == 99);
    }
#endif
}
