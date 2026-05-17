#!/usr/bin/env bash
# =============================================================================
# lib/assertions.sh — test assertion helpers for tui-smoke cases
#
# Sourced by individual case files.
# All assertions print diagnostics to stderr and exit non-zero on failure.
# Compatible with bash 3.2+ (macOS system bash).
# =============================================================================

[ -n "${_TUI_ASSERTIONS_LOADED:-}" ] && return 0
_TUI_ASSERTIONS_LOADED=1

# Source tmux helpers (path relative to this file)
_ASSERTIONS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=./tmux_helpers.sh
source "${_ASSERTIONS_DIR}/tmux_helpers.sh"

# ---------------------------------------------------------------------------
# assert_contains <name> <pattern>
# Asserts the current screen of session NAME contains PATTERN (grep -E).
# Exits 1 on failure.
# ---------------------------------------------------------------------------
assert_contains() {
    local name="${1:-}"
    local pattern="${2:-}"
    local screen
    screen="$(tui_capture "$name")" || {
        echo "FAIL: assert_contains — could not capture session '$(tui_session_name "$name")'" >&2
        return 1
    }
    if ! echo "$screen" | grep -qE "$pattern"; then
        echo "FAIL: assert_contains — pattern '$pattern' NOT found in screen" >&2
        echo "--- screen dump ---" >&2
        echo "$screen" >&2
        echo "--- end dump ---" >&2
        return 1
    fi
    return 0
}

# Alias used in spec example
assert_screen_contains() { assert_contains "$@"; }

# ---------------------------------------------------------------------------
# assert_not_contains <name> <pattern>
# Asserts the current screen does NOT contain PATTERN.
# Exits 1 on failure.
# ---------------------------------------------------------------------------
assert_not_contains() {
    local name="${1:-}"
    local pattern="${2:-}"
    local screen
    screen="$(tui_capture "$name")" || {
        echo "FAIL: assert_not_contains — could not capture session '$(tui_session_name "$name")'" >&2
        return 1
    }
    if echo "$screen" | grep -qE "$pattern"; then
        echo "FAIL: assert_not_contains — pattern '$pattern' FOUND in screen (expected absent)" >&2
        echo "--- screen dump ---" >&2
        echo "$screen" >&2
        echo "--- end dump ---" >&2
        return 1
    fi
    return 0
}

# Alias
assert_screen_not_contains() { assert_not_contains "$@"; }

# ---------------------------------------------------------------------------
# assert_status_row <name> <expected_text>
# Asserts that the last non-blank line (status bar) contains EXPECTED_TEXT.
# BatBox renders a status bar on the bottom row.
# ---------------------------------------------------------------------------
assert_status_row() {
    local name="${1:-}"
    local expected="${2:-}"
    local screen
    screen="$(tui_capture "$name")" || {
        echo "FAIL: assert_status_row — could not capture session '$(tui_session_name "$name")'" >&2
        return 1
    }
    # Get last non-empty line
    local last_line
    last_line="$(echo "$screen" | grep -v '^[[:space:]]*$' | tail -1)"
    if ! echo "$last_line" | grep -qF "$expected"; then
        echo "FAIL: assert_status_row — expected '$expected' in last line, got: '$last_line'" >&2
        return 1
    fi
    return 0
}
