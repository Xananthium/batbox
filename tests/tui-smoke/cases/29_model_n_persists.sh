#!/usr/bin/env bash
# =============================================================================
# cases/29_model_n_persists.sh — /model <name> switches the live active model
#
# PEXT3 1.3 load-bearing acceptance gate:
#   After the user sends `/model beta-model`, the next inference request must
#   use "beta-model" — NOT the original launch-time default "alpha-model".
#   This proves that ModelCmd mutates ctx.cfg->api.default_model under the
#   cfg_mutex so the change takes effect immediately in the running session.
#
# TUI note on interactive vs direct-arg form:
#   In TUI mode ctx.input is an empty istringstream, so the interactive picker
#   (/model with no arg) reaches EOF immediately and cannot accept numeric input.
#   The direct-arg form (/model <name>) is the correct TUI path.  Numeric-index
#   selection is exercised by test_model_cmd_live_mutation.cpp TEST_CASE 2.
#
# Slash palette interaction:
#   Typing "/model" opens the autocomplete palette.  The first Enter selects
#   "/model" from the palette and fills the input bar; typing " beta-model"
#   appends the argument; the second Enter dispatches "/model beta-model".
#   This is the same two-Enter pattern used by case 28, extended with an arg.
#
# Test sequence:
#   1. Start mock-model-recorder (port 8847) — records the "model" field of
#      every /v1/chat/completions request to /tmp/batbox-qa-model-log.txt.
#   2. Start BatBox with:
#        BATBOX_MODELS=alpha-model,beta-model
#        BATBOX_DEFAULT_MODEL=alpha-model
#   3. Wait for TUI to render.
#   4. Send "/model" (no Enter) → Enter (palette select) → type " beta-model"
#      → Enter (dispatch "/model beta-model").
#   5. Assert "Switched to model 'beta-model'" appears in the TUI output.
#   6. Send a plain chat message: "ping".
#   7. Wait for the chat response ("Hi!") to appear.
#   8. Assert the last line of /tmp/batbox-qa-model-log.txt is "beta-model".
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at ../../build/src/batbox
#   - tmux not installed
#   - python3 not available
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
    echo "SKIP: 29_model_n_persists — $*"
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

SESSION_NAME="smoke-29"
MOCK_PORT="8847"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"
MODEL_LOG="/tmp/batbox-qa-model-log.txt"
RECORDER_PID_FILE="/tmp/batbox-qa-mock-model-recorder.pid"
RECORDER_SCRIPT="${HARNESS_ROOT}/fixtures/mock_model_recorder.py"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    if [ -f "${RECORDER_PID_FILE}" ]; then
        _rec_pid="$(cat "${RECORDER_PID_FILE}")" 2>/dev/null || true
        [ -n "${_rec_pid:-}" ] && kill "${_rec_pid}" 2>/dev/null || true
        rm -f "${RECORDER_PID_FILE}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock-model-recorder
# ---------------------------------------------------------------------------
echo "Starting mock-model-recorder on port ${MOCK_PORT}..."
python3 "${RECORDER_SCRIPT}" \
    --port "${MOCK_PORT}" \
    --log-file "${MODEL_LOG}" \
    > /tmp/batbox-qa-mock-model-recorder.log 2>&1 &

RECORDER_PID=$!

# Wait up to 3s for PID file.
_deadline=$(( $(date +%s) + 3 ))
while [ "$(date +%s)" -le "$_deadline" ]; do
    [ -f "${RECORDER_PID_FILE}" ] && break
    sleep 0.1
done
if [ ! -f "${RECORDER_PID_FILE}" ]; then
    echo "${RECORDER_PID}" > "${RECORDER_PID_FILE}"
fi
sleep 0.3

# ---------------------------------------------------------------------------
# Start BatBox with two-model list; default is alpha-model.
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
tui_session_up "${SESSION_NAME}" "${BATBOX_BIN}" \
    "BATBOX_NO_SPLASH=true" \
    "BATBOX_API_BASE_URL=${MOCK_BASE_URL}" \
    "BATBOX_MODELS=alpha-model,beta-model" \
    "BATBOX_DEFAULT_MODEL=alpha-model"

echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "."; then
    echo "FAIL: BatBox did not render within 15s"
    exit 1
fi
sleep 1

# ---------------------------------------------------------------------------
# Switch model: /model beta-model via the slash palette.
#
# Step 1: type "/model" (no Enter) — palette opens, filters to /model.
# Step 2: Enter — palette selects /model, fills input bar with "/model ".
# Step 3: type " beta-model" — appends the argument (space already in bar or
#         prepended here for safety).
# Step 4: Enter — dispatches "/model beta-model".
# ---------------------------------------------------------------------------
echo "Typing '/model' to open palette..."
"${HARNESS}" send --name "${SESSION_NAME}" --no-enter "/model"
sleep 0.5

echo "Pressing Enter to select /model from palette..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 0.3

echo "Typing ' beta-model' as the argument..."
"${HARNESS}" send --name "${SESSION_NAME}" --no-enter " beta-model"
sleep 0.3

echo "Pressing Enter to dispatch '/model beta-model'..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 1

# ---------------------------------------------------------------------------
# Assert the switch confirmation appeared in the TUI.
# ---------------------------------------------------------------------------
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "Switched to model 'beta-model'"; then
    echo "FAIL: 'Switched to model beta-model' not found in TUI output"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi
echo "OK: TUI confirmed switch to beta-model."

# ---------------------------------------------------------------------------
# Send a chat message — triggers a real inference request.
# The request's model field must now be "beta-model".
# ---------------------------------------------------------------------------
echo "Sending chat message 'ping'..."
"${HARNESS}" send --name "${SESSION_NAME}" "ping"

echo "Waiting for mock response 'Hi!'..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 20 "Hi"; then
    echo "FAIL: Mock response 'Hi!' did not appear after chat turn"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# Give the recorder a moment to flush the log.
sleep 0.5

# ---------------------------------------------------------------------------
# Assert: the last recorded model in the log is "beta-model"
# ---------------------------------------------------------------------------
if [ ! -f "${MODEL_LOG}" ]; then
    echo "FAIL: Model log '${MODEL_LOG}' does not exist — no request was recorded"
    exit 1
fi

LAST_MODEL="$(tail -1 "${MODEL_LOG}" 2>/dev/null | tr -d '[:space:]')"

if [ -z "${LAST_MODEL}" ]; then
    echo "FAIL: Model log '${MODEL_LOG}' is empty — inference request was not recorded"
    echo "--- recorder log ---"
    cat /tmp/batbox-qa-mock-model-recorder.log || true
    echo "--- end ---"
    exit 1
fi

if [ "${LAST_MODEL}" != "beta-model" ]; then
    echo "FAIL: Expected last recorded model 'beta-model', got '${LAST_MODEL}'"
    echo "--- model log ---"
    cat "${MODEL_LOG}" || true
    echo "--- end ---"
    exit 1
fi

echo "PASS: 29_model_n_persists — /model switch took effect; inference used '${LAST_MODEL}'"
exit 0
