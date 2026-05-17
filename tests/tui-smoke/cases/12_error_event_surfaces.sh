#!/usr/bin/env bash
# =============================================================================
# cases/12_error_event_surfaces.sh — TUI-T19 smoke test
#
# Drives the real build/src/batbox binary (--print mode) against
# mock_reasoning_only.py in --mode error-event, which reproduces the exact
# server behaviour that TUI-T17 handles:
#
#   Server returns HTTP 200 with a single SSE event:
#     event: error
#     data: {"error":{"message":"The number of tokens to keep from the
#            initial prompt is greater than the context length..."}}
#   Then closes the socket cleanly — no [DONE], no content.
#
# Expected behaviour AFTER TUI-T17:
#   - batbox --print exits NON-ZERO (exit code 1)
#   - stderr contains "server:"          (TUI-T17 prefix proves the new path)
#   - stderr contains "context length"   (the server's actual message arrived)
#   - stderr does NOT contain "BATBOX_MAX_TOKENS"
#     (proves T12's generic guard is NOT firing; T17's specific path took
#     precedence — the generic guard would wrongly tell the user to increase
#     BATBOX_MAX_TOKENS when the correct advice is to reduce prompt size)
#
# This case is a HEADLESS test — it invokes batbox --print directly, not
# through a tmux TUI session.  The --print path and TUI path share the same
# Client::stream_chat / Conversation::run_turn error surface; --print is
# deterministic and requires no tmux pane-capture polling.
#
# SKIP CONDITIONS (exits 0 without failing):
#   - build/src/batbox binary not present or not executable
#   - python3 not available
#
# Note: tmux is NOT required for this case.
#
# Registered in CMakeLists.txt as tui_smoke_12_error_event_surfaces under
# the "tui-smoke" label.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Resolve paths
# ---------------------------------------------------------------------------
_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 12_error_event_surfaces — $*"
    exit 0
}

# python3 required to run the mock
if ! command -v python3 >/dev/null 2>&1; then
    _skip "python3 not available"
fi

# BatBox binary required
BATBOX_BIN="${HARNESS_ROOT}/../../build/src/batbox"
BATBOX_BIN="$(cd "$(dirname "${BATBOX_BIN}")" 2>/dev/null && pwd)/$(basename "${BATBOX_BIN}")" 2>/dev/null || true
if [ ! -x "${BATBOX_BIN}" ]; then
    _skip "BatBox binary not found at build/src/batbox — build first"
fi

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
MOCK_PORT="8828"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

# ---------------------------------------------------------------------------
# Cleanup on exit
# ---------------------------------------------------------------------------
cleanup() {
    "${HARNESS}" mock-reasoning stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start the mock server in error-event mode
# ---------------------------------------------------------------------------
echo "Starting mock-reasoning in error-event mode on port ${MOCK_PORT}..."
"${HARNESS}" mock-reasoning start --port "${MOCK_PORT}" --mode error-event

# One tick of slack to ensure the accept loop is ready.
sleep 0.2

# ---------------------------------------------------------------------------
# Run batbox --print against the mock.
# Redirect stderr to stdout so we can capture everything together.
# Allow non-zero exit (we expect it).
# ---------------------------------------------------------------------------
echo "Running: batbox --print 'hello' -> ${MOCK_BASE_URL}"

COMBINED_OUT=""
EXIT_CODE=0

COMBINED_OUT="$(
    BATBOX_API_BASE_URL="${MOCK_BASE_URL}" \
    BATBOX_API_KEY="test" \
    BATBOX_NO_SPLASH=true \
        "${BATBOX_BIN}" --print "hello" 2>&1
)" || EXIT_CODE=$?

echo "Exit code: ${EXIT_CODE}"
echo "Output: ${COMBINED_OUT}"

# ---------------------------------------------------------------------------
# Assertion 1: exit code must be non-zero
# ---------------------------------------------------------------------------
if [ "${EXIT_CODE}" -eq 0 ]; then
    echo "FAIL: batbox exited 0 (expected non-zero)"
    echo "  TUI-T17's error-event handler may not be active."
    echo "  Output was: ${COMBINED_OUT}"
    exit 1
fi

echo "PASS: batbox exited non-zero (${EXIT_CODE})"

# ---------------------------------------------------------------------------
# Assertion 2: output must contain "server:" prefix
# Proves TUI-T17's branch fired (it prefixes the server message with "server:")
# rather than the generic T12 guard.
# ---------------------------------------------------------------------------
if echo "${COMBINED_OUT}" | grep -q "server:"; then
    echo "PASS: output contains 'server:' prefix (TUI-T17 path confirmed)"
else
    echo "FAIL: output did not contain 'server:'"
    echo "  TUI-T17's error-event handler did not surface the server message."
    echo "  Actual output: ${COMBINED_OUT}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assertion 3: output must contain "context length"
# Proves the server's actual error message reached the user.
# ---------------------------------------------------------------------------
if echo "${COMBINED_OUT}" | grep -q "context length"; then
    echo "PASS: output contains 'context length' (server message propagated)"
else
    echo "FAIL: output did not contain 'context length'"
    echo "  The server's actual error message was not surfaced."
    echo "  Actual output: ${COMBINED_OUT}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Assertion 4: output must NOT contain "BATBOX_MAX_TOKENS"
# Proves the generic T12 guard did NOT fire.  If T12's guard fires instead of
# T17's specific path, it suggests to increase BATBOX_MAX_TOKENS — which is
# the wrong advice when the server says the prompt is too large.
# ---------------------------------------------------------------------------
if echo "${COMBINED_OUT}" | grep -q "BATBOX_MAX_TOKENS"; then
    echo "FAIL: output contains 'BATBOX_MAX_TOKENS' — T12 generic guard fired"
    echo "  TUI-T17's specific error-event handler should have taken precedence."
    echo "  Actual output: ${COMBINED_OUT}"
    exit 1
fi

echo "PASS: output does not contain 'BATBOX_MAX_TOKENS' (T17 path took precedence)"

# ---------------------------------------------------------------------------
# All assertions passed
# ---------------------------------------------------------------------------
echo "PASS: 12_error_event_surfaces"
exit 0
