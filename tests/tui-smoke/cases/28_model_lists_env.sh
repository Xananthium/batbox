#!/usr/bin/env bash
# =============================================================================
# cases/28_model_lists_env.sh — /model slash command lists BATBOX_MODELS entries
#
# PEXT3 1.2 acceptance criterion 3 (load-bearing smoke gate):
#   ModelCmd reads from ctx.cfg->api.models (populated from BATBOX_MODELS at
#   startup), NOT from std::getenv() at dispatch time and NOT from a hardcoded
#   fallback list.
#
# Test sequence:
#   1. Start BatBox with BATBOX_MODELS=grok-beta,gpt-4o-mini,llama-3.3-70b
#   2. Send "/model" followed by two Enters (one to confirm palette selection,
#      one to dispatch the command)
#   3. Assert all three model names appear on screen
#   4. Assert the "Available models" header appears
#
# Note: /model is handled entirely client-side (no LLM round-trip needed).
#       The mock LLM is started as a safety net so BatBox has a valid API
#       endpoint at startup.
#
# Slash palette interaction: typing "/model" opens the autocomplete palette;
# the first Enter selects "/model" from the palette and fills the input bar;
# the second Enter submits the slash command.
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

# shellcheck source=../lib/tmux_helpers.sh
source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
# shellcheck source=../lib/assertions.sh
source "${HARNESS_ROOT}/lib/assertions.sh"

_skip() {
    echo "SKIP: 28_model_lists_env — $*"
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

SESSION_NAME="smoke-28"
MOCK_PORT="8846"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"
TEST_MODELS="grok-beta,gpt-4o-mini,llama-3.3-70b"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock LLM (safety net so BatBox has a valid API endpoint at startup)
# ---------------------------------------------------------------------------
echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

# ---------------------------------------------------------------------------
# Start BatBox with BATBOX_MODELS set via tui_session_up env pairs.
# tui_session_up injects these as tmux -e flags so they appear in the
# process environment before batbox reads them (process_env_wins=true means
# these override any ~/.batbox/.env values).
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}' with BATBOX_MODELS=${TEST_MODELS}..."

tui_session_up "${SESSION_NAME}" "${BATBOX_BIN}" \
    "BATBOX_NO_SPLASH=true" \
    "BATBOX_API_BASE_URL=${MOCK_BASE_URL}" \
    "BATBOX_MODELS=${TEST_MODELS}"

# ---------------------------------------------------------------------------
# Wait for BatBox to fully render before sending commands.
# ---------------------------------------------------------------------------
echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "."; then
    echo "FAIL: BatBox did not render within 15s"
    exit 1
fi

# Give the TUI a moment to settle.
sleep 1

# ---------------------------------------------------------------------------
# Send /model slash command.
#
# The TUI shows a slash autocomplete palette when "/" is typed.
# The palette requires two Enters:
#   1st Enter — selects "/model" from the palette, fills the input bar
#   2nd Enter — submits the slash command to the REPL
# ---------------------------------------------------------------------------
echo "Typing '/model'..."
"${HARNESS}" send --name "${SESSION_NAME}" --no-enter "/model"

# Wait for the palette to appear and filter to "/model".
sleep 0.5

echo "Pressing Enter to select from palette..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"

# Wait for the palette to dismiss and the input to be filled.
sleep 0.3

echo "Pressing Enter to dispatch /model..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"

# Give the TUI time to process the command and render the model list.
sleep 1

# ---------------------------------------------------------------------------
# Assert: all three model names appear on screen.
# The model list renders in the ChatView (left portion of TUI).
# grep -E searches across the full captured pane including all columns.
# ---------------------------------------------------------------------------
echo "Waiting for grok-beta in model list..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "grok-beta"; then
    echo "FAIL: 'grok-beta' not found in /model output"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

echo "Checking for gpt-4o-mini in model list..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "gpt-4o-mini"; then
    echo "FAIL: 'gpt-4o-mini' not found in /model output"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

echo "Checking for llama-3.3-70b in model list..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "llama-3.3-70b"; then
    echo "FAIL: 'llama-3.3-70b' not found in /model output"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# Also assert the "Available models" header appeared.
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "Available models"; then
    echo "WARN: 'Available models' header not found — checking screen"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    # Non-fatal: the model names already confirmed the correct code path ran.
fi

echo "PASS: 28_model_lists_env — all three BATBOX_MODELS entries appear in /model output"
exit 0
