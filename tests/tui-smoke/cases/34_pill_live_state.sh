#!/usr/bin/env bash
# =============================================================================
# cases/34_pill_live_state.sh — UX-C: status pill reads live state
#
# Reproduces the user-reported bug from the live TUI session and asserts the
# fix:
#
#   Bug #1 (stale model in pill):
#     After /model picker switches the active model, the bottom status pill
#     ("◉ <model> · <N>tk · $<X.XXX> · <mode>") must show the NEW model name
#     on the next render frame.  Pre-fix it showed the launch-time default.
#
#   Bug #2 (tokens dead):
#     After a streaming turn that returns a usage block, the pill token count
#     must be non-zero.  Pre-fix it was stuck at 0tk regardless of activity.
#
# Test sequence:
#   1. Start mock_model_recorder on port 8854 (records "model" field of every
#      /v1/chat/completions request to /tmp/batbox-qa-model-log-34.txt; emits
#      a terminal chunk with usage so on_usage_delta_cb_ fires).
#   2. Launch BatBox with BATBOX_MODELS=alpha-model,beta-model,gamma-model
#      and BATBOX_DEFAULT_MODEL=alpha-model.
#   3. Wait for TUI to render; verify initial pill shows "◉ alpha-model".
#   4. Open /model picker (type "/model", Enter, Enter).
#   5. Navigate Down, Down → gamma-model; press Enter to confirm.
#   6. ASSERT #1: pill row shows "◉ gamma-model" (UX-C bug #1 fix).
#   7. Send a chat message "ping"; wait for mock response "Hi".
#   8. ASSERT #2: recorder log shows gamma-model as the request's model field
#      (confirms /model also re-routed inference).
#   9. ASSERT #3: pill token count is non-zero, matching [1-9][0-9]*tk.
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
    echo "SKIP: 34_pill_live_state — $*"
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

SESSION_NAME="smoke-34"
MOCK_PORT="8854"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"
MODEL_LOG="/tmp/batbox-qa-model-log-34.txt"
RECORDER_PID_FILE="/tmp/batbox-qa-mock-model-recorder-34.pid"
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
# Start mock_model_recorder
# ---------------------------------------------------------------------------
echo "Starting mock_model_recorder on port ${MOCK_PORT}..."
python3 "${RECORDER_SCRIPT}" \
    --port "${MOCK_PORT}" \
    --log-file "${MODEL_LOG}" \
    > /tmp/batbox-qa-mock-model-recorder-34.log 2>&1 &

RECORDER_PID=$!

# Wait up to 3s for PID file; if it doesn't appear under our renamed path,
# fall back to writing the captured PID so cleanup() can stop it.
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
# Launch BatBox with the three-model list (default = alpha-model).
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
# Sanity: initial pill shows alpha-model.
# ---------------------------------------------------------------------------
echo "Asserting initial pill shows alpha-model..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "alpha-model"; then
    echo "FAIL: initial pill does not show alpha-model"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# ---------------------------------------------------------------------------
# Open /model picker via slash palette.
# ---------------------------------------------------------------------------
echo "Typing '/model' to open palette..."
"${HARNESS}" send --name "${SESSION_NAME}" --no-enter "/model"
sleep 0.5

echo "Pressing Enter to select /model from palette..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 0.3

echo "Pressing Enter to dispatch '/model' (no args) — opens ModalPicker..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 0.7

# Navigate Down, Down → gamma-model (index 2).
echo "Pressing Down → beta-model..."
"${HARNESS}" key --name "${SESSION_NAME}" "Down"
sleep 0.2

echo "Pressing Down → gamma-model..."
"${HARNESS}" key --name "${SESSION_NAME}" "Down"
sleep 0.2

echo "Pressing Enter to confirm gamma-model selection..."
"${HARNESS}" key --name "${SESSION_NAME}" "Enter"
sleep 1

# Wait for the standard switch confirmation (proves commit_model_switch fired).
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "Switched to model 'gamma-model'"; then
    echo "FAIL: 'Switched to model gamma-model' not found in TUI output"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi
echo "OK: /model picker committed switch to gamma-model."

# ---------------------------------------------------------------------------
# ASSERT #1 — UX-C bug #1: pill model went LIVE.
#
# The status pill starts with "◉ " (Unicode FISHEYE U+25C9 + space).  Anchor
# on "◉ gamma-model" so we are matching the pill row specifically, not any
# previous echo of "gamma-model" in the chat history.
# ---------------------------------------------------------------------------
echo "Waiting for pill to show '◉ gamma-model' live (UX-C bug #1 assertion)..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "◉ gamma-model"; then
    echo "FAIL: UX-C bug #1 — pill still shows old model after /model switch"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi
echo "OK: pill shows gamma-model live."

# ---------------------------------------------------------------------------
# Send a chat message and verify the next inference request uses gamma-model.
# ---------------------------------------------------------------------------
echo "Sending chat message 'ping'..."
"${HARNESS}" send --name "${SESSION_NAME}" "ping"

echo "Waiting for mock response 'Hi'..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 20 "Hi"; then
    echo "FAIL: Mock response did not appear after chat turn"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi

# Let the recorder flush.
sleep 0.5

# ---------------------------------------------------------------------------
# ASSERT #2 — recorder log shows gamma-model (request layer is live too).
# ---------------------------------------------------------------------------
if [ ! -f "${MODEL_LOG}" ]; then
    echo "FAIL: Model log '${MODEL_LOG}' missing — no request was recorded"
    exit 1
fi

LAST_MODEL="$(tail -1 "${MODEL_LOG}" 2>/dev/null | tr -d '[:space:]')"
if [ -z "${LAST_MODEL:-}" ]; then
    echo "FAIL: Model log empty — inference request was not recorded"
    echo "--- recorder stdout ---"
    cat /tmp/batbox-qa-mock-model-recorder-34.log || true
    echo "--- end ---"
    exit 1
fi
if [ "${LAST_MODEL}" != "gamma-model" ]; then
    echo "FAIL: Expected last recorded model 'gamma-model', got '${LAST_MODEL}'"
    cat "${MODEL_LOG}" || true
    exit 1
fi
echo "OK: recorder confirms inference request used gamma-model."

# ---------------------------------------------------------------------------
# ASSERT #3 — UX-C bug #2: pill token count is non-zero.
#
# The mock now emits a terminal chunk with usage{prompt_tokens=5, completion_tokens=2}
# so on_usage_delta_cb_ should fire with total_tokens=7.  Pill format is "NNtk".
# We accept any [1-9][0-9]*tk match (i.e. "7tk", "12tk", anything but "0tk").
# ---------------------------------------------------------------------------
echo "Waiting for non-zero token count in pill (UX-C bug #2 assertion)..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "[1-9][0-9]*tk"; then
    echo "FAIL: UX-C bug #2 — pill tokens still 0 after streaming turn with usage"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    exit 1
fi
echo "OK: pill tokens > 0 after turn."

echo "PASS: 34_pill_live_state — pill model went live + tokens advanced."
exit 0
