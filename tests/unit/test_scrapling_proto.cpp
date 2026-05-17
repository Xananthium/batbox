// ---------------------------------------------------------------------------
// tests/unit/test_scrapling_proto.cpp
//
// doctest suite for batbox::sidecar::proto — ScraplingProto.hpp / .cpp.
//
// Each test round-trips a struct through to_json() → JSON string → from_json()
// and verifies that all fields survive the trip intact.  Fixture JSON strings
// are crafted to match exactly what the Python FastAPI server would produce so
// that the same strings can be used for cross-side verification.
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_scrapling_proto.cpp \
//       src/sidecar/ScraplingProto.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_scrapling_proto && /tmp/test_scrapling_proto
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/sidecar/ScraplingProto.hpp>
#include <batbox/core/Json.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace batbox::sidecar::proto;
using batbox::Json;

// ---------------------------------------------------------------------------
// Helper: round-trip a struct via to_json() → dump → parse → from_json()
// ---------------------------------------------------------------------------
template <typename T>
T round_trip(const T& original) {
    const Json j      = original.to_json();
    const std::string s = batbox::dump(j);
    auto parsed = batbox::parse(s);
    REQUIRE(parsed.has_value());
    return T::from_json(parsed.value());
}

// ============================================================================
// TEST SUITE: HealthResponse
// ============================================================================
TEST_SUITE("HealthResponse") {

    TEST_CASE("default status is 'ok'") {
        HealthResponse r;
        CHECK(r.status == "ok");
    }

    TEST_CASE("to_json produces {status: 'ok'}") {
        HealthResponse r;
        Json j = r.to_json();
        REQUIRE(j.is_object());
        CHECK(j["status"] == "ok");
    }

    TEST_CASE("round-trip default") {
        HealthResponse orig;
        auto rt = round_trip(orig);
        CHECK(rt.status == orig.status);
    }

    TEST_CASE("from_json — canned Python fixture") {
        // Exactly what GET /healthz returns from the FastAPI app
        const std::string fixture = R"({"status":"ok"})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = HealthResponse::from_json(parsed.value());
        CHECK(r.status == "ok");
    }

    TEST_CASE("from_json — missing status defaults to 'ok'") {
        auto r = HealthResponse::from_json(Json::object());
        CHECK(r.status == "ok");
    }
}

// ============================================================================
// TEST SUITE: FetchRequest
// ============================================================================
TEST_SUITE("FetchRequest") {

    TEST_CASE("defaults are correct") {
        FetchRequest r;
        CHECK(r.url.empty());
        CHECK(std::abs(r.timeout - 30.0) < 1e-9);
        CHECK(r.stealth == false);
        CHECK(r.respect_robots == true);
        CHECK(r.max_bytes == 5'242'880);
    }

    TEST_CASE("to_json includes all fields") {
        FetchRequest req;
        req.url           = "https://example.com";
        req.timeout       = 15.0;
        req.stealth       = true;
        req.respect_robots = false;
        req.max_bytes     = 1024;

        Json j = req.to_json();
        CHECK(j["url"]            == "https://example.com");
        CHECK(j["timeout"]        == 15.0);
        CHECK(j["stealth"]        == true);
        CHECK(j["respect_robots"] == false);
        CHECK(j["max_bytes"]      == 1024);
    }

    TEST_CASE("round-trip all fields") {
        FetchRequest orig;
        orig.url            = "https://batbox.dev/docs";
        orig.timeout        = 60.0;
        orig.stealth        = true;
        orig.respect_robots = false;
        orig.max_bytes      = 1'048'576;

        auto rt = round_trip(orig);
        CHECK(rt.url            == orig.url);
        CHECK(std::abs(rt.timeout - orig.timeout) < 1e-9);
        CHECK(rt.stealth        == orig.stealth);
        CHECK(rt.respect_robots == orig.respect_robots);
        CHECK(rt.max_bytes      == orig.max_bytes);
    }

    TEST_CASE("from_json — minimal fixture (only url)") {
        // Python Pydantic fills the rest with defaults
        const std::string fixture =
            R"({"url":"https://example.com","timeout":30.0,"stealth":false,"respect_robots":true,"max_bytes":5242880})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = FetchRequest::from_json(parsed.value());
        CHECK(r.url            == "https://example.com");
        CHECK(std::abs(r.timeout - 30.0) < 1e-9);
        CHECK(r.stealth        == false);
        CHECK(r.respect_robots == true);
        CHECK(r.max_bytes      == 5'242'880);
    }

    TEST_CASE("from_json — missing optional fields use defaults") {
        Json j{{"url", "https://test.com"}};
        auto r = FetchRequest::from_json(j);
        CHECK(r.url   == "https://test.com");
        CHECK(std::abs(r.timeout - 30.0) < 1e-9);
        CHECK(r.stealth        == false);
        CHECK(r.respect_robots == true);
        CHECK(r.max_bytes      == 5'242'880);
    }
}

// ============================================================================
// TEST SUITE: FetchResponse
// ============================================================================
TEST_SUITE("FetchResponse") {

    TEST_CASE("defaults are correct") {
        FetchResponse r;
        CHECK(r.url.empty());
        CHECK(r.markdown.empty());
        CHECK(r.status_code    == 0);
        CHECK(r.content_type.empty());
        CHECK(r.content_length == 0);
        CHECK(r.fetched_at.empty());
        CHECK(r.truncated      == false);
        CHECK(r.is_error       == false);
        CHECK(r.error_message.empty());
    }

    TEST_CASE("to_json includes all fields") {
        FetchResponse resp;
        resp.url            = "https://example.com";
        resp.markdown       = "# Hello\nWorld";
        resp.status_code    = 200;
        resp.content_type   = "text/html";
        resp.content_length = 42;
        resp.fetched_at     = "2026-05-15T00:00:00+00:00";
        resp.truncated      = false;
        resp.is_error       = false;
        resp.error_message  = "";

        Json j = resp.to_json();
        CHECK(j["url"]            == "https://example.com");
        CHECK(j["markdown"]       == "# Hello\nWorld");
        CHECK(j["status_code"]    == 200);
        CHECK(j["content_type"]   == "text/html");
        CHECK(j["content_length"] == 42);
        CHECK(j["fetched_at"]     == "2026-05-15T00:00:00+00:00");
        CHECK(j["truncated"]      == false);
        CHECK(j["is_error"]       == false);
        CHECK(j["error_message"]  == "");
    }

    TEST_CASE("round-trip success response") {
        FetchResponse orig;
        orig.url            = "https://example.com/page";
        orig.markdown       = "## Heading\n\nContent here.";
        orig.status_code    = 200;
        orig.content_type   = "text/html";
        orig.content_length = 1234;
        orig.fetched_at     = "2026-05-15T12:34:56+00:00";
        orig.truncated      = false;
        orig.is_error       = false;
        orig.error_message  = "";

        auto rt = round_trip(orig);
        CHECK(rt.url            == orig.url);
        CHECK(rt.markdown       == orig.markdown);
        CHECK(rt.status_code    == orig.status_code);
        CHECK(rt.content_type   == orig.content_type);
        CHECK(rt.content_length == orig.content_length);
        CHECK(rt.fetched_at     == orig.fetched_at);
        CHECK(rt.truncated      == orig.truncated);
        CHECK(rt.is_error       == orig.is_error);
        CHECK(rt.error_message  == orig.error_message);
    }

    TEST_CASE("round-trip error response") {
        FetchResponse orig;
        orig.url           = "https://fail.example.com";
        orig.markdown      = "";
        orig.status_code   = 0;
        orig.fetched_at    = "2026-05-15T00:00:00+00:00";
        orig.is_error      = true;
        orig.error_message = "Fetch error for 'https://fail.example.com': connection refused";

        auto rt = round_trip(orig);
        CHECK(rt.url           == orig.url);
        CHECK(rt.is_error      == true);
        CHECK(rt.error_message == orig.error_message);
        CHECK(rt.status_code   == 0);
    }

    TEST_CASE("round-trip truncated response") {
        FetchResponse orig;
        orig.url       = "https://large.example.com";
        orig.markdown  = "lots of content…";
        orig.truncated = true;
        orig.status_code = 200;

        auto rt = round_trip(orig);
        CHECK(rt.truncated == true);
        CHECK(rt.url       == orig.url);
    }

    TEST_CASE("from_json — canned Python fixture (success)") {
        // Matches the structure returned by POST /fetch in app.py
        const std::string fixture = R"({
            "url": "https://example.com",
            "markdown": "# Example Domain\n\nThis domain is for use in illustrative examples.",
            "status_code": 200,
            "content_type": "text/html",
            "content_length": 1256,
            "fetched_at": "2026-05-15T00:00:00+00:00",
            "truncated": false,
            "is_error": false,
            "error_message": ""
        })";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = FetchResponse::from_json(parsed.value());
        CHECK(r.url          == "https://example.com");
        CHECK(r.status_code  == 200);
        CHECK(r.content_type == "text/html");
        CHECK(r.truncated    == false);
        CHECK(r.is_error     == false);
    }
}

// ============================================================================
// TEST SUITE: SearchResult
// ============================================================================
TEST_SUITE("SearchResult") {

    TEST_CASE("defaults are empty strings") {
        SearchResult r;
        CHECK(r.title.empty());
        CHECK(r.url.empty());
        CHECK(r.snippet.empty());
    }

    TEST_CASE("round-trip all fields") {
        SearchResult orig;
        orig.title   = "BatBox — AI Terminal";
        orig.url     = "https://batbox.dev";
        orig.snippet = "A local-first AI terminal assistant.";

        auto rt = round_trip(orig);
        CHECK(rt.title   == orig.title);
        CHECK(rt.url     == orig.url);
        CHECK(rt.snippet == orig.snippet);
    }

    TEST_CASE("from_json — canned fixture") {
        const std::string fixture =
            R"({"title":"Example","url":"https://example.com","snippet":"An example page."})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = SearchResult::from_json(parsed.value());
        CHECK(r.title   == "Example");
        CHECK(r.url     == "https://example.com");
        CHECK(r.snippet == "An example page.");
    }
}

// ============================================================================
// TEST SUITE: SearchRequest
// ============================================================================
TEST_SUITE("SearchRequest") {

    TEST_CASE("defaults are correct") {
        SearchRequest r;
        CHECK(r.query.empty());
        CHECK(r.n           == 10);
        CHECK(r.engine      == "ddg");
        CHECK(r.searxng_url.empty());
    }

    TEST_CASE("round-trip ddg request") {
        SearchRequest orig;
        orig.query = "batbox C++ terminal";
        orig.n     = 5;

        auto rt = round_trip(orig);
        CHECK(rt.query  == orig.query);
        CHECK(rt.n      == 5);
        CHECK(rt.engine == "ddg");
        CHECK(rt.searxng_url.empty());
    }

    TEST_CASE("round-trip searxng request") {
        SearchRequest orig;
        orig.query       = "nlohmann json performance";
        orig.n           = 20;
        orig.engine      = "searxng";
        orig.searxng_url = "https://searx.example.com";

        auto rt = round_trip(orig);
        CHECK(rt.query       == orig.query);
        CHECK(rt.n           == 20);
        CHECK(rt.engine      == "searxng");
        CHECK(rt.searxng_url == "https://searx.example.com");
    }

    TEST_CASE("from_json — canned Python fixture") {
        const std::string fixture =
            R"({"query":"open source C++ JSON","n":10,"engine":"ddg","searxng_url":""})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = SearchRequest::from_json(parsed.value());
        CHECK(r.query       == "open source C++ JSON");
        CHECK(r.n           == 10);
        CHECK(r.engine      == "ddg");
        CHECK(r.searxng_url.empty());
    }
}

// ============================================================================
// TEST SUITE: SearchResponse
// ============================================================================
TEST_SUITE("SearchResponse") {

    TEST_CASE("defaults are correct") {
        SearchResponse r;
        CHECK(r.query.empty());
        CHECK(r.engine.empty());
        CHECK(r.results.empty());
        CHECK(r.is_error    == false);
        CHECK(r.error_message.empty());
    }

    TEST_CASE("to_json — empty results array") {
        SearchResponse resp;
        resp.query  = "test";
        resp.engine = "ddg";
        Json j = resp.to_json();
        CHECK(j["query"]   == "test");
        CHECK(j["results"].is_array());
        CHECK(j["results"].size() == 0u);
    }

    TEST_CASE("round-trip with multiple results") {
        SearchResponse orig;
        orig.query  = "C++ JSON libraries";
        orig.engine = "ddg";
        orig.results = {
            {"nlohmann/json", "https://github.com/nlohmann/json",  "A modern C++ JSON library."},
            {"simdjson",      "https://github.com/simdjson/simdjson", "Parsing gigabytes of JSON per second."},
            {"RapidJSON",     "https://rapidjson.org",              "A fast JSON parser/generator."},
        };

        auto rt = round_trip(orig);
        CHECK(rt.query          == orig.query);
        CHECK(rt.engine         == orig.engine);
        REQUIRE(rt.results.size() == 3u);
        CHECK(rt.results[0].title   == "nlohmann/json");
        CHECK(rt.results[0].url     == "https://github.com/nlohmann/json");
        CHECK(rt.results[0].snippet == "A modern C++ JSON library.");
        CHECK(rt.results[1].title   == "simdjson");
        CHECK(rt.results[2].title   == "RapidJSON");
    }

    TEST_CASE("round-trip error response") {
        SearchResponse orig;
        orig.query         = "fails";
        orig.engine        = "searxng";
        orig.is_error      = true;
        orig.error_message = "SearXNG instance unreachable";

        auto rt = round_trip(orig);
        CHECK(rt.is_error      == true);
        CHECK(rt.error_message == orig.error_message);
        CHECK(rt.results.empty());
    }

    TEST_CASE("from_json — canned Python fixture") {
        // Matches the structure returned by POST /search in app.py
        const std::string fixture = R"({
            "query": "BatBox terminal",
            "engine": "ddg",
            "results": [
                {"title": "BatBox", "url": "https://batbox.dev", "snippet": "AI terminal."},
                {"title": "GitHub", "url": "https://github.com/batbox", "snippet": "Source code."}
            ],
            "is_error": false,
            "error_message": ""
        })";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = SearchResponse::from_json(parsed.value());
        CHECK(r.query  == "BatBox terminal");
        CHECK(r.engine == "ddg");
        REQUIRE(r.results.size() == 2u);
        CHECK(r.results[0].title   == "BatBox");
        CHECK(r.results[0].url     == "https://batbox.dev");
        CHECK(r.results[1].snippet == "Source code.");
        CHECK(r.is_error == false);
    }
}

// ============================================================================
// TEST SUITE: SelectRequest
// ============================================================================
TEST_SUITE("SelectRequest") {

    TEST_CASE("defaults are correct") {
        SelectRequest r;
        CHECK(r.url.empty());
        CHECK(r.selector.empty());
        CHECK(std::abs(r.timeout - 30.0) < 1e-9);
        CHECK(r.stealth   == false);
        CHECK(r.attribute.empty());
    }

    TEST_CASE("round-trip CSS selector with URL") {
        SelectRequest orig;
        orig.url      = "https://example.com";
        orig.selector = "h1";
        orig.timeout  = 10.0;

        auto rt = round_trip(orig);
        CHECK(rt.url      == orig.url);
        CHECK(rt.selector == "h1");
        CHECK(std::abs(rt.timeout - 10.0) < 1e-9);
        CHECK(rt.stealth   == false);
        CHECK(rt.attribute.empty());
    }

    TEST_CASE("round-trip XPath selector with raw HTML") {
        SelectRequest orig;
        orig.url       = "<html><body><a href='x'>link</a></body></html>";
        orig.selector  = "//a/@href";
        orig.attribute = "href";
        orig.stealth   = false;

        auto rt = round_trip(orig);
        CHECK(rt.url       == orig.url);
        CHECK(rt.selector  == "//a/@href");
        CHECK(rt.attribute == "href");
    }

    TEST_CASE("from_json — canned Python fixture") {
        const std::string fixture =
            R"({"url":"https://example.com","selector":"h2","timeout":30.0,"stealth":false,"attribute":""})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = SelectRequest::from_json(parsed.value());
        CHECK(r.url       == "https://example.com");
        CHECK(r.selector  == "h2");
        CHECK(std::abs(r.timeout - 30.0) < 1e-9);
        CHECK(r.stealth   == false);
        CHECK(r.attribute.empty());
    }
}

// ============================================================================
// TEST SUITE: SelectResponse
// ============================================================================
TEST_SUITE("SelectResponse") {

    TEST_CASE("defaults are correct") {
        SelectResponse r;
        CHECK(r.url.empty());
        CHECK(r.selector.empty());
        CHECK(r.matches.empty());
        CHECK(r.count     == 0);
        CHECK(r.is_error  == false);
        CHECK(r.error_message.empty());
    }

    TEST_CASE("to_json — empty matches array") {
        SelectResponse resp;
        resp.url      = "https://example.com";
        resp.selector = "h1";
        resp.count    = 0;
        Json j = resp.to_json();
        CHECK(j["matches"].is_array());
        CHECK(j["matches"].size() == 0u);
    }

    TEST_CASE("round-trip with matches") {
        SelectResponse orig;
        orig.url      = "https://example.com";
        orig.selector = "h2";
        orig.matches  = {"First heading", "Second heading", "Third heading"};
        orig.count    = 3;

        auto rt = round_trip(orig);
        CHECK(rt.url      == orig.url);
        CHECK(rt.selector == orig.selector);
        REQUIRE(rt.matches.size() == 3u);
        CHECK(rt.matches[0] == "First heading");
        CHECK(rt.matches[1] == "Second heading");
        CHECK(rt.matches[2] == "Third heading");
        CHECK(rt.count == 3);
    }

    TEST_CASE("round-trip error response") {
        SelectResponse orig;
        orig.url           = "https://example.com";
        orig.selector      = ".bad[selector";
        orig.matches       = {};
        orig.count         = 0;
        orig.is_error      = true;
        orig.error_message = "Selector error: invalid CSS selector";

        auto rt = round_trip(orig);
        CHECK(rt.is_error      == true);
        CHECK(rt.error_message == orig.error_message);
        CHECK(rt.matches.empty());
        CHECK(rt.count == 0);
    }

    TEST_CASE("from_json — canned Python fixture") {
        // Matches the structure returned by POST /select in app.py
        const std::string fixture = R"({
            "url": "https://example.com",
            "selector": "h1",
            "matches": ["Example Domain"],
            "count": 1,
            "is_error": false,
            "error_message": ""
        })";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = SelectResponse::from_json(parsed.value());
        CHECK(r.url      == "https://example.com");
        CHECK(r.selector == "h1");
        REQUIRE(r.matches.size() == 1u);
        CHECK(r.matches[0] == "Example Domain");
        CHECK(r.count    == 1);
        CHECK(r.is_error == false);
    }
}

// ============================================================================
// TEST SUITE: ShutdownResponse
// ============================================================================
TEST_SUITE("ShutdownResponse") {

    TEST_CASE("default shutting_down is false") {
        ShutdownResponse r;
        CHECK(r.shutting_down == false);
    }

    TEST_CASE("to_json — shutting_down true") {
        ShutdownResponse r;
        r.shutting_down = true;
        Json j = r.to_json();
        CHECK(j["shutting_down"] == true);
    }

    TEST_CASE("round-trip") {
        ShutdownResponse orig;
        orig.shutting_down = true;
        auto rt = round_trip(orig);
        CHECK(rt.shutting_down == true);
    }

    TEST_CASE("from_json — canned Python fixture") {
        // Matches what POST /shutdown returns: {"shutting_down": true}
        const std::string fixture = R"({"shutting_down":true})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = ShutdownResponse::from_json(parsed.value());
        CHECK(r.shutting_down == true);
    }

    TEST_CASE("from_json — missing field defaults to false") {
        auto r = ShutdownResponse::from_json(Json::object());
        CHECK(r.shutting_down == false);
    }
}

// ============================================================================
// TEST SUITE: ErrorResponse
// ============================================================================
TEST_SUITE("ErrorResponse") {

    TEST_CASE("defaults are empty strings") {
        ErrorResponse r;
        CHECK(r.error.empty());
        CHECK(r.detail.empty());
        CHECK(r.path.empty());
    }

    TEST_CASE("round-trip") {
        ErrorResponse orig;
        orig.error  = "ValueError";
        orig.detail = "Invalid URL scheme";
        orig.path   = "/fetch";

        auto rt = round_trip(orig);
        CHECK(rt.error  == orig.error);
        CHECK(rt.detail == orig.detail);
        CHECK(rt.path   == orig.path);
    }

    TEST_CASE("from_json — canned Python fixture") {
        // Matches the generic_exception_handler output in app.py
        const std::string fixture =
            R"({"error":"RuntimeError","detail":"Fetch error for 'x': timeout","path":"/fetch"})";
        auto parsed = batbox::parse(fixture);
        REQUIRE(parsed.has_value());
        auto r = ErrorResponse::from_json(parsed.value());
        CHECK(r.error  == "RuntimeError");
        CHECK(r.detail == "Fetch error for 'x': timeout");
        CHECK(r.path   == "/fetch");
    }

    TEST_CASE("from_json — missing fields default to empty") {
        auto r = ErrorResponse::from_json(Json::object());
        CHECK(r.error.empty());
        CHECK(r.detail.empty());
        CHECK(r.path.empty());
    }
}

// ============================================================================
// TEST SUITE: Cross-field fidelity — Python field name alignment
// ============================================================================
TEST_SUITE("Python field name alignment") {

    TEST_CASE("FetchResponse JSON keys match Python Pydantic field names exactly") {
        FetchResponse resp;
        resp.url            = "u";
        resp.markdown       = "m";
        resp.status_code    = 200;
        resp.content_type   = "ct";
        resp.content_length = 1;
        resp.fetched_at     = "ts";
        resp.truncated      = false;
        resp.is_error       = false;
        resp.error_message  = "";
        Json j = resp.to_json();

        // Python field names (from app.py FetchResponse model)
        CHECK(j.contains("url"));
        CHECK(j.contains("markdown"));
        CHECK(j.contains("status_code"));
        CHECK(j.contains("content_type"));
        CHECK(j.contains("content_length"));
        CHECK(j.contains("fetched_at"));
        CHECK(j.contains("truncated"));
        CHECK(j.contains("is_error"));
        CHECK(j.contains("error_message"));
    }

    TEST_CASE("SearchRequest JSON keys match Python Pydantic field names exactly") {
        SearchRequest req;
        req.query       = "q";
        req.n           = 5;
        req.engine      = "ddg";
        req.searxng_url = "";
        Json j = req.to_json();

        // Python field names (from app.py SearchRequest model)
        CHECK(j.contains("query"));
        CHECK(j.contains("n"));
        CHECK(j.contains("engine"));
        CHECK(j.contains("searxng_url"));
    }

    TEST_CASE("SelectRequest JSON keys match Python Pydantic field names exactly") {
        SelectRequest req;
        req.url       = "u";
        req.selector  = "s";
        req.timeout   = 30.0;
        req.stealth   = false;
        req.attribute = "";
        Json j = req.to_json();

        // Python field names (from app.py SelectRequest model)
        CHECK(j.contains("url"));
        CHECK(j.contains("selector"));
        CHECK(j.contains("timeout"));
        CHECK(j.contains("stealth"));
        CHECK(j.contains("attribute"));
    }

    TEST_CASE("SearchResponse results array contains title/url/snippet keys") {
        SearchResponse resp;
        resp.query  = "q";
        resp.engine = "ddg";
        resp.results.push_back({"T", "https://x.com", "S"});
        Json j = resp.to_json();

        REQUIRE(j["results"].is_array());
        REQUIRE(j["results"].size() == 1u);
        const auto& sr = j["results"][0];
        CHECK(sr.contains("title"));
        CHECK(sr.contains("url"));
        CHECK(sr.contains("snippet"));
    }
}
