# src/conversation

Autonomous exact compaction measures the complete request with the serving tokenizer. If the latest completed tool group alone exceeds 100,000 tokens, it can summarize that whole group and add an explicit continuation turn. Summary inputs are measured and split at UTF-8 boundaries into at most 90,000-token chunks, then reduced with a 20,000-token reasoning/output allowance. A failed or incomplete chunk leaves the active context and transcript unchanged; successful adoption requires a durable checkpoint. Original tool bodies remain in the transcript.

Conversation engine implementations: message handling, compaction, context estimation, plan mode, system prompt, and tool call orchestration.

Autonomous coordinator and worker conversations opt into `set_generation_quality_guard(true)`.
The guard rejects an extreme contiguous English prose loop: a phrase of one to
four whitespace tokens repeated at least 64 times, spanning at least 128 tokens
and 20% of the response. It excludes fenced/inline code, indented/quoted data,
numeric/boolean values and syntax; Markdown emphasis and separators around a
repeating word are recognized. This deliberately narrow heuristic does not
judge factual correctness, and unmarked intentional prose repetition can still
look identical to a model loop. A `length` finish alone remains valid.

A rejected final is appended with its exact original content and diagnostic
metadata before the caller receives a retryable error. Only the active context
uses a brief recovery user message; raw prior transcript entries remain intact.
Restore applies the same filter to old finals and the latest compaction suffix,
so a rejected pending final cannot be mistaken for completed work. Existing
coordinator/worker backoff bounds retries. Repetitive compaction summaries are
rejected before checkpoint adoption. The 200,000-token context and 100,000-token
compaction policies are unchanged.

`GenerationQuality.cpp` also gates explicitly tagged `generated_observation`
records before memory handoff and filters a caller-owned recall copy. The memory
service exposes `kind` and `content` at the top level of each result in both
`python-sidecar/batbox_memory/store.py` (CPU retrieval) and `graph.py` (HippoRAG).
Untagged, user, tool and verified-source records are preserved. Pending rejected
generated records are quarantined by the engine; existing stored memories are
never deleted by this guard.

After checking the complete generated observation, native insertion attaches
`metadata.batbox_generation_quality = {version: 1, full_content_accepted: true}`.
Both recall backends preserve this receipt on each chunk. Recall filtering trusts
that full-content decision so a fenced data block stays recallable when interior
chunks lose their fences. Insertion and outbox flushing always inspect the full
content again, regardless of a supplied receipt; historical unmarked generated
loops still receive the narrow filter.

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


### Autonomous exact context policy

`Conversation::set_exact_compaction_tokens(100000)` opts a session into the serving
backend's `/tokenize` endpoint. `BATBOX_EXACT_COMPACTION_TOKENS=100000` sets the
same policy through `CompactConfig::exact_token_threshold` for all conversations,
including native workers. Standing workers defer compaction to this exact gate.
Counts include system instructions, messages, tool
schemas, reminders, and the same stable chat-template options used for generation.
The gate runs before every completion, including tool follow-ups. Tokenizer errors
stop the request; they do not silently switch to a character estimate.

At the threshold, `Compactor::compact` asks the configured model for a factual
handoff and retains a recent suffix whose boundary is expanded to preserve tool
call/result pairs. Empty or truncated summaries and oversized replacement contexts
are rejected without changing the original history. A successful replacement is
appended as a `context_checkpoint` record to the existing session transcript before
it becomes active. `Conversation::restore` replays the newest checkpoint plus later
messages; all earlier raw messages remain in the transcript.

The persistent checkpoint, complete tool-group boundaries, stable prompt/schema
prefixes, and bounded retry approach were informed by the user-provided
`CLAUDECODE.tar` reference (`src/services/compact/autoCompact.ts`,
`sessionMemoryCompact.ts`, and `src/services/api/promptCacheBreakDetection.ts`).
This implementation uses Batbox's native data structures and local vLLM APIs;
it does not import that archive's runtime or vendor-specific cache-edit protocol.

Interactive sessions retain their existing estimator/notepad policy unless they
opt into the exact policy. The native inference client's template options must be
set once before running a session to keep tokenization and generation aligned.

Autonomous prompt appends use `user_message_durable()` and advance their outer
checkpoint only after it succeeds. Tool calls are parsed with `prepare_calls()`
and their complete assistant intent is persisted before dispatch. The durable
dispatch callback persists each result before permitting the following tool;
a failed write stops dispatch. `recover_interrupted_tools()` closes missing
results with an explicit unknown-outcome record on restart or same-process retry,
without executing the interrupted action again.
