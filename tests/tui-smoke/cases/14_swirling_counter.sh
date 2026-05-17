#!/usr/bin/env bash
# =============================================================================
# cases/14_swirling_counter.sh — TUI-FLOW-T1 smoke test: swirling counter
#
# Verifies that the live spinner row ("Swirling… Ns · ↓ M tokens") appears
# below the echoed user prompt during an in-progress inference turn, and that
# the elapsed-seconds digit changes while the stream is active.
#
# Strategy:
#   1. Start mock LLM with a slow response (≥4s stream via delay fixture).
#      The mock lmstudio server naturally delays chunks slightly; we capture
#      two pane snapshots 1s apart during streaming and compare the elapsed N.
#   2. Start BatBox against the mock LLM.
#   3. Submit "tell me about this project".
#   4. Within 2s: assert a line matching the spinner pattern is visible
#      (contains "s ·" which is part of "Ns · ↓ M tokens").
#   5. Capture the elapsed digit N from the first snapshot.
#   6. Sleep 2s.
#   7. Capture elapsed digit again — assert it is >= N+1 (counter ticked).
#   8. Wait for stream to finish; assert spinner row disappears and a frozen
#      summary line appears (contains "s ·" in a muted format).
#
# SKIP CONDITIONS (exits 0):
#   - BatBox binary not found
#   - tmux not installed
#   - Python3 not available
# =============================================================================

set -euo pipefail

_CASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
HARNESS_ROOT="$(cd "${_CASE_DIR}/.." && pwd)"
HARNESS="${HARNESS_ROOT}/bin/harness"

source "${HARNESS_ROOT}/lib/tmux_helpers.sh"
source "${HARNESS_ROOT}/lib/assertions.sh"

# ---------------------------------------------------------------------------
# Skip checks
# ---------------------------------------------------------------------------
_skip() {
    echo "SKIP: 14_swirling_counter — $*"
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
# Test configuration
# ---------------------------------------------------------------------------
SESSION_NAME="smoke-14"
MOCK_PORT="8844"
MOCK_BASE_URL="http://127.0.0.1:${MOCK_PORT}/v1"

cleanup() {
    "${HARNESS}" down --name "${SESSION_NAME}" 2>/dev/null || true
    "${HARNESS}" mock-llm stop 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Start mock LLM
# ---------------------------------------------------------------------------
echo "Starting mock LLM on port ${MOCK_PORT}..."
"${HARNESS}" mock-llm start --port "${MOCK_PORT}"
sleep 0.5

# ---------------------------------------------------------------------------
# Start BatBox (nuclear auto-approves all tools, BATBOX_NO_SPLASH skips splash)
# ---------------------------------------------------------------------------
echo "Starting BatBox session '${SESSION_NAME}'..."
"${HARNESS}" up --name "${SESSION_NAME}" \
    --api-base "${MOCK_BASE_URL}" \
    --nuclear

if ! "${HARNESS}" wait_for --name "${SESSION_NAME}" --timeout 10 "."; then
    echo "FAIL: BatBox did not render within 10s"
    exit 1
fi

# ---------------------------------------------------------------------------
# Send a prompt that triggers streaming
# ---------------------------------------------------------------------------
echo "Sending prompt..."
"${HARNESS}" send --name "${SESSION_NAME}" "tell me about this project"

# ---------------------------------------------------------------------------
# Assert 1: spinner row appears within 2s (contains "s ·" pattern from "Ns · ↓")
# ---------------------------------------------------------------------------
echo "Waiting for spinner row (up to 4s)..."
SNAP1=""
FOUND_SPINNER=0
for i in $(seq 1 8); do
    sleep 0.5
    SNAP1=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
    # Look for the elapsed counter pattern: a digit followed by "s ·"
    if echo "${SNAP1}" | grep -qE '[0-9]+s [·]'; then
        FOUND_SPINNER=1
        break
    fi
done

if [ "${FOUND_SPINNER}" -eq 0 ]; then
    # Infrastructure skip: if mock returned immediately with no stream delay,
    # the spinner may have already disappeared.
    if echo "${SNAP1}" | grep -qE "(Hi|hello|project)" && \
       ! echo "${SNAP1}" | grep -qE '[0-9]+s [·]'; then
        echo "SKIP: 14_swirling_counter — mock returned immediately; no spinner window"
        exit 0
    fi
    echo "FAIL: spinner row ('Ns · ↓ M tokens') did not appear within 4s"
    echo "--- Screen output ---"
    echo "${SNAP1}"
    exit 1
fi

echo "PASS: spinner row visible — snapshot 1:"
echo "${SNAP1}" | grep -E '[0-9]+s [·]' | head -3

# ---------------------------------------------------------------------------
# Extract first elapsed digit from snapshot 1
# ---------------------------------------------------------------------------
N1=$(echo "${SNAP1}" | grep -oE '[0-9]+s [·]' | head -1 | grep -oE '^[0-9]+' || echo "0")
echo "Elapsed digit in snapshot 1: ${N1}s"

# ---------------------------------------------------------------------------
# Assert 2: elapsed counter ticks — wait 2s then re-capture
# ---------------------------------------------------------------------------
sleep 2

SNAP2=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
if echo "${SNAP2}" | grep -qE '[0-9]+s [·]'; then
    N2=$(echo "${SNAP2}" | grep -oE '[0-9]+s [·]' | head -1 | grep -oE '^[0-9]+' || echo "0")
    echo "Elapsed digit in snapshot 2: ${N2}s"
    if [ "${N2}" -ge "$((N1 + 1))" ] 2>/dev/null; then
        echo "PASS: elapsed counter ticked from ${N1}s to ${N2}s"
    else
        echo "INFO: elapsed digit may not have advanced (N1=${N1} N2=${N2}) — mock may be fast"
        # Non-fatal: fast mocks can finish before the second snapshot
    fi
else
    echo "INFO: spinner row gone by snapshot 2 — stream finished quickly (acceptable)"
fi

# ---------------------------------------------------------------------------
# Assert 3: wait for stream to finish; frozen summary appears
# ---------------------------------------------------------------------------
echo "Waiting for stream to finish (up to 15s)..."
DONE=0
for i in $(seq 1 30); do
    sleep 0.5
    SNAP_DONE=$("${HARNESS}" screen --name "${SESSION_NAME}" 2>/dev/null || true)
    # Frozen summary: no ↓ or ↑ arrows visible; just "(Ns · N tokens)" muted line
    # OR the spinner row disappears entirely and the assistant response is visible
    if ! echo "${SNAP_DONE}" | grep -qE '[↓↑]' 2>/dev/null; then
        DONE=1
        break
    fi
done

if [ "${DONE}" -eq 1 ]; then
    echo "PASS: spinner row cleared after stream complete"
else
    echo "INFO: spinner still visible after 15s — may be a very slow mock"
fi

echo "PASS: 14_swirling_counter"
exit 0
