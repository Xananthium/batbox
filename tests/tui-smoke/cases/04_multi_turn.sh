#!/usr/bin/env bash
# =============================================================================
# cases/04_multi_turn.sh — multi-turn conversation history smoke test (UI-D4)
#
# Verifies that after a successful first turn, BatBox includes the prior
# user+assistant messages in the second request to the model.
#
# Design: the mock fixture for turn 2 is keyed on the 3-message history.
#   - If BatBox sends history: mock matches → "You told me 42." → PASS
#   - If BatBox drops history: mock gets no match → fallback "Hi!" → FAIL
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at ../../build/src/batbox
#   - tmux not installed
#   - Python3 not available
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
    echo "SKIP: 04_multi_turn — $*"
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
SESSION_NAME="smoke-04"
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
# Wait for BatBox to render (any content in the pane)
# ---------------------------------------------------------------------------
echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render any output within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Turn 1: send the first message
# Fixture bc2c63147e506520.jsonl matches [{"content":"remember the number 42","role":"user"}]
# and responds: "I'll remember that."
# ---------------------------------------------------------------------------
echo "Turn 1: sending 'remember the number 42'..."
"${HARNESS}" send --name "${SESSION_NAME}" "remember the number 42"

echo "Waiting for turn-1 response..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "I'll remember"; then
    echo "FAIL: turn-1 response 'I'll remember that.' did not appear within 15s"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# Wait for turn-1 run_turn() to fully complete before sending turn-2.
# The assistant message is appended to messages_ after stream_chat() returns
# (after the [DONE] SSE sentinel), not merely after the first token appears.
# A 1s pause ensures the detached worker thread has posted stream_done and
# Conversation::run_turn() has pushed the assistant WireMessage to messages_.
sleep 1

# ---------------------------------------------------------------------------
# Turn 2: send the follow-up message
# Fixture acdd3e5e63d065cc.jsonl matches the 3-message history:
#   [user:"remember the number 42", assistant:"I'll remember that.", user:"what number..."]
# and responds: "You told me 42."
#
# If BatBox fails to include history, the mock receives a 1-message payload and
# falls back to "Hi!" — the wait_for below will then time out, failing the test.
# ---------------------------------------------------------------------------
echo "Turn 2: sending 'what number did I tell you?'..."
"${HARNESS}" send --name "${SESSION_NAME}" "what number did I tell you?"

echo "Waiting for turn-2 response (must include conversation history)..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "You told me 42"; then
    echo "FAIL: turn-2 response did not recall '42' — history likely not sent to model"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# Final assertion: "42" is visible in the current screen
assert_contains "${SESSION_NAME}" "42"

echo "PASS: 04_multi_turn"
exit 0
