#!/usr/bin/env bash
# =============================================================================
# egress_scan.sh — BatBox static-egress binary scanner
#
# PURPOSE:
#   Grep a compiled binary for hardcoded forbidden hostnames / telemetry tokens.
#   This enforces BatBox's "local-only, no-telemetry" stance (pmdraft.md §Privacy,
#   "Static-Egress Test" section). Any listed token appearing as a compiled-in
#   string is a policy violation and fails the build / ctest run.
#
# RATIONALE FOR THE FORBIDDEN LIST (pmdraft.md "Static-Egress Test"):
#   These are analytics, feature-flag, and distribution services that BatBox
#   explicitly does NOT call. Their presence in a stripped binary means someone
#   accidentally linked or hardcoded a connection to one of them.
#
#   - datadoghq.com / dd-api.com     : Datadog APM / telemetry
#   - statsig.com                    : feature flagging / experimentation
#   - growthbook.io / growthbook.com : feature flagging (open-source variant)
#   - storage.googleapis.com/claude-code-dist : upstream update distribution
#   - mcp-registry                   : MCP hosted registry (BatBox is local-only)
#   - event_logging                  : generic telemetry path token
#   - api.anthropic.com              : hardcoded Anthropic endpoint
#     NOTE: this catches compiled-in strings only. The user's runtime-configurable
#     base URL (stored in ~/.batbox/.env) will NOT appear here because it is read
#     at runtime from the environment, never baked into the binary.
#   - VOICE_STREAM_BASE_URL          : voice streaming endpoint symbol (not in scope)
#
# WHAT IS ALLOWED:
#   - User-configured endpoints in .env / settings.json (runtime, not compiled-in)
#   - Source code comments and docstrings mentioning these hosts do NOT appear in
#     a stripped Release binary — only actual string literals do.
#   - Third-party library copyright strings: if a vendored library coincidentally
#     embeds a URL that matches, add it to the ALLOWLIST below.
#
# USAGE:
#   egress_scan.sh <binary-or-file>
#
#   Exit 0 = clean (no forbidden strings found)
#   Exit 1 = violation found (matching lines printed to stderr)
#   Exit 2 = usage / file error
#
# CROSS-PLATFORM NOTES:
#   - macOS: uses /usr/bin/strings (BSD strings, built-in)
#   - Linux: prefers llvm-strings (if available) → strings (GNU binutils)
#   - Minimum string length passed to `strings` is 6 to reduce false positives
#     from coincidental byte sequences.
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Usage check
# ---------------------------------------------------------------------------
if [[ $# -lt 1 ]]; then
    echo "[egress-scan] ERROR: no binary path supplied." >&2
    echo "Usage: egress_scan.sh <binary-path>" >&2
    exit 2
fi

BINARY="$1"

# ---------------------------------------------------------------------------
# File existence and readability check
# ---------------------------------------------------------------------------
if [[ ! -e "$BINARY" ]]; then
    echo "[egress-scan] ERROR: file not found: $BINARY" >&2
    exit 2
fi

if [[ ! -f "$BINARY" ]]; then
    echo "[egress-scan] ERROR: not a regular file: $BINARY" >&2
    exit 2
fi

if [[ ! -r "$BINARY" ]]; then
    echo "[egress-scan] ERROR: file is not readable: $BINARY" >&2
    exit 2
fi

# Handle empty binary gracefully — empty file has no strings, scan passes.
if [[ ! -s "$BINARY" ]]; then
    echo "[egress-scan] WARNING: file is empty (0 bytes): $BINARY — scan passes." >&2
    exit 0
fi

# ---------------------------------------------------------------------------
# Select the `strings` tool
# ---------------------------------------------------------------------------
STRINGS_CMD=""

if command -v llvm-strings >/dev/null 2>&1; then
    STRINGS_CMD="llvm-strings"
elif [[ -x /usr/bin/strings ]]; then
    # macOS system strings (BSD)
    STRINGS_CMD="/usr/bin/strings"
elif command -v strings >/dev/null 2>&1; then
    # GNU binutils strings (Linux)
    STRINGS_CMD="strings"
else
    echo "[egress-scan] ERROR: no 'strings' tool found. Install llvm-binutils or GNU binutils." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Forbidden token list (verbatim from pmdraft.md "Static-Egress Test" section)
# ---------------------------------------------------------------------------
# Each entry is a fixed string (not a regex) for predictable, fast matching.
# grep -F performs literal string matching — no regex escaping needed.
FORBIDDEN_TOKENS=(
    "datadoghq.com"
    "dd-api.com"
    "statsig.com"
    "growthbook.io"
    "growthbook.com"
    "storage.googleapis.com/claude-code-dist"
    "mcp-registry"
    "event_logging"
    "api.anthropic.com"
    "VOICE_STREAM_BASE_URL"
)

# ---------------------------------------------------------------------------
# Allow-list: substrings that, if present in a matching line, indicate the
# match is from a known-safe vendored library (e.g., a copyright notice).
# Format: "<forbidden-token>:<allowlisted-context-substring>"
# If a line matches a forbidden token AND contains an allowlisted context
# string, it is suppressed from the violation list.
#
# Add entries here when a third-party library legitimately contains one of
# the forbidden host strings in its copyright / license text.
#
# Example (disabled):
#   ALLOWLIST=("api.anthropic.com:Copyright Anthropic PBC")
# ---------------------------------------------------------------------------
ALLOWLIST=()

# ---------------------------------------------------------------------------
# Run the scan
# ---------------------------------------------------------------------------
echo "[egress-scan] scanning: $BINARY"
echo "[egress-scan] using:    $STRINGS_CMD"

# Extract printable strings of length >= 6 from the binary.
# -n 6 / --min-len=6 supported by both BSD strings and GNU strings.
STRINGS_OUTPUT=$("$STRINGS_CMD" -n 6 "$BINARY" 2>/dev/null || true)

if [[ -z "$STRINGS_OUTPUT" ]]; then
    echo "[egress-scan] no printable strings found in binary — scan passes." >&2
    exit 0
fi

VIOLATIONS=()

for TOKEN in "${FORBIDDEN_TOKENS[@]}"; do
    # Find all lines from strings output that contain this forbidden token.
    MATCHES=$(echo "$STRINGS_OUTPUT" | grep -F "$TOKEN" || true)

    if [[ -z "$MATCHES" ]]; then
        continue
    fi

    # Check each matching line against the allow-list.
    while IFS= read -r MATCH_LINE; do
        ALLOWED=false
        for ALLOW_ENTRY in "${ALLOWLIST[@]+"${ALLOWLIST[@]}"}"; do
            ALLOW_TOKEN="${ALLOW_ENTRY%%:*}"
            ALLOW_CONTEXT="${ALLOW_ENTRY#*:}"
            if [[ "$ALLOW_TOKEN" == "$TOKEN" ]] && [[ "$MATCH_LINE" == *"$ALLOW_CONTEXT"* ]]; then
                ALLOWED=true
                break
            fi
        done

        if [[ "$ALLOWED" == "false" ]]; then
            VIOLATIONS+=("  [${TOKEN}] ${MATCH_LINE}")
        fi
    done <<< "$MATCHES"
done

# ---------------------------------------------------------------------------
# Report results
# ---------------------------------------------------------------------------
if [[ ${#VIOLATIONS[@]} -eq 0 ]]; then
    echo "[egress-scan] PASS — no forbidden hostnames found in binary."
    exit 0
else
    echo "[egress-scan] FAIL — forbidden hostname(s) found in binary:" >&2
    echo "[egress-scan] Binary: $BINARY" >&2
    echo "[egress-scan] Violations:" >&2
    for V in "${VIOLATIONS[@]}"; do
        echo "$V" >&2
    done
    echo "[egress-scan] Build rejected. Remove all hardcoded forbidden host references." >&2
    exit 1
fi
