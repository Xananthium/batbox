#!/usr/bin/env bash
# =============================================================================
# cases/26_placeholder_rotation.sh — TUI-FLOW-T9: contextual placeholder rotation
#
# The smoke harness always sets BATBOX_NO_SPLASH=true which prevents the
# SplashBanner from mounting, which in turn means the placeholder code path
# is never triggered during harness-driven sessions.
#
# This test therefore validates the binary starts cleanly (regression guard)
# and emits a NOTE about the placeholder being observable only in live sessions.
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

_skip() {
    echo "SKIP: 26_placeholder_rotation — $*"
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
SESSION_NAME="smoke-26"
MOCK_PORT="8861"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock LLM and BatBox session
# ---------------------------------------------------------------------------
echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

# ---------------------------------------------------------------------------
# [A] Verify binary starts and renders (BATBOX_NO_SPLASH=true is set by harness
#     so splash/placeholder is suppressed — just confirm clean startup)
# ---------------------------------------------------------------------------
echo "Waiting for BatBox to render (no-splash mode)..."
# Wait for the status bar (always visible, regardless of splash setting)
# Wait for the prompt prefix "> " which is always rendered in no-splash mode
sleep 2   # give the binary time to start and render
SNAP_EARLY=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
if [ -z "${SNAP_EARLY}" ]; then
    echo "FAIL [A]: pane empty after 2s — binary may have crashed"
    exit 1
fi

echo "--- Pane ---"
echo "${SNAP_EARLY}"
echo "--- End pane ---"
SNAP="${SNAP_EARLY}"
echo "PASS [A]: binary starts and renders without crash"

# ---------------------------------------------------------------------------
# NOTE: Placeholder rotation (TUI-FLOW-T9 acceptance criteria AC1–AC4) is
# validated by the unit tests in test_input_bar.cpp (T9-AC1 through T9-AC6).
# The smoke harness always sets BATBOX_NO_SPLASH=true which suppresses the
# splash banner and placeholder code path.  To observe rotation manually:
#   BATBOX_NO_SPLASH=false BATBOX_API_BASE_URL=... ./build/src/batbox
# ---------------------------------------------------------------------------
echo "NOTE: Placeholder rotation coverage is in unit tests T9-AC1..T9-AC6"
echo "PASS: 26_placeholder_rotation"
exit 0
