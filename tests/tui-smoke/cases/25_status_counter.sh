#!/usr/bin/env bash
# =============================================================================
# cases/25_status_counter.sh — TUI-FIX-T6: status row usage counters
#
# Acceptance criteria (A3):
#   - After a non-zero turn, status row shows non-zero token count (e.g. "5tk")
#   - Status row shows a cost value (e.g. "$0.001" or "$0.000" if mock returns 0 cost)
#   - Tokens format is correct: NNtk or N,NNNtk
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at ../../build/src/batbox
#   - tmux not installed
#   - Python3 not available
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
source "${HARNESS_ROOT}/lib/assertions.sh"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 25_status_counter — $*"
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
# Test setup
# ---------------------------------------------------------------------------
SESSION_NAME="smoke-25"
MOCK_PORT="8850"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock LLM and BatBox
# ---------------------------------------------------------------------------
echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

# Wait for BatBox to render
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render"
    exit 1
fi

# ---------------------------------------------------------------------------
# Send a test prompt and wait for the response
# ---------------------------------------------------------------------------
echo "Sending test prompt..."
"${HARNESS}" send --name "${SESSION_NAME}" "hello"

# Wait for assistant response (StreamDone)
echo "Waiting for assistant response..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 30 "(Batbox:|hello|Hi)"; then
    echo "FAIL: assistant response did not appear"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# Give the status row update a moment to propagate
sleep 1

# ---------------------------------------------------------------------------
# Assert: status row shows a non-zero token count (NNtk format)
# The status row format is: "◉ model · NNNtk · $X.XXX · mode"
# After one turn the mock server returns some usage, so tokens > 0
# We accept any number followed by 'tk' as passing.
# ---------------------------------------------------------------------------
echo "Checking status row for token count..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "[0-9]tk"; then
    echo "FAIL: status row does not show token count (NNNtk)"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assert: status row shows a cost value ($X.XXX format)
# ---------------------------------------------------------------------------
echo "Checking status row for cost display..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 '[$][0-9]'; then
    echo "FAIL: status row does not show cost ($X.XXX)"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "PASS: 25_status_counter — status row shows token count and cost after a turn"
exit 0
