// include/batbox/permissions/PermissionMode.hpp
//
// batbox::permissions::PermissionMode — four runtime permission modes.
//
// Decision of Record #6 (pmdraft.md):
//   - default    : confirm before destructive actions
//   - plan       : read-only until plan is approved
//   - acceptEdits: auto-accept file edits, still confirm bash
//   - nuclear    : auto-accept everything — no prompts (☢️)
//
// Entry paths (both reach the same Nuclear state):
//   1. CLI flag  : batbox --nuclear
//   2. TUI picker: Shift+Tab or Ctrl+/ cycles through all four modes in-session
//
// The mode is sticky for the session but NOT persisted across restarts.
// Every new batbox invocation starts in Default unless --nuclear is passed.
//
// Nuclear triggers:
//   - magenta banner: "☢️ NUCLEAR MODE — ALL PERMISSIONS BYPASSED"
//   - persistent magenta status bar for the rest of the session

#pragma once

#include <batbox/core/Result.hpp>
#include <string_view>

namespace batbox::permissions {

// ---------------------------------------------------------------------------
// Enum
// ---------------------------------------------------------------------------

/// Four runtime permission modes (Decision of Record #6).
enum class PermissionMode {
    Default,      ///< Confirm before destructive actions (write/edit/bash with side effects)
    Plan,         ///< Read-only; no writes until plan is approved
    AcceptEdits,  ///< Auto-accept file edits but still confirm bash
    Nuclear,      ///< Auto-accept everything — no prompts (☢️ NUCLEAR)
};

// ---------------------------------------------------------------------------
// Print helper
// ---------------------------------------------------------------------------

/// Returns the canonical lowercase name for a mode.
/// Default → "default", Plan → "plan", AcceptEdits → "acceptedits",
/// Nuclear → "nuclear".
/// The returned string_view has static storage duration.
[[nodiscard]] std::string_view to_string(PermissionMode mode) noexcept;

// ---------------------------------------------------------------------------
// Parse helper
// ---------------------------------------------------------------------------

/// Parse a string_view into a PermissionMode.
///
/// Accepted canonical names (case-insensitive):
///   "default", "plan", "acceptedits", "nuclear"
///
/// Also accepted aliases:
///   "accept-edits", "accept_edits"            → AcceptEdits
///   "skip-permissions", "dangerously-skip-permissions",
///   "skip_permissions"                         → Nuclear  (deprecated aliases;
///                                                a deprecation note is logged
///                                                by the caller — this function
///                                                only signals via the ok value)
///
/// Returns an error Result if the input matches none of the above.
///
/// Blueprint contract name: mode_from_string
[[nodiscard]] batbox::Result<PermissionMode> mode_from_string(std::string_view s);

// ---------------------------------------------------------------------------
// TUI mode-picker cycle
// ---------------------------------------------------------------------------

/// Returns the next mode in the Shift+Tab / Ctrl+/ cycle:
///   Default → Plan → AcceptEdits → Nuclear → Default → ...
///
/// Blueprint contract name: cycle_next
[[nodiscard]] PermissionMode cycle_next(PermissionMode mode) noexcept;

// ---------------------------------------------------------------------------
// Banner helpers  (Nuclear-only)
// ---------------------------------------------------------------------------

/// Returns true only for Nuclear mode — the only mode that emits a warning
/// banner on activation.
[[nodiscard]] bool requires_banner(PermissionMode mode) noexcept;

/// Returns the banner text for Nuclear mode:
///   "☢️ NUCLEAR MODE — ALL PERMISSIONS BYPASSED"
/// Returns an empty string_view for all other modes.
/// The returned string_view has static storage duration.
[[nodiscard]] std::string_view banner_text(PermissionMode mode) noexcept;

} // namespace batbox::permissions
