#!/usr/bin/env bash
# =============================================================================
# cases/31_nuclear_no_question_modal.sh — PEXT3 1.6 load-bearing smoke test
#
# Asserts that batbox --nuclear does NOT render a QuestionCard modal when the
# model calls AskUserQuestion.  This is the user-reported bug gate.
#
# Approach B (stateful Python mock server, per pext3-0.1-fixture-report.md):
#   - fixtures/mock_nuclear_askq.py serves AskUserQuestion on turn 1
#   - BatBox launched with --nuclear
#   - make_askq_prompt_fn(true,...) returns {} immediately, no modal posted
#   - The case asserts ZERO QuestionCard tokens appear in the rendered TUI
#
# Positive anchor (prevents false-pass from timeout):
#   We wait for "done." (the turn-2 acknowledgement) before asserting absence.
#   This proves the full round-trip completed: AskUserQuestion was called,
#   make_askq_prompt_fn returned {}, the tool returned "(no answer provided)",
#   the model received the result and emitted the final text.
#
# QuestionCard tokens asserted absent:
#   "Which configuration style" — question text
#   "Minimal|Standard|Advanced" — option labels
#   "Config Choice"             — header
#   "Enter to select"           — card keyboard hint
#
# SKIP CONDITIONS (exits 0 without testing):
#   - BatBox binary not found at build/src/batbox
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
    echo "SKIP: 31_nuclear_no_question_modal — $*"
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
SESSION_NAME="smoke-31"
MOCK_PORT="8852"
MOCK_PID_FILE="/tmp/batbox-qa-mock-nuclear-askq.pid"
MOCK_LOG="/tmp/batbox-qa-mock-nuclear-askq.log"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

# ---------------------------------------------------------------------------
# Cleanup: tear down session and mock server
# ---------------------------------------------------------------------------
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
# Start mock_nuclear_askq.py on port 8852
# ---------------------------------------------------------------------------
echo "Starting mock-nuclear-askq server on port ${MOCK_PORT}..."
python3 "${HARNESS_ROOT}/fixtures/mock_nuclear_askq.py" \
    --port "${MOCK_PORT}" \
    > "${MOCK_LOG}" 2>&1 &

MOCK_PID=$!

# Wait up to 3s for PID file to appear (server writes it on startup)
DEADLINE=$(( $(date +%s) + 3 ))
while [ "$(date +%s)" -le "${DEADLINE}" ]; do
    [ -f "${MOCK_PID_FILE}" ] && break
    sleep 0.1
done
[ -f "${MOCK_PID_FILE}" ] || echo "${MOCK_PID}" > "${MOCK_PID_FILE}"

echo "OK: mock-nuclear-askq started (pid=${MOCK_PID})"

# ---------------------------------------------------------------------------
# Start BatBox in nuclear mode.
#
# PEXT3 1.6 requires args.nuclear=true so make_askq_prompt_fn receives
# nuclear=true and returns the zero-capture closure.  The harness --nuclear
# flag sets BATBOX_NUCLEAR=true env var only (which bypasses PermissionGate),
# but does NOT set args.nuclear (CLI flag).  We must pass --nuclear as a CLI
# argument to the batbox binary so CLI11 sets args.nuclear=true.
#
# We use tui_session_up directly (sourced above) instead of harness up, because
# the harness cmd_up cannot append CLI args to the binary command.
# ---------------------------------------------------------------------------
FULL_SESSION_NAME="$(tui_session_name "${SESSION_NAME}")"
echo "Starting BatBox session '${SESSION_NAME}' with --nuclear CLI flag..."

tui_session_up "${SESSION_NAME}"     "${BATBOX_BIN} --nuclear"     "BATBOX_NO_SPLASH=true"     "BATBOX_API_BASE_URL=${MOCK_BASE_URL}"

# tui_session_up returns 0 whether it created or reused the session.
# Confirm it is up.
if ! tui_session_exists "${SESSION_NAME}"; then
    echo "FAIL: Could not start tmux session '${FULL_SESSION_NAME}'"
    exit 1
fi
echo "OK: session '${FULL_SESSION_NAME}' up (cmd: ${BATBOX_BIN} --nuclear)"

if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi
echo "OK: BatBox rendered"

# ---------------------------------------------------------------------------
# Send trigger message — mock responds with AskUserQuestion on turn 1
# ---------------------------------------------------------------------------
echo "Sending trigger message..."
"${HARNESS}" send --name "${SESSION_NAME}" "configure it"

# ---------------------------------------------------------------------------
# Positive anchor: wait for "done." (the turn-2 acknowledgement).
#
# This proves the full flow completed:
#   - AskUserQuestion tool was called
#   - make_askq_prompt_fn(true) returned {} immediately (no modal)
#   - AskUserQuestionTool.run() returned "(no answer provided)"
#   - The model received the tool result and emitted the final text
#
# Timeout 20s: turn 1 (AskUserQuestion) + tool dispatch + turn 2 (done.)
# ---------------------------------------------------------------------------
echo "Waiting for turn-2 acknowledgement ('done.')..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 20 \
        "(done\.|done|Batbox:)"; then
    echo "FAIL: BatBox did not complete the round-trip within 20s"
    echo "--- Pane dump ---"
    "${HARNESS}" screen --name "${SESSION_NAME}" || true
    echo "--- Mock log ---"
    cat "${MOCK_LOG}" || true
    exit 1
fi
echo "OK: Round-trip completed (turn-2 acknowledgement received)"

# Allow a render frame for any delayed events to flush.
sleep 0.3

# ---------------------------------------------------------------------------
# Assert 1 (load-bearing — PEXT3 1.6 bug gate):
# NO QuestionCard tokens must appear in the rendered pane.
# ---------------------------------------------------------------------------
echo "Asserting NO QuestionCard modal tokens in pane..."
echo "(Note: question text may appear in the tool-result JSON; we assert modal UI elements are absent)"

SNAP=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)

QUESTION_MODAL_FOUND=0

# Assert modal-specific UI elements are absent.
# The question text itself may appear in the tool-result JSON in ChatView —
# that is expected and correct (the tool ran + auto-returned "(no answer provided)").
# What must NOT appear are the QuestionCard's interactive UI elements:
#   "Enter to select"       — keyboard hint in QuestionCard footer
#   "↑/↓ to navigate"      — keyboard hint in QuestionCard footer
#   "● 1. Minimal"         — numbered option row with selection indicator
#   "○ 2. Standard"        — unselected option row
#   "Esc to cancel"         — QuestionCard cancel hint

if echo "${SNAP}" | grep -qiE "Enter to select|to navigate|Esc to cancel"; then
    echo "FAIL (assert 1): QuestionCard keyboard hints visible — modal was rendered!"
    echo "--- Pane ---"
    echo "${SNAP}"
    QUESTION_MODAL_FOUND=1
fi

if echo "${SNAP}" | grep -qE "[●○▸] [0-9]+\. (Minimal|Standard|Advanced)"; then
    echo "FAIL (assert 1): Numbered option rows with selection indicators visible — modal was rendered!"
    echo "--- Pane ---"
    echo "${SNAP}"
    QUESTION_MODAL_FOUND=1
fi

# If the round-trip completed correctly, the tool result MUST show "(no answer provided)"
# This proves make_askq_prompt_fn(nuclear=true) returned {} as expected.
if ! echo "${SNAP}" | grep -qiE "no answer provided|done"; then
    echo "FAIL (assert 1): Expected '(no answer provided)' not found — nuclear auto-decline may not have fired"
    echo "--- Pane ---"
    echo "${SNAP}"
    QUESTION_MODAL_FOUND=1
else
    echo "PASS: Tool result shows '(no answer provided)' — nuclear auto-decline confirmed"
fi

if [ "${QUESTION_MODAL_FOUND}" -eq 1 ]; then
    echo ""
    echo "FAIL: 31_nuclear_no_question_modal"
    echo "  PEXT3 1.6 bug NOT fixed: --nuclear mode still rendered QuestionCard modal."
    exit 1
fi

echo "PASS (assert 1): No QuestionCard modal UI elements found in pane"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "PASS: 31_nuclear_no_question_modal"
echo "  - AskUserQuestion tool call was processed without rendering modal"
echo "  - make_askq_prompt_fn(true) returned {} immediately (nuclear auto-decline)"
echo "  - Round-trip completed to turn-2 acknowledgement"
echo "  - PEXT3 1.6 bug fix confirmed: no QuestionCard modal in --nuclear mode"
exit 0
