#!/usr/bin/env bash
# =============================================================================
# cases/05_slash_palette.sh — UI-D5 smoke test
#
# Verifies that the slash command palette is non-empty when '/' is typed.
#
# Pass criteria (from ui-triage.md UI-D5):
#   At least 3 known command names visible in palette when "/" is typed.
#   Checks for: clear, model, exit
#
# SKIP CONDITIONS:
#   - BatBox binary not found at ../../build/src/batbox
#   - tmux not installed
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
source "${HARNESS_ROOT}/lib/assertions.sh"

_skip() {
    echo "SKIP: 05_slash_palette — $*"
    exit 0
}

if ! command -v tmux >/dev/null 2>&1; then
    _skip "tmux not installed (brew install tmux)"
fi

BATBOX_BIN="${HARNESS_ROOT}/../../build/src/batbox"
BATBOX_BIN="$(cd "$(dirname "$BATBOX_BIN")" 2>/dev/null && pwd)/$(basename "$BATBOX_BIN")" 2>/dev/null || true
if [ ! -x "${BATBOX_BIN}" ]; then
    _skip "BatBox binary not found at build/src/batbox — build first"
fi

SESSION_NAME="smoke-05"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start BatBox (no mock LLM needed — we never submit a message)
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}"

echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Type '/' (no Enter) to open the slash palette
# ---------------------------------------------------------------------------
echo "Sending '/' to open slash palette..."
"${HARNESS}" send --name "${SESSION_NAME}" "/" --no-enter

# ---------------------------------------------------------------------------
# Assert: at least 3 known command names are visible in the palette
# ---------------------------------------------------------------------------
echo "Waiting for 'clear' to appear in palette..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "clear"; then
    echo "FAIL: 'clear' not visible in slash palette"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "Waiting for 'model' to appear in palette..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 2 "model"; then
    echo "FAIL: 'model' not visible in slash palette"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "Waiting for 'exit' to appear in palette..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 2 "exit"; then
    echo "FAIL: 'exit' not visible in slash palette"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "PASS: 05_slash_palette — palette populated with >=3 command names"
exit 0
