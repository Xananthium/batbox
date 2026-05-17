// src/config/AgentsConfig.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::load_agents_config.
//
// Reads ~/.batbox/agents.json (or whatever path is supplied by the caller).
// The file is a flat JSON object: { "agent_name": "model_name", ... }.
// Missing file → empty map (not an error).
// Malformed JSON / non-object → Err with a descriptive message.
// Non-string values are skipped with a stderr warning; parsing continues.
// ---------------------------------------------------------------------------

#include <batbox/config/AgentsConfig.hpp>
#include <batbox/core/Json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace batbox::config {

[[nodiscard]]
batbox::Result<AgentModelMap, std::string>
load_agents_config(std::filesystem::path path) {
    // Missing file is not an error — treat as "no per-agent overrides".
    if (!std::filesystem::exists(path)) {
        return AgentModelMap{};
    }

    // Attempt to open and read the file.
    std::ifstream file(path);
    if (!file.is_open()) {
        std::string msg = "AgentsConfig: cannot open '";
        msg += path.string();
        msg += "': ";
        msg += std::strerror(errno);
        return batbox::Err(std::move(msg));
    }

    std::ostringstream buf;
    buf << file.rdbuf();
    const std::string raw = buf.str();

    // Parse JSON using nlohmann via batbox::parse().
    auto parse_result = batbox::parse(raw);
    if (!parse_result.has_value()) {
        std::string msg = "AgentsConfig: '";
        msg += path.string();
        msg += "': JSON parse error: ";
        msg += parse_result.error();
        return batbox::Err(std::move(msg));
    }

    const batbox::Json& root = parse_result.value();

    // Top-level must be a JSON object.
    if (!root.is_object()) {
        std::string msg = "AgentsConfig: '";
        msg += path.string();
        msg += "': expected a JSON object at the top level, got ";
        msg += root.type_name();
        return batbox::Err(std::move(msg));
    }

    AgentModelMap result;
    result.reserve(root.size());

    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().is_string()) {
            // Non-string value: warn and skip; do not fail the whole load.
            std::cerr << "AgentsConfig: '" << path.string()
                      << "': entry '" << it.key()
                      << "' has a non-string value ("
                      << it.value().type_name()
                      << ") — skipping\n";
            continue;
        }
        result.emplace(it.key(), it.value().template get<std::string>());
    }

    return result;
}

} // namespace batbox::config
