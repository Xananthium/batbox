#!/usr/bin/env bash
# =============================================================================
# tests/perf/measure_cold_build.sh
#
# Cold build timer for batbox.
#
# Usage:
#   measure_cold_build.sh --target=<macos|linux>
#
# Output (stdout):
#   BUILD_TIME_SEC=<integer>
#
# Exit code:
#   0  — build completed within budget
#   1  — build exceeded budget (hard fail)
#
# Budget thresholds (wall-clock, includes vcpkg bootstrap + full build):
#   macOS (arm64) :  25 minutes (20-min nominal + 1.25x CI overhead)
#   Linux (x64)   :  30 minutes (20-min nominal + 1.50x CI overhead — ubuntu
#                                 runners are slower for C++ compilation)
#
# COLD BUILD CONTRACT
# -------------------
# This script intentionally removes the build/ directory and the vcpkg/
# sibling clone before measuring.  It passes -DBATBOX_VCPKG_AUTO_FETCH=ON so
# CMake clones and bootstraps vcpkg from scratch.  vcpkg then installs all
# manifest dependencies from the network.
#
# AS COMPONENTS LAND:
# Each new add_subdirectory() that lands in CMakeLists.txt (B.2 through B.19)
# will add compilation units to the build.  The 20-minute nominal budget was
# sized for the final, fully-populated source tree.  If a component causes the
# cold build to exceed the budget, that component's author is responsible for
# investigating (split translation units, PCH, etc.) before merging.
#
# CACHING STRATEGY (CI):
# - The build/ directory is NEVER cached (defeating the cold-build guarantee).
# - The vcpkg binary cache (~/.cache/vcpkg or %LOCALAPPDATA%/vcpkg/archives)
#   is NOT restored in the workflow that runs this script; cold = cold.
# - A separate post-run step in the workflow can SAVE the vcpkg installed/
#   tree so that non-cold (PR, push) workflows can restore it for fast builds.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
TARGET=""
for arg in "$@"; do
  case "$arg" in
    --target=macos) TARGET="macos" ;;
    --target=linux) TARGET="linux" ;;
    --target=*)
      echo "ERROR: unknown --target value '${arg#--target=}'. Must be macos or linux." >&2
      exit 1
      ;;
    *)
      echo "ERROR: unknown argument '$arg'" >&2
      echo "Usage: $0 --target=<macos|linux>" >&2
      exit 1
      ;;
  esac
done

if [[ -z "$TARGET" ]]; then
  echo "ERROR: --target is required." >&2
  echo "Usage: $0 --target=<macos|linux>" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Budget (seconds)
# ---------------------------------------------------------------------------
case "$TARGET" in
  macos) BUDGET_SEC=$((25 * 60)) ;;   # 1500 s
  linux) BUDGET_SEC=$((30 * 60)) ;;   # 1800 s
esac

# ---------------------------------------------------------------------------
# Resolve repo root (script lives at <repo>/tests/perf/)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
VCPKG_DIR="${REPO_ROOT}/vcpkg"

echo "=== Cold build measurement ==="
echo "  Target   : ${TARGET}"
echo "  Budget   : $((BUDGET_SEC / 60)) min (${BUDGET_SEC} s)"
echo "  Repo root: ${REPO_ROOT}"

# ---------------------------------------------------------------------------
# Wipe previous build artefacts and vcpkg clone to guarantee a cold start
# ---------------------------------------------------------------------------
echo "--- Removing build/ and vcpkg/ for cold-cache guarantee ---"
rm -rf "${BUILD_DIR}"
rm -rf "${VCPKG_DIR}"

# ---------------------------------------------------------------------------
# Time the full CMake configure + build
# ---------------------------------------------------------------------------
echo "--- cmake configure ---"
START_EPOCH=$(date +%s)

cmake -B "${BUILD_DIR}" \
      -S "${REPO_ROOT}" \
      -DBATBOX_VCPKG_AUTO_FETCH=ON \
      -DCMAKE_BUILD_TYPE=Release

echo "--- cmake build (j8) ---"
cmake --build "${BUILD_DIR}" -- -j8

END_EPOCH=$(date +%s)
ELAPSED=$(( END_EPOCH - START_EPOCH ))

# ---------------------------------------------------------------------------
# Emit machine-readable result
# ---------------------------------------------------------------------------
echo ""
echo "BUILD_TIME_SEC=${ELAPSED}"

ELAPSED_MIN=$(( ELAPSED / 60 ))
ELAPSED_REM=$(( ELAPSED % 60 ))
echo "=== Build completed in ${ELAPSED_MIN}m ${ELAPSED_REM}s (${ELAPSED}s) ==="

# ---------------------------------------------------------------------------
# Budget enforcement
# ---------------------------------------------------------------------------
if [[ $ELAPSED -gt $BUDGET_SEC ]]; then
  echo "ERROR: Build time ${ELAPSED}s exceeds ${TARGET} budget of ${BUDGET_SEC}s." >&2
  echo "       Investigate compilation bottlenecks, add PCH, or split TUs." >&2
  exit 1
fi

echo "PASS: Build is within ${TARGET} budget (${ELAPSED}s / ${BUDGET_SEC}s)."
exit 0
