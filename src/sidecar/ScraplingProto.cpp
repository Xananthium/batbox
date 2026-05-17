// ---------------------------------------------------------------------------
// src/sidecar/ScraplingProto.cpp
//
// JSON serialisation / deserialisation for the Scrapling sidecar IPC structs.
//
// Each to_json() / from_json() pair mirrors the Pydantic model in
// python-sidecar/scrapling_server/app.py.  Field names, types, and defaults
// are intentionally kept identical so that round-trips against the FastAPI
// server are transparent.
//
// Defensive from_json() policy:
//   • Missing fields are filled with the struct's default values (same as
//     Pydantic's default= behaviour on the Python side).
//   • Type mismatches do NOT throw — batbox::get_or<T> returns the default.
//   • Optional string arrays (e.g. results, matches) default to empty vectors.
// ---------------------------------------------------------------------------

#include "batbox/sidecar/ScraplingProto.hpp"

#include <batbox/core/Json.hpp>

namespace batbox::sidecar::proto {

// ============================================================================
// HealthResponse
// ============================================================================

batbox::Json HealthResponse::to_json() const {
    return batbox::Json{
        {"status", status},
    };
}

HealthResponse HealthResponse::from_json(const batbox::Json& j) {
    HealthResponse r;
    r.status = batbox::get_or<std::string>(j, "status", "ok");
    return r;
}

// ============================================================================
// FetchRequest
// ============================================================================

batbox::Json FetchRequest::to_json() const {
    return batbox::Json{
        {"url",            url},
        {"timeout",        timeout},
        {"stealth",        stealth},
        {"respect_robots", respect_robots},
        {"max_bytes",      max_bytes},
    };
}

FetchRequest FetchRequest::from_json(const batbox::Json& j) {
    FetchRequest r;
    r.url            = batbox::get_or<std::string>(j, "url",            {});
    r.timeout        = batbox::get_or<double>     (j, "timeout",        30.0);
    r.stealth        = batbox::get_or<bool>       (j, "stealth",        false);
    r.respect_robots = batbox::get_or<bool>       (j, "respect_robots", true);
    r.max_bytes      = batbox::get_or<int>        (j, "max_bytes",      5'242'880);
    return r;
}

// ============================================================================
// FetchResponse
// ============================================================================

batbox::Json FetchResponse::to_json() const {
    return batbox::Json{
        {"url",            url},
        {"markdown",       markdown},
        {"status_code",    status_code},
        {"content_type",   content_type},
        {"content_length", content_length},
        {"fetched_at",     fetched_at},
        {"truncated",      truncated},
        {"is_error",       is_error},
        {"error_message",  error_message},
    };
}

FetchResponse FetchResponse::from_json(const batbox::Json& j) {
    FetchResponse r;
    r.url            = batbox::get_or<std::string>(j, "url",            {});
    r.markdown       = batbox::get_or<std::string>(j, "markdown",       {});
    r.status_code    = batbox::get_or<int>        (j, "status_code",    0);
    r.content_type   = batbox::get_or<std::string>(j, "content_type",   {});
    r.content_length = batbox::get_or<int>        (j, "content_length", 0);
    r.fetched_at     = batbox::get_or<std::string>(j, "fetched_at",     {});
    r.truncated      = batbox::get_or<bool>       (j, "truncated",      false);
    r.is_error       = batbox::get_or<bool>       (j, "is_error",       false);
    r.error_message  = batbox::get_or<std::string>(j, "error_message",  {});
    return r;
}

// ============================================================================
// SearchResult
// ============================================================================

batbox::Json SearchResult::to_json() const {
    return batbox::Json{
        {"title",   title},
        {"url",     url},
        {"snippet", snippet},
    };
}

SearchResult SearchResult::from_json(const batbox::Json& j) {
    SearchResult r;
    r.title   = batbox::get_or<std::string>(j, "title",   {});
    r.url     = batbox::get_or<std::string>(j, "url",     {});
    r.snippet = batbox::get_or<std::string>(j, "snippet", {});
    return r;
}

// ============================================================================
// SearchRequest
// ============================================================================

batbox::Json SearchRequest::to_json() const {
    return batbox::Json{
        {"query",       query},
        {"n",           n},
        {"engine",      engine},
        {"searxng_url", searxng_url},
    };
}

SearchRequest SearchRequest::from_json(const batbox::Json& j) {
    SearchRequest r;
    r.query       = batbox::get_or<std::string>(j, "query",       {});
    r.n           = batbox::get_or<int>        (j, "n",           10);
    r.engine      = batbox::get_or<std::string>(j, "engine",      "ddg");
    r.searxng_url = batbox::get_or<std::string>(j, "searxng_url", {});
    return r;
}

// ============================================================================
// SearchResponse
// ============================================================================

batbox::Json SearchResponse::to_json() const {
    batbox::Json results_arr = batbox::Json::array();
    for (const auto& sr : results) {
        results_arr.push_back(sr.to_json());
    }
    return batbox::Json{
        {"query",         query},
        {"engine",        engine},
        {"results",       results_arr},
        {"is_error",      is_error},
        {"error_message", error_message},
    };
}

SearchResponse SearchResponse::from_json(const batbox::Json& j) {
    SearchResponse r;
    r.query         = batbox::get_or<std::string>(j, "query",         {});
    r.engine        = batbox::get_or<std::string>(j, "engine",        "ddg");
    r.is_error      = batbox::get_or<bool>       (j, "is_error",      false);
    r.error_message = batbox::get_or<std::string>(j, "error_message", {});

    if (j.contains("results") && j.at("results").is_array()) {
        for (const auto& elem : j.at("results")) {
            r.results.push_back(SearchResult::from_json(elem));
        }
    }
    return r;
}

// ============================================================================
// SelectRequest
// ============================================================================

batbox::Json SelectRequest::to_json() const {
    return batbox::Json{
        {"url",       url},
        {"selector",  selector},
        {"timeout",   timeout},
        {"stealth",   stealth},
        {"attribute", attribute},
    };
}

SelectRequest SelectRequest::from_json(const batbox::Json& j) {
    SelectRequest r;
    r.url       = batbox::get_or<std::string>(j, "url",       {});
    r.selector  = batbox::get_or<std::string>(j, "selector",  {});
    r.timeout   = batbox::get_or<double>     (j, "timeout",   30.0);
    r.stealth   = batbox::get_or<bool>       (j, "stealth",   false);
    r.attribute = batbox::get_or<std::string>(j, "attribute", {});
    return r;
}

// ============================================================================
// SelectResponse
// ============================================================================

batbox::Json SelectResponse::to_json() const {
    batbox::Json matches_arr = batbox::Json::array();
    for (const auto& m : matches) {
        matches_arr.push_back(m);
    }
    return batbox::Json{
        {"url",           url},
        {"selector",      selector},
        {"matches",       matches_arr},
        {"count",         count},
        {"is_error",      is_error},
        {"error_message", error_message},
    };
}

SelectResponse SelectResponse::from_json(const batbox::Json& j) {
    SelectResponse r;
    r.url           = batbox::get_or<std::string>(j, "url",           {});
    r.selector      = batbox::get_or<std::string>(j, "selector",      {});
    r.count         = batbox::get_or<int>        (j, "count",         0);
    r.is_error      = batbox::get_or<bool>       (j, "is_error",      false);
    r.error_message = batbox::get_or<std::string>(j, "error_message", {});

    if (j.contains("matches") && j.at("matches").is_array()) {
        for (const auto& elem : j.at("matches")) {
            if (elem.is_string()) {
                r.matches.push_back(elem.get<std::string>());
            }
        }
    }
    return r;
}

// ============================================================================
// ShutdownResponse
// ============================================================================

batbox::Json ShutdownResponse::to_json() const {
    return batbox::Json{
        {"shutting_down", shutting_down},
    };
}

ShutdownResponse ShutdownResponse::from_json(const batbox::Json& j) {
    ShutdownResponse r;
    r.shutting_down = batbox::get_or<bool>(j, "shutting_down", false);
    return r;
}

// ============================================================================
// ErrorResponse
// ============================================================================

batbox::Json ErrorResponse::to_json() const {
    return batbox::Json{
        {"error",  error},
        {"detail", detail},
        {"path",   path},
    };
}

ErrorResponse ErrorResponse::from_json(const batbox::Json& j) {
    ErrorResponse r;
    r.error  = batbox::get_or<std::string>(j, "error",  {});
    r.detail = batbox::get_or<std::string>(j, "detail", {});
    r.path   = batbox::get_or<std::string>(j, "path",   {});
    return r;
}

} // namespace batbox::sidecar::proto
