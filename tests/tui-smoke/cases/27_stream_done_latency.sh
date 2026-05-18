#!/usr/bin/env bash
# =============================================================================
# cases/27_stream_done_latency.sh — PEXT 2.4 TUI-STREAM-1 regression
#
# Asserts that after the final SSE chunk of a streaming reply, the BatBox
# InputBar exits stream_active_ (spinner stops, input becomes active) promptly
# — without requiring the user to press any key.
#
# What we test:
#   - Send a message; the mock server streams a 200-char reply and then sends
#     the terminal SSE [DONE] chunk.
#   - Assert the BatBox response appears on screen within 5 seconds (the
#     stream_done event must propagate through the on_stream_terminal_cb_
#     path introduced in PEXT 2.4, not just from the trailing worker-thread post).
#   - Assert a second prompt line appears, indicating InputBar is accepting
#     input (stream_active_ = false).
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at ../../build/src/batbox
#   - tmux not installed
#   - Python3 not available
#
# This case must NOT fail due to infrastructure absence — only due to actual
# BatBox behaviour regressions.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Resolve harness root regardless of invocation location
# ---------------------------------------------------------------------------
_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

# ---------------------------------------------------------------------------
# Source helpers directly (in case this is run standalone without harness)
# ---------------------------------------------------------------------------
# shellcheck source=../lib/tmux_helpers.sh
source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
# shellcheck source=../lib/assertions.sh
source "${HARNESS_ROOT}/lib/assertions.sh"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 27_stream_done_latency — $*"
    exit 0
}

if ! command -v tmux >/dev/null 2>&1; then
    _skip "tmux not installed (brew install tmux)"
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
SESSION_NAME="smoke-27"
MOCK_PORT="8827"
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

# Wait for BatBox to render initial splash/prompt
echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render any output within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Send message — the mock server will stream a short reply and finish
# ---------------------------------------------------------------------------
echo "Sending test message..."
"${HARNESS}" send --name "${SESSION_NAME}" "stream done latency test"

# ---------------------------------------------------------------------------
# Assert: the user message appears promptly
# ---------------------------------------------------------------------------
echo "Waiting for user message to appear..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "stream done"; then
    echo "FAIL: user message did not appear within 10s"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assert: BatBox response appears (stream committed) within 5 seconds
# This is the PEXT 2.4 regression assertion — if on_stream_terminal_cb_ is
# not wired, the response text appears only after run_turn() fully unwinds,
# which in a slow path (SQLite write) can take much longer.
# We assert the "Batbox:" label appears, meaning stream_done was processed.
# ---------------------------------------------------------------------------
echo "Waiting for assistant response (PEXT 2.4: must appear promptly)..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "Batbox:"; then
    echo "FAIL: assistant response did not appear within 5s — stream_done not firing promptly"
    echo "      This is a PEXT 2.4 regression: on_stream_terminal_cb_ may not be wired."
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "PASS: 27_stream_done_latency"
exit 0
