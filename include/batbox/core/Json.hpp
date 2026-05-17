// include/batbox/core/Json.hpp
// ---------------------------------------------------------------------------
// batbox JSON utilities — dual-API wrapper around nlohmann/json + simdjson.
//
// Design:
//   nlohmann::json path  — ergonomic in-memory construction and mutation
//                          (config writes, settings, request serialisation).
//   simdjson path        — zero-copy fast parse on hot paths
//                          (sidecar response chunks, large streaming payloads).
//
// Type alias:
//   batbox::Json          →  nlohmann::json
//
// Free functions (all in namespace batbox):
//   parse(sv)             →  Result<Json, std::string>  via nlohmann (fallback path)
//   parse_fast(sv)        →  Result<Json, std::string>  via simdjson→nlohmann bridge
//   dump(j)               →  std::string  (compact serialisation)
//   pretty(j)             →  std::string  (4-space indented)
//   get_or<T>(j,key,def)  →  T            (safe field access with default)
//   path_get(j,dotted)    →  std::optional<Json>  (dotted-path navigation)
//
// parse_simdjson_doc(sv)  →  Result<simdjson::dom::element, std::string>
//   Thin simdjson wrapper for callers that want a live dom::element for
//   zero-copy field inspection without converting to nlohmann.  The returned
//   element is valid only as long as the parser (stored in thread_local
//   storage inside the implementation) is not reused — copy values out before
//   calling again.
//
// Build (standalone, no CMake needed — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_json_helpers.cpp src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_json && /tmp/test_json
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Result.hpp>

#include <nlohmann/json.hpp>
#include <simdjson.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace batbox {

// ============================================================================
// Type alias
// ============================================================================

/// batbox::Json is nlohmann::json — use it for all in-memory JSON construction
/// and ergonomic field access.  For hot-path read-only parsing of large blobs,
/// prefer parse_fast() or parse_simdjson_doc().
using Json = nlohmann::json;

// ============================================================================
// Parsing
// ============================================================================

/// Parse JSON from a string_view using nlohmann::json (pure C++ path).
/// Safe for all sizes; preferred when you need a mutable Json tree.
/// Returns Err with the parser's what() message on malformed input.
[[nodiscard]] inline Result<Json, std::string>
parse(std::string_view sv) noexcept {
    try {
        return Json::parse(sv.begin(), sv.end());
    } catch (const nlohmann::json::parse_error& e) {
        return Err(std::string(e.what()));
    } catch (const std::exception& e) {
        return Err(std::string(e.what()));
    }
}

// parse_fast and parse_simdjson_doc have non-trivial implementations that
// use a thread_local simdjson parser — defined in src/core/Json.cpp.

/// Parse JSON using simdjson (SIMD-accelerated) and convert the result to a
/// nlohmann::json tree.  On the hot path this is faster than parse() for large
/// payloads; for small objects the overhead of the conversion may dominate.
/// Returns Err on malformed input or simdjson parse failure.
[[nodiscard]] Result<Json, std::string>
parse_fast(std::string_view sv) noexcept;

/// Low-level simdjson accessor: returns a live dom::element backed by a
/// thread_local parser.  The element is valid until the next call to
/// parse_simdjson_doc() on the same thread.  Copy primitive values out
/// immediately; do not store the element across async suspension points.
[[nodiscard]] Result<simdjson::dom::element, std::string>
parse_simdjson_doc(std::string_view bytes) noexcept;

// ============================================================================
// Serialisation
// ============================================================================

/// Compact (minified) JSON serialisation.
[[nodiscard]] inline std::string dump(const Json& j) {
    return j.dump();
}

/// Pretty-print JSON with 4-space indentation.
[[nodiscard]] inline std::string pretty(const Json& j) {
    return j.dump(4);
}

// ============================================================================
// Safe field access helpers
// ============================================================================

/// Return j[key] as T, or default_val when the key is absent or the value
/// cannot be converted to T.  Never throws.
///
///   auto host = batbox::get_or<std::string>(cfg, "host", "localhost");
///   auto port = batbox::get_or<int>(cfg, "port", 8080);
template <typename T>
[[nodiscard]] T get_or(const Json& j, std::string_view key, T default_val) noexcept {
    try {
        auto it = j.find(key);
        if (it == j.end()) return default_val;
        return it->template get<T>();
    } catch (...) {
        return default_val;
    }
}

/// Navigate a dotted path such as "model.limits.tokens" through nested
/// objects and return the Json node at that location, or std::nullopt when
/// any segment is missing or the parent is not an object.
///
///   auto tokens = batbox::path_get(cfg, "model.limits.tokens");
[[nodiscard]] inline std::optional<Json>
path_get(const Json& j, std::string_view dotted_path) noexcept {
    try {
        const Json* cur = &j;
        std::string_view remaining = dotted_path;

        while (!remaining.empty()) {
            auto dot = remaining.find('.');
            std::string_view seg = (dot == std::string_view::npos)
                                       ? remaining
                                       : remaining.substr(0, dot);
            remaining = (dot == std::string_view::npos)
                            ? std::string_view{}
                            : remaining.substr(dot + 1);

            if (!cur->is_object()) return std::nullopt;
            auto it = cur->find(seg);
            if (it == cur->end()) return std::nullopt;
            cur = &(*it);
        }

        return std::optional<Json>(*cur);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace batbox
