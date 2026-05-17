// include/batbox/permissions/PermissionGate.hpp
// ---------------------------------------------------------------------------
// batbox::permissions::PermissionGate — decision-flow engine for tool dispatch.
//
// This is the central gate that every tool call passes through before execution.
// It honours the four permission modes and the allow/deny/ask rule store, then
// falls back to an injected prompt_user_ callback for interactive decisions.
//
// Decision flow (ned-cpp.md §2.C12):
//   1. Nuclear mode → Allow (bypass everything)
//   2. Plan mode + non-read-only tool → Deny("plan mode: read-only")
//   3. AcceptEdits mode + Edit or Write tool → Allow
//   4. Deny rules match → Deny
//   5. Allow rules match → Allow
//   6. BATBOX_AUTO_APPROVE_READS=true + read-only tool → Allow
//   7. Default → invoke prompt_user_ callback
//
// Thread safety:
//   set_mode() and current_mode() are protected by a mutex; ask() is also
//   mutex-guarded so that mode and rule reads are consistent.  The
//   prompt_user_ callback is invoked outside the mutex — callers must
//   ensure the callback itself is thread-safe or that ask() is only called
//   from a single thread (typical: the conversation dispatch loop).
//
// Blueprint contract: batbox::permissions::PermissionGate (task CPP 12.4)
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionRule.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/permissions/PatternMatcher.hpp>
#include <batbox/core/Json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace batbox::tools { struct ToolContext; }

namespace batbox::permissions {

// ===========================================================================
// Decision — result of PermissionGate::ask()
// ===========================================================================

/// The outcome of a PermissionGate::ask() call.
///
/// `kind`         — Allow or Deny.
/// `persist_rule` — when present, the caller MUST add this rule to the store
///                  (the user chose "always allow" or "always deny").
/// `edit_text`    — when present, the user edited the args and the tool should
///                  retry with this updated argument string (future use).
struct Decision {
    /// Discriminant: whether the tool is permitted to run.
    enum class Kind { Allow, Deny };

    Kind kind = Kind::Deny;

    /// When the user selected "always allow" or "always deny", the gate
    /// populates this field and the caller must persist it via PermissionStore.
    std::optional<PermissionRule> persist_rule;

    /// Future: populated when the user chose "edit args" — carries the
    /// user-modified argument blob for the tool to retry with.
    std::optional<std::string> edit_text;

    // ---- Convenience factories ----------------------------------------------

    /// Construct a simple one-shot Allow decision.
    [[nodiscard]] static Decision allow() {
        Decision d;
        d.kind = Kind::Allow;
        return d;
    }

    /// Construct a simple one-shot Deny decision.
    [[nodiscard]] static Decision deny() {
        Decision d;
        d.kind = Kind::Deny;
        return d;
    }

    /// Construct an Allow decision that should also persist a new allow rule.
    [[nodiscard]] static Decision allow_with_rule(std::string_view pattern) {
        Decision d;
        d.kind         = Kind::Allow;
        d.persist_rule = PermissionRule{std::string(pattern), PermissionRule::Kind::Allow};
        return d;
    }

    /// Construct a Deny decision that should also persist a new deny rule.
    [[nodiscard]] static Decision deny_with_rule(std::string_view pattern) {
        Decision d;
        d.kind         = Kind::Deny;
        d.persist_rule = PermissionRule{std::string(pattern), PermissionRule::Kind::Deny};
        return d;
    }
};

// ===========================================================================
// PermissionGate
// ===========================================================================

/// Central permission gate for tool dispatch.
///
/// Constructed with:
///   - A PermissionStore (owns the allow/deny/ask rule lists on disk).
///   - An initial PermissionMode (may be updated at runtime via set_mode()).
///   - A PromptFn callback provided by the TUI (or a test mock).
///
/// The TUI wires in the actual ModalPicker callback.  Tests inject a
/// lambda that immediately returns Allow/Deny to avoid any UI interaction.
class PermissionGate {
public:
    // -------------------------------------------------------------------------
    // PromptFn — callback type injected by the TUI
    // -------------------------------------------------------------------------

    /// Signature of the interactive-prompt callback.
    ///
    /// @param tool_name  The name of the tool requesting permission.
    /// @param args       The JSON arguments passed to the tool.
    ///
    /// @returns A Decision.  The callback is responsible for:
    ///   - Returning Decision::allow()            for one-shot allow.
    ///   - Returning Decision::deny()             for one-shot deny.
    ///   - Returning Decision::allow_with_rule()  for "always allow".
    ///   - Returning Decision::deny_with_rule()   for "always deny".
    ///
    /// The callback MUST NOT throw.  Return Decision::deny() on error.
    using PromptFn = std::function<Decision(std::string_view tool_name,
                                            const batbox::Json& args)>;

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with an existing PermissionStore, an initial mode, and a
    /// prompt callback.
    ///
    /// @param store      Shared ownership of the rule store.
    /// @param mode       Initial permission mode (e.g. loaded from CLI flag).
    /// @param prompt_fn  TUI/test callback; called when no rule matches.
    PermissionGate(std::shared_ptr<PermissionStore> store,
                   PermissionMode                   mode,
                   PromptFn                         prompt_fn);

    // -------------------------------------------------------------------------
    // Core API — blueprint contract
    // -------------------------------------------------------------------------

    /// Evaluate the permission decision for a tool invocation.
    ///
    /// Decision flow:
    ///   1. Nuclear mode → Allow (short-circuit, no rules checked)
    ///   2. Plan mode + non-read-only tool → Deny("plan mode: read-only")
    ///   3. AcceptEdits mode + Edit/Write tool → Allow
    ///   4. Check deny rules → Deny on first match
    ///   5. Check allow rules → Allow on first match
    ///   6. BATBOX_AUTO_APPROVE_READS + read-only tool → Allow
    ///   7. Invoke prompt_user_ callback
    ///
    /// After step 7, if the returned Decision carries a persist_rule, the gate
    /// automatically persists it to the PermissionStore before returning.
    ///
    /// Blueprint contract name: ask
    [[nodiscard]] Decision ask(std::string_view              tool_name,
                               const batbox::Json&           args,
                               const batbox::tools::ToolContext& ctx);

    /// Update the current permission mode (thread-safe).
    ///
    /// Blueprint contract name: set_mode
    void set_mode(PermissionMode mode);

    /// Return the current permission mode atomically.
    ///
    /// Blueprint contract name: current_mode
    [[nodiscard]] PermissionMode current_mode() const;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /// Return a reference to the underlying PermissionStore.
    [[nodiscard]] PermissionStore& store() noexcept { return *store_; }
    [[nodiscard]] const PermissionStore& store() const noexcept { return *store_; }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Returns true when `tool_name` is in the set of write tools that
    /// AcceptEdits mode auto-approves: {"Edit", "Write", "MultiEdit"}.
    [[nodiscard]] static bool is_accept_edits_tool(std::string_view tool_name) noexcept;

    /// Returns true when `tool_name` corresponds to a read-only tool.
    /// Used for BATBOX_AUTO_APPROVE_READS handling.
    /// The set is: {"Read", "Glob", "Grep", "CtxInspect", "ToolSearch"}.
    [[nodiscard]] static bool is_read_only_tool(std::string_view tool_name) noexcept;

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------

    std::shared_ptr<PermissionStore> store_;
    PermissionMode                   mode_;
    PromptFn                         prompt_fn_;
    mutable std::mutex               mutex_;
};

} // namespace batbox::permissions
