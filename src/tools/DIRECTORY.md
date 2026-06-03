# src/tools

Tool implementations: all 39 ITool subclasses, the ToolRegistry, TaskStore, and CronScheduler.

## Files

### ToolRegistry.cpp
`register_tool()` implementation: inserts into tools_ map and tracks insertion_order_; throws on null or duplicate. `find_by_name()`: unordered_map lookup. `available_tool_schemas()`: filters by allowed names; wraps each schema_json() in {"type":"function","function":...}. `dispatch()`: finds tool, checks plan-mode and allowed_tools constraints, calls run(), catches exceptions; **(S7)** routes the invoked run() result (normal or exception-wrapped) through `envelope_.process()` — the universal subagent-dispatch seam — before returning. Pre-run rejections (unknown/not-allowed/plan-mode) short-circuit before the seam.

### ToolSubagentEnvelope.cpp
S7 (DIS-979) implementation of the universal subagent-dispatch seam. `process()`: applies the decision hook; if it engulfs, runs the distiller hook, else returns the result unchanged. Default-constructs to `PassThroughDecider` + `IdentityDistiller` (pure pass-through). `set_decider`/`set_distiller` swap hooks, ignoring null to keep the never-null invariant. This is the one place batbox guarantees every tool result becomes a subagent result; S1 fills the decision hook, S4 the distiller hook.

### TaskStore.cpp
`create_task()`: generates UUIDv4; sets timestamps; calls save(). `list_tasks()` and `get_task()`: call load() and filter. `update_task()`: loads, finds by id, applies optional fields, saves. `save()`: serialises vector<Task> as JSON array; writes to .tmp; renames atomically.

### CronScheduler.cpp
`start()` launches jthread running scheduler_loop(); `scheduler_loop()` ticks every 60 s aligned to minute boundary; checks each enabled CronEntry against wall-clock time; fires callback on match; persists changes via atomic write.

### AskUserQuestionTool.cpp — `run()` parses QuestionSpec from JSON args; calls prompt_fn; returns selected label strings as JSON array.
### BashTool.cpp — `run()` extracts command, cwd, timeout from args; delegates to BashRunner::run(); converts BashResult to ToolResult.
### ConfigTool.cpp — `run()` dispatches get/set/list subcommands; reads/writes Config fields under cfg_mutex.
### CronCreateTool.cpp — `run()` parses expression and prompt; calls CronScheduler add entry; returns new entry id.
### CronDeleteTool.cpp — `run()` removes cron entry by id from CronScheduler.
### CronListTool.cpp — `run()` returns all CronEntry objects as JSON array.
### CtxInspectTool.cpp — `run()` estimates tokens on current messages; returns token count, limit, and compact threshold.
### EditTool.cpp — `run()` reads file; replaces old_string with new_string; writes atomically; returns unified diff string.
### EnterPlanModeTool.cpp — `run()` calls PlanMode::enter_plan(); returns "Plan mode activated" confirmation.
### ExitPlanModeTool.cpp — `run()` calls PlanMode::approve() with plan_text from args; returns the plan_id.
### GlobTool.cpp — `run()` walks filesystem from args.path with std::filesystem::recursive_directory_iterator; filters against glob pattern; returns matching paths.
### GrepTool.cpp — `run()` searches with std::regex or ripgrep subprocess; returns file:line:content matches.
### ListMcpResourcesTool.cpp — `run()` calls McpClient::resources_list; formats and returns resource list.
### ListPeersTool.cpp — `run()` calls AgentSupervisor::snapshot; returns agent id/name/status as JSON.
### McpTool.cpp — `run()` calls McpClient::tools_call with server, tool_name, args.
### PowerShellTool.cpp — `run()` invokes pwsh/powershell.exe subprocess with command string.
### ReadMcpResourceTool.cpp — `run()` calls McpClient::resources_read with server and URI.
### ReadTool.cpp — `run()` reads file with optional offset/limit; returns content lines.
### RemoteTriggerTool.cpp — `run()` calls AgentSupervisor::enqueue_message for target agent_id.
### SendMessageTool.cpp — `run()` routes to Team::broadcast() or AgentSupervisor::enqueue_message.
### SkillTool.cpp — `run()` looks up skill; renders template variables from args; injects via Repl::feed_line() equivalent.
### SleepTool.cpp — `run()` sleeps in 100 ms increments; polls cancel_token; returns elapsed seconds.
### SnipTool.cpp — `run()` dispatches save/load/list/delete on the snippet JSON file; atomic writes.
### TaskCreateTool.cpp — `run()` calls TaskStore::create_task; returns created Task as JSON.
### TaskGetTool.cpp — `run()` calls TaskStore::get_task; returns Task JSON or error.
### TaskListTool.cpp — `run()` calls TaskStore::list_tasks with optional filters; returns array.
### TaskOutputTool.cpp — `run()` calls AgentSupervisor::snapshot; finds target agent; returns last_5_lines.
### TaskStopTool.cpp — `run()` calls AgentSupervisor::cancel for target agent_id.
### TaskTool.cpp — `run()` resolves subagent_type via AgentSpec::from_type(); calls AgentSupervisor::spawn; returns agent_id.
### TaskUpdateTool.cpp — `run()` calls TaskStore::update_task with optional field overrides.
### TeamCreateTool.cpp — `run()` calls TeamRegistry::create_team; optionally adds initial member agent_ids.
### TeamDeleteTool.cpp — `run()` calls TeamRegistry::delete_team by name.
### TodoWriteTool.cpp — `run()` serialises todo JSON into session state metadata.
### NotepadStore.cpp — (DIS-981 S6) disk-backed, session-keyed working-memory pad: `append()` (with light `## section` header, never overwrite), `read()`/`grep()`/`reinjection_slice()`, `export_pad()`/`archive()`. Out-of-band markdown file per session key → survives compaction by construction. Process-wide write mutex; reads lock-free.
### NotepadAppendTool.cpp — (DIS-981 S6) `notepad_append` write ITool; jots `note` (+optional `section`) to the session's pad. is_read_only=false, requires_confirmation=false. LEAST_FORCE: jots the nugget, not the transcript.
### NotepadReadTool.cpp — (DIS-981 S6) `notepad_read` read/grep ITool; `query` greps matching entries, omit for whole pad. is_read_only=true.
### ToolSearchTool.cpp — `run()` fuzzy-searches ToolRegistry by name and description; returns ranked results.
### VerifyPlanExecutionTool.cpp — `run()` compares PlanMode plan_text against tool call history; reports unexecuted steps.
### WebFetchTool.cpp — `run()` calls SidecarManager::request<FetchRequest,FetchResponse>("/fetch",...); returns FetchResponse.markdown.
### WebSearchTool.cpp — `run()` calls SidecarManager::request<SearchRequest,SearchResponse>("/search",...); formats results.
### WorkflowTool.cpp — `run()` parses step definitions from args; constructs Workflow; calls Workflow::execute(); returns step outputs.
### WriteTool.cpp — `run()` validates path; writes content to tmp file; renames atomically; returns byte count.

### ThresholdEngulfDecider.cpp — S1 (DIS-980). `should_engulf()`: engulf iff a non-error result's body strictly exceeds the configured byte threshold; never engulfs errors; size is the trigger not tool identity.
### ReportGoldTool.cpp — S4 (DIS-980). The `report_gold(answer, confidence?, follow_up_ok?)` structured handoff: `schema_json()`, `parse()` (shared shape parser), `run()` surfaces the parsed result in `structured_payload`. The distiller's internal contract (not in the 39-tool surface).
### SubagentDistiller.cpp — S4 (DIS-980). `distill()`: one-shot call to the LOCAL distill endpoint (`cfg.distill.*`, not `cfg.api`), forces `report_gold` via tool_choice, harvests the gold, falls back to the original on any failure/cancel (never throws). `extract_gold()` (DIS-1007) is the no-throw single-source-of-truth report_gold harvest distill() builds its result from and StandingSelector reuses for the standing first-turn. `install_subagent_distillation()` wires the decider+distiller into the registry envelope at startup (no-op when disabled; S7 seam untouched).

### SelectionHeuristic.cpp — DIS-1007. The predict-ahead investigation-vs-lookup classifier: `ISelectionHeuristic` interface + `ShapeSelectionHeuristic` returning `DispatchMode{Closed,Standing}`. Investigation iff (broad-search tool: grep/glob/web_search/web_fetch — tool identity IS a legitimate input here, unlike S1's size-not-identity engulf trigger) OR (result shape: large body / many sections). Errors are never investigations. Caller NEVER flags (no `background` boolean — anti-pattern #3); the harness infers from `tool_name` + `args` + `ToolResult`. Pure, cheap, side-effect-free.
### StandingSelector.cpp — DIS-1007. THE closed-vs-standing selection organ ("predict-ahead, confirm-after"). A new `IResultDistiller` that WRAPS the closed `SubagentDistiller` it owns. CLOSED (default, byte-identical): no investigation signal → delegate verbatim to the closed one-shot. STANDING (investigation predicted): distill the first turn to gold via the shared `report_gold` contract (`extract_gold`), then CONFIRM-AFTER on `follow_up_ok`/`confidence` — keep-warm → spawn a real warm `SubAgent` on the distill endpoint (`EndpointOverride{use_distill_endpoint=true}`) via `AgentSupervisor::spawn`/`promote` (LRU-bounded, lossless eviction); done/trivial-lookup → return the gold closed-equivalent, no window. Fail-closed (AC5): any standing-path failure → closed fallback → original result; never throws/hangs. Four observable paths: CLOSED / PROMOTE / FOLLOW_UP_OK-CANCEL / standing→closed FALLBACK. `install_standing_selection()` wires decider + StandingSelector into the envelope (no-op when disabled; null supervisor → closed-identical; S7 seam untouched).

### CMakeLists.txt
Build rules for the tools static library.
