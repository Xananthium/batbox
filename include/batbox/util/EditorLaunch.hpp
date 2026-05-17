// include/batbox/util/EditorLaunch.hpp
// ---------------------------------------------------------------------------
// batbox::util::resolve_editor() — TUI-FIX-T7
//
// Resolves which editor binary to use, honouring $EDITOR but falling back
// to nano (then pico, then vi) when $EDITOR is unset or points at vi/vim.
//
// Priority:
//   1. $EDITOR — if set and not "vi" or "vim" (basename comparison), use it.
//   2. nano    — if accessible via $PATH.
//   3. pico    — if accessible via $PATH.
//   4. vi      — unconditional last resort.
//
// batbox::util::edit_string_in_editor() — higher-level helper:
//   Writes `text` to a temp file, suspends the FTXUI screen (if provided),
//   execs the resolved editor, waits for exit, reads the file back, and
//   returns the edited string.  On any error the original `text` is returned
//   unchanged.
//
// Thread-safety: resolve_editor() reads only process-level env vars and PATH —
//   safe to call from any thread before exec.  edit_string_in_editor() must be
//   called from the UI thread (it drives the FTXUI suspend/resume cycle).
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>

// Forward-declare ScreenInteractive to avoid pulling in all of FTXUI here.
namespace ftxui { class ScreenInteractive; }

namespace batbox::util {

/// Resolve which editor binary to exec.
///
/// Resolution order (first match wins):
///   1. $EDITOR — if non-empty and basename is not "vi" or "vim".
///   2. "nano"  — if found in $PATH (via binary_accessible()).
///   3. "pico"  — if found in $PATH.
///   4. "vi"    — unconditional fallback.
///
/// @returns  The binary name (or full path if $EDITOR was a full path) to
///           pass to exec / std::system.
[[nodiscard]]
std::string resolve_editor();

/// Check whether `binary` is accessible (executable) anywhere in $PATH.
///
/// Uses access(2) / _access on the individual PATH entries rather than
/// spawning a subshell — fast, no side effects.
///
/// @param binary  A bare binary name (no path separators).
/// @returns       true if at least one $PATH entry contains an executable
///                file named `binary`.
[[nodiscard]]
bool binary_accessible(const std::string& binary);

/// Open `text` in the resolved editor and return the edited content.
///
/// Steps:
///   1. Write `text` to a uniquely-named temp file (mkstemp).
///   2. If `screen` is non-null, run editor via screen->WithRestoredIO() so
///      FTXUI suspends its raw-mode terminal handling while the editor runs.
///      If `screen` is null, exec the editor directly via std::system().
///   3. Wait for the editor process to exit.
///   4. Read the temp file back.
///   5. Unlink the temp file.
///   6. Return the edited content, or the original `text` on any error.
///
/// @param text    Content to pre-populate the editor with.
/// @param screen  Pointer to the live ScreenInteractive (may be null for
///                headless / test contexts).
/// @returns       Edited text (may equal `text` if the user made no changes
///                or if an error occurred).
[[nodiscard]]
std::string edit_string_in_editor(const std::string& text,
                                  ftxui::ScreenInteractive* screen = nullptr);

} // namespace batbox::util
