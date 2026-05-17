// include/batbox/plugins/MarketplaceJson.hpp
// ---------------------------------------------------------------------------
// Parser for .claude-plugin/marketplace.json and .batbox-plugin/marketplace.json.
//
// Both filenames use an identical schema (Decision of Record #7 — plugin
// compatibility with the claude-code plugin ecosystem).
//
// Schema:
//   {
//     "name":        string   (REQUIRED)
//     "version":     string   (optional, defaults to "")
//     "description": string   (optional, defaults to "")
//     "skills":      [string] (optional, path strings relative to plugin root)
//     "agents":      [string] (optional, path strings relative to plugin root)
//     "commands":    [string] (optional, path strings relative to plugin root)
//     "mcpServers":  {        (optional map, keyed by server name)
//       "<name>": {
//         // stdio transport:
//         "command": string
//         "args":    [string]    (optional)
//         "env":     {k:v,...}   (optional)
//         // OR remote transport:
//         "transport": "sse"|"http"|"ws"
//         "url":       string
//         "headers":   {k:v,...} (optional)
//       }
//     }
//   }
//
// Unknown fields at any level are logged as warnings and skipped (forward-
// compatible — new marketplace.json fields added upstream will not break old
// BatBox builds).
//
// Required fields:
//   - "name" at the top level.  Missing → error.
//
// All other fields are optional and default to empty / empty-collection.
//
// Public API (namespace batbox::plugins):
//
//   struct McpServerSpec   — one mcpServers entry (stdio or remote)
//   struct Marketplace     — fully-parsed marketplace.json
//
//   Result<Marketplace> parse_marketplace_json(const Json& j)
//     Parse a pre-parsed JSON object.  Returns Err<std::string> with a
//     human-readable message on schema errors.
//
//   Result<Marketplace> parse_marketplace_json(const fs::path& path)
//     Read + parse the file at `path`.  Returns Err on file I/O errors or
//     JSON / schema errors.
//
//   std::optional<fs::path> find_marketplace_in_dir(const fs::path& dir)
//     Probe `dir` for .claude-plugin/marketplace.json, then
//     .batbox-plugin/marketplace.json.  Returns the first that exists, or
//     std::nullopt if neither is present.
//
// Build (standalone, no CMake — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_marketplace_json.cpp src/plugins/MarketplaceJson.cpp \
//       src/core/Json.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_marketplace_json && /tmp/test_marketplace_json
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::plugins {

// ============================================================================
// McpServerSpec — one entry in the "mcpServers" map.
// ============================================================================

/// Discriminates between stdio-subprocess and remote-URL transports.
enum class McpTransport {
    Stdio,   ///< Launch a local subprocess; command + args + env
    Sse,     ///< Remote Server-Sent Events transport
    Http,    ///< Remote HTTP/streamable JSON-RPC transport
    Ws,      ///< Remote WebSocket transport
};

/// Parsed representation of a single mcpServers entry.
struct McpServerSpec {
    McpTransport transport = McpTransport::Stdio;

    // stdio fields (used when transport == Stdio)
    std::string              command;  ///< executable path or name
    std::vector<std::string> args;     ///< argv[1..] for the subprocess
    std::unordered_map<std::string, std::string> env; ///< extra env vars

    // remote fields (used when transport != Stdio)
    std::string url;     ///< wss://, https://, http:// endpoint
    std::unordered_map<std::string, std::string> headers; ///< e.g. Authorization

    bool operator==(const McpServerSpec&) const = default;
};

// ============================================================================
// Marketplace — top-level marketplace.json structure.
// ============================================================================

/// Fully-parsed contents of a marketplace.json file.
struct Marketplace {
    std::string name;         ///< REQUIRED — plugin / marketplace name
    std::string version;      ///< optional; empty string when absent
    std::string description;  ///< optional; empty string when absent

    /// Relative paths to skill markdown files (from the plugin root dir).
    std::vector<fs::path> skills;

    /// Relative paths to agent markdown files (from the plugin root dir).
    std::vector<fs::path> agents;

    /// Relative paths to command markdown files (from the plugin root dir).
    std::vector<fs::path> commands;

    /// Named MCP server configurations bundled with this plugin.
    std::unordered_map<std::string, McpServerSpec> mcp_servers;

    bool operator==(const Marketplace&) const = default;
};

// ============================================================================
// Free functions
// ============================================================================

/// Parse a pre-loaded JSON object into a Marketplace.
///
/// Validates that the required "name" field is present and is a string.
/// Optional fields default to empty when absent.
/// Unknown top-level or nested fields are logged as warnings and ignored.
///
/// @param j   A nlohmann::json value (must be an object).
/// @return    Ok<Marketplace> on success; Err<std::string> with a human-
///            readable message on validation failure.
[[nodiscard]] Result<Marketplace>
parse_marketplace_json(const Json& j);

/// Read the file at `path`, parse its JSON, then validate as Marketplace.
///
/// Convenience overload that opens the file and delegates to the Json overload.
///
/// @param path  Absolute or relative path to a marketplace.json file.
/// @return      Ok<Marketplace> on success; Err<std::string> on I/O, JSON
///              parse, or schema validation failure.
[[nodiscard]] Result<Marketplace>
parse_marketplace_json(const fs::path& path);

/// Probe `dir` for a marketplace.json using both known subdirectory names.
///
/// Checks in order:
///   1. <dir>/.claude-plugin/marketplace.json
///   2. <dir>/.batbox-plugin/marketplace.json
///
/// Returns the first path that exists as a regular file, or std::nullopt when
/// neither is found.
///
/// Does NOT parse the file — callers pass the returned path to
/// parse_marketplace_json(path) when they are ready to load it.
[[nodiscard]] std::optional<fs::path>
find_marketplace_in_dir(const fs::path& dir);

} // namespace batbox::plugins
