#!/usr/bin/env bash
# =============================================================================
# cases/13_fatal_error_surfaces.sh — TUI-T19 smoke test (fatal_error variant)
#
# Identical to 12_error_event_surfaces.sh but drives mock_reasoning_only.py
# in --mode fatal-error, which emits:
#
#   event: fatal_error
#   data: {"error":{"message":"The number of tokens to keep from the
#          initial prompt is greater than the context length..."}}
#
# TUI-T17's write_cb branch handles BOTH event names:
#   if (ev.event == "error" || ev.event == "fatal_error")
#
# Acceptance criteria are identical to case 12:
#   - exit non-zero
#   - output contains "server:"
#   - output contains "context length"
#   - output does NOT contain "BATBOX_MAX_TOKENS"
#
# SKIP CONDITIONS (exits 0 without failing):
#   - build/src/batbox binary not present or not executable
#   - python3 not available
#
# Registered in CMakeLists.txt as tui_smoke_13_fatal_error_surfaces under
# the "tui-smoke" label.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 13_fatal_error_surfaces — $*"
    exit 0
}

if ! command -v python3 >/dev/null 2>&1; then
    _skip "python3 not available"
fi

BATBOX_BIN="${HARNESS_ROOT}/../../build/src/batbox"
BATBOX_BIN="$(cd "$(dirname "${BATBOX_BIN}")" 2>/dev/null && pwd)/$(basename "${BATBOX_BIN}")" 2>/dev/null || true
if [ ! -x "${BATBOX_BIN}" ]; then
    _skip "BatBox binary not found at build/src/batbox — build first"
fi

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
MOCK_PORT="8828"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

# ---------------------------------------------------------------------------
# Cleanup on exit
# ---------------------------------------------------------------------------
cleanup() {
    "${HARNESS}" mock-reasoning stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start the mock server in fatal-error mode
# ---------------------------------------------------------------------------
echo "Starting mock-reasoning in fatal-error mode on port ${MOCK_PORT}..."
"${HARNESS}" mock-reasoning start --port "${MOCK_PORT}" --mode fatal-error

sleep 0.2

# ---------------------------------------------------------------------------
# Run batbox --print against the mock
# ---------------------------------------------------------------------------
echo "Running: batbox --print 'hello' -> ${MOCK_BASE_URL}"

COMBINED_OUT=""
EXIT_CODE=0

COMBINED_OUT="$(
    BATBOX_API_BASE_URL="${MOCK_BASE_URL}" \
    BATBOX_API_KEY="test" \
    BATBOX_NO_SPLASH=true \
        "${BATBOX_BIN}" --print "hello" 2>&1
)" || EXIT_CODE=$?

echo "Exit code: ${EXIT_CODE}"
echo "Output: ${COMBINED_OUT}"

# ---------------------------------------------------------------------------
# Assertion 1: exit code must be non-zero
# ---------------------------------------------------------------------------
if [ "${EXIT_CODE}" -eq 0 ]; then
    echo "FAIL: batbox exited 0 (expected non-zero)"
    echo "  TUI-T17's fatal_error handler may not be active."
    echo "  Output was: ${COMBINED_OUT}"
    exit 1
fi

echo "PASS: batbox exited non-zero (${EXIT_CODE})"

# ---------------------------------------------------------------------------
# Assertion 2: output must contain "server:" prefix
# ---------------------------------------------------------------------------
if echo "${COMBINED_OUT}" | grep -q "server:"; then
    echo "PASS: output contains 'server:' prefix (TUI-T17 path confirmed)"
else
    echo "FAIL: output did not contain 'server:'"
    echo "  Actual output: ${COMBINED_OUT}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assertion 3: output must contain "context length"
# ---------------------------------------------------------------------------
if echo "${COMBINED_OUT}" | grep -q "context length"; then
    echo "PASS: output contains 'context length' (server message propagated)"
else
    echo "FAIL: output did not contain 'context length'"
    echo "  Actual output: ${COMBINED_OUT}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assertion 4: output must NOT contain "BATBOX_MAX_TOKENS"
# ---------------------------------------------------------------------------
if echo "${COMBINED_OUT}" | grep -q "BATBOX_MAX_TOKENS"; then
    echo "FAIL: output contains 'BATBOX_MAX_TOKENS' — T12 generic guard fired"
    echo "  TUI-T17's specific fatal_error handler should have taken precedence."
    echo "  Actual output: ${COMBINED_OUT}"
    exit 1
fi

echo "PASS: output does not contain 'BATBOX_MAX_TOKENS' (T17 path took precedence)"

# ---------------------------------------------------------------------------
# All assertions passed
# ---------------------------------------------------------------------------
echo "PASS: 13_fatal_error_surfaces"
exit 0
