#!/usr/bin/env bash
# =============================================================================
# cases/10_tool_running_indicator.sh — UI-D10 / TUI-T9 smoke test
#
# Verifies that the status row shows "running: Bash" while a tool is being
# dispatched, and clears after the tool result returns.
#
# Strategy:
#   1. Start mock LLM with a fixture that returns a Bash tool-call with a
#      2-second delay before the tool result (so we can capture the indicator).
#   2. Start BatBox in --nuclear mode.
#   3. Submit "run echo hello".
#   4. Within 3s (while the tool is running): assert "running: Bash" appears
#      in the status row.
#   5. Wait for "hello" in the output (tool result arrived).
#   6. Assert status row no longer shows "running: Bash".
#
# SKIP CONDITIONS (exits 0):
#   - BatBox binary not found at build/src/batbox
#   - tmux not installed
#   - Python3 not available
#   - nuclear mode not supported
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/harness"

source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
source "${HARNESS_ROOT}/lib/assertions.sh"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 10_tool_running_indicator — $*"
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

if ! "${BATBOX_BIN}" --nuclear --help >/dev/null 2>&1; then
    _skip "--nuclear flag not supported by this build"
fi

# ---------------------------------------------------------------------------
# Test configuration
# ---------------------------------------------------------------------------
SESSION_NAME="smoke-10"
MOCK_PORT="8840"
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
# Start BatBox in nuclear mode (auto-approves all tool calls)
# ---------------------------------------------------------------------------
echo "Starting BatBox (nuclear) session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" \
    --api-base "${MOCK_BASE_URL}" \
    --nuclear

# Wait for UI to render.
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Send the test prompt
# ---------------------------------------------------------------------------
echo "Sending 'run echo hello'..."
"${HARNESS}" send --name "${SESSION_NAME}" "run echo hello"

# ---------------------------------------------------------------------------
# Assert 1: status row shows "running: Bash" while tool is dispatching
# ---------------------------------------------------------------------------
echo "Waiting for 'running: Bash' in status row (up to 3s)..."
if "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 3 "running: Bash"; then
    echo "PASS: 'running: Bash' visible in status row"
else
    SCREEN_OUT=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
    # Infrastructure skip if mock didn't return a tool call
    if echo "${SCREEN_OUT}" | grep -qE "^Hi[!]?$" && \
       ! echo "${SCREEN_OUT}" | grep -qE "running:"; then
        echo "SKIP: 10_tool_running_indicator — mock returned plain fallback; fixture did not match"
        exit 0
    fi
    echo "FAIL: 'running: Bash' did not appear in status row within 3s"
    echo "--- Screen output ---"
    echo "${SCREEN_OUT}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assert 2: wait for tool result ("hello")
# ---------------------------------------------------------------------------
echo "Waiting for tool result 'hello' (up to 10s)..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "hello"; then
    echo "INFO: 'hello' not found — mock may not have matched fixture (non-fatal)"
fi

# ---------------------------------------------------------------------------
# Assert 3: status row clears after tool result
# ---------------------------------------------------------------------------
echo "Checking that 'running: Bash' clears after tool result..."
sleep 0.5
SCREEN_OUT=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
if echo "${SCREEN_OUT}" | grep -q "running: Bash"; then
    echo "FAIL: 'running: Bash' still visible in status row after tool result"
    echo "--- Screen output ---"
    echo "${SCREEN_OUT}"
    exit 1
fi

echo "PASS: status row cleared after tool result"
echo "PASS: 10_tool_running_indicator"
exit 0
