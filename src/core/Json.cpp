// src/core/Json.cpp
// ---------------------------------------------------------------------------
// Implementation of the simdjson ↔ nlohmann bridge functions declared in
// include/batbox/core/Json.hpp.
//
// Why a .cpp at all?
//   simdjson::dom::parser carries significant state (a padded buffer, SIMD
//   dispatch table, etc.).  Creating a fresh parser on every call wastes that
//   setup cost.  We keep one parser per thread in thread_local storage.
//   thread_local objects with non-trivial constructors must not be defined in
//   a header because each translation unit that includes the header would get
//   its own definition, violating ODR.  A single .cpp gives us one definition.
//
// simdjson → nlohmann conversion:
//   simdjson does not expose a "to nlohmann::json" utility in its public API.
//   We walk the dom::element tree recursively and rebuild an equivalent
//   nlohmann::json tree.  This is O(n) in the number of JSON values — the
//   SIMD speedup is in the tokenisation pass, which still dominates for large
//   payloads.
// ---------------------------------------------------------------------------

#include <batbox/core/Json.hpp>

#include <simdjson.h>

namespace batbox {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/// Recursively convert a simdjson::dom::element to nlohmann::json.
/// Throws std::runtime_error on unrecognised type (defensive only; simdjson
/// covers all JSON types).
Json element_to_json(simdjson::dom::element elem) {
    switch (elem.type()) {
        case simdjson::dom::element_type::OBJECT: {
            Json obj = Json::object();
            for (auto [k, v] : simdjson::dom::object(elem)) {
                obj[std::string(k)] = element_to_json(v);
            }
            return obj;
        }
        case simdjson::dom::element_type::ARRAY: {
            Json arr = Json::array();
            for (auto v : simdjson::dom::array(elem)) {
                arr.push_back(element_to_json(v));
            }
            return arr;
        }
        case simdjson::dom::element_type::STRING: {
            std::string_view sv;
            if (elem.get(sv) == simdjson::SUCCESS) {
                return Json(std::string(sv));
            }
            return Json("");
        }
        case simdjson::dom::element_type::INT64: {
            int64_t v{};
            if (elem.get(v) == simdjson::SUCCESS) return Json(v);
            return Json(0);
        }
        case simdjson::dom::element_type::UINT64: {
            uint64_t v{};
            if (elem.get(v) == simdjson::SUCCESS) return Json(v);
            return Json(0u);
        }
        case simdjson::dom::element_type::DOUBLE: {
            double v{};
            if (elem.get(v) == simdjson::SUCCESS) return Json(v);
            return Json(0.0);
        }
        case simdjson::dom::element_type::BOOL: {
            bool v{};
            if (elem.get(v) == simdjson::SUCCESS) return Json(v);
            return Json(false);
        }
        case simdjson::dom::element_type::NULL_VALUE:
            return Json(nullptr);
        default:
            throw std::runtime_error("simdjson: unknown element type");
    }
}

} // anonymous namespace

// ============================================================================
// parse_simdjson_doc
// ============================================================================

Result<simdjson::dom::element, std::string>
parse_simdjson_doc(std::string_view bytes) noexcept {
    // One parser per thread — avoids malloc on every call after the first.
    thread_local simdjson::dom::parser tl_parser;

    simdjson::dom::element doc;
    auto ec = tl_parser.parse(bytes.data(), bytes.size()).get(doc);
    if (ec != simdjson::SUCCESS) {
        return Err(std::string(simdjson::error_message(ec)));
    }
    return doc;
}

// ============================================================================
// parse_fast  (simdjson → nlohmann bridge)
// ============================================================================

Result<Json, std::string>
parse_fast(std::string_view sv) noexcept {
    auto res = parse_simdjson_doc(sv);
    if (!res.has_value()) {
        return Err(res.error());
    }
    try {
        return element_to_json(res.value());
    } catch (const std::exception& e) {
        return Err(std::string(e.what()));
    } catch (...) {
        return Err(std::string("parse_fast: unknown error during conversion"));
    }
}

} // namespace batbox
