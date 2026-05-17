// tests/unit/test_json_helpers.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox JSON helpers (Json.hpp / Json.cpp).
//
// Build + run (standalone, no CMake needed — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_json_helpers.cpp src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_json && /tmp/test_json
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/core/Json.hpp>

#include <cmath>
#include <string>

using namespace batbox;

// ============================================================================
// TEST SUITE 1: Type alias
// ============================================================================
TEST_SUITE("Json — type alias") {

    TEST_CASE("Json alias compiles and is nlohmann::json") {
        Json j;
        CHECK(j.is_null());
    }

    TEST_CASE("Json object construction") {
        Json j = {{"name", "batbox"}, {"version", 1}};
        CHECK(j.is_object());
        CHECK(j["name"] == "batbox");
        CHECK(j["version"] == 1);
    }

    TEST_CASE("Json array construction") {
        Json j = Json::array({1, 2, 3});
        CHECK(j.is_array());
        CHECK(j.size() == 3u);
    }
}

// ============================================================================
// TEST SUITE 2: parse() — nlohmann path
// ============================================================================
TEST_SUITE("parse — nlohmann path") {

    TEST_CASE("parse valid object") {
        auto r = parse(R"({"key":"value","n":42})");
        REQUIRE(r.has_value());
        CHECK(r.value()["key"] == "value");
        CHECK(r.value()["n"] == 42);
    }

    TEST_CASE("parse valid array") {
        auto r = parse(R"([1,2,3])");
        REQUIRE(r.has_value());
        CHECK(r.value().is_array());
        CHECK(r.value().size() == 3u);
    }

    TEST_CASE("parse valid nested object") {
        auto r = parse(R"({"a":{"b":{"c":99}}})");
        REQUIRE(r.has_value());
        CHECK(r.value()["a"]["b"]["c"] == 99);
    }

    TEST_CASE("parse empty object") {
        auto r = parse("{}");
        REQUIRE(r.has_value());
        CHECK(r.value().is_object());
        CHECK(r.value().empty());
    }

    TEST_CASE("parse null literal") {
        auto r = parse("null");
        REQUIRE(r.has_value());
        CHECK(r.value().is_null());
    }

    TEST_CASE("parse string literal") {
        auto r = parse(R"("hello")");
        REQUIRE(r.has_value());
        CHECK(r.value() == "hello");
    }

    TEST_CASE("parse returns error on malformed JSON — missing closing brace") {
        auto r = parse(R"({"key": "value")");
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("parse returns error on completely invalid input") {
        auto r = parse("not json at all !!!");
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("parse returns error on empty string") {
        auto r = parse("");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("parse returns error on trailing comma") {
        auto r = parse(R"({"key":1,})");
        CHECK_FALSE(r.has_value());
    }
}

// ============================================================================
// TEST SUITE 3: parse_fast() — simdjson→nlohmann bridge
// ============================================================================
TEST_SUITE("parse_fast — simdjson bridge") {

    TEST_CASE("parse_fast valid object") {
        auto r = parse_fast(R"({"model":"gpt-4","tokens":2048})");
        REQUIRE(r.has_value());
        CHECK(r.value()["model"] == "gpt-4");
        CHECK(r.value()["tokens"] == 2048);
    }

    TEST_CASE("parse_fast valid array") {
        auto r = parse_fast(R"([10, 20, 30])");
        REQUIRE(r.has_value());
        CHECK(r.value().is_array());
        CHECK(r.value()[1] == 20);
    }

    TEST_CASE("parse_fast string value") {
        auto r = parse_fast(R"({"msg":"hello"})");
        REQUIRE(r.has_value());
        CHECK(r.value()["msg"] == "hello");
    }

    TEST_CASE("parse_fast boolean values") {
        auto r = parse_fast(R"({"ok":true,"fail":false})");
        REQUIRE(r.has_value());
        CHECK(r.value()["ok"] == true);
        CHECK(r.value()["fail"] == false);
    }

    TEST_CASE("parse_fast null value") {
        auto r = parse_fast(R"({"x":null})");
        REQUIRE(r.has_value());
        CHECK(r.value()["x"].is_null());
    }

    TEST_CASE("parse_fast floating-point value") {
        auto r = parse_fast(R"({"pi":3.14159})");
        REQUIRE(r.has_value());
        double v = r.value()["pi"].get<double>();
        CHECK(std::abs(v - 3.14159) < 1e-5);
    }

    TEST_CASE("parse_fast nested object") {
        auto r = parse_fast(R"({"a":{"b":{"c":7}}})");
        REQUIRE(r.has_value());
        CHECK(r.value()["a"]["b"]["c"] == 7);
    }

    TEST_CASE("parse_fast array of objects") {
        auto r = parse_fast(R"([{"id":1},{"id":2}])");
        REQUIRE(r.has_value());
        CHECK(r.value()[0]["id"] == 1);
        CHECK(r.value()[1]["id"] == 2);
    }

    TEST_CASE("parse_fast malformed input returns error") {
        auto r = parse_fast("{broken json");
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("parse_fast empty input returns error") {
        auto r = parse_fast("");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("parse_fast callable multiple times on same thread") {
        for (int i = 0; i < 5; ++i) {
            auto r = parse_fast(R"({"i":1})");
            REQUIRE(r.has_value());
        }
    }
}

// ============================================================================
// TEST SUITE 4: parse_simdjson_doc()
// ============================================================================
TEST_SUITE("parse_simdjson_doc") {

    TEST_CASE("parse_simdjson_doc returns element for valid JSON") {
        auto r = parse_simdjson_doc(R"({"hello":"world"})");
        REQUIRE(r.has_value());
        std::string_view sv;
        auto ec = r.value()["hello"].get(sv);
        CHECK(ec == simdjson::SUCCESS);
        CHECK(sv == "world");
    }

    TEST_CASE("parse_simdjson_doc returns error for malformed input") {
        auto r = parse_simdjson_doc("{not valid");
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("parse_simdjson_doc returns error for empty input") {
        auto r = parse_simdjson_doc("");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("parse_simdjson_doc int field") {
        auto r = parse_simdjson_doc(R"({"count":42})");
        REQUIRE(r.has_value());
        int64_t v{};
        auto ec = r.value()["count"].get(v);
        CHECK(ec == simdjson::SUCCESS);
        CHECK(v == 42);
    }
}

// ============================================================================
// TEST SUITE 5: dump() and pretty()
// ============================================================================
TEST_SUITE("dump and pretty") {

    TEST_CASE("dump produces compact JSON string") {
        Json j = {{"a", 1}, {"b", "x"}};
        std::string s = dump(j);
        CHECK_FALSE(s.empty());
        // Compact: no indentation present beyond minimal
        CHECK(s.find('\n') == std::string::npos);
        // Round-trips
        auto r = parse(s);
        REQUIRE(r.has_value());
        CHECK(r.value() == j);
    }

    TEST_CASE("pretty produces indented JSON string") {
        Json j = {{"a", 1}};
        std::string s = pretty(j);
        CHECK_FALSE(s.empty());
        CHECK(s.find('\n') != std::string::npos);
        // Contains 4-space indent
        CHECK(s.find("    ") != std::string::npos);
        // Round-trips
        auto r = parse(s);
        REQUIRE(r.has_value());
        CHECK(r.value() == j);
    }

    TEST_CASE("dump of null is 'null'") {
        Json j = nullptr;
        CHECK(dump(j) == "null");
    }

    TEST_CASE("dump of array") {
        Json j = Json::array({1, 2, 3});
        std::string s = dump(j);
        CHECK(s == "[1,2,3]");
    }
}

// ============================================================================
// TEST SUITE 6: get_or<T>()
// ============================================================================
TEST_SUITE("get_or") {

    TEST_CASE("get_or<std::string> returns value when key present") {
        Json j = {{"name", "batbox"}};
        CHECK(get_or<std::string>(j, "name", "default") == "batbox");
    }

    TEST_CASE("get_or<std::string> returns default when key missing") {
        Json j = Json::object();
        CHECK(get_or<std::string>(j, "missing", "default") == "default");
    }

    TEST_CASE("get_or<int> returns value when key present") {
        Json j = {{"port", 9090}};
        CHECK(get_or<int>(j, "port", 8080) == 9090);
    }

    TEST_CASE("get_or<int> returns default when key missing") {
        Json j = Json::object();
        CHECK(get_or<int>(j, "port", 8080) == 8080);
    }

    TEST_CASE("get_or<bool> returns value when key present") {
        Json j = {{"enabled", true}};
        CHECK(get_or<bool>(j, "enabled", false) == true);
    }

    TEST_CASE("get_or<bool> returns default when key missing") {
        Json j = Json::object();
        CHECK(get_or<bool>(j, "enabled", false) == false);
    }

    TEST_CASE("get_or<double> returns default when key missing") {
        Json j = Json::object();
        double v = get_or<double>(j, "ratio", 1.5);
        CHECK(std::abs(v - 1.5) < 1e-9);
    }

    TEST_CASE("get_or returns default on type mismatch") {
        Json j = {{"port", "not_a_number"}};
        // Requesting int from a string value
        CHECK(get_or<int>(j, "port", 0) == 0);
    }

    TEST_CASE("get_or works on non-object Json — returns default") {
        Json j = Json::array({1, 2, 3});
        CHECK(get_or<std::string>(j, "key", "fallback") == "fallback");
    }

    TEST_CASE("get_or with string_view key literal") {
        Json j = {{"host", "localhost"}};
        std::string host = get_or<std::string>(j, "host", "127.0.0.1");
        CHECK(host == "localhost");
    }
}

// ============================================================================
// TEST SUITE 7: path_get()
// ============================================================================
TEST_SUITE("path_get") {

    TEST_CASE("path_get single-segment key found") {
        Json j = {{"key", "value"}};
        auto r = path_get(j, "key");
        REQUIRE(r.has_value());
        CHECK(r.value() == "value");
    }

    TEST_CASE("path_get single-segment key missing returns nullopt") {
        Json j = Json::object();
        auto r = path_get(j, "missing");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("path_get two-level dotted path") {
        Json j = {{"model", {{"name", "claude"}}}};
        auto r = path_get(j, "model.name");
        REQUIRE(r.has_value());
        CHECK(r.value() == "claude");
    }

    TEST_CASE("path_get three-level dotted path") {
        Json j = {{"a", {{"b", {{"c", 42}}}}}};
        auto r = path_get(j, "a.b.c");
        REQUIRE(r.has_value());
        CHECK(r.value() == 42);
    }

    TEST_CASE("path_get missing intermediate segment returns nullopt") {
        Json j = {{"a", {{"b", 1}}}};
        auto r = path_get(j, "a.x.c");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("path_get on non-object intermediate returns nullopt") {
        Json j = {{"a", 42}};
        // "a" exists but is not an object — can't descend further
        auto r = path_get(j, "a.b");
        CHECK_FALSE(r.has_value());
    }

    TEST_CASE("path_get returns the subtree for a nested object") {
        Json j = {{"limits", {{"tokens", 4096}, {"requests", 100}}}};
        auto r = path_get(j, "limits");
        REQUIRE(r.has_value());
        CHECK(r.value().is_object());
        CHECK(r.value()["tokens"] == 4096);
    }

    TEST_CASE("path_get empty path returns nullopt") {
        Json j = {{"key", 1}};
        // Empty dotted path — no segment to navigate, return the root?
        // By contract: empty remaining means we return *cur == j (the root).
        auto r = path_get(j, "");
        // An empty path navigates zero segments and returns the root node.
        REQUIRE(r.has_value());
        CHECK(r.value() == j);
    }
}

// ============================================================================
// TEST SUITE 8: Round-trip fidelity
// ============================================================================
TEST_SUITE("round-trip") {

    TEST_CASE("parse then dump is idempotent") {
        const std::string original = R"({"id":1,"name":"test","active":true})";
        auto r = parse(original);
        REQUIRE(r.has_value());
        std::string serialised = dump(r.value());
        auto r2 = parse(serialised);
        REQUIRE(r2.has_value());
        CHECK(r.value() == r2.value());
    }

    TEST_CASE("parse_fast then dump matches parse then dump") {
        const std::string_view input = R"({"x":1,"y":2})";
        auto r_fast = parse_fast(input);
        auto r_slow = parse(input);
        REQUIRE(r_fast.has_value());
        REQUIRE(r_slow.has_value());
        CHECK(r_fast.value() == r_slow.value());
    }

    TEST_CASE("large-ish JSON round-trips correctly via parse_fast") {
        // Build a moderately sized JSON payload to exercise the SIMD path.
        Json arr = Json::array();
        for (int i = 0; i < 100; ++i) {
            arr.push_back({{"id", i}, {"val", i * 1.5}, {"tag", "item"}});
        }
        std::string big = dump(arr);
        auto r = parse_fast(big);
        REQUIRE(r.has_value());
        CHECK(r.value().size() == 100u);
        CHECK(r.value()[50]["id"] == 50);
    }
}
