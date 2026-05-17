// include/batbox/config/KeybindingsConfig.hpp
// ---------------------------------------------------------------------------
// batbox::config::load_keybindings — parse ~/.batbox/keybindings.json
// (or any caller-supplied path) and merge with built-in defaults.
//
// File format:
//   A flat JSON object mapping action names to key descriptors.
//   Example:
//     {
//       "send":        "Ctrl+Enter",
//       "cancel":      "Ctrl+C",
//       "cycle_mode":  "Shift+Tab"
//     }
//
// Action vocabulary (all recognised action names):
//   "send"        — submit the current input buffer to the model
//   "cancel"      — cancel the in-flight request / close menus
//   "cycle_mode"  — cycle the permission mode (Shift+Tab is the LOCKED default)
//   "newline"     — insert a newline into the input (multi-line input)
//   "history_up"  — navigate to previous history entry
//   "history_down"— navigate to next history entry
//   "clear"       — clear the current input buffer
//   "vim_toggle"  — toggle vim-mode keybindings on/off (/vim shortcut)
//
// Key descriptor format:
//   Zero or more modifiers joined with '+', followed by the base key:
//     Ctrl+C         → Ctrl modifier + C
//     Shift+Tab      → Shift modifier + Tab
//     Alt+Enter      → Alt modifier + Enter
//     Ctrl+Shift+K   → Ctrl + Shift + K
//     Enter          → plain Enter (no modifier)
//
//   Recognised modifier tokens (case-insensitive): Ctrl, Shift, Alt, Meta
//   Base key: any single character, or a named key (Tab, Enter, Backspace,
//             Up, Down, Left, Right, Escape, Space, Delete, Home, End,
//             PageUp, PageDown, F1–F12).
//
// Semantics:
//   load_keybindings(path) reads the file at 'path', parses it, and returns
//   a map of action → key-descriptor strings.  The returned map starts with
//   all built-in defaults and overlays any entries found in the file.
//
//   Missing file: returns Ok(defaults map) — not an error.
//   Unknown action name: warning logged via BATBOX_LOG_WARN, entry skipped.
//   Malformed JSON: returns Err with a descriptive message.
//   Non-string value for an action: warning logged, entry skipped.
//   Non-object top level: returns Err.
//
// Locked binding:
//   "cycle_mode" is a fixed UX affordance; its default (Shift+Tab) may be
//   overridden in the file but the action itself is always present in defaults.
//
// Thread safety: stateless; safe to call from multiple threads concurrently.
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/core/Result.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace batbox::config {

/// Type alias for the keybinding map: action name → key descriptor string.
using KeybindingMap = std::unordered_map<std::string, std::string>;

// ---------------------------------------------------------------------------
// default_keybindings()
//
// Returns the built-in default keybinding map.
// Never fails.  No I/O.
//
// Defaults:
//   send          → "Ctrl+Enter"
//   cancel        → "Escape"
//   cycle_mode    → "Shift+Tab"   (locked default; always present)
//   newline       → "Shift+Enter"
//   history_up    → "Up"
//   history_down  → "Down"
//   clear         → "Ctrl+L"
//   vim_toggle    → "Escape"      (same as cancel; context-dependent)
// ---------------------------------------------------------------------------
[[nodiscard]]
KeybindingMap default_keybindings();

// ---------------------------------------------------------------------------
// load_keybindings(path)
//
// Reads and parses the keybindings.json file at 'path'.
//
// Returns:
//   Ok(map)  — defaults merged with file overrides; unknown actions skipped
//              with a BATBOX_LOG_WARN warning.  Missing file returns defaults.
//   Err(msg) — file exists but is unreadable, not valid JSON, or the
//              top-level value is not a JSON object.
// ---------------------------------------------------------------------------
[[nodiscard]]
batbox::Result<KeybindingMap, std::string>
load_keybindings(std::filesystem::path path);

} // namespace batbox::config
