# include/batbox/conversation

Conversation engine headers: auto-compaction, context window estimation, message types, plan mode state machine, system prompt composer, and tool call orchestration.

## Files

### Compactor.hpp  (DIS-983 S5 — protected-tail compaction; notepad sink)
Protected-tail compaction. The DEFAULT sink is the NOTEPAD (S5); the LLM-summary
path is retained but demoted (no longer on the Conversation auto-compact path).

- `Compactor::Compactor(keep_last_n, on_status=nullptr)` — constructs with the number of recent messages to preserve verbatim after compaction
- `Compactor::compact_to_notepad(msgs, notepad_ref) -> Result<vector<Message>>` — **THE SINK.** Network-free structural prune: replaces each head Role::Tool message body with a small tombstone that cites the working notepad (`notepad_ref`, the pad path) and records the dropped byte count, keeping role + tool_call_id so the wire pairing stays valid; preserves the protected tail and authored (user/assistant) head text verbatim. Gold lives out-of-band in the notepad → nothing is silently lost (AC3). No-op when the head is empty
- `Compactor::compact(msgs, client, ct) -> Result<vector<Message>>` — LEGACY LLM-summary path; sends the head to the model for a one-paragraph summary and prepends it as a System message. Demoted (not deleted) — kept only so the summariser tests keep proving the head/tail split + status callback
- `Compactor::keep_last_n() -> size_t` — returns the configured preservation count
- `compaction_should_run(needs, manages) -> bool` (free fn) — S9 stand-down gate: `needs && !manages`. Compaction runs only when the gate fires AND the provider does not manage its own context window (Provider::manages_own_context()==true → stand down)

### NotepadReminder.hpp  (DIS-981 S6 — per-turn notepad re-injection)
Surfaces the working notepad each turn as a TAIL reminder, never in the cached system-prompt prefix (cache discipline: only ever mutate the tail).

- `compose_notepad_reminder(pad_slice) -> string` — pure formatter; wraps the slice in `<notepad>…</notepad>`; empty slice → ""
- `apply_notepad_reminder(req, pad_slice) -> bool` — appends a trailing {role:"system"} reminder message to req.messages (tail-only); empty slice = no-op returns false; cached prefix preserved

### StandingReminder.hpp  (DIS-988 S2/S3 AC4 — per-turn warm-subagent status re-injection)
Surfaces the parent's warm standing-subagent list each turn as a bounded TAIL reminder, same cache discipline as NotepadReminder (never the cached prefix). Layer-local `StandingHandle{id,name,status_line}` POD keeps the conversation layer decoupled from the agents layer.

- `compose_standing_reminder(handles, max_handles=8) -> string` — pure formatter; wraps up to `max_handles` warm-subagent lines in `<warm_subagents>…</warm_subagents>` with a "(+N more)" bound; empty handles → ""
- `apply_standing_reminder(req, handles, max_handles=8) -> bool` — appends a trailing {role:"system"} reminder to req.messages (tail-only); empty handles = no-op returns false; cached prefix preserved
- Wired into `Conversation` via `set_standing_handles_provider(...)`; App supplies the provider from `AgentSupervisor::standing_status()`

### ContextWindow.hpp
Token estimation and context limit management.

- `ContextWindow::ContextWindow(cfg)` — constructs using model name and context limits from Config
- `ContextWindow::estimate_tokens(messages) -> size_t` — estimates total token count for a message vector using byte-to-token heuristic
- `ContextWindow::needs_compact(estimated_tokens) -> bool` — returns true when estimated_tokens exceeds 80% of the model's context limit
- `ContextWindow::set_model_context_limit(tokens)` — overrides the context limit for the current model
- `ContextWindow::set_model(model_name)` — switches to a different model's context limit
- `ContextWindow::estimate_tokens_from_bytes(bytes) -> size_t` — static; converts byte count to approximate token count using 3.5 bytes/token heuristic
- `ContextWindow::context_limit_for_model(name) -> size_t` — static; returns known context limit for model name; defaults to 8192 for unknowns
- `ContextWindow::uses_o200k(name) -> bool` — static; returns true when model uses the o200k tokenizer (GPT-4o family)
- `ContextWindow::estimate_string(text, is_o200k) -> size_t` — static; estimates token count for a single string

### Conversation.hpp
Single-session conversation engine: manages messages, tool calls, and inference turns.

- `Conversation::Conversation(client, store, cfg, working_dir, on_delta_cb, registry, gate, plan_mode)` — constructs; does not start a session
- `Conversation::user_message(text)` — appends a user-role message to the pending queue
- `Conversation::run_turn(ct) -> Result<void>` — sends all pending messages to the model, dispatches tool calls, appends assistant/tool messages, auto-compacts when needed. Compaction (S5): proactive pre-flight prune via `Compactor::compact_to_notepad` (gated by `compaction_should_run`); reactive prune+retry-once when the endpoint returns a context-overflow error (`inference::is_overflow_error`); both stand down when `manages_own_context()`
- `Conversation::set_manages_own_context(v)` / `manages_own_context()` — S9 stand-down wiring (S5, AC5): when true the active provider owns its context window and batbox skips ALL compaction. Defaults false; set from `Provider::manages_own_context()`
- `Conversation::restore(sf) -> Result<void>` — loads message history from a SessionFile into the live conversation
- `Conversation::messages() -> vector<Message>` — returns current message list (read-only)
- `Conversation::session_id() -> string` — returns the UUID of the active session
- `Conversation::clear_messages()` — discards all messages and resets context
- `Conversation::start_session() -> Result<void>` — creates a new SessionFile and JSONL index entry
- `Conversation::set_on_message_appended_cb(cb)` — registers callback fired when any message is appended
- `Conversation::set_on_tool_running_cb(cb)` — registers callback fired when a tool starts executing
- `Conversation::set_on_tool_done_cb(cb)` — registers callback fired when a tool finishes
- `Conversation::set_on_reasoning_started_cb(cb)` — registers callback fired when model reasoning begins (o1/o3 models)
- `Conversation::set_on_reasoning_stopped_cb(cb)` — registers callback fired when model reasoning ends
- `Conversation::set_on_usage_delta_cb(cb)` — registers callback fired with token/cost delta after each turn
- `Conversation::compose_system_prompt(plan_mode=false) -> string` — builds the system prompt from base + plan mode prefix + skill injections

### Message.hpp
Wire-format message types: roles, tool calls, usage tracking.

- `to_wire_role(r) -> string_view` — maps Role enum to "user"/"assistant"/"system"/"tool" string
- `role_from_string(s) -> Role` — parses wire role string to Role enum; defaults to User on unknown
- `from_json(j) -> Message` — deserialises a JSON object into a Message; populates tool_calls for assistant messages
- `to_json(m) -> Json` — serialises a Message to the OpenAI wire format JSON object

### PlanMode.hpp
Plan mode state machine: Inactive → Planning → Approved lifecycle.

- `PlanMode::state() -> PlanState` — returns current state (Inactive/Planning/Approved)
- `PlanMode::is_planning() -> bool` — returns true when state is Planning
- `PlanMode::is_approved() -> bool` — returns true when state is Approved
- `PlanMode::plan_text() -> string` — returns the plan text set during the Planning state
- `PlanMode::plan_id() -> uint32_t` — returns the monotonic id of the current plan
- `PlanMode::enter_plan()` — transitions Inactive → Planning
- `PlanMode::approve(plan_text) -> uint32_t` — transitions Planning → Approved; sets plan_text; returns new plan_id
- `PlanMode::reject()` — transitions Planning → Inactive; clears plan text
- `PlanMode::advance_turn()` — called after each inference turn; transitions Approved → Inactive after plan execution
- `PlanMode::transition(new_state)` — force-transitions to any state; fires observers
- `PlanMode::is_tool_allowed(tool_name) -> bool` — returns true when the tool may run in current mode
- `PlanMode::is_write_denied() -> bool` — returns true when in Planning state (write tools blocked)
- `PlanMode::banner() -> string` — returns the human-readable mode banner shown in the TUI status line
- `PlanMode::add_observer(fn) -> PlanObserverHandle` — registers a callback fired on every state transition; returns RAII handle that auto-removes on destruction

### SystemPrompt.hpp
System prompt constants and composer.

- `compose_system_prompt(plan_mode, working_dir) -> string` — concatenates k_base_system_prompt with optional plan_mode prefix and working_dir instruction; returns the full system prompt string sent on every turn
- `k_base_system_prompt` — constant string: the base system prompt text for all batbox sessions
- `k_plan_mode_prefix` — constant string: the plan mode preamble injected when plan_mode is true

### ToolCallOrchestrator.hpp
Accumulates streaming tool_call deltas and dispatches the completed calls.

- `ToolCallOrchestrator::ToolCallOrchestrator(registry, gate, progress_cb=nullptr)` — constructs with a reference to the ToolRegistry and PermissionGate
- `ToolCallOrchestrator::accumulate(delta)` — feeds one StreamDelta into the ToolCallAccumulator; no-op when delta has no tool_calls
- `ToolCallOrchestrator::dispatch_all(ctx) -> vector<Message>` — finalizes accumulated tool calls, runs the permission gate, dispatches each via registry.dispatch(), returns tool-result messages
