// include/batbox/agents/AgentSpec.hpp
//
// batbox::agents::AgentSpec — frontmatter-driven agent configuration.
//
// Loaded from ~/.batbox/agents/<name>.md YAML frontmatter by AgentSpec.cpp.
// Used by AgentSupervisor::spawn() to configure a new SubAgent.
//
// Blueprint contract: batbox::agents::AgentSpec (blueprints row 16753)
// Full implementation: CPP 6.2

#pragma once

#include <batbox/core/Result.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace batbox::agents {

// =============================================================================
// AgentSpec — agent configuration loaded from .md frontmatter
// =============================================================================

/// Configuration record for a named agent.
///
/// Loaded by AgentSpec::from_file(path) from a markdown file whose YAML
/// frontmatter block provides the metadata fields; the remainder of the file
/// provides the initial system prompt body.
///
/// YAML frontmatter keys recognised:
///   name          (required) — display name, e.g. "senior-dev"
///   description   (optional) — one-sentence description
///   model         (optional) — model override, e.g. "claude-opus-4-5"
///   tools         (optional) — flow or block list of allowed tool names
///
/// [[ref]] tokens in prompt_body are detected at parse time for cycle
/// detection; callers can scan prompt_body for the pattern \[\[.+\]\].
///
/// Blueprint contract: batbox::agents::AgentSpec (blueprints row 16753)
struct AgentSpec {
    // -------------------------------------------------------------------------
    // Fields (matching the blueprint signature exactly)
    // -------------------------------------------------------------------------

    /// Display name of the agent (e.g. "senior-dev", "junior-dev").
    std::string name;

    /// One-sentence description surfaced in the TUI sub-agent panel.
    std::string description;

    /// Specific model override (e.g. "claude-opus-4-5"); nullopt → use global default.
    std::optional<std::string> model;

    /// Allowlisted tool names for this agent.  Empty → use parent's allowed set.
    std::vector<std::string> allowed_tools;

    /// System prompt body (the non-frontmatter portion of the .md file).
    /// May contain [[AgentName]] reference tokens detected at parse time.
    std::string prompt_body;

    /// Filesystem path to the source .md file (for error messages and reload).
    std::filesystem::path source_path;

    // -------------------------------------------------------------------------
    // Factories
    // -------------------------------------------------------------------------

    /// Parse an AgentSpec from a markdown file with YAML frontmatter.
    ///
    /// The file must have a YAML frontmatter block containing at least a
    /// "name" key.  The remainder of the file becomes prompt_body.
    ///
    /// Returns Err with a human-readable message when:
    ///   - The file cannot be read
    ///   - The frontmatter is malformed
    ///   - The required "name" field is absent
    ///
    /// @param path  Absolute or relative path to the .md agent file.
    [[nodiscard]] static Result<AgentSpec>
    from_file(const std::filesystem::path& path);

    /// Construct a minimal AgentSpec from a subagent_type string.
    ///
    /// Attempts to load ~/.batbox/agents/<subagent_type>.md via from_file().
    /// Falls back to a generic spec when the file does not exist so that the
    /// Task tool can always produce a valid AgentSpec.
    ///
    /// @param subagent_type  The "subagent_type" argument from the Task tool call.
    [[nodiscard]] static AgentSpec from_type(std::string_view subagent_type);
};

} // namespace batbox::agents
