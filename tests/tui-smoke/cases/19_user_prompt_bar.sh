#!/usr/bin/env bash
# =============================================================================
# cases/19_user_prompt_bar.sh — TUI-FLOW-T7 smoke test: grey prompt bar
#
# Verifies that submitted user prompts render as a single-line "> <text>" bar
# (Claude Code style) rather than the old two-line "You: <text>" block.
#
# Assertions:
#   [A] The pane contains "> " after the user submits a message.
#   [B] The pane does NOT contain "You:" for the user's submitted message.
#   [C] The body text of the message is visible on the same line as "> ".
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

# ---------------------------------------------------------------------------
# Skip guards
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 19_user_prompt_bar — $*"
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
SESSION_NAME="smoke-19"
MOCK_PORT="8856"
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

if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Submit a user prompt
# ---------------------------------------------------------------------------
PROMPT_TEXT="hello from smoke test"
echo "Sending prompt: '${PROMPT_TEXT}'..."
"${HARNESS}" send --name "${SESSION_NAME}" "${PROMPT_TEXT}"

# ---------------------------------------------------------------------------
# Poll up to 10s for the prompt text to appear in the pane
# ---------------------------------------------------------------------------
echo "Waiting for prompt to appear in pane..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "hello"; then
    echo "FAIL: prompt text did not appear within 10s"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# Give one render cycle to settle
sleep 0.3
SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)

# ---------------------------------------------------------------------------
# [A] Assert "> " prefix appears in the pane
# ---------------------------------------------------------------------------
if ! echo "${SNAP}" | grep -q "> "; then
    echo "FAIL [A]: '> ' prefix not found in pane — user prompt bar not rendered"
    echo "--- Pane ---"
    echo "${SNAP}"
    exit 1
fi
echo "PASS [A]: '> ' prefix found in pane"

# ---------------------------------------------------------------------------
# [B] Assert "You:" label is NOT present
# ---------------------------------------------------------------------------
if echo "${SNAP}" | grep -q "You:"; then
    echo "FAIL [B]: 'You:' label found in pane — old two-line render still active"
    echo "--- Pane ---"
    echo "${SNAP}"
    exit 1
fi
echo "PASS [B]: 'You:' label absent from pane"

# ---------------------------------------------------------------------------
# [C] Assert the body text is visible
# ---------------------------------------------------------------------------
if ! echo "${SNAP}" | grep -q "hello"; then
    echo "FAIL [C]: prompt body text 'hello' not found in pane"
    echo "--- Pane ---"
    echo "${SNAP}"
    exit 1
fi
echo "PASS [C]: prompt body text visible in pane"

echo "PASS: 19_user_prompt_bar"
exit 0
