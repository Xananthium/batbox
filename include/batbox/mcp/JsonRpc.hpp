// include/batbox/mcp/JsonRpc.hpp
// ---------------------------------------------------------------------------
// JSON-RPC 2.0 envelope helpers for the MCP client.
//
// Supports the four JSON-RPC 2.0 message shapes used by MCP:
//   JsonRpcRequest      — client → server call expecting a response.
//   JsonRpcNotification — client → server fire-and-forget (no id, no reply).
//   JsonRpcResponse     — server → client reply (result XOR error).
//   JsonRpcError        — error payload carried inside JsonRpcResponse.
//
// Free builder functions (round-trip with the underlying Json type):
//   make_request(method, params, id)       → Json
//   make_notification(method, params)      → Json
//   make_response(id, result)              → Json
//   make_error_response(id, code, msg)     → Json
//   make_error_response(id, code, msg, data) → Json
//
// Receiver-side dispatcher:
//   parse_message(j)   → std::variant<JsonRpcRequest,
//                                      JsonRpcNotification,
//                                      JsonRpcResponse>
//
// ID counter:
//   next_id()          → int64_t   (thread-safe, monotonically increasing)
//
// Standard error codes (JSON-RPC 2.0 §5.1 + MCP extensions):
//   namespace batbox::mcp::errc
//   kParseError, kInvalidRequest, kMethodNotFound, kInvalidParams,
//   kInternalError, kMcpServerError, kMcpToolError, kMcpCapabilityError
//
// Build standalone (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_json_rpc.cpp src/mcp/JsonRpc.cpp \
//       -o /tmp/test_json_rpc && /tmp/test_json_rpc
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace batbox::mcp {

// ============================================================================
// Standard error codes (JSON-RPC 2.0 §5.1)
// ============================================================================

/// Error code constants — JSON-RPC 2.0 reserved range and MCP-specific codes.
namespace errc {

/// -32700: Invalid JSON received by the server.
inline constexpr int kParseError     = -32700;
/// -32600: The JSON sent is not a valid Request object.
inline constexpr int kInvalidRequest = -32600;
/// -32601: The method does not exist or is not available.
inline constexpr int kMethodNotFound = -32601;
/// -32602: Invalid method parameter(s).
inline constexpr int kInvalidParams  = -32602;
/// -32603: Internal JSON-RPC error.
inline constexpr int kInternalError  = -32603;

// JSON-RPC 2.0 spec reserves -32099 to -32000 for implementation-defined
// server errors.  MCP uses the following codes in that range:

/// -32000: Generic MCP server-side processing error.
inline constexpr int kMcpServerError     = -32000;
/// -32001: The invoked tool returned an error.
inline constexpr int kMcpToolError       = -32001;
/// -32002: The server does not support the requested capability.
inline constexpr int kMcpCapabilityError = -32002;

} // namespace errc

// ============================================================================
// JsonRpcError — error payload in a response
// ============================================================================

/// Structured representation of a JSON-RPC 2.0 error object:
///   { "code": <int>, "message": "<string>", "data": <any>? }
struct JsonRpcError {
    int                code;
    std::string        message;
    std::optional<Json> data;   ///< optional additional information

    /// Serialise to a Json object suitable for embedding in a response.
    [[nodiscard]] Json to_json() const;

    /// Deserialise from a Json object.  Returns Err when required fields are
    /// missing or have the wrong type.
    [[nodiscard]] static Result<JsonRpcError, std::string>
    from_json(const Json& j);
};

// ============================================================================
// JsonRpcRequest — a call with an id
// ============================================================================

/// Parsed representation of a JSON-RPC 2.0 request:
///   { "jsonrpc": "2.0", "id": <int|string>, "method": "<string>",
///     "params": <object|array>? }
struct JsonRpcRequest {
    std::variant<int64_t, std::string> id;     ///< request id (never null for requests)
    std::string                        method;
    Json                               params; ///< null when absent in the wire message

    /// Serialise to a complete JSON-RPC 2.0 request object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise.  Returns Err when "jsonrpc" != "2.0", "id" is missing,
    /// "method" is absent, or the id type is not int|string.
    [[nodiscard]] static Result<JsonRpcRequest, std::string>
    from_json(const Json& j);
};

// ============================================================================
// JsonRpcNotification — fire-and-forget (no id)
// ============================================================================

/// Parsed representation of a JSON-RPC 2.0 notification:
///   { "jsonrpc": "2.0", "method": "<string>", "params": <object|array>? }
/// Notifications have no "id" field; the receiver never sends a response.
struct JsonRpcNotification {
    std::string method;
    Json        params; ///< null when absent

    /// Serialise to a complete JSON-RPC 2.0 notification object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise.  Returns Err when "jsonrpc" != "2.0" or "method" is absent.
    [[nodiscard]] static Result<JsonRpcNotification, std::string>
    from_json(const Json& j);
};

// ============================================================================
// JsonRpcResponse — server reply (result XOR error)
// ============================================================================

/// Parsed representation of a JSON-RPC 2.0 response:
///   { "jsonrpc": "2.0", "id": <int|string|null>,
///     "result": <any>? XOR "error": <error-object>? }
/// Exactly one of result or error must be present per the spec.
struct JsonRpcResponse {
    std::variant<int64_t, std::string> id;     ///< may be null in spec; here we store as 0/"" sentinel
    std::optional<Json>              result;
    std::optional<JsonRpcError>      error;

    /// Serialise to a complete JSON-RPC 2.0 response object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise.  Returns Err when "jsonrpc" != "2.0", both result and error
    /// are present, or neither is present.
    [[nodiscard]] static Result<JsonRpcResponse, std::string>
    from_json(const Json& j);
};

// ============================================================================
// parse_message — receiver-side dispatcher
// ============================================================================

/// The three message kinds that arrive from the MCP server.
using AnyMessage = std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse>;

/// Inspect a raw JSON value and dispatch to the correct envelope type.
///
/// Dispatch rules (per JSON-RPC 2.0 spec):
///   - Has "method" + has "id"          → JsonRpcRequest
///   - Has "method" + no "id"           → JsonRpcNotification
///   - Has "result" or "error" + "id"   → JsonRpcResponse
///
/// Returns Err with a descriptive message when the message is not a valid
/// JSON-RPC 2.0 object, "jsonrpc" != "2.0", the id type is unsupported, or
/// the message is a JSON array (batch — not supported by MCP).
[[nodiscard]] Result<AnyMessage, std::string>
parse_message(const Json& j);

// ============================================================================
// Free builder functions
// ============================================================================

/// Build a JSON-RPC 2.0 request object.
///
/// @param id      Request id (int or string).
/// @param method  RPC method name.
/// @param params  Parameters — typically a Json object or null.
/// @return        Complete Json ready to serialise.
[[nodiscard]] Json
make_request(int64_t id, std::string_view method, Json params = Json(nullptr));

/// String-id overload of make_request.
[[nodiscard]] Json
make_request(std::string_view id, std::string_view method, Json params = Json(nullptr));

/// Build a JSON-RPC 2.0 notification (no id, no response expected).
///
/// @param method  RPC method name.
/// @param params  Parameters — typically a Json object or null.
/// @return        Complete Json ready to serialise.
[[nodiscard]] Json
make_notification(std::string_view method, Json params = Json(nullptr));

/// Build a JSON-RPC 2.0 success response.
///
/// @param id      Matching request id (int).
/// @param result  Result payload.
/// @return        Complete Json ready to serialise.
[[nodiscard]] Json
make_response(int64_t id, Json result);

/// String-id overload of make_response.
[[nodiscard]] Json
make_response(std::string_view id, Json result);

/// Build a JSON-RPC 2.0 error response.
///
/// @param id      Matching request id (int).
/// @param code    One of the errc:: constants.
/// @param msg     Human-readable error description.
/// @param data    Optional extra data (default: omitted).
/// @return        Complete Json ready to serialise.
[[nodiscard]] Json
make_error_response(int64_t id, int code, std::string_view msg,
                    Json data = Json(nullptr));

/// String-id overload of make_error_response.
[[nodiscard]] Json
make_error_response(std::string_view id, int code, std::string_view msg,
                    Json data = Json(nullptr));

// ============================================================================
// Thread-safe ID counter
// ============================================================================

/// Return the next unique request id.  The counter starts at 1, is
/// monotonically increasing, and is safe to call from multiple threads.
[[nodiscard]] int64_t next_id() noexcept;

} // namespace batbox::mcp
