#pragma once

// ---------------------------------------------------------------------------
// include/batbox/sidecar/ScraplingProto.hpp
//
// Request / response structs for the BatBox ↔ Scrapling sidecar IPC.
//
// Each struct mirrors the corresponding Pydantic model in
// python-sidecar/scrapling_server/app.py exactly — field names, types, and
// default values are kept in lock-step so that nlohmann round-trips against
// the FastAPI server without silent mismatches.
//
// Endpoints covered:
//   GET  /healthz   → HealthResponse
//   POST /fetch     → FetchRequest  / FetchResponse
//   POST /search    → SearchRequest / SearchResponse  (contains SearchResult)
//   POST /select    → SelectRequest / SelectResponse
//   POST /shutdown  → ShutdownResponse
//   (any error)     → ErrorResponse
//
// Serialisation:
//   Each type provides:
//     nlohmann::json  to_json()  const;
//     static T        from_json(const nlohmann::json&);
//
//   Callers may use batbox::dump() / batbox::parse() for string conversion.
//
// nlohmann ADL hooks (to_json / from_json free functions) are intentionally
// NOT defined so that callers must be explicit about serialisation — this
// prevents accidental silent conversions inside STL containers.
//
// Namespace: batbox::sidecar::proto
// ---------------------------------------------------------------------------

#include <batbox/core/Json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace batbox::sidecar::proto {

// ============================================================================
// GET /healthz
// ============================================================================

/// Response body for GET /healthz.
/// Python model: {"status": "ok"}
struct HealthResponse {
    std::string status{"ok"};

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static HealthResponse from_json(const batbox::Json& j);
};

// ============================================================================
// POST /fetch
// ============================================================================

/// Request body for POST /fetch.
///
/// Field mapping (C++ → Python):
///   url           → url            (required)
///   timeout       → timeout        (default 30.0 s)
///   stealth       → stealth        (default false)
///   respect_robots→ respect_robots (default true)
///   max_bytes     → max_bytes      (default 5 242 880 = 5 MiB)
struct FetchRequest {
    std::string url;
    double      timeout{30.0};
    bool        stealth{false};
    bool        respect_robots{true};
    int         max_bytes{5'242'880};

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static FetchRequest from_json(const batbox::Json& j);
};

/// Response body for POST /fetch.
///
/// Field mapping (C++ → Python):
///   url            → url
///   markdown       → markdown
///   status_code    → status_code
///   content_type   → content_type   (default "")
///   content_length → content_length (default 0)
///   fetched_at     → fetched_at     (default "")
///   truncated      → truncated      (default false)
///   is_error       → is_error       (default false)
///   error_message  → error_message  (default "")
struct FetchResponse {
    std::string url;
    std::string markdown;
    int         status_code{0};
    std::string content_type;
    int         content_length{0};
    std::string fetched_at;
    bool        truncated{false};
    bool        is_error{false};
    std::string error_message;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static FetchResponse from_json(const batbox::Json& j);
};

// ============================================================================
// POST /search
// ============================================================================

/// A single search result entry — element of SearchResponse::results.
///
/// Field mapping: title → title, url → url, snippet → snippet.
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static SearchResult from_json(const batbox::Json& j);
};

/// Request body for POST /search.
///
/// Field mapping (C++ → Python):
///   query       → query      (required)
///   n           → n          (default 10, 1–50)
///   engine      → engine     (default "ddg"; "ddg" | "searxng")
///   searxng_url → searxng_url (default ""; required when engine=="searxng")
struct SearchRequest {
    std::string query;
    int         n{10};
    std::string engine{"ddg"};
    std::string searxng_url;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static SearchRequest from_json(const batbox::Json& j);
};

/// Response body for POST /search.
///
/// Field mapping (C++ → Python):
///   query         → query
///   engine        → engine
///   results       → results
///   is_error      → is_error      (default false)
///   error_message → error_message (default "")
struct SearchResponse {
    std::string               query;
    std::string               engine;
    std::vector<SearchResult> results;
    bool                      is_error{false};
    std::string               error_message;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static SearchResponse from_json(const batbox::Json& j);
};

// ============================================================================
// POST /select
// ============================================================================

/// Request body for POST /select.
///
/// Field mapping (C++ → Python):
///   url       → url       (required; HTTP URL or raw HTML string)
///   selector  → selector  (required; CSS selector or XPath expression)
///   timeout   → timeout   (default 30.0 s)
///   stealth   → stealth   (default false)
///   attribute → attribute (default ""; if set, return this attribute per match)
struct SelectRequest {
    std::string url;
    std::string selector;
    double      timeout{30.0};
    bool        stealth{false};
    std::string attribute;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static SelectRequest from_json(const batbox::Json& j);
};

/// Response body for POST /select.
///
/// Field mapping (C++ → Python):
///   url           → url
///   selector      → selector
///   matches       → matches       (list of matched text / attribute values)
///   count         → count         (default 0)
///   is_error      → is_error      (default false)
///   error_message → error_message (default "")
struct SelectResponse {
    std::string              url;
    std::string              selector;
    std::vector<std::string> matches;
    int                      count{0};
    bool                     is_error{false};
    std::string              error_message;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static SelectResponse from_json(const batbox::Json& j);
};

// ============================================================================
// POST /shutdown
// ============================================================================

/// Response body for POST /shutdown.
/// Python handler returns {"shutting_down": true}.
struct ShutdownResponse {
    bool shutting_down{false};

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static ShutdownResponse from_json(const batbox::Json& j);
};

// ============================================================================
// Error envelope (HTTP 4xx / 5xx from the generic_exception_handler)
// ============================================================================

/// Structured error body returned by the sidecar's generic exception handler.
///
/// Python handler produces:
///   {"error": "<ExceptionType>", "detail": "<message>", "path": "/endpoint"}
struct ErrorResponse {
    std::string error;
    std::string detail;
    std::string path;

    [[nodiscard]] batbox::Json to_json() const;
    [[nodiscard]] static ErrorResponse from_json(const batbox::Json& j);
};

} // namespace batbox::sidecar::proto
