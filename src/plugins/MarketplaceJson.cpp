// src/plugins/MarketplaceJson.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::plugins::parse_marketplace_json and
// batbox::plugins::find_marketplace_in_dir.
//
// See include/batbox/plugins/MarketplaceJson.hpp for the full API contract.
// ---------------------------------------------------------------------------

#include <batbox/plugins/MarketplaceJson.hpp>
#include <batbox/core/Logging.hpp>

#include <fstream>
#include <sstream>
#include <set>

namespace batbox::plugins {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Known top-level keys in a marketplace.json.  Anything outside this set
/// triggers a WARN log and is silently ignored (forward-compatibility).
const std::set<std::string> kKnownTopLevelKeys = {
    "$schema", "name", "version", "description",
    "skills", "agents", "commands", "mcpServers",
    // Claude-code marketplace.json also has these optional fields
    "owner", "author", "plugins", "metadata", "category", "source",
    "homepage",
};

/// Known keys inside a single mcpServers entry.
const std::set<std::string> kKnownMcpServerKeys = {
    "command", "args", "env", "transport", "url", "headers",
};

/// Parse a JSON string array into a vector<fs::path>.
/// Returns Err if the value is not an array or any element is not a string.
/// Field name is passed for error message clarity.
Result<std::vector<fs::path>>
parse_path_array(const Json& arr, std::string_view field_name) {
    if (!arr.is_array()) {
        return Err(std::string("field '") + std::string(field_name) +
                   "' must be an array of strings");
    }
    std::vector<fs::path> out;
    out.reserve(arr.size());
    for (const auto& elem : arr) {
        if (!elem.is_string()) {
            return Err(std::string("field '") + std::string(field_name) +
                       "' must be an array of strings; got non-string element");
        }
        out.emplace_back(elem.get<std::string>());
    }
    return out;
}

/// Parse a JSON string→string object into an unordered_map<string,string>.
/// Returns Err if `obj` is not an object or any value is not a string.
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

/// Parse one mcpServers entry (the value, not the key).
Result<McpServerSpec>
parse_mcp_server_spec(const Json& spec, std::string_view server_name) {
    if (!spec.is_object()) {
        return Err(std::string("mcpServers['") + std::string(server_name) +
                   "'] must be an object");
    }

    // Warn on unknown keys
    auto lg = batbox::log::get("plugins");
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        if (kKnownMcpServerKeys.find(it.key()) == kKnownMcpServerKeys.end()) {
            SPDLOG_LOGGER_WARN(lg,
                "MarketplaceJson: unknown field in mcpServers['{}'].{} — ignored",
                server_name, it.key());
        }
    }

    McpServerSpec out;

    // Determine transport
    if (spec.contains("transport")) {
        const auto& t = spec["transport"];
        if (!t.is_string()) {
            return Err(std::string("mcpServers['") + std::string(server_name) +
                       "'].transport must be a string");
        }
        const std::string tstr = t.get<std::string>();
        if (tstr == "sse") {
            out.transport = McpTransport::Sse;
        } else if (tstr == "http") {
            out.transport = McpTransport::Http;
        } else if (tstr == "ws") {
            out.transport = McpTransport::Ws;
        } else {
            return Err(std::string("mcpServers['") + std::string(server_name) +
                       "'].transport has unknown value '" + tstr +
                       "'; expected sse, http, or ws");
        }
    } else {
        out.transport = McpTransport::Stdio;
    }

    if (out.transport == McpTransport::Stdio) {
        // command is optional for stdio (could be configured elsewhere)
        if (spec.contains("command")) {
            const auto& cmd = spec["command"];
            if (!cmd.is_string()) {
                return Err(std::string("mcpServers['") + std::string(server_name) +
                           "'].command must be a string");
            }
            out.command = cmd.get<std::string>();
        }

        if (spec.contains("args")) {
            const auto& args_json = spec["args"];
            if (!args_json.is_array()) {
                return Err(std::string("mcpServers['") + std::string(server_name) +
                           "'].args must be an array");
            }
            for (const auto& a : args_json) {
                if (!a.is_string()) {
                    return Err(std::string("mcpServers['") + std::string(server_name) +
                               "'].args must be an array of strings");
                }
                out.args.push_back(a.get<std::string>());
            }
        }

        if (spec.contains("env")) {
            auto env_res = parse_string_map(spec["env"],
                std::string("mcpServers['") + std::string(server_name) + "'].env");
            if (!env_res) return Err(std::move(env_res.error()));
            out.env = std::move(env_res.value());
        }
    } else {
        // Remote transport: url required, headers optional
        if (!spec.contains("url")) {
            return Err(std::string("mcpServers['") + std::string(server_name) +
                       "'] with transport='" +
                       (out.transport == McpTransport::Sse ? "sse" :
                        out.transport == McpTransport::Http ? "http" : "ws") +
                       "' requires a 'url' field");
        }
        const auto& url = spec["url"];
        if (!url.is_string()) {
            return Err(std::string("mcpServers['") + std::string(server_name) +
                       "'].url must be a string");
        }
        out.url = url.get<std::string>();

        if (spec.contains("headers")) {
            auto hdr_res = parse_string_map(spec["headers"],
                std::string("mcpServers['") + std::string(server_name) + "'].headers");
            if (!hdr_res) return Err(std::move(hdr_res.error()));
            out.headers = std::move(hdr_res.value());
        }
    }

    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// parse_marketplace_json(const Json&)
// ---------------------------------------------------------------------------

Result<Marketplace>
parse_marketplace_json(const Json& j) {
    auto lg = batbox::log::get("plugins");

    if (!j.is_object()) {
        return Err(std::string("marketplace.json must be a JSON object; got ") +
                   std::string(j.type_name()));
    }

    // Warn on unknown top-level keys (forward-compatibility)
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (kKnownTopLevelKeys.find(it.key()) == kKnownTopLevelKeys.end()) {
            SPDLOG_LOGGER_WARN(lg,
                "MarketplaceJson: unknown top-level field '{}' — ignored",
                it.key());
        }
    }

    Marketplace m;

    // --- name (REQUIRED) ---
    if (!j.contains("name") || !j["name"].is_string()) {
        if (!j.contains("name")) {
            return Err(std::string("marketplace.json is missing required field 'name'"));
        }
        return Err(std::string("marketplace.json field 'name' must be a string"));
    }
    m.name = j["name"].get<std::string>();
    if (m.name.empty()) {
        return Err(std::string("marketplace.json field 'name' must not be empty"));
    }

    // --- version (optional) ---
    if (j.contains("version")) {
        const auto& v = j["version"];
        if (!v.is_string()) {
            return Err(std::string("marketplace.json field 'version' must be a string"));
        }
        m.version = v.get<std::string>();
    }

    // --- description (optional) ---
    if (j.contains("description")) {
        const auto& d = j["description"];
        if (!d.is_string()) {
            return Err(std::string("marketplace.json field 'description' must be a string"));
        }
        m.description = d.get<std::string>();
    }

    // --- skills (optional string array) ---
    if (j.contains("skills")) {
        auto r = parse_path_array(j["skills"], "skills");
        if (!r) return Err(std::move(r.error()));
        m.skills = std::move(r.value());
    }

    // --- agents (optional string array) ---
    if (j.contains("agents")) {
        auto r = parse_path_array(j["agents"], "agents");
        if (!r) return Err(std::move(r.error()));
        m.agents = std::move(r.value());
    }

    // --- commands (optional string array) ---
    if (j.contains("commands")) {
        auto r = parse_path_array(j["commands"], "commands");
        if (!r) return Err(std::move(r.error()));
        m.commands = std::move(r.value());
    }

    // --- mcpServers (optional object) ---
    if (j.contains("mcpServers")) {
        const auto& servers = j["mcpServers"];
        if (!servers.is_object()) {
            return Err(std::string("marketplace.json field 'mcpServers' must be an object"));
        }
        m.mcp_servers.reserve(servers.size());
        for (auto it = servers.begin(); it != servers.end(); ++it) {
            auto r = parse_mcp_server_spec(it.value(), it.key());
            if (!r) return Err(std::move(r.error()));
            m.mcp_servers.emplace(it.key(), std::move(r.value()));
        }
    }

    return m;
}

// ---------------------------------------------------------------------------
// parse_marketplace_json(const fs::path&)
// ---------------------------------------------------------------------------

Result<Marketplace>
parse_marketplace_json(const fs::path& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        return Err(std::string("cannot open marketplace.json: ") + path.string());
    }

    std::ostringstream buf;
    buf << ifs.rdbuf();
    if (ifs.fail() && !ifs.eof()) {
        return Err(std::string("error reading marketplace.json: ") + path.string());
    }

    auto json_result = batbox::parse(buf.str());
    if (!json_result) {
        return Err(std::string("JSON parse error in ") + path.string() +
                   ": " + json_result.error());
    }

    return parse_marketplace_json(json_result.value());
}

// ---------------------------------------------------------------------------
// find_marketplace_in_dir
// ---------------------------------------------------------------------------

std::optional<fs::path>
find_marketplace_in_dir(const fs::path& dir) {
    // Check .claude-plugin/marketplace.json first (upstream compat)
    {
        fs::path candidate = dir / ".claude-plugin" / "marketplace.json";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
    }

    // Check .batbox-plugin/marketplace.json next (BatBox-native)
    {
        fs::path candidate = dir / ".batbox-plugin" / "marketplace.json";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
    }

    return std::nullopt;
}

} // namespace batbox::plugins
