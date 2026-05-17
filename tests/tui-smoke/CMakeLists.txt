# =============================================================================
# tests/tui-smoke/CMakeLists.txt
#
# Registers TUI smoke tests (tmux-based) with CTest under the label "tui-smoke".
#
# This subdirectory is only included when BATBOX_TUI_SMOKE=ON:
#
#   cmake -DBATBOX_TUI_SMOKE=ON <build-dir>
#   ctest -L tui-smoke           # run only tui-smoke tests
#   ctest -L tui-smoke -V        # verbose output
#
# Normal builds (BATBOX_TUI_SMOKE=OFF, the default) do NOT include these tests
# so they never appear in ctest / ctest -j8 output.
#
# Requirements:
#   - tmux installed (brew install tmux)
#   - BatBox binary built at build/src/batbox
#   - The case scripts contain SKIP guards for missing infrastructure
# =============================================================================

# Locate the harness script — path relative to this CMakeLists.txt
set(_HARNESS "${CMAKE_CURRENT_SOURCE_DIR}/bin/harness")
set(_CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cases")

# ---------------------------------------------------------------------------
# Helper to register a single case file as a CTest test.
# Usage: _add_tui_smoke_test(<test_name> <case_file>)
# ---------------------------------------------------------------------------
function(_add_tui_smoke_test test_name case_file)
    add_test(
        NAME    "${test_name}"
        COMMAND bash "${_HARNESS}" run "${case_file}"
    )
    set_tests_properties("${test_name}" PROPERTIES
        # Label used for ctest -L tui-smoke filtering
        LABELS            "tui-smoke"
        # Allow up to 120s per test (BatBox startup + mock LLM round-trip)
        TIMEOUT           120
        # Run from project root so relative binary paths (build/src/batbox) resolve
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        # Default environment: mock LLM endpoint; override with live URL when needed
        ENVIRONMENT       "BATBOX_API_BASE_URL=http://127.0.0.1:8824/v1;BATBOX_NO_SPLASH=true"
    )
endfunction()

# ---------------------------------------------------------------------------
# Register case 00 — first message round-trip via mock LLM
# ---------------------------------------------------------------------------
_add_tui_smoke_test(
    tui_smoke_00_first_message
    "${_CASES_DIR}/00_smoke_first_message.sh"
)

# ---------------------------------------------------------------------------
# Additional cases registered here as they are authored (TUI-T2 onward):
#
# _add_tui_smoke_test(tui_smoke_01_tool_permission   "${_CASES_DIR}/01_tool_call_permission_modal.sh")
# _add_tui_smoke_test(tui_smoke_02_nuclear_bypass    "${_CASES_DIR}/02_tool_call_nuclear_bypass.sh")
# _add_tui_smoke_test(tui_smoke_03_tool_roundtrip    "${_CASES_DIR}/03_tool_result_roundtrip.sh")
_add_tui_smoke_test(tui_smoke_04_multi_turn "${_CASES_DIR}/04_multi_turn.sh")
_add_tui_smoke_test(tui_smoke_05_slash_palette  "${_CASES_DIR}/05_slash_palette.sh")
_add_tui_smoke_test(tui_smoke_06_slash_dispatch "${_CASES_DIR}/06_slash_dispatch.sh")
# _add_tui_smoke_test(tui_smoke_07_resume_restore    "${_CASES_DIR}/07_resume_session_restore.sh")
# _add_tui_smoke_test(tui_smoke_08_escape_cancel     "${_CASES_DIR}/08_escape_cancels_input.sh")
# _add_tui_smoke_test(tui_smoke_09_tool_in_chat      "${_CASES_DIR}/09_tool_call_visible_in_chatview.sh")

# ---------------------------------------------------------------------------
# Case 11 — TUI-T14: reasoning-only truncation guard (headless --print mode)
#
# Drives mock_reasoning_only.py (port 8828) against the real binary.
# Expects: exit non-zero + stderr "stream ended without content".
# This case does NOT require tmux — it uses batbox --print directly.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_11_reasoning_truncation "${_CASES_DIR}/11_reasoning_only_truncation.sh")
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Case 12 — TUI-T19: SSE error-event surfacing (headless --print mode)
#
# Drives mock_reasoning_only.py in --mode error-event (port 8828) against
# the real binary.  Expects: exit non-zero + stderr "server:" + "context
# length" + NOT "BATBOX_MAX_TOKENS".
# Confirms TUI-T17's error-event handler takes precedence over T12's generic
# guard, surfacing the server's actual context-overflow message.
# This case does NOT require tmux — it uses batbox --print directly.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_12_error_event_surfaces "${_CASES_DIR}/12_error_event_surfaces.sh")

# ---------------------------------------------------------------------------
# Case 13 — TUI-T19: SSE fatal_error-event surfacing (headless --print mode)
#
# Same as case 12 but using --mode fatal-error.  Confirms TUI-T17 handles
# BOTH "error" and "fatal_error" event names in the same branch.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_13_fatal_error_surfaces "${_CASES_DIR}/13_fatal_error_surfaces.sh")

# ---------------------------------------------------------------------------
# Case 14 — TUI-FLOW-T1: live swirling counter (elapsed-s + token count)
#
# Verifies that the spinner row appears under the echoed user prompt during
# streaming, that the elapsed-seconds digit ticks at least once, and that
# the row disappears (replaced by frozen summary) when streaming ends.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_14_swirling_counter "${_CASES_DIR}/14_swirling_counter.sh")

# ---------------------------------------------------------------------------
# Case 15 — TUI-FLOW-T2: progressive tool-call card (gerund in-flight → past done)
#
# Drives mock_tool_call.py (port 8848) which returns a Read tool_calls SSE
# stream followed by a plain text confirmation.  Asserts that "Reading" appears
# while the tool is dispatching, then "Read" appears once it completes.
# Requires tmux + BatBox binary built at build/src/batbox.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_15_tool_card "${_CASES_DIR}/15_tool_card.sh")

# ---------------------------------------------------------------------------
# Case 16 — TUI-PLAN-T2: PlanApprovalCard renders and resolves with [A]/[R]
#
# Drives a mock ExitPlanMode SSE tool-call response and asserts the card
# renders with "Plan Review" / "[A]" hints, then resolves cleanly on 'A'.
# Requires tmux + BatBox binary built at build/src/batbox.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_16_plan_approval "${_CASES_DIR}/16_plan_approval.sh")

# ---------------------------------------------------------------------------
# Case 17 — TUI-PLAN-T3: plan text must NOT render twice (double-render regression)
#
# Drives mock_plan_mode.py (port 8849). Asserts "step 1" appears at most once
# in the final pane snapshot after both streaming and ExitPlanMode complete.
# Requires tmux + BatBox binary built at build/src/batbox.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_17_plan_no_double_render "${_CASES_DIR}/17_plan_no_double_render.sh")

# ---------------------------------------------------------------------------
# Case 20 — TUI-ASKQ-T4: QuestionCard modal appears and resolves on Enter
#
# Drives a mock AskUserQuestion SSE tool-call response and asserts the card
# renders with question text and option labels, then resolves cleanly on Enter.
# Requires tmux + BatBox binary built at build/src/batbox.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_20_question_card_modal "${_CASES_DIR}/20_question_card_modal.sh")

# ---------------------------------------------------------------------------
# Case 22 — TUI-FLOW-T10: changelog in splash
#
# Asserts that a seeded agentic/changelog.md entry appears in the "What's new"
# splash panel and that state.json records last_seen_changelog_version.
# Requires tmux + BatBox binary built at build/src/batbox.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_22_splash_changelog "${_CASES_DIR}/22_splash_changelog.sh")

# ---------------------------------------------------------------------------
# Case 23 — TUI-ASKQ-T6: full /plan + AskUserQuestion + PlanApprovalCard e2e
#
# Drives mock_plan_with_cards.py (port 8850) which simulates the complete
# plan-mode flow: two AskUserQuestion cards, plan streaming, ExitPlanMode,
# and the final approval acknowledgement.
# Regression: asserts plan text appears at most once (TUI-PLAN-T3 guard).
# Requires tmux + BatBox binary built at build/src/batbox.
# ---------------------------------------------------------------------------
_add_tui_smoke_test(tui_smoke_23_plan_with_cards_e2e "${_CASES_DIR}/23_plan_with_cards_e2e.sh")
