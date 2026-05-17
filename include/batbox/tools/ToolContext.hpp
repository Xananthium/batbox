// include/batbox/tools/ToolContext.hpp
//
// batbox::tools::ToolContext — per-dispatch context injected into every
// ITool::run() call.
//
// Design rationale:
//   Tools must not reach for global state.  Everything they legitimately need
//   is passed through ToolContext: the working directory, the active permission
//   mode, identity tokens (session_id, agent_id), a cancellation handle, and
//   the optional allow-list that sub-agents use to restrict which tools they
//   may call.
//
// Fields:
//   cwd             — filesystem path; tools resolve relative file arguments
//                     against this, NOT against process cwd (which may be
//                     different in multi-agent scenarios).
//   mode            — PermissionMode for this dispatch (Default / Plan /
//                     AcceptEdits / Nuclear).  Tools inspect this to decide
//                     whether to skip prompts (Nuclear) or enforce read-only
//                     (Plan).
//   session_id      — opaque session identifier; tools may embed it in output
//                     for tracing / session store writes.
//   agent_id        — opaque agent identifier; empty string for the top-level
//                     conversation.  Sub-agents carry their own id.
//   cancel_token    — cooperative cancellation handle; tools poll
//                     cancel_token.is_cancelled() or call
//                     cancel_token.throw_if_cancelled() at checkpoints.
//   allowed_tools   — when present, the dispatch layer has already verified
//                     that the current tool's name is in this set; tools
//                     themselves need not re-check.  Absent for the top-level
//                     conversation (all tools allowed).
//
// Blueprint contract: batbox::tools::ToolContext (blueprints table row 16620)

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// ToolContext
// =============================================================================

struct ToolContext {
    // -------------------------------------------------------------------------
    // Data members — exactly as locked in the blueprint contract.
    // -------------------------------------------------------------------------

    /// Working directory for this dispatch; resolve relative paths against this.
    std::filesystem::path cwd;

    /// Active permission mode.  Determines whether destructive ops require
    /// confirmation (Default), are blocked entirely (Plan), auto-accepted
    /// (AcceptEdits for file ops, Nuclear for all ops).
    permissions::PermissionMode mode = permissions::PermissionMode::Default;

    /// Identifier for the current session (persisted to SessionStore).
    std::string session_id;

    /// Identifier for the dispatching agent.  Empty string for the root
    /// conversation; non-empty for sub-agents spawned via Task/TeamCreate.
    std::string agent_id;

    /// Cancellation handle.  Long-running tools MUST poll this at safe
    /// checkpoints (I/O boundaries, loop iterations) and abort promptly.
    CancelToken cancel_token;

    /// If present: the set of tool names this agent is permitted to call.
    /// The dispatch layer enforces the allow-list before calling run(); this
    /// field is informational for tools that compose other tools internally.
    std::optional<std::vector<std::string>> allowed_tools;

    // -------------------------------------------------------------------------
    // Convenience helpers
    // -------------------------------------------------------------------------

    /// Returns true when the permission mode is Plan — tools should refuse
    /// all write operations.
    [[nodiscard]] bool is_plan_mode() const noexcept {
        return mode == permissions::PermissionMode::Plan;
    }

    /// Returns true when the permission mode is Nuclear — tools may skip all
    /// confirmation prompts.
    [[nodiscard]] bool is_nuclear() const noexcept {
        return mode == permissions::PermissionMode::Nuclear;
    }

    /// Returns true if cancellation has been requested.
    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancel_token.is_cancelled();
    }

    /// Returns true if the given tool name appears in the allowed_tools list,
    /// or if allowed_tools is absent (meaning all tools are allowed).
    [[nodiscard]] bool tool_is_allowed(const std::string& tool_name) const {
        if (!allowed_tools.has_value()) return true;
        const auto& list = *allowed_tools;
        for (const auto& name : list) {
            if (name == tool_name) return true;
        }
        return false;
    }
};

} // namespace batbox::tools
