#!/usr/bin/env bash
# =============================================================================
# cases/33_input_wrap.sh — UX-B smoke test: input bar text wrapping
#
# Verifies that when the user types a long prompt (200+ chars) into the TUI
# input bar, the text wraps to multiple visual lines instead of scrolling
# horizontally off the right edge of the screen.
#
# Strategy: launch BatBox in a 60-column tmux pane (narrow enough to force
# wrapping at typical prompt lengths), type a 200+ char string WITHOUT
# pressing Enter, capture the pane, then assert:
#
#   [A] The BEGINNING of the string is visible (e.g. "make me a buzzfeed").
#   [B] The END of the string is visible (a unique tail token that would be
#       clipped if horizontal scrolling were used instead of wrapping).
#   [C] No line in the capture is longer than the pane width (i.e., no
#       single line contains both the start and the end of the long string).
#       This proves wrapping is happening, not just that tmux joined lines.
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at build/src/batbox
#   - tmux not installed
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
source "${HARNESS_ROOT}/lib/assertions.sh"

# ---------------------------------------------------------------------------
# Skip guards
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 33_input_wrap — $*"
    exit 0
}

if ! command -v tmux >/dev/null 2>&1; then
    _skip "tmux not installed"
fi

BATBOX_BIN="${HARNESS_ROOT}/../../build/src/batbox"
BATBOX_BIN="$(cd "$(dirname "$BATBOX_BIN")" 2>/dev/null && pwd)/$(basename "$BATBOX_BIN")" 2>/dev/null || true
if [ ! -x "${BATBOX_BIN}" ]; then
    _skip "BatBox binary not found at build/src/batbox — build first"
fi

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SESSION_NAME="smoke-33"
FULL_SESSION="batbox-qa-${SESSION_NAME}"
PANE_WIDTH=60

# Long prompt that must wrap at 60 cols. "WRAPEND" is the unique tail token.
# Total length: 210+ chars. The beginning and end are both unique and
# would both need to appear for the test to pass.
LONG_PROMPT="make me a buzzfeed parody website called buzzmeet in a folder here called buzzmeet, that is about how couples met in strange ways, how I hooked up with my partner WRAPEND"

cleanup() {
    tmux kill-session -t "${FULL_SESSION}" 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Create a narrow (60-col) tmux session directly — bypass harness up so we
# can control the terminal width. The harness default is 220 cols which would
# not force wrapping.
# ---------------------------------------------------------------------------
echo "Creating narrow (${PANE_WIDTH}-col) BatBox session '${FULL_SESSION}'..."
tmux new-session -d -s "${FULL_SESSION}" \
    -x "${PANE_WIDTH}" -y 30 \
    -e "BATBOX_NO_SPLASH=true" \
    -e "BATBOX_API_BASE_URL=http://127.0.0.1:8824/v1" \
    "${BATBOX_BIN}"

sleep 0.5

# ---------------------------------------------------------------------------
# Wait for BatBox to render
# ---------------------------------------------------------------------------
echo "Waiting for BatBox to render..."
if ! tui_wait_for "${SESSION_NAME}" "." 10; then
    echo "FAIL: BatBox did not render any output within 10s"
    exit 1
fi

# Give extra settle time after initial render
sleep 0.3

# ---------------------------------------------------------------------------
# Type the long prompt WITHOUT pressing Enter
# ---------------------------------------------------------------------------
echo "Typing long prompt (${#LONG_PROMPT} chars) without Enter..."
tmux send-keys -t "${FULL_SESSION}" -l "${LONG_PROMPT}"

# Allow FTXUI to re-render with the typed text
sleep 0.5

# ---------------------------------------------------------------------------
# Capture pane WITHOUT -J (preserve line breaks so we can check per-line width)
# ---------------------------------------------------------------------------
RAW_SNAP=$(tmux capture-pane -t "${FULL_SESSION}" -p 2>/dev/null \
    | sed 's/\x1b\[[0-9;]*[mGKHFABCDJP]//g' \
    | sed 's/\x1b[()][AB012]//g' \
    | sed 's/\x1b[=>]//g')

echo "--- Captured pane (${PANE_WIDTH} cols) ---"
echo "${RAW_SNAP}"
echo "--- End pane ---"

# ---------------------------------------------------------------------------
# [A] The beginning of the long prompt must be visible
# ---------------------------------------------------------------------------
if ! echo "${RAW_SNAP}" | grep -q "make me a buzzfeed"; then
    echo "FAIL [A]: beginning of long prompt not visible — 'make me a buzzfeed' not found"
    exit 1
fi
echo "PASS [A]: beginning of long prompt visible"

# ---------------------------------------------------------------------------
# [B] The tail token must be visible (proves no right-edge truncation)
# ---------------------------------------------------------------------------
if ! echo "${RAW_SNAP}" | grep -q "WRAPEND"; then
    echo "FAIL [B]: tail of long prompt not visible — 'WRAPEND' not found"
    echo "This means the text is scrolling off the right edge instead of wrapping."
    exit 1
fi
echo "PASS [B]: tail of long prompt visible (no right-edge truncation)"

# ---------------------------------------------------------------------------
# [C] No single raw line contains both the head and the tail
#     (proves real wrapping, not tmux -J line-joining)
# ---------------------------------------------------------------------------
if echo "${RAW_SNAP}" | grep -q "make me a buzzfeed.*WRAPEND"; then
    echo "FAIL [C]: entire prompt fits on one line — wrapping did not occur"
    exit 1
fi
echo "PASS [C]: prompt spans multiple visual lines (wrapping confirmed)"

echo "PASS: 33_input_wrap"
exit 0
