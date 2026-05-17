// include/batbox/config/McpConfig.hpp
// ---------------------------------------------------------------------------
// McpConfig — loader for ~/.batbox/mcp.json and ~/.claude/mcp.json.
//
// Schema (top-level "mcpServers" map):
//   {
//     "mcpServers": {
//       "<name>": {
//         // Stdio transport (default when "transport" key is absent):
//         "command": string          (REQUIRED for stdio)
//         "args":    [string]        (optional, default [])
//         "env":     {k:v,...}       (optional, extra env vars for subprocess)
//
//         // OR one of the remote transports ("sse", "http", "ws"):
//         "transport": "sse"|"http"|"ws"
//         "url":       string        (REQUIRED for remote)
//         "headers":   {k:v,...}     (optional; values support ${env:NAME} expansion)
//       },
//       ...
//     }
//   }
//
// Key design decisions:
//   - A stdio entry with no "command" is recorded as-is (not an error); the MCP
//     client layer validates before attempting to spawn.
//   - Per-entry parse errors are NON-FATAL: the bad entry is skipped and a WARN
//     is logged; all valid entries are still returned.
//   - ${env:NAME} references in header values are resolved against the process
//     environment at parse time.  An unset variable expands to an empty string.
//   - Unknown transport values are per-entry errors (see above).
//   - The two canonical paths are tried in order:
//       1. ~/.batbox/mcp.json  (primary BatBox config)
//       2. ~/.claude/mcp.json  (claude-code compat)
//     load_mcp_configs() attempts both and merges; later entries (claude) do NOT
//     override earlier (batbox) entries of the same name.
//
// Public API (namespace batbox::config):
//
//   struct StdioConfig    — stdio subprocess transport fields
//   struct HttpConfig     — HTTP/streamable JSON-RPC remote transport fields
//   struct SseConfig      — Server-Sent Events remote transport fields
//   struct WsConfig       — WebSocket remote transport fields
//
//   struct McpServerConfig — one parsed mcpServers entry:
//                            { name, variant<StdioConfig,HttpConfig,SseConfig,WsConfig> }
//
//   Result<std::vector<McpServerConfig>>
//   load_mcp_config(std::filesystem::path path)
//     Parse the "mcpServers" map from a single mcp.json file at `path`.
//     Per-entry errors are non-fatal.  Returns Err only for file I/O or
//     top-level JSON schema errors.
//
//   std::vector<McpServerConfig>
//   load_mcp_configs()
//     Try ~/.batbox/mcp.json then ~/.claude/mcp.json.  Returns the merged
//     result (batbox entries take precedence on name collision).
//     Missing files are silently skipped; only schema/I/O errors are logged.
//
// Build (standalone, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_mcp_config.cpp src/config/McpConfig.cpp \
//       src/core/Json.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_mcp_config && /tmp/test_mcp_config
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace batbox::config {

// ============================================================================
// Per-transport configuration structs
// ============================================================================

/// Configuration for a stdio (subprocess) MCP server.
struct StdioConfig {
    std::string              command; ///< Executable path or name; may be empty
    std::vector<std::string> args;    ///< argv[1..] passed to the subprocess
    std::unordered_map<std::string, std::string> env; ///< Extra env vars for subprocess
};

/// Configuration for an HTTP/streamable JSON-RPC MCP server.
struct HttpConfig {
    std::string url;     ///< http:// or https:// endpoint
    std::unordered_map<std::string, std::string> headers; ///< e.g. Authorization; ${env:NAME} expanded
};

/// Configuration for a Server-Sent Events MCP server.
struct SseConfig {
    std::string url;     ///< https:// SSE endpoint
    std::unordered_map<std::string, std::string> headers; ///< ${env:NAME} expanded
};

/// Configuration for a WebSocket MCP server.
struct WsConfig {
    std::string url;     ///< ws:// or wss:// endpoint
    std::unordered_map<std::string, std::string> headers; ///< ${env:NAME} expanded
};

// ============================================================================
// McpServerConfig — one fully-parsed mcpServers entry
// ============================================================================

/// Fully-parsed representation of a single mcpServers entry.
///
/// The active transport is indicated by which alternative is held in `impl`.
/// Use std::visit or std::get<T> to dispatch:
///
///   std::visit(overloaded{
///     [](const StdioConfig& c) { /* spawn process */ },
///     [](const HttpConfig&  c) { /* HTTP connect */ },
///     [](const SseConfig&   c) { /* SSE connect  */ },
///     [](const WsConfig&    c) { /* WS connect   */ },
///   }, server.impl);
struct McpServerConfig {
    std::string name; ///< Key from the "mcpServers" map (e.g. "filesystem")
    std::variant<StdioConfig, HttpConfig, SseConfig, WsConfig> impl;
};

// ============================================================================
// Free functions
// ============================================================================

/// Parse the "mcpServers" map from the mcp.json file at `path`.
///
/// Per-entry parse errors are NON-FATAL: the offending entry is logged as a
/// WARN and skipped; remaining valid entries are still returned.
///
/// Returns Err only for:
///   - File not found / cannot open
///   - Top-level JSON parse failure
///   - Top-level "mcpServers" field is present but is not a JSON object
///
/// @param path  Absolute or tilde-expanded path to an mcp.json file.
/// @return      Ok<std::vector<McpServerConfig>> on success (possibly empty);
///              Err<std::string> with a human-readable message on hard failure.
[[nodiscard]] Result<std::vector<McpServerConfig>>
load_mcp_config(std::filesystem::path path);

/// Load MCP server configurations from both known config locations.
///
/// Attempt order:
///   1. ~/.batbox/mcp.json  — primary BatBox config
///   2. ~/.claude/mcp.json  — claude-code compatibility layer
///
/// Missing files are silently skipped.  I/O or schema errors in either file
/// are logged as WARN and that file is skipped; the other is still attempted.
///
/// On name collision between the two files, the ~/.batbox/mcp.json entry
/// takes precedence (batbox-native config wins over compat config).
///
/// @return  Merged vector of McpServerConfig entries; may be empty.
[[nodiscard]] std::vector<McpServerConfig>
load_mcp_configs();

} // namespace batbox::config
