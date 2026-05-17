#!/usr/bin/env bash
# =============================================================================
# cases/17_plan_no_double_render.sh — TUI-PLAN-T3 regression: plan text
# must NOT render twice after streaming + ExitPlanMode tool call.
#
# Root cause (now fixed): streaming_buffer_ was never cleared when a
# MessageAppended("assistant") event fired mid-tool-call loop, so the plan
# text accumulated in the buffer got committed a second time at StreamDone.
#
# Strategy:
#   1. Start mock_plan_mode.py on port 8849.  Turn 1 streams "# Plan\nstep
#      1\nstep 2" then emits ExitPlanMode (finish_reason=tool_calls). Turn 2
#      returns "Plan submitted." (finish_reason=stop).
#   2. Start BatBox in --nuclear mode (auto-approves tool calls).
#   3. Submit "make a plan".
#   4. Poll up to 10s for "step 1" to appear in the live streaming tail.
#      If it appears, record the count (should be 1 during streaming).
#   5. Wait up to 25s for "Plan submitted." to confirm both turns finished.
#   6. Capture the final pane and assert "step 1" appears AT MOST once
#      (0 = live tail cleared cleanly, 1 = committed cleanly, 2+ = double render).
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
    echo "SKIP: 17_plan_no_double_render — $*"
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
SESSION_NAME="smoke-17"
MOCK_PORT="8849"
MOCK_PID_FILE="/tmp/batbox-qa-mock-plan-mode.pid"
MOCK_LOG="/tmp/batbox-qa-mock-plan-mode.log"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    if [ -f "${MOCK_PID_FILE}" ]; then
        local pid
        pid="$(cat "${MOCK_PID_FILE}" 2>/dev/null || true)"
        if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null || true
        fi
        rm -f "${MOCK_PID_FILE}"
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock plan-mode server
# ---------------------------------------------------------------------------
echo "Starting mock plan-mode server on port ${MOCK_PORT}..."
python3 "${HARNESS_ROOT}/fixtures/mock_plan_mode.py" \
    --port "${MOCK_PORT}" \
    > "${MOCK_LOG}" 2>&1 &

MOCK_PID=$!

# Wait up to 3s for PID file to appear
DEADLINE=$(( $(date +%s) + 3 ))
while [ "$(date +%s)" -le "${DEADLINE}" ]; do
    [ -f "${MOCK_PID_FILE}" ] && break
    sleep 0.1
done
[ -f "${MOCK_PID_FILE}" ] || echo "${MOCK_PID}" > "${MOCK_PID_FILE}"

echo "OK: mock plan-mode started (pid=${MOCK_PID})"

# ---------------------------------------------------------------------------
# Start BatBox
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up \
    --name "${SESSION_NAME}" \
    --api-base "${MOCK_BASE_URL}" \
    --nuclear

if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Submit prompt
# ---------------------------------------------------------------------------
echo "Sending prompt..."
"${HARNESS}" send --name "${SESSION_NAME}" "make a plan"

# ---------------------------------------------------------------------------
# Poll for "step 1" to appear in the live streaming tail (optional check)
# ---------------------------------------------------------------------------
echo "Polling for 'step 1' during streaming (up to 5s)..."
SAW_STEP1_DURING_STREAM=0
for i in $(seq 1 10); do
    sleep 0.5
    SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
    if echo "${SNAP}" | grep -qi "step 1"; then
        SAW_STEP1_DURING_STREAM=1
        COUNT_DURING=$(echo "${SNAP}" | grep -ic "step 1" || true)
        if [ "${COUNT_DURING}" -gt 1 ]; then
            echo "FAIL: 'step 1' appeared ${COUNT_DURING} times during streaming — DOUBLE RENDER"
            echo "--- Pane during stream ---"
            echo "${SNAP}"
            exit 1
        fi
        echo "INFO: 'step 1' appeared ${COUNT_DURING} time(s) during streaming (as expected)"
        break
    fi
done

if [ "${SAW_STEP1_DURING_STREAM}" -eq 0 ]; then
    echo "INFO: 'step 1' not captured during streaming window (mock may be too fast) — skipping in-flight check"
fi

# ---------------------------------------------------------------------------
# Wait for both turns to complete (look for follow-up "Plan submitted." OR
# tool error indicating ExitPlanMode was attempted)
# ---------------------------------------------------------------------------
echo "Waiting for conversation to complete (up to 25s)..."
DONE=0
for i in $(seq 1 50); do
    sleep 0.5
    SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
    # Either the follow-up "Plan submitted." or the ExitPlanMode tool error
    if echo "${SNAP}" | grep -qi "submitted\|Planning state\|ExitPlanMode"; then
        DONE=1
        break
    fi
done

if [ "${DONE}" -eq 0 ]; then
    echo "FAIL: conversation did not complete within 25s"
    echo "--- Final pane output ---"
    "${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true
    exit 1
fi

# Give the UI one render cycle to settle
sleep 0.3
FINAL_SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)

# ---------------------------------------------------------------------------
# Assert: "step 1" appears AT MOST ONCE (0 is fine — streamed tail cleared;
# >1 means double render regression)
# ---------------------------------------------------------------------------
STEP1_COUNT=$(echo "${FINAL_SNAP}" | grep -ic "step 1" || true)

if [ "${STEP1_COUNT}" -gt 1 ]; then
    echo "FAIL: 'step 1' appears ${STEP1_COUNT} times in final pane — DOUBLE RENDER (TUI-PLAN-T3 regression)"
    echo "--- Final pane ---"
    echo "${FINAL_SNAP}"
    exit 1
fi

echo "PASS: 'step 1' count in final pane = ${STEP1_COUNT} (no double render)"

STEP2_COUNT=$(echo "${FINAL_SNAP}" | grep -ic "step 2" || true)
if [ "${STEP2_COUNT}" -gt 1 ]; then
    echo "FAIL: 'step 2' appears ${STEP2_COUNT} times in final pane — DOUBLE RENDER"
    echo "--- Final pane ---"
    echo "${FINAL_SNAP}"
    exit 1
fi

echo "PASS: 'step 2' count in final pane = ${STEP2_COUNT} (no double render)"
echo "PASS: 17_plan_no_double_render"
exit 0
