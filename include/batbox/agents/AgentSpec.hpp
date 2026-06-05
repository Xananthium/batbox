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
// EndpointOverride — optional per-agent inference-endpoint override (AC1, DIS-988)
// =============================================================================

/// Optional override that points a SubAgent's inference Client at a NON-`cfg.api`
/// endpoint.
///
/// Motivation (DIS-988 / S2+S3): a *standing* subagent is meant to run on the
/// local 3090 (free compute, like the S1 distiller) rather than the single,
/// usually-cloud `cfg.api` endpoint.  `AgentSpec::model` can only override the
/// model *name*; it cannot move the *endpoint*.  This struct adds that.
///
/// When an AgentSpec carries `std::nullopt` here (the default), the SubAgent
/// targets `cfg.api` exactly as before — existing behaviour and tests are
/// unchanged.  When set, `SubAgent::run()` overrides the per-agent Config's
/// `api.*` fields the same way `tools::SubagentDistiller` does (DIS-980),
/// then constructs the Client via the S8 `inference::ProviderRegistry`.
struct EndpointOverride {
    /// Convenience selector: when true, ignore the explicit fields below and
    /// pull the endpoint from `cfg.distill.*` (the same local 3090 endpoint the
    /// S1 distiller uses).  This is the "run on the local distill endpoint"
    /// shortcut the AC1 spec calls out.
    bool use_distill_endpoint = false;

    /// Explicit endpoint base URL, e.g. "http://127.0.0.1:11434/v1".
    /// Used only when `use_distill_endpoint == false`.  Empty → leave
    /// `cfg.api.base_url` unchanged.
    std::string base_url;

    /// API key for the explicit endpoint (usually empty or "ollama" locally).
    /// Used only when `use_distill_endpoint == false`.
    std::string api_key;

    /// Model name on the explicit endpoint.  Used only when
    /// `use_distill_endpoint == false`.  Empty → leave the resolved
    /// `cfg.api.default_model` unchanged.
    std::string model;
};

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

    /// Optional inference-endpoint override (AC1, DIS-988).  nullopt (default)
    /// → target `cfg.api` unchanged.  Set programmatically by the standing-mode
    /// promote path so a warm subagent can run on the local 3090; not parsed
    /// from frontmatter (the selection of *which* agent goes standing is the
    /// step-8 heuristic, not a static .md property).
    std::optional<EndpointOverride> endpoint;

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
