// src/permissions/PermissionGate.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::permissions::PermissionGate.
// See include/batbox/permissions/PermissionGate.hpp for the full contract.
// ---------------------------------------------------------------------------

#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/tools/ToolContext.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>          // std::getenv
#include <string_view>

namespace batbox::permissions {

// ===========================================================================
// Construction
// ===========================================================================

PermissionGate::PermissionGate(std::shared_ptr<PermissionStore> store,
                                PermissionMode                   mode,
                                PromptFn                         prompt_fn)
    : store_(std::move(store))
    , mode_(mode)
    , prompt_fn_(std::move(prompt_fn))
{
}

// ===========================================================================
// set_mode / current_mode
// ===========================================================================

void PermissionGate::set_mode(PermissionMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
}

PermissionMode PermissionGate::current_mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

// ===========================================================================
// Internal helpers
// ===========================================================================

bool PermissionGate::is_accept_edits_tool(std::string_view tool_name) noexcept {
    // The set of tools that AcceptEdits mode auto-approves (file-edit tools).
    static constexpr std::array<std::string_view, 3> kEditTools = {
        "Edit", "Write", "MultiEdit"
    };
    for (auto t : kEditTools) {
        if (tool_name == t) return true;
    }
    return false;
}

bool PermissionGate::is_read_only_tool(std::string_view tool_name) noexcept {
    // Tools where is_read_only() == true — used for BATBOX_AUTO_APPROVE_READS.
    static constexpr std::array<std::string_view, 5> kReadOnlyTools = {
        "Read", "Glob", "Grep", "CtxInspect", "ToolSearch"
    };
    for (auto t : kReadOnlyTools) {
        if (tool_name == t) return true;
    }
    return false;
}

// ===========================================================================
// ask — the decision-flow engine
// ===========================================================================

Decision PermissionGate::ask(std::string_view                      tool_name,
                              const batbox::Json&                    args,
                              const batbox::tools::ToolContext&      ctx)
{
    // Snapshot mode and rules under the lock so we have a consistent view.
    PermissionMode mode_snapshot;
    std::vector<std::string> deny_rules_snapshot;
    std::vector<std::string> allow_rules_snapshot;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_snapshot        = mode_;
        // Also incorporate the mode carried in ctx — ctx.mode reflects the
        // per-dispatch mode which may differ from the session-level mode_ when
        // sub-agents run with a constrained mode.  We use ctx.mode as the
        // authoritative mode for this call, matching the blueprint contract:
        //   ask(tool_name, args, ctx) uses ctx.mode for gate decisions.
        //
        // NOTE: mode_ (the stored field) is the session-level mode updated by
        // set_mode().  ctx.mode is the per-dispatch mode supplied by the
        // conversation engine.  For the decision logic we trust ctx.mode,
        // which is consistent with how ToolContext.is_plan_mode() /
        // is_nuclear() work in the individual tool implementations.
        mode_snapshot        = ctx.mode;
        deny_rules_snapshot  = store_->deny_rules();
        allow_rules_snapshot = store_->allow_rules();
    }

    // -----------------------------------------------------------------------
    // Step 1: Nuclear mode → Allow unconditionally.
    // -----------------------------------------------------------------------
    if (mode_snapshot == PermissionMode::Nuclear) {
        return Decision::allow();
    }

    // -----------------------------------------------------------------------
    // Step 2: Plan mode + non-read-only tool → Deny.
    //
    // Read-only tools (Read, Glob, Grep, ToolSearch, CtxInspect, …) are
    // permitted even in Plan mode; everything else is blocked.
    // We use is_read_only_tool() for the built-in set.  For tools not in the
    // known read-only set we conservatively treat them as non-read-only.
    // -----------------------------------------------------------------------
    if (mode_snapshot == PermissionMode::Plan) {
        if (!is_read_only_tool(tool_name)) {
            return Decision::deny();
        }
        // Read-only tools pass through in Plan mode — continue to rule checks.
    }

    // -----------------------------------------------------------------------
    // Step 3: AcceptEdits mode + Edit/Write/MultiEdit → Allow.
    // -----------------------------------------------------------------------
    if (mode_snapshot == PermissionMode::AcceptEdits) {
        if (is_accept_edits_tool(tool_name)) {
            return Decision::allow();
        }
        // Other tools in AcceptEdits mode still go through rule checks and
        // may end up prompting.
    }

    // -----------------------------------------------------------------------
    // Step 4: Deny rules (highest priority rule list) — first match wins.
    // -----------------------------------------------------------------------
    for (const auto& rule : deny_rules_snapshot) {
        if (matches(rule, tool_name, args)) {
            return Decision::deny();
        }
    }

    // -----------------------------------------------------------------------
    // Step 5: Allow rules — first match wins.
    // -----------------------------------------------------------------------
    for (const auto& rule : allow_rules_snapshot) {
        if (matches(rule, tool_name, args)) {
            return Decision::allow();
        }
    }

    // -----------------------------------------------------------------------
    // Step 6: BATBOX_AUTO_APPROVE_READS — env-var shortcut for read-only tools.
    // -----------------------------------------------------------------------
    {
        const char* auto_reads = std::getenv("BATBOX_AUTO_APPROVE_READS");
        if (auto_reads != nullptr) {
            std::string_view val(auto_reads);
            if (val == "1" || val == "true" || val == "yes") {
                if (is_read_only_tool(tool_name)) {
                    return Decision::allow();
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Step 7: No rule matched — invoke the interactive prompt callback.
    //
    // The prompt is called outside the mutex to avoid deadlock if the callback
    // re-enters the gate (e.g. to add a persist rule).
    // -----------------------------------------------------------------------
    if (prompt_fn_) {
        Decision d = prompt_fn_(tool_name, args);

        // If the user chose "always allow" or "always deny", persist the rule.
        if (d.persist_rule.has_value()) {
            const PermissionRule& rule = *d.persist_rule;
            if (rule.kind == PermissionRule::Kind::Allow) {
                // Ignore errors — rule persistence is best-effort; the
                // in-flight allow/deny decision is still returned to the caller.
                (void)store_->add_allow_rule(rule.pattern);
            } else if (rule.kind == PermissionRule::Kind::Deny) {
                (void)store_->add_deny_rule(rule.pattern);
            }
            // PermissionRule::Kind::Ask is unusual here but benign — no persist.
        }

        return d;
    }

    // No callback wired (e.g. headless / test with null callback).
    // Default-deny is the safe fallback — never silently allow unknown tools.
    return Decision::deny();
}

} // namespace batbox::permissions
