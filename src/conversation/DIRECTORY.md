# src/conversation

Conversation engine implementations: message handling, compaction, context estimation, plan mode, system prompt, and tool call orchestration.

## Files

### Conversation.cpp
`Conversation::user_message()`, `run_turn()`, `restore()`, `start_session()`, `clear_messages()`, `compose_system_prompt()` implementations; drives the inference loop: assembles ChatRequest, calls Client::stream_chat(), accumulates deltas, dispatches tool calls via ToolCallOrchestrator, persists messages to SessionStore.

### Compactor.cpp
`Compactor::compact()` implementation: builds a summarization prompt; calls Client::chat() to produce a compact representation; splices summary message at front of the last keep_last_n messages.

### NotepadReminder.cpp
(DIS-981 S6) Per-turn notepad re-injection (goose `get_moim` / opencode `reminders.apply` equivalent). `compose_notepad_reminder(slice)` wraps a pad slice in a `<notepad>…</notepad>` block; `apply_notepad_reminder(req, slice)` appends it as the FINAL (tail) message of a built ChatRequest — tail-only mutation so the cached system-prompt prefix is preserved; empty slice = no-op. Conversation.cpp calls it after `build_chat_request` (preflight for iter-0 body + the tool loop for iter>0); `compose_system_prompt` is untouched.

### StandingReminder.cpp
(DIS-988 S2/S3 AC4) Per-turn warm-subagent status re-injection (goose `get_moim` status surface). `compose_standing_reminder(handles)` wraps up to N warm-subagent lines in a `<warm_subagents>…</warm_subagents>` block (bounded, "(+N more)"); `apply_standing_reminder(req, handles)` appends it as a tail message — same cache discipline as NotepadReminder; empty handles = no-op. Conversation.cpp calls it at all three notepad injection points via the `standing_handles_provider_` callback (set by App from `AgentSupervisor::standing_status()`); unset/empty → cache untouched.

### ContextWindow.cpp
`estimate_tokens()`, `needs_compact()`, `context_limit_for_model()`, `uses_o200k()`, `estimate_string()` implementations; static model→context-limit table; byte-to-token heuristic constants.

### Message.cpp
`from_json()`, `to_json()`, `role_from_string()`, `to_wire_role()` implementations; handles tool_calls array serialization for assistant messages; populates tool_call_id and tool_name for tool-result messages.

### PlanMode.cpp
State machine transitions (Inactive/Planning/Approved); observer notification; is_tool_allowed() allowlist lookup; banner text formatting.

### SystemPrompt.cpp
`compose_system_prompt()` implementation: concatenates base prompt, optional plan mode prefix, and working directory instruction; does not perform I/O.

### ToolCallOrchestrator.cpp
`accumulate()` and `dispatch_all()` implementations: feeds StreamDelta into ToolCallAccumulator; for each finalized ToolCall runs PermissionGate::ask() then ToolRegistry::dispatch(); returns vector of tool-result Messages.

### CMakeLists.txt
Build rules for the conversation static library.
