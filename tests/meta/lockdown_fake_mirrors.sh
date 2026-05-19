#!/usr/bin/env bash
# tests/meta/lockdown_fake_mirrors.sh
# ---------------------------------------------------------------------------
# PEXT3 1.7 — Lockdown: ban fake-mirror closure helpers from the test tree.
#
# Rule: the denylist below contains function names that are known-bad
# "fake mirror" helpers — local hand-copies of production factory logic that
# caused tests to pass while the real binary remained broken (see feedback
# memory: verify-user-visible-fixes).  If any denylist symbol is found in
# tests/ source files, this script fails loudly with file:line and the rule.
#
# Usage (called by CMake add_test):
#   lockdown_fake_mirrors.sh <TESTS_DIR>
#
# To add a new forbidden symbol: append one entry to DENYLIST below.
# That is the ONLY line that changes.
# ---------------------------------------------------------------------------

set -euo pipefail

TESTS_DIR="${1:?Usage: $0 <tests-dir>}"
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

# ---------------------------------------------------------------------------
# DENYLIST — one entry per forbidden symbol.  Add new symbols here only.
# ---------------------------------------------------------------------------
DENYLIST=(
    "make_nuclear_plan_confirm_fn"
    "make_nuclear_askq_prompt_fn"
)

FOUND=0

for SYMBOL in "${DENYLIST[@]}"; do
    # grep -rn: recursive, show line numbers.
    # --include: C/C++ source and headers only.
    # grep exits 0 on match, 1 on no match, 2 on error.
    while IFS= read -r LINE; do
        FILE="${LINE%%:*}"
        # Exclude this script itself from the scan.
        ABSPATH="$(cd "$(dirname "$FILE")" 2>/dev/null && pwd)/$(basename "$FILE")"
        if [ "$ABSPATH" = "$SELF" ]; then
            continue
        fi
        echo "LOCKDOWN VIOLATION: '${SYMBOL}' found in ${LINE}"
        echo "  Rule: fake-mirror closure helpers are permanently banned (PEXT3 1.7)."
        echo "  Fix:  call the real factory (e.g. make_plan_confirm_fn / make_askq_prompt_fn)."
        FOUND=1
    done < <(grep -rn --include="*.cpp" --include="*.hpp" --include="*.h" \
                  "${SYMBOL}" "${TESTS_DIR}" 2>/dev/null || true)
done

if [ "${FOUND}" -eq 0 ]; then
    echo "lockdown_fake_mirrors: PASS — no forbidden symbols found in ${TESTS_DIR}"
    exit 0
else
    exit 1
fi
