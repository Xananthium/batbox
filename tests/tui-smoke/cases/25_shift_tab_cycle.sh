#!/usr/bin/env bash
# =============================================================================
# cases/25_shift_tab_cycle.sh — TUI-PERM-T1 smoke test: Shift+Tab mode cycle
#
# Verifies:
#   [A] On initial launch the footer chip shows "mode: default" (Default mode).
#   [B] One Shift+Tab press changes the chip to "mode: plan".
#   [C] A second Shift+Tab press changes it to "mode: accept edits".
#   [D] A third press changes it to "mode: NUCLEAR".
#   [E] A fourth press wraps back to "mode: default".
#
# Shift+Tab is sent as raw escape sequence \x1b[Z (the xterm/VT100 Backtab).
#
# SKIP CONDITIONS (exits 0):
#   - BatBox binary not found at build/src/batbox
#   - tmux not installed
#   - python3 not available
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
source "${HARNESS_ROOT}/lib/assertions.sh"

_skip() {
    echo "SKIP: 25_shift_tab_cycle — $*"
    exit 0
}

if ! command -v tmux >/dev/null 2>&1; then
    _skip "tmux not installed"
fi

if ! command -v python3 >/dev/null 2>&1; then
    _skip "python3 not available"
fi

BATBOX_BIN="${HARNESS_ROOT}/../../build/src/batbox"
BATBOX_BIN="$(cd "$(dirname "$BATBOX_BIN")" 2>/dev/null && pwd)/$(basename "$BATBOX_BIN")" 2>/dev/null || true
if [ ! -x "${BATBOX_BIN}" ]; then
    _skip "BatBox binary not found at build/src/batbox — build first"
fi

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SESSION_NAME="smoke-25"
MOCK_PORT="8862"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock LLM
# ---------------------------------------------------------------------------
echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

# ---------------------------------------------------------------------------
# Start BatBox session
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

# ---------------------------------------------------------------------------
# [A] Wait for initial render and verify "mode: default" chip
# ---------------------------------------------------------------------------
echo "Waiting for initial render (mode chip)..."
# Wait for the footer chip row which renders on every frame regardless of splash.
# "mode: default" appears as soon as InputBar renders its footer row.
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "mode: default"; then
    echo "FAIL [A]: footer mode chip did not appear within 10s"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# Give an extra frame for the footer chips row to stabilise
sleep 0.3

SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
echo "--- Pane (initial) ---"
echo "${SNAP}"
echo "--- End pane ---"

if echo "${SNAP}" | grep -q "mode: default"; then
    echo "PASS [A]: 'mode: default' chip visible on initial render"
else
    echo "WARN [A]: 'mode: default' chip not found — may be a timing/render issue"
    echo "  (Continuing; the chip label is rendered by render_footer_chips_row)"
fi

# ---------------------------------------------------------------------------
# [B] Press Shift+Tab once → expect "mode: plan"
# ---------------------------------------------------------------------------
echo "Pressing Shift+Tab (1st)..."
"${HARNESS}" key --name "${SESSION_NAME}" BTab
sleep 0.3

SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
echo "--- Pane (after 1st Shift+Tab) ---"
echo "${SNAP}"
echo "--- End pane ---"

if echo "${SNAP}" | grep -q "mode: plan"; then
    echo "PASS [B]: 'mode: plan' chip visible after 1st Shift+Tab"
else
    echo "FAIL [B]: 'mode: plan' not found after 1st Shift+Tab"
    exit 1
fi

# ---------------------------------------------------------------------------
# [C] Press Shift+Tab again → expect "mode: accept edits"
# ---------------------------------------------------------------------------
echo "Pressing Shift+Tab (2nd)..."
"${HARNESS}" key --name "${SESSION_NAME}" BTab
sleep 0.3

SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
echo "--- Pane (after 2nd Shift+Tab) ---"
echo "${SNAP}"
echo "--- End pane ---"

if echo "${SNAP}" | grep -q "mode: accept edits"; then
    echo "PASS [C]: 'mode: accept edits' chip visible after 2nd Shift+Tab"
else
    echo "FAIL [C]: 'mode: accept edits' not found after 2nd Shift+Tab"
    exit 1
fi

# ---------------------------------------------------------------------------
# [D] Press Shift+Tab again → expect "mode: NUCLEAR"
# ---------------------------------------------------------------------------
echo "Pressing Shift+Tab (3rd)..."
"${HARNESS}" key --name "${SESSION_NAME}" BTab
sleep 0.3

SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
echo "--- Pane (after 3rd Shift+Tab) ---"
echo "${SNAP}"
echo "--- End pane ---"

if echo "${SNAP}" | grep -q "mode: NUCLEAR"; then
    echo "PASS [D]: 'mode: NUCLEAR' chip visible after 3rd Shift+Tab"
else
    echo "FAIL [D]: 'mode: NUCLEAR' not found after 3rd Shift+Tab"
    exit 1
fi

# ---------------------------------------------------------------------------
# [E] Press Shift+Tab again → expect wrap back to "mode: default"
# ---------------------------------------------------------------------------
echo "Pressing Shift+Tab (4th — wrap)..."
"${HARNESS}" key --name "${SESSION_NAME}" BTab
sleep 0.3

SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
echo "--- Pane (after 4th Shift+Tab — wrap) ---"
echo "${SNAP}"
echo "--- End pane ---"

if echo "${SNAP}" | grep -q "mode: default"; then
    echo "PASS [E]: 'mode: default' chip visible after wrap (4th Shift+Tab)"
else
    echo "FAIL [E]: 'mode: default' not found after wrap Shift+Tab"
    exit 1
fi

echo "PASS: 25_shift_tab_cycle"
exit 0
