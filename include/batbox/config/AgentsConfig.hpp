// include/batbox/config/AgentsConfig.hpp
// ---------------------------------------------------------------------------
// batbox::config::load_agents_config — parse ~/.batbox/agents.json.
//
// File format:
//   A flat JSON object mapping agent names to model-name override strings.
//   Example:
//     {
//       "verify": "gpt-4o-mini",
//       "debug": "o1-preview"
//     }
//
// Semantics:
//   Each key is an agent name; the corresponding value is the model identifier
//   that should be used instead of the global default when spawning that agent.
//
// Return value:
//   Result<AgentModelMap, std::string>
//     - Ok(map)  — parsed map; empty map when the file is absent (not an error).
//     - Err(msg) — non-empty error message when the file exists but cannot be
//                  read, is not valid JSON, or does not contain a top-level
//                  object.  Individual entries whose value is not a string are
//                  skipped with a warning to stderr.
//
// Thread safety: stateless; safe to call from multiple threads concurrently.
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/core/Result.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace batbox::config {

/// Map type returned by load_agents_config(): agent name → model override.
using AgentModelMap = std::unordered_map<std::string, std::string>;

// ---------------------------------------------------------------------------
// load_agents_config()
//
// Reads and parses the file at 'path' (typically ~/.batbox/agents.json).
//
// Missing file: returns Ok(empty map) — the caller treats it as "no overrides".
// Unreadable or non-JSON file: returns Err with a descriptive message.
// Non-object top level: returns Err("agents.json: expected a JSON object ...").
// String value entries are stored in the returned map.
// Non-string value entries are skipped with a stderr warning.
// ---------------------------------------------------------------------------
[[nodiscard]]
batbox::Result<AgentModelMap, std::string>
load_agents_config(std::filesystem::path path);

} // namespace batbox::config
