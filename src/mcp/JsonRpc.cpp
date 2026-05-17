// src/mcp/JsonRpc.cpp
// ---------------------------------------------------------------------------
// JSON-RPC 2.0 envelope helpers — implementation.
// See include/batbox/mcp/JsonRpc.hpp for the public API.
// ---------------------------------------------------------------------------

#include <batbox/mcp/JsonRpc.hpp>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace batbox::mcp {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/// Validate that j is a JSON object and has "jsonrpc" == "2.0".
/// Returns an error string or empty string on success.
[[nodiscard]] std::string check_envelope(const Json& j) {
    if (!j.is_object()) {
        return "JSON-RPC message must be a JSON object";
    }
    auto it = j.find("jsonrpc");
    if (it == j.end() || !it->is_string() || it->get<std::string>() != "2.0") {
        return "missing or invalid \"jsonrpc\" field (expected \"2.0\")";
    }
    return {};
}

/// Parse a JSON-RPC id field (int or string only; null allowed for responses).
/// Returns Err when the type is unsupported.
/// null_ok: when true, treats a null/missing id as int64_t{0}.
[[nodiscard]] Result<std::variant<int64_t, std::string>, std::string>
parse_id(const Json& j, bool null_ok = false) {
    if (j.is_number_integer()) {
        return std::variant<int64_t, std::string>{j.get<int64_t>()};
    }
    if (j.is_string()) {
        return std::variant<int64_t, std::string>{j.get<std::string>()};
    }
    if (null_ok && (j.is_null())) {
        return std::variant<int64_t, std::string>{int64_t{0}};
    }
    return Err(std::string("JSON-RPC \"id\" must be a string or integer"));
}

/// Build the common error object { "code": ..., "message": ..., "data"?: ... }.
[[nodiscard]] Json build_error_object(int code, std::string_view msg,
                                       const Json& data) {
    Json err = Json::object();
    err["code"]    = code;
    err["message"] = std::string(msg);
    if (!data.is_null()) {
        err["data"] = data;
    }
    return err;
}

} // namespace

// ============================================================================
// JsonRpcError
// ============================================================================

Json JsonRpcError::to_json() const {
    Json j = Json::object();
    j["code"]    = code;
    j["message"] = message;
    if (data.has_value()) {
        j["data"] = *data;
    }
    return j;
}

Result<JsonRpcError, std::string>
JsonRpcError::from_json(const Json& j) {
    if (!j.is_object()) {
        return Err(std::string("JSON-RPC error must be a JSON object"));
    }

    auto code_it = j.find("code");
    if (code_it == j.end() || !code_it->is_number_integer()) {
        return Err(std::string("JSON-RPC error missing integer \"code\" field"));
    }

    auto msg_it = j.find("message");
    if (msg_it == j.end() || !msg_it->is_string()) {
        return Err(std::string("JSON-RPC error missing string \"message\" field"));
    }

    JsonRpcError err;
    err.code    = code_it->get<int>();
    err.message = msg_it->get<std::string>();

    auto data_it = j.find("data");
    if (data_it != j.end()) {
        err.data = *data_it;
    }

    return err;
}

// ============================================================================
// JsonRpcRequest
// ============================================================================

Json JsonRpcRequest::to_json() const {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";

    if (std::holds_alternative<int64_t>(id)) {
        j["id"] = std::get<int64_t>(id);
    } else {
        j["id"] = std::get<std::string>(id);
    }

    j["method"] = method;

    if (!params.is_null()) {
        j["params"] = params;
    }

    return j;
}

Result<JsonRpcRequest, std::string>
JsonRpcRequest::from_json(const Json& j) {
    if (auto err = check_envelope(j); !err.empty()) {
        return Err(err);
    }

    // Must have "method"
    auto method_it = j.find("method");
    if (method_it == j.end() || !method_it->is_string()) {
        return Err(std::string("JSON-RPC request missing string \"method\" field"));
    }

    // Must have "id"
    auto id_it = j.find("id");
    if (id_it == j.end()) {
        return Err(std::string("JSON-RPC request missing \"id\" field"));
    }

    auto parsed_id = parse_id(*id_it, /*null_ok=*/false);
    if (!parsed_id) {
        return Err(parsed_id.error());
    }

    JsonRpcRequest req;
    req.id     = std::move(*parsed_id);
    req.method = method_it->get<std::string>();

    auto params_it = j.find("params");
    req.params = (params_it != j.end()) ? *params_it : Json(nullptr);

    return req;
}

// ============================================================================
// JsonRpcNotification
// ============================================================================

Json JsonRpcNotification::to_json() const {
    Json j       = Json::object();
    j["jsonrpc"]  = "2.0";
    j["method"]   = method;

    if (!params.is_null()) {
        j["params"] = params;
    }

    return j;
}

Result<JsonRpcNotification, std::string>
JsonRpcNotification::from_json(const Json& j) {
    if (auto err = check_envelope(j); !err.empty()) {
        return Err(err);
    }

    auto method_it = j.find("method");
    if (method_it == j.end() || !method_it->is_string()) {
        return Err(std::string("JSON-RPC notification missing string \"method\" field"));
    }

    JsonRpcNotification notif;
    notif.method = method_it->get<std::string>();

    auto params_it = j.find("params");
    notif.params = (params_it != j.end()) ? *params_it : Json(nullptr);

    return notif;
}

// ============================================================================
// JsonRpcResponse
// ============================================================================

Json JsonRpcResponse::to_json() const {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";

    if (std::holds_alternative<int64_t>(id)) {
        int64_t int_id = std::get<int64_t>(id);
        // Preserve null id per spec (we use 0 as sentinel for null)
        // but in practice MCP always provides a real id; emit as integer.
        j["id"] = int_id;
    } else {
        j["id"] = std::get<std::string>(id);
    }

    if (result.has_value()) {
        j["result"] = *result;
    } else if (error.has_value()) {
        j["error"] = error->to_json();
    }

    return j;
}

Result<JsonRpcResponse, std::string>
JsonRpcResponse::from_json(const Json& j) {
    if (auto err = check_envelope(j); !err.empty()) {
        return Err(err);
    }

    // id is present (may be null for some error responses per spec)
    auto id_it = j.find("id");
    if (id_it == j.end()) {
        return Err(std::string("JSON-RPC response missing \"id\" field"));
    }

    auto parsed_id = parse_id(*id_it, /*null_ok=*/true);
    if (!parsed_id) {
        return Err(parsed_id.error());
    }

    bool has_result = (j.find("result") != j.end());
    bool has_error  = (j.find("error")  != j.end());

    if (has_result && has_error) {
        return Err(std::string(
            "JSON-RPC response must contain exactly one of \"result\" or \"error\""));
    }
    if (!has_result && !has_error) {
        return Err(std::string(
            "JSON-RPC response must contain \"result\" or \"error\""));
    }

    JsonRpcResponse resp;
    resp.id = std::move(*parsed_id);

    if (has_result) {
        resp.result = j["result"];
    } else {
        auto parsed_err = JsonRpcError::from_json(j["error"]);
        if (!parsed_err) {
            return Err(parsed_err.error());
        }
        resp.error = std::move(*parsed_err);
    }

    return resp;
}

// ============================================================================
// parse_message
// ============================================================================

Result<AnyMessage, std::string>
parse_message(const Json& j) {
    // Reject batch (JSON array) — not supported by MCP.
    if (j.is_array()) {
        return Err(std::string(
            "JSON-RPC batch messages are not supported"));
    }

    if (auto err = check_envelope(j); !err.empty()) {
        return Err(err);
    }

    bool has_method = (j.find("method") != j.end());
    bool has_id     = (j.find("id")     != j.end());
    bool has_result = (j.find("result") != j.end());
    bool has_error  = (j.find("error")  != j.end());

    if (has_method && has_id) {
        // Request
        auto req = JsonRpcRequest::from_json(j);
        if (!req) return Err(req.error());
        return AnyMessage{std::move(*req)};
    }

    if (has_method && !has_id) {
        // Notification
        auto notif = JsonRpcNotification::from_json(j);
        if (!notif) return Err(notif.error());
        return AnyMessage{std::move(*notif)};
    }

    if (has_result || has_error) {
        // Response
        auto resp = JsonRpcResponse::from_json(j);
        if (!resp) return Err(resp.error());
        return AnyMessage{std::move(*resp)};
    }

    return Err(std::string(
        "JSON-RPC message does not match request, notification, or response shape"));
}

// ============================================================================
// Free builder functions
// ============================================================================

Json make_request(int64_t id, std::string_view method, Json params) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"]      = id;
    j["method"]  = std::string(method);
    if (!params.is_null()) {
        j["params"] = std::move(params);
    }
    return j;
}

Json make_request(std::string_view id, std::string_view method, Json params) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"]      = std::string(id);
    j["method"]  = std::string(method);
    if (!params.is_null()) {
        j["params"] = std::move(params);
    }
    return j;
}

Json make_notification(std::string_view method, Json params) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["method"]  = std::string(method);
    if (!params.is_null()) {
        j["params"] = std::move(params);
    }
    return j;
}

Json make_response(int64_t id, Json result) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"]      = id;
    j["result"]  = std::move(result);
    return j;
}

Json make_response(std::string_view id, Json result) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"]      = std::string(id);
    j["result"]  = std::move(result);
    return j;
}

Json make_error_response(int64_t id, int code, std::string_view msg, Json data) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"]      = id;
    j["error"]   = build_error_object(code, msg, data);
    return j;
}

Json make_error_response(std::string_view id, int code, std::string_view msg,
                          Json data) {
    Json j      = Json::object();
    j["jsonrpc"] = "2.0";
    j["id"]      = std::string(id);
    j["error"]   = build_error_object(code, msg, data);
    return j;
}

// ============================================================================
// Thread-safe ID counter
// ============================================================================

int64_t next_id() noexcept {
    // Starts at 1; each call increments atomically.  Wrapping at INT64_MAX is
    // practically impossible in any real session.
    static std::atomic<int64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace batbox::mcp
