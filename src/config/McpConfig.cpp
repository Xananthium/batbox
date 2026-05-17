// src/config/McpConfig.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::load_mcp_config and load_mcp_configs.
//
// See include/batbox/config/McpConfig.hpp for the full API contract.
// ---------------------------------------------------------------------------

#include <batbox/config/McpConfig.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace batbox::config {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Known keys inside a single mcpServers entry.
const std::set<std::string> kKnownMcpServerKeys = {
    "command", "args", "env", "transport", "url", "headers",
};

/// Expand ${env:NAME} references in a string against the process environment.
/// Unset variables expand to an empty string.
std::string expand_env_refs(const std::string& value) {
    // Pattern: ${env:NAME} where NAME is one or more word chars
    static const std::regex kEnvRef(R"(\$\{env:([A-Za-z_][A-Za-z0-9_]*)\})");

    std::string result;
    result.reserve(value.size());

    auto begin = value.cbegin();
    auto end   = value.cend();

    std::sregex_iterator it(begin, end, kEnvRef);
    std::sregex_iterator none;

    std::string::const_iterator last_pos = begin;
    for (; it != none; ++it) {
        const std::smatch& m = *it;
        // Append literal text before this match
        result.append(last_pos, m[0].first);
        // Append the env var value (or empty if unset)
        const char* env_val = std::getenv(m[1].str().c_str());
        if (env_val) {
            result += env_val;
        }
        last_pos = m[0].second;
    }
    // Append remaining literal text after the last match
    result.append(last_pos, end);
    return result;
}

/// Parse a JSON string→string object into an unordered_map<string,string>,
/// applying ${env:NAME} expansion to each value.
/// Returns Err if `obj` is not an object or any value is not a string.
Result<std::unordered_map<std::string, std::string>>
parse_string_map_with_env(const Json& obj, std::string_view field_name) {
    if (!obj.is_object()) {
        return Err(std::string("field '") + std::string(field_name) +
                   "' must be an object (string→string map)");
    }
    std::unordered_map<std::string, std::string> out;
    out.reserve(obj.size());
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it.value().is_string()) {
            return Err(std::string("field '") + std::string(field_name) +
                       "': value for key '" + it.key() + "' must be a string");
        }
        out.emplace(it.key(), expand_env_refs(it.value().get<std::string>()));
    }
    return out;
}

/// Parse a JSON string→string object into an unordered_map<string,string>
/// WITHOUT env expansion (used for stdio "env" field — those are raw values
/// passed as subprocess environment variables, not expanded at parse time).
Result<std::unordered_map<std::string, std::string>>
parse_string_map(const Json& obj, std::string_view field_name) {
    if (!obj.is_object()) {
        return Err(std::string("field '") + std::string(field_name) +
                   "' must be an object (string→string map)");
    }
    std::unordered_map<std::string, std::string> out;
    out.reserve(obj.size());
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it.value().is_string()) {
            return Err(std::string("field '") + std::string(field_name) +
                       "': value for key '" + it.key() + "' must be a string");
        }
        out.emplace(it.key(), it.value().get<std::string>());
    }
    return out;
}

/// Parse one mcpServers entry from its JSON value and server name.
///
/// Returns Err on schema violations (unknown transport, missing url for
/// remote, type errors). Per the task contract, callers treat this as a
/// non-fatal per-entry error.
Result<McpServerConfig>
parse_one_server(const Json& spec, const std::string& server_name) {
    if (!spec.is_object()) {
        return Err("mcpServers['" + server_name + "'] must be an object");
    }

    auto lg = batbox::log::get("config");

    // Warn on unknown keys (forward-compatibility)
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        if (kKnownMcpServerKeys.find(it.key()) == kKnownMcpServerKeys.end()) {
            SPDLOG_LOGGER_WARN(lg,
                "McpConfig: unknown field in mcpServers['{}'].{} — ignored",
                server_name, it.key());
        }
    }

    McpServerConfig out;
    out.name = server_name;

    // ------------------------------------------------------------------
    // Determine transport
    // ------------------------------------------------------------------
    bool is_remote = false;
    std::string transport_str;

    if (spec.contains("transport")) {
        const auto& t = spec["transport"];
        if (!t.is_string()) {
            return Err("mcpServers['" + server_name + "'].transport must be a string");
        }
        transport_str = t.get<std::string>();

        if (transport_str == "stdio") {
            is_remote = false;
        } else if (transport_str == "sse" || transport_str == "http" || transport_str == "ws") {
            is_remote = true;
        } else {
            return Err("mcpServers['" + server_name + "'].transport has unknown value '" +
                       transport_str + "'; expected stdio, sse, http, or ws");
        }
    } else {
        // No "transport" key → default to stdio
        is_remote = false;
        transport_str = "stdio";
    }

    // ------------------------------------------------------------------
    // Parse transport-specific fields
    // ------------------------------------------------------------------
    if (!is_remote) {
        // ---- stdio ----
        StdioConfig stdio_cfg;

        if (spec.contains("command")) {
            const auto& cmd = spec["command"];
            if (!cmd.is_string()) {
                return Err("mcpServers['" + server_name + "'].command must be a string");
            }
            stdio_cfg.command = cmd.get<std::string>();
        }
        // command may be absent — not required at parse time

        if (spec.contains("args")) {
            const auto& args_json = spec["args"];
            if (!args_json.is_array()) {
                return Err("mcpServers['" + server_name + "'].args must be an array");
            }
            for (const auto& a : args_json) {
                if (!a.is_string()) {
                    return Err("mcpServers['" + server_name + "'].args must be an array of strings");
                }
                stdio_cfg.args.push_back(a.get<std::string>());
            }
        }

        if (spec.contains("env")) {
            auto env_res = parse_string_map(spec["env"],
                "mcpServers['" + server_name + "'].env");
            if (!env_res) return Err(std::move(env_res.error()));
            stdio_cfg.env = std::move(env_res.value());
        }

        out.impl = std::move(stdio_cfg);

    } else {
        // ---- remote (sse | http | ws) ----

        // url is required for all remote transports
        if (!spec.contains("url")) {
            return Err("mcpServers['" + server_name + "'] with transport='" +
                       transport_str + "' requires a 'url' field");
        }
        const auto& url_json = spec["url"];
        if (!url_json.is_string()) {
            return Err("mcpServers['" + server_name + "'].url must be a string");
        }
        const std::string url = url_json.get<std::string>();

        // Parse optional headers with ${env:NAME} expansion
        std::unordered_map<std::string, std::string> headers;
        if (spec.contains("headers")) {
            auto hdr_res = parse_string_map_with_env(spec["headers"],
                "mcpServers['" + server_name + "'].headers");
            if (!hdr_res) return Err(std::move(hdr_res.error()));
            headers = std::move(hdr_res.value());
        }

        if (transport_str == "sse") {
            SseConfig sse_cfg;
            sse_cfg.url     = url;
            sse_cfg.headers = std::move(headers);
            out.impl = std::move(sse_cfg);
        } else if (transport_str == "http") {
            HttpConfig http_cfg;
            http_cfg.url     = url;
            http_cfg.headers = std::move(headers);
            out.impl = std::move(http_cfg);
        } else {
            // ws
            WsConfig ws_cfg;
            ws_cfg.url     = url;
            ws_cfg.headers = std::move(headers);
            out.impl = std::move(ws_cfg);
        }
    }

    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// load_mcp_config(path)
// ---------------------------------------------------------------------------

Result<std::vector<McpServerConfig>>
load_mcp_config(std::filesystem::path path) {
    auto lg = batbox::log::get("config");

    // Expand tilde in path
    path = batbox::paths::expand_tilde(path.string());

    // Open file
    std::ifstream ifs(path);
    if (!ifs) {
        return Err("McpConfig: cannot open '" + path.string() + "'");
    }

    // Read entire file
    std::ostringstream buf;
    buf << ifs.rdbuf();
    if (ifs.fail() && !ifs.eof()) {
        return Err("McpConfig: error reading '" + path.string() + "'");
    }

    // Parse JSON
    auto json_result = batbox::parse(buf.str());
    if (!json_result) {
        return Err("McpConfig: JSON parse error in '" + path.string() +
                   "': " + json_result.error());
    }

    const Json& doc = json_result.value();
    if (!doc.is_object()) {
        return Err("McpConfig: '" + path.string() +
                   "' must be a JSON object; got " +
                   std::string(doc.type_name()));
    }

    // "mcpServers" is optional — if absent, return empty list
    if (!doc.contains("mcpServers")) {
        return std::vector<McpServerConfig>{};
    }

    const Json& servers = doc["mcpServers"];
    if (!servers.is_object()) {
        return Err("McpConfig: '" + path.string() +
                   "' field 'mcpServers' must be a JSON object");
    }

    std::vector<McpServerConfig> result;
    result.reserve(servers.size());

    for (auto it = servers.begin(); it != servers.end(); ++it) {
        auto entry_result = parse_one_server(it.value(), it.key());
        if (!entry_result) {
            // Per-entry error is non-fatal: log and skip this entry
            SPDLOG_LOGGER_WARN(lg,
                "McpConfig: skipping mcpServers['{}'] in '{}': {}",
                it.key(), path.string(), entry_result.error());
            continue;
        }
        result.push_back(std::move(entry_result.value()));
    }

    SPDLOG_LOGGER_DEBUG(lg,
        "McpConfig: loaded {} server(s) from '{}'",
        result.size(), path.string());

    return result;
}

// ---------------------------------------------------------------------------
// load_mcp_configs()
// ---------------------------------------------------------------------------

std::vector<McpServerConfig>
load_mcp_configs() {
    auto lg = batbox::log::get("config");

    // Name set for collision detection (batbox wins over claude-compat)
    std::unordered_map<std::string, bool> seen_names;
    std::vector<McpServerConfig> merged;

    auto try_load = [&](std::filesystem::path path) {
        // Expand tilde first so the exists check works
        path = batbox::paths::expand_tilde(path.string());

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            // Missing file is silently skipped
            return;
        }

        auto res = load_mcp_config(path);
        if (!res) {
            SPDLOG_LOGGER_WARN(lg,
                "McpConfig: failed to load '{}': {} — skipping",
                path.string(), res.error());
            return;
        }

        for (auto& entry : res.value()) {
            if (seen_names.count(entry.name)) {
                SPDLOG_LOGGER_DEBUG(lg,
                    "McpConfig: duplicate server name '{}' from '{}' — earlier definition takes precedence",
                    entry.name, path.string());
                continue;
            }
            seen_names[entry.name] = true;
            merged.push_back(std::move(entry));
        }
    };

    // 1. ~/.batbox/mcp.json — primary (takes precedence)
    try_load("~/.batbox/mcp.json");

    // 2. ~/.claude/mcp.json — claude-code compat layer
    try_load("~/.claude/mcp.json");

    SPDLOG_LOGGER_DEBUG(lg,
        "McpConfig: total {} server(s) loaded from all config paths",
        merged.size());

    return merged;
}

} // namespace batbox::config
