#!/usr/bin/env bash
# =============================================================================
# cases/06_slash_dispatch.sh — UI-D6 smoke test
#
# Verifies that /clear dispatches to SlashCommandRegistry rather than
# being sent to the LLM as a chat message.
#
# Pass criteria (from ui-triage.md UI-D6):
#   /clear produces a visible side-effect; the response shows "Conversation
#   cleared." (or equivalent) rather than an LLM reply.
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
    echo "SKIP: 06_slash_dispatch — $*"
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

SESSION_NAME="smoke-06"
MOCK_PORT="8826"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock LLM (so BatBox can start; we don't want it to hang if /clear
# accidentally falls through to inference)
# ---------------------------------------------------------------------------
echo "Starting mock LLM on port ${MOCK_PORT}..."
if command -v python3 >/dev/null 2>&1; then
    "${HARNESS}" mock-llm start --port "${MOCK_PORT}" || true
fi

sleep 0.5

echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Submit /clear via Enter — this should dispatch to SlashCommandRegistry
# NOT to the LLM.  The result should be visible quickly (not after LLM
# round-trip latency).
# ---------------------------------------------------------------------------
echo "Sending '/clear'..."
"${HARNESS}" send --name "${SESSION_NAME}" "/clear"

# ---------------------------------------------------------------------------
# Assert: "Conversation cleared." appears within 3s.
# If /clear went to the LLM instead, we would NOT see this text quickly
# (and would instead see a slow LLM reply or nothing within 3s).
# ---------------------------------------------------------------------------
echo "Waiting for 'Conversation cleared.' to appear..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "(Conversation cleared|cleared)"; then
    echo "FAIL: /clear did not produce 'Conversation cleared.' — command may have gone to LLM"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "PASS: 06_slash_dispatch — /clear dispatched to SlashCommandRegistry"
exit 0
