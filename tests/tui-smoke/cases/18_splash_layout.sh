#!/usr/bin/env bash
# =============================================================================
# cases/18_splash_layout.sh — TUI-FLOW-T4: SplashBanner layout smoke test
#
# Asserts that on launch (before the first prompt is sent):
#   1. The screen contains "BatBox v" (the titled border box)
#   2. The screen contains "Tips for getting started" (right panel)
#
# After sending a message, asserts that "Tips for getting started" disappears
# (splash has collapsed to the single "welcome back!" line).
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
    echo "SKIP: 18_splash_layout — $*"
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

SESSION_NAME="smoke-18"
MOCK_PORT="8842"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" --api-base "${MOCK_BASE_URL}"

# ---------------------------------------------------------------------------
# Assert 1: Initial frame shows "BatBox v" (acceptance criterion 1)
# ---------------------------------------------------------------------------
echo "Waiting for BatBox to render initial splash..."
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "BatBox v"; then
    echo "FAIL: 'BatBox v' not found in initial frame"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assert 2: Initial frame shows "Tips for getting started" (acceptance criterion 1)
# ---------------------------------------------------------------------------
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 5 "Tips for getting"; then
    echo "FAIL: 'Tips for getting started' not found in initial frame"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "PASS (initial frame): BatBox v and Tips for getting started are present"

# ---------------------------------------------------------------------------
# Assert 3: After first submit, splash collapses (acceptance criterion 4)
# Send a message and assert "Tips for getting started" disappears
# ---------------------------------------------------------------------------
echo "Sending first message to trigger splash collapse..."
"${HARNESS}" send --name "${SESSION_NAME}" "hello"

# Wait for user message to appear in chat
if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 15 "hello"; then
    echo "FAIL: user message did not appear"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

# Allow a render frame for collapse to propagate
sleep 0.5

# The full tips panel should now be gone (collapsed to single "welcome back!" line)
# We check that "Tips for getting" is no longer visible
if "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 2 "Tips for getting"; then
    echo "FAIL: 'Tips for getting started' still visible after first submit (splash did not collapse)"
    "${HARNESS}" screen --name "${SESSION_NAME}"
    exit 1
fi

echo "PASS: 18_splash_layout — splash collapses correctly after first submit"
exit 0
