#!/usr/bin/env bash
# =============================================================================
# lib/tmux_helpers.sh — low-level tmux session management helpers
#
# Sourced by bin/harness and individual case files.
# Compatible with bash 3.2+ (macOS system bash).
# All public functions are prefixed tui_ to avoid namespace collisions.
# =============================================================================

# Guard against double-sourcing
[ -n "${_TUI_TMUX_HELPERS_LOADED:-}" ] && return 0
_TUI_TMUX_HELPERS_LOADED=1

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
TUI_SESSION_PREFIX="batbox-qa"
TUI_DEFAULT_NAME="batbox-qa"
TUI_MOCK_PID_FILE="/tmp/batbox-qa-mock-llm.pid"
TUI_MOCK_PORT="${BATBOX_MOCK_PORT:-8824}"
TUI_MOCK_HOST="127.0.0.1"
TUI_MOCK_BASE_URL="http://${TUI_MOCK_HOST}:${TUI_MOCK_PORT}/v1"

# ---------------------------------------------------------------------------
# _tui_require_tmux
# Prints an error and exits 1 if tmux is not on PATH.
# ---------------------------------------------------------------------------
_tui_require_tmux() {
    if ! command -v tmux >/dev/null 2>&1; then
        echo "ERROR: tmux not found. Install with: brew install tmux" >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# tui_session_name <name>
# Prepends the batbox-qa- prefix if not already present.
# ---------------------------------------------------------------------------
tui_session_name() {
    local name="${1:-}"
    case "$name" in
        "${TUI_SESSION_PREFIX}-"*) echo "$name" ;;
        "") echo "${TUI_DEFAULT_NAME}" ;;
        *)  echo "${TUI_SESSION_PREFIX}-${name}" ;;
    esac
}

# ---------------------------------------------------------------------------
# tui_session_exists <session>
# Returns 0 if the session exists, 1 otherwise.
# ---------------------------------------------------------------------------
tui_session_exists() {
    local session
    session="$(tui_session_name "${1:-}")"
    tmux has-session -t "$session" 2>/dev/null
}

# ---------------------------------------------------------------------------
# tui_session_up <session> [cmd] [env_pairs...]
#
# Creates a new detached session running <cmd> (default: bash) if it does not
# already exist.  Idempotent: if the session exists, does nothing and returns 0.
#
# env_pairs: KEY=VALUE strings forwarded to tmux -e flags (one per pair).
# ---------------------------------------------------------------------------
tui_session_up() {
    local session
    session="$(tui_session_name "${1:-}")"
    local cmd="${2:-bash}"
    shift 2 2>/dev/null || shift ${#} 2>/dev/null
    # remaining args are env pairs

    _tui_require_tmux || return 1

    if tui_session_exists "$session"; then
        echo "INFO: Session '$session' already exists — reusing." >&2
        return 0
    fi

    local env_flags=""
    local pair
    for pair in "$@"; do
        env_flags="$env_flags -e $(printf '%q' "$pair")"
    done

    # shellcheck disable=SC2086
    eval tmux new-session -d -s "$session" -x 220 -y 50 $env_flags "$cmd" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "ERROR: tmux new-session failed (exit $rc)" >&2
        return $rc
    fi
    # Give the shell a moment to settle
    sleep 0.15
    return 0
}

# ---------------------------------------------------------------------------
# tui_session_down <session>
# Kills the session. No-op if it does not exist.
# ---------------------------------------------------------------------------
tui_session_down() {
    local session
    session="$(tui_session_name "${1:-}")"
    _tui_require_tmux || return 1
    if tui_session_exists "$session"; then
        tmux kill-session -t "$session" 2>/dev/null
    fi
    return 0
}

# ---------------------------------------------------------------------------
# tui_send_keys <session> <keys...>
# Sends keystroke(s) to the first pane of the session.
# Multiple arguments are joined with spaces and passed as a single send-keys call.
# ---------------------------------------------------------------------------
tui_send_keys() {
    local session
    session="$(tui_session_name "${1:-}")"
    shift
    _tui_require_tmux || return 1
    if ! tui_session_exists "$session"; then
        echo "ERROR: Session '$session' does not exist" >&2
        return 1
    fi
    tmux send-keys -t "$session" "$@"
}

# ---------------------------------------------------------------------------
# tui_send_text <session> <text> [--no-enter]
# Sends literal text followed by Enter (unless --no-enter).
# Text is sent raw; special tmux key names are NOT interpreted here.
# ---------------------------------------------------------------------------
tui_send_text() {
    local session
    session="$(tui_session_name "${1:-}")"
    local text="${2:-}"
    local no_enter="${3:-}"
    _tui_require_tmux || return 1
    if ! tui_session_exists "$session"; then
        echo "ERROR: Session '$session' does not exist" >&2
        return 1
    fi
    # Send the text literally (no key interpretation)
    tmux send-keys -t "$session" -l "$text"
    if [ "$no_enter" != "--no-enter" ]; then
        tmux send-keys -t "$session" "Enter"
    fi
}

# ---------------------------------------------------------------------------
# tui_send_key <session> <key-name>
# Sends a single tmux special key (Enter, Tab, BSpace, Escape, Up, Down,
# Left, Right, C-c, C-d, …).  The key name is passed directly to send-keys.
# ---------------------------------------------------------------------------
tui_send_key() {
    local session
    session="$(tui_session_name "${1:-}")"
    local key="${2:-}"
    _tui_require_tmux || return 1
    if ! tui_session_exists "$session"; then
        echo "ERROR: Session '$session' does not exist" >&2
        return 1
    fi
    tmux send-keys -t "$session" "$key"
}

# ---------------------------------------------------------------------------
# tui_capture <session>
# Dumps the current visible pane contents to stdout, ANSI-stripped.
# Uses capture-pane -p -e to get escape sequences then strips them, so the
# output is plain text suitable for grep.
# ---------------------------------------------------------------------------
tui_capture() {
    local session
    session="$(tui_session_name "${1:-}")"
    _tui_require_tmux || return 1
    if ! tui_session_exists "$session"; then
        echo "ERROR: Session '$session' does not exist" >&2
        return 1
    fi
    # capture-pane -p prints to stdout; -J joins wrapped lines; strip ANSI codes
    tmux capture-pane -t "$session" -p -J 2>/dev/null \
        | sed 's/\x1b\[[0-9;]*[mGKHFABCDJP]//g' \
        | sed 's/\x1b[()][AB012]//g' \
        | sed 's/\x1b[=>]//g'
}

# ---------------------------------------------------------------------------
# tui_wait_for <session> <pattern> <timeout_s>
# Polls tui_capture at 5 Hz until <pattern> appears (grep -q) or timeout.
# Returns 0 on match, 1 on timeout.
# <pattern> is passed to grep as an extended regex (-E).
# ---------------------------------------------------------------------------
tui_wait_for() {
    local session
    session="$(tui_session_name "${1:-}")"
    local pattern="${2:-}"
    local timeout_s="${3:-10}"
    _tui_require_tmux || return 1

    local deadline
    deadline=$(( $(date +%s) + timeout_s ))
    while [ "$(date +%s)" -le "$deadline" ]; do
        if tui_capture "$session" 2>/dev/null | grep -qE "$pattern"; then
            return 0
        fi
        sleep 0.2
    done
    echo "TIMEOUT: pattern '$pattern' not found in session '$session' after ${timeout_s}s" >&2
    echo "--- screen dump ---" >&2
    tui_capture "$session" >&2 || true
    echo "--- end dump ---" >&2
    return 1
}
