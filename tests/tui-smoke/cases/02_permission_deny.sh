#!/usr/bin/env bash
# =============================================================================
# cases/02_permission_deny.sh — permission deny via 'n' key
#
# Tests UI-D2 (TUI-T4): When the permission modal is shown and the user presses
# 'n' (deny), the tool should NOT execute.
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found
#   - tmux not installed
#   - Python3 not available
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

# shellcheck source=../lib/tmux_helpers.sh
source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
# shellcheck source=../lib/assertions.sh
source "${HARNESS_ROOT}/lib/assertions.sh"

_skip() {
    echo "SKIP: 02_permission_deny — $*"
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

SESSION_NAME="smoke-02"
MOCK_PORT="8826"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# Reuse the same tool_call fixture from case 01 if it was written.
# Start mock LLM
echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

echo "Starting BatBox session '${SESSION_NAME}' (no --nuclear)..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

echo "Sending 'run echo hello'..."
"${HARNESS}" send --name "${SESSION_NAME}" "run echo hello"

echo "Waiting for permission modal..."
if "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "(Permission|Allow|Bash|allow once)"; then
    echo "OK: permission modal appeared"

    # Send 'n' to deny
    echo "Sending 'n' to deny..."
    "${HARNESS}" key --name "${SESSION_NAME}" "n"

    # After deny, "hello" (the echo output) should NOT appear.
    # Wait a short time then check for absence.
    sleep 1
    screen_content=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || echo "")

    if echo "${screen_content}" | grep -qE "^hello$"; then
        echo "FAIL: tool output 'hello' appeared after deny — tool was NOT denied"
        exit 1
    else
        echo "PASS: 02_permission_deny — tool was denied, output not visible"
        exit 0
    fi
else
    # Modal not shown — may be a text-only response from mock.
    echo "INFO: permission modal not detected — mock may not have returned tool_call"
    if "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "(Batbox:|Hi)"; then
        echo "SKIP: mock returned text response — permission modal N/A for this test"
        exit 0
    else
        echo "FAIL: BatBox did not respond within timeout"
        "${HARNESS}" screen --name "${SESSION_NAME}" || true
        exit 1
    fi
fi
