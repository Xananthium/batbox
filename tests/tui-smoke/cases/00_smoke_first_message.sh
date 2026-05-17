#!/usr/bin/env bash
# =============================================================================
# cases/00_smoke_first_message.sh — first end-to-end smoke test
#
# Launches BatBox against mock_lmstudio.py, sends "hi", and asserts that:
#   - The user's message ("You: hi" or the raw input line) is visible
#   - A BatBox response line appears
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at ../../build/src/batbox
#   - tmux not installed
#   - Python3 not available
#
# This case must NOT fail due to infrastructure absence — only due to
# actual BatBox behaviour regressions.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Resolve harness root regardless of invocation location
# ---------------------------------------------------------------------------
_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

# ---------------------------------------------------------------------------
# Source helpers directly (in case this is run standalone without harness run)
# ---------------------------------------------------------------------------
# shellcheck source=../lib/tmux_helpers.sh
source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
# shellcheck source=../lib/assertions.sh
source "${HARNESS_ROOT}/lib/assertions.sh"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 00_smoke_first_message — $*"
    exit 0
}

# Check for tmux
if ! command -v tmux >/dev/null 2>&1; then
    _skip "tmux not installed (brew install tmux)"
fi

# Check for python3
if ! command -v python3 >/dev/null 2>&1; then
    _skip "python3 not available"
fi

# Check for BatBox binary
BATBOX_BIN="${HARNESS_ROOT}/../../build/src/batbox"
BATBOX_BIN="$(cd "$(dirname "$BATBOX_BIN")" 2>/dev/null && pwd)/$(basename "$BATBOX_BIN")" 2>/dev/null || true
if [ ! -x "${BATBOX_BIN}" ]; then
    _skip "BatBox binary not found at build/src/batbox — build first"
fi

# ---------------------------------------------------------------------------
# Test setup
# ---------------------------------------------------------------------------
SESSION_NAME="smoke-00"
MOCK_PORT="8824"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

# Ensure cleanup on exit (including error exit)
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

# Give mock a moment to be ready
sleep 0.5

# ---------------------------------------------------------------------------
# Start BatBox session
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

# ---------------------------------------------------------------------------
# Wait for the input cursor indicator (BatBox is ready to accept input)
# The cursor/prompt typically shows a block cursor character or the input bar
# We wait up to 10s for any content to appear in the pane
# ---------------------------------------------------------------------------
echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render any output within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Send the test message
# ---------------------------------------------------------------------------
echo "Sending test message..."
"${HARNESS}" send --name "${SESSION_NAME}" "hi"

# ---------------------------------------------------------------------------
# Assert: user's message appears in the chat view
# BatBox renders user messages as "You: <text>" or similar
# We check for the text "hi" appearing in the screen
# ---------------------------------------------------------------------------
echo "Waiting for user message to appear..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "hi"; then
    echo "FAIL: user message 'hi' did not appear on screen"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assert: a response line from BatBox appears
# The mock server returns "Hi!" — we check for "Hi" in the screen
# We also accept "Batbox:" prefix if that's how it renders
# ---------------------------------------------------------------------------
echo "Waiting for assistant response..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 30 "(Batbox:|Hi[!]?)"; then
    echo "FAIL: assistant response did not appear within 30s"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# Final assertion
assert_contains "${SESSION_NAME}" "(hi|Hi)"

echo "PASS: 00_smoke_first_message"
exit 0
