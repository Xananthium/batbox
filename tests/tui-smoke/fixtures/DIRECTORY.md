# tests/tui-smoke/fixtures

Mock server scripts and transcript files used by the tmux-based TUI smoke tests.

## Files

### mock_lmstudio.py
FastAPI stub serving LM Studio-compatible /v1/chat/completions responses for the basic TUI startup and send-message smoke tests; returns a fixed assistant response.

### mock_plan_mode.py
Mock server that returns a plan-mode response with EnterPlanMode tool call; used to test the PlanApprovalCard and plan mode banner rendering.

### mock_plan_with_cards.py
Mock server that sequences plan creation followed by VerifyPlanExecution and ExitPlanMode tool calls; tests the full plan mode lifecycle in the TUI.

### mock_reasoning_only.py
Mock server that returns a response with only reasoning_content (no content); tests the reasoning indicator rendering in StreamingMessageView.

### mock_tool_call.py
Mock server that returns a BashTool call response; tests PermissionCard rendering, the allow/deny flow, and tool result display in ChatView.
