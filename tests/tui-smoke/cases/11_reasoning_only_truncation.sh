#!/usr/bin/env bash
# =============================================================================
# cases/11_reasoning_only_truncation.sh — TUI-T14 smoke test
#
# Drives the real build/src/batbox binary (--print mode) against
# mock_reasoning_only.py, which faithfully reproduces the server behaviour that
# triggered the silent-success bug fixed in TUI-T12:
#
#   Server sends: N SSE chunks of {"delta":{"reasoning_content":"..."}}
#   Then closes the TCP connection cleanly.
#   NO [DONE] sentinel.  NO finish_reason chunk.  NO content delta.
#
# Expected behaviour AFTER TUI-T12:
#   - batbox --print exits NON-ZERO (exit code 1)
#   - stderr contains "stream ended without content" OR "reasoning"
#     (substring from the Err string at Client.cpp:612-614)
#
# This case is a HEADLESS test — it invokes batbox --print directly, not
# through a tmux TUI session.  This is intentional: the TUI path and the
# --print path share the same Client::stream_chat / Conversation::run_turn
# error surface; the --print path is deterministic and requires no tmux
# pane capture polling.
#
# SKIP CONDITIONS (exits 0 without failing):
#   - build/src/batbox binary not present or not executable
#   - python3 not available
#
# Note: tmux is NOT required for this case.
#
# Registered in CMakeLists.txt as tui_smoke_11_reasoning_truncation under
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
    echo "SKIP: 11_reasoning_only_truncation — $*"
    exit 0
}

# python3 required to run the mock
if ! command -v python3 >/dev/null 2>&1; then
    _skip "python3 not available"
fi

# BatBox binary required
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
REASONING_PID_FILE="/tmp/batbox-qa-mock-reasoning-only.pid"

# ---------------------------------------------------------------------------
# Cleanup on exit
# ---------------------------------------------------------------------------
cleanup() {
    "${HARNESS}" mock-reasoning stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start the reasoning-only mock server
# ---------------------------------------------------------------------------
echo "Starting mock-reasoning-only on port ${MOCK_PORT}..."
"${HARNESS}" mock-reasoning start --port "${MOCK_PORT}"

# Give the server a moment to be ready (PID file already guarantees binding,
# but one tick of extra slack avoids a rare accept-loop race on macOS).
sleep 0.2

# ---------------------------------------------------------------------------
# Run batbox --print against the mock
# Capture stderr; allow non-zero exit (we expect it).
# ---------------------------------------------------------------------------
echo "Running: batbox --print 'hello' → ${MOCK_BASE_URL}"

STDERR_OUT=""
EXIT_CODE=0

STDERR_OUT="$(
    BATBOX_API_BASE_URL="${MOCK_BASE_URL}" \
    BATBOX_API_KEY="lmstudio" \
    BATBOX_NO_SPLASH=true \
        "${BATBOX_BIN}" --print "hello" 2>&1 >/dev/null
)" || EXIT_CODE=$?

echo "Exit code: ${EXIT_CODE}"
echo "Stderr: ${STDERR_OUT}"

# ---------------------------------------------------------------------------
# Assertion 1: exit code must be non-zero
# ---------------------------------------------------------------------------
if [ "${EXIT_CODE}" -eq 0 ]; then
    echo "FAIL: batbox exited 0 (expected non-zero — silent empty success not fixed)"
    echo "  This means TUI-T12 guard is not active or not reachable."
    echo "  Stderr was: ${STDERR_OUT}"
    exit 1
fi

echo "PASS: batbox exited non-zero (${EXIT_CODE})"

# ---------------------------------------------------------------------------
# Assertion 2: stderr must contain the expected error substring
# "stream ended without content" comes from Client.cpp:612-614 (TUI-T12 guard)
# "reasoning" is an acceptable broader match if the message is rephrased
# ---------------------------------------------------------------------------
if echo "${STDERR_OUT}" | grep -qE "stream ended without content|reasoning"; then
    echo "PASS: stderr contains expected error message"
else
    echo "FAIL: stderr did not contain 'stream ended without content' or 'reasoning'"
    echo "  Actual stderr: ${STDERR_OUT}"
    echo "  Expected one of:"
    echo "    - 'stream ended without content' (TUI-T12 Err string)"
    echo "    - 'reasoning' (broader match)"
    exit 1
fi

# ---------------------------------------------------------------------------
# All assertions passed
# ---------------------------------------------------------------------------
echo "PASS: 11_reasoning_only_truncation"
exit 0
