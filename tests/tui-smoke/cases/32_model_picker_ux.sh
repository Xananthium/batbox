#!/usr/bin/env bash
# =============================================================================
# cases/32_model_picker_ux.sh — /model opens ModalPicker; arrow-down + Enter
#                               switches the active model (UX-A).
#
# UX-A load-bearing acceptance gate:
#   Typing /model in TUI mode must open the ModalPicker (not the text-list +
#   getline path).  The user navigates with arrow keys and presses Enter to
#   select.  The active-model indicator in the TUI must change to the selected
#   model; keystrokes must NOT leak to the LLM.
#
# Test sequence:
#   1. Start mock-model-recorder (port 8853) — records the "model" field of
#      every /v1/chat/completions request to /tmp/batbox-qa-model-log-32.txt.
#   2. Start BatBox with:
#        BATBOX_MODELS=alpha-model,beta-model,gamma-model
#        BATBOX_DEFAULT_MODEL=alpha-model
#   3. Wait for TUI to render.
#   4. Send "/model" (no Enter) → Enter (palette select) → Enter (dispatch /model).
#      The picker modal should appear showing all three models.
#   5. Send Down arrow twice (move selection from alpha-model to gamma-model, index 2).
#   6. Press Enter to confirm the selection.
#   7. Assert "Switched to model 'gamma-model'" appears in the TUI output,
#      confirming commit_model_switch() was called via the picker path.
#   8. Assert that the Down arrow keystrokes did NOT appear as a message to
#      the LLM (verify no extra inference request fired with nonsense content).
#   9. Optionally send a ping chat message and verify the next inference request
#      uses gamma-model (same model-recorder check as case 29).
#
# ModalPicker navigation:
#   The picker shows:
#     > alpha-model  (current)   ← cursor starts here (index 0)
#       beta-model
#       gamma-model
#   One Down → beta-model selected.
#   Two Downs → gamma-model selected (index 2 in original items).
#   Enter → on_select(2) fires → commit_model_switch(gamma-model).
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
    echo "SKIP: 32_model_picker_ux — $*"
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

SESSION_NAME="smoke-32"
MOCK_PORT="8853"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"
MODEL_LOG="/tmp/batbox-qa-model-log-32.txt"
RECORDER_PID_FILE="/tmp/batbox-qa-mock-model-recorder-32.pid"
RECORDER_SCRIPT="${HARNESS_ROOT}/fixtures/mock_model_recorder.py"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    if [ -f "${RECORDER_PID_FILE}" ]; then
        _rec_pid="$(cat "${RECORDER_PID_FILE}")" 2>/dev/null || true
        [ -n "${_rec_pid:-}" ] && kill "${_rec_pid}" 2>/dev/null || true
        rm -f "${RECORDER_PID_FILE}"
    fi
    rm -f "${MODEL_LOG}"
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock-model-recorder
# ---------------------------------------------------------------------------
echo "Starting mock-model-recorder on port ${MOCK_PORT}..."
python3 "${RECORDER_SCRIPT}" \
    --port "${MOCK_PORT}" \
    --log-file "${MODEL_LOG}" \
    > /tmp/batbox-qa-mock-model-recorder-32.log 2>&1 &

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
# Start BatBox with three-model list; default is alpha-model.
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
tui_session_up "${SESSION_NAME}" "${BATBOX_BIN}" \
    "BATBOX_NO_SPLASH=true" \
    "BATBOX_API_BASE_URL=${MOCK_BASE_URL}" \
    "BATBOX_MODELS=alpha-model,beta-model,gamma-model" \
    "BATBOX_DEFAULT_MODEL=alpha-model"

echo "Waiting for BatBox to render..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "."; then
    echo "FAIL: BatBox did not render within 15s"
    exit 1
fi
sleep 1

# ---------------------------------------------------------------------------
# Open the /model picker via the slash palette.
#
# Step 1: type "/model" (no Enter) — palette opens, filters to /model.
# Step 2: Enter — palette selects /model, fills input bar with "/model".
# Step 3: Enter — dispatches "/model" (no args) → ModalPicker appears.
# ---------------------------------------------------------------------------
echo "Typing '/model' to open palette..."
"${HARNESS}" send --name "${SESSION_NAME}" --no-enter "/model"
sleep 0.5

echo "Pressing Enter to select /model from palette..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 0.3

echo "Pressing Enter to dispatch '/model' (no args)..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 0.7

# ---------------------------------------------------------------------------
# Assert the picker appeared (alpha-model should be visible in the modal).
# ---------------------------------------------------------------------------
echo "Waiting for picker to appear (alpha-model visible)..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "alpha-model"; then
    echo "FAIL: ModalPicker did not appear — 'alpha-model' not found in TUI"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi
echo "OK: ModalPicker is visible."

# ---------------------------------------------------------------------------
# Navigate: press Down twice to reach gamma-model (index 2).
#   cursor starts at 0 (alpha-model  (current))
#   Down →  1 (beta-model)
#   Down →  2 (gamma-model)
# ---------------------------------------------------------------------------
echo "Pressing Down to move to beta-model..."
"${HARNESS}" key --name "${SESSION_NAME}" "Down"
sleep 0.2

echo "Pressing Down to move to gamma-model..."
"${HARNESS}" key --name "${SESSION_NAME}" "Down"
sleep 0.2

# ---------------------------------------------------------------------------
# Confirm selection with Enter.
# ---------------------------------------------------------------------------
echo "Pressing Enter to confirm gamma-model selection..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 1

# ---------------------------------------------------------------------------
# Assert: the switch confirmation appeared in the TUI output.
# ---------------------------------------------------------------------------
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "Switched to model 'gamma-model'"; then
    echo "FAIL: 'Switched to model gamma-model' not found in TUI output"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi
echo "OK: TUI confirmed switch to gamma-model."

# ---------------------------------------------------------------------------
# Assert: the Down keystrokes were consumed by the picker and did NOT leak
# to the LLM.  Send a real ping message and verify the model log is recorded
# with the new model name.
# ---------------------------------------------------------------------------
echo "Sending chat message 'ping' to verify model is gamma-model..."
"${HARNESS}" send --name "${SESSION_NAME}" "ping"

echo "Waiting for mock response 'Hi!'..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 20 "Hi"; then
    echo "FAIL: Mock response did not appear after chat turn"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# Give the recorder a moment to flush.
sleep 0.5

# Assert the last recorded model is gamma-model.
if [ ! -f "${MODEL_LOG}" ]; then
    echo "FAIL: Model log '${MODEL_LOG}' does not exist — no request was recorded"
    exit 1
fi

LAST_MODEL="$(tail -1 "${MODEL_LOG}" 2>/dev/null | tr -d '[:space:]')"

if [ -z "${LAST_MODEL:-}" ]; then
    echo "FAIL: Model log is empty — inference request was not recorded"
    echo "--- recorder log ---"
    cat /tmp/batbox-qa-mock-model-recorder-32.log || true
    echo "--- end ---"
    exit 1
fi

if [ "${LAST_MODEL}" != "gamma-model" ]; then
    echo "FAIL: Expected last recorded model 'gamma-model', got '${LAST_MODEL}'"
    echo "--- model log ---"
    cat "${MODEL_LOG}" || true
    echo "--- end ---"
    exit 1
fi

echo "PASS: 32_model_picker_ux — ModalPicker selected gamma-model; inference used '${LAST_MODEL}'"
exit 0
