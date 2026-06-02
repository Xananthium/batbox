# include/batbox/tools

Tool subsystem headers: the ITool interface, ToolRegistry, ToolContext/ToolResult, TaskStore, CronScheduler, and all 39 concrete tool headers.

## Files

### ITool.hpp
Pure-virtual interface all tool implementations satisfy.

- `ITool::name() -> string_view` — returns stable snake_case tool identifier; must match schema_json() "name" field
- `ITool::description() -> string_view` — returns one-sentence description included in the tool schema
- `ITool::schema_json() -> Json` — returns the complete OpenAI tools[*].function JSON object with name, description, and parameters
- `ITool::run(args, ctx) -> ToolResult` — executes the tool; must poll ctx.cancel_token; must never throw; returns ToolResult::error on failure
- `ITool::is_read_only() -> bool` — default false; Plan-mode gate passes read-only tools unconditionally
- `ITool::requires_confirmation() -> bool` — default true; false suppresses interactive permission prompts for this tool

### ToolRegistry.hpp
Central name-to-ITool dispatch table.

- `ToolRegistry::register_tool(tool)` — registers unique_ptr<ITool>; throws on duplicate name or null
- `ToolRegistry::find_by_name(name) -> const ITool*` — returns non-owning pointer; nullptr when not found; valid for registry lifetime
- `ToolRegistry::size() -> size_t` — count of registered tools
- `ToolRegistry::empty() -> bool` — true when no tools registered
- `ToolRegistry::available_tool_schemas(filter) -> vector<Json>` — returns OpenAI tools array; filter=nullopt includes all; filter=vector limits to named tools
- `ToolRegistry::dispatch(name, args, ctx) -> Result<ToolResult, string>` — looks up tool by name; calls run(); wraps thrown exceptions as ToolResult::error; returns Err when tool not found or not allowed; **(S7)** routes every invoked run() result through `envelope()` — the universal subagent-dispatch seam — before returning
- `ToolRegistry::envelope() -> ToolSubagentEnvelope&` — **(S7)** the single seam every dispatched result flows through; install decision/distiller hooks here (default = pass-through)

### ToolSubagentEnvelope.hpp
The universal subagent-dispatch seam (S7, DIS-979). Interposes at `ToolRegistry::dispatch` so every tool result — native and MCP — flows through one unbypassable boundary; default hooks are pure pass-through (byte-identical to pre-S7). S1 fills the decision hook, S4 the distiller hook, without re-touching the seam.

- `IEngulfDecider::should_engulf(tool_name, args, result, ctx) -> bool` — decision hook: "engulf this result into a subagent?"; S7 default `PassThroughDecider` returns false
- `IResultDistiller::distill(tool_name, args, result, ctx) -> ToolResult` — distiller hook: "engulf + distill to the golden line"; S7 default `IdentityDistiller` returns result unchanged
- `ToolSubagentEnvelope::process(tool_name, args, result, ctx) -> ToolResult` — if decider engulfs → run distiller, else pass through
- `ToolSubagentEnvelope::set_decider(hook)` / `set_distiller(hook)` — swap hooks at runtime; null is ignored (never-null invariant)
- `ToolSubagentEnvelope::decider()` / `distiller()` — non-owning const view of the installed hooks

### ToolContext.hpp
Per-dispatch context injected into every ITool::run() call.

- `ToolContext::is_plan_mode() -> bool` — returns true when mode == Plan; tools refuse write operations
- `ToolContext::is_nuclear() -> bool` — returns true when mode == Nuclear; tools skip confirmation prompts
- `ToolContext::is_cancelled() -> bool` — returns true when cancel_token has fired
- `ToolContext::tool_is_allowed(tool_name) -> bool` — returns true when tool_name is in allowed_tools or allowed_tools is absent

### ToolResult.hpp
Result type returned by every ITool::run() call.

- `ToolResult::ok(body) -> ToolResult` — constructs success result with text body; no structured payload
- `ToolResult::ok(body, payload) -> ToolResult` — constructs success result with text body and structured JSON payload for TUI consumers
- `ToolResult::error(body) -> ToolResult` — constructs error result; is_error=true; model receives the error and can self-correct
- `ToolResult::error(body, payload) -> ToolResult` — constructs error result with diagnostic JSON payload

### TaskStore.hpp
Persistent task storage for task CRUD tools backed by ~/.batbox/tasks.json.

- `TaskStore::TaskStore(tasks_path)` — constructs with custom path; parent dirs created on first write
- `TaskStore::default_path() -> fs::path` — returns ~/.batbox/tasks.json
- `TaskStore::create_task(params) -> optional<Task>` — generates UUIDv4; sets created_at/updated_at; saves atomically; returns nullopt on empty title or I/O failure
- `TaskStore::list_tasks(status_filter, tag_filter) -> vector<Task>` — returns all tasks matching optional status and tag filters
- `TaskStore::get_task(id) -> optional<Task>` — looks up task by id; returns nullopt when not found
- `TaskStore::update_task(id, params) -> bool` — applies partial update; sets updated_at; returns true on success
- `TaskStore::load() -> vector<Task>` — reads tasks.json; returns empty vector on missing/parse-error
- `TaskStore::save(tasks) -> bool` — atomically writes task list via tmp+rename; returns false on I/O error

### CronScheduler.hpp
Cron-style scheduler that fires CronEntry prompts on matching wall-clock times.

- `CronScheduler::CronScheduler(cron_path)` — constructs; loads existing entries from cron_path
- `CronScheduler::start()` — starts the jthread tick loop; fires entries on schedule
- `CronScheduler::stop()` — requests jthread stop; returns after thread joins
- `CronScheduler::set_fire_callback(cb)` — registers the callback invoked with matching CronEntry when a cron expression fires

### AskUserQuestionTool.hpp
Presents a structured multiple-choice or single-choice question to the user via TUI modal.

- `AskUserQuestionTool` — is_read_only=false, requires_confirmation=false; run() parses QuestionSpec from args, calls prompt_fn, returns selected labels

### BashTool.hpp
Executes shell commands via forkpty with timeout, output cap, and cooperative cancellation.

- `BashTool` — is_read_only=false, requires_confirmation=true; run() delegates to BashRunner::run() with parsed timeout and max_output_bytes args

### ConfigTool.hpp
Reads and writes batbox configuration fields at runtime.

- `ConfigTool` — is_read_only=false, requires_confirmation=false; run() supports get/set/list subcommands on the live Config under cfg_mutex

### CronCreateTool.hpp
Creates a new cron schedule entry.

- `CronCreateTool` — is_read_only=false, requires_confirmation=false; run() parses cron expression and prompt from args, delegates to CronScheduler

### CronDeleteTool.hpp
Deletes an existing cron schedule entry by id.

- `CronDeleteTool` — is_read_only=false, requires_confirmation=false; run() calls CronScheduler remove by entry id

### CronListTool.hpp
Lists all cron schedule entries.

- `CronListTool` — is_read_only=true, requires_confirmation=false; run() returns all CronEntry objects as JSON

### CtxInspectTool.hpp
Reports current conversation context window usage and model limits.

- `CtxInspectTool` — is_read_only=true, requires_confirmation=false; run() returns token count estimate, context limit, and compaction threshold

### EditTool.hpp
Applies a targeted string replacement within an existing file.

- `EditTool` — is_read_only=false, requires_confirmation=true; run() reads file, replaces old_string with new_string, writes atomically, returns unified diff

### EnterPlanModeTool.hpp
Transitions the conversation into Plan mode.

- `EnterPlanModeTool` — is_read_only=true, requires_confirmation=false; run() calls PlanMode::enter_plan() and returns confirmation

### ExitPlanModeTool.hpp
Transitions out of Plan mode after user approval.

- `ExitPlanModeTool` — is_read_only=false, requires_confirmation=false; run() calls PlanMode::approve() with the provided plan text; the tool IS the confirmation mechanism

### GlobTool.hpp
Recursively finds files matching a glob pattern.

- `GlobTool` — is_read_only=true, requires_confirmation=false; run() walks the filesystem from args.path, returns list of matching relative file paths

### GrepTool.hpp
Searches file content for a regex pattern.

- `GrepTool` — is_read_only=true, requires_confirmation=false; run() uses std::regex or ripgrep depending on availability, returns matching lines with file:line context

### ListMcpResourcesTool.hpp
Lists resources exposed by a named MCP server.

- `ListMcpResourcesTool` — is_read_only=true, requires_confirmation=false; run() calls McpClient::resources_list and returns the resource list as JSON

### ListPeersTool.hpp
Lists currently running sub-agents from AgentSupervisor.

- `ListPeersTool` — is_read_only=true, requires_confirmation=false; run() returns snapshot of all live agents with id/name/status

### McpTool.hpp
Calls an arbitrary method on a named MCP server.

- `McpTool` — is_read_only=false, requires_confirmation=true; run() calls McpClient::tools_call with server, tool_name, and args from the JSON arguments

### PowerShellTool.hpp
Executes a PowerShell command (Windows/cross-platform shim).

- `PowerShellTool` — is_read_only=false, requires_confirmation=true; run() invokes pwsh or powershell.exe with the command string

### ReadMcpResourceTool.hpp
Reads the content of a specific MCP server resource by URI.

- `ReadMcpResourceTool` — is_read_only=true, requires_confirmation=false; run() calls McpClient::resources_read with server and URI

### ReadTool.hpp
Reads a file from disk with optional line range and offset.

- `ReadTool` — is_read_only=true, requires_confirmation=false; run() reads file content; supports offset and limit args; returns content or Err when path not found

### RemoteTriggerTool.hpp
Sends a trigger payload to an agent's input queue by agent_id.

- `RemoteTriggerTool` — is_read_only=false, requires_confirmation=true; run() calls AgentSupervisor::enqueue_message for the target agent

### SendMessageTool.hpp
Sends a message to a named team or specific agent.

- `SendMessageTool` — is_read_only=false, requires_confirmation=false; run() looks up team or agent and calls broadcast() or enqueue_message()

### SkillTool.hpp
Executes a named skill by expanding its template body as a user message.

- `SkillTool` — is_read_only=false, requires_confirmation=false; run() looks up skill in SkillLoader, renders template variables from args, injects result as user message

### SleepTool.hpp
Pauses execution for a specified number of seconds.

- `SleepTool` — is_read_only=true, requires_confirmation=false; run() sleeps for args.seconds while polling cancel_token; returns elapsed time

### SnipTool.hpp
CRUD operations on the user's text snippet store.

- `SnipTool` — is_read_only=false, requires_confirmation=false; run() dispatches to save/load/list/delete subcommands on the snippet JSON file

### TaskCreateTool.hpp
Creates a new persistent task in ~/.batbox/tasks.json.

- `TaskCreateTool` — is_read_only=false, requires_confirmation=false; run() calls TaskStore::create_task with title, description, status, parent_id, tags

### TaskGetTool.hpp
Retrieves a single task by id.

- `TaskGetTool` — is_read_only=true, requires_confirmation=false; run() calls TaskStore::get_task and returns the Task as JSON

### TaskListTool.hpp
Lists tasks with optional status and tag filters.

- `TaskListTool` — is_read_only=true, requires_confirmation=false; run() calls TaskStore::list_tasks and returns matching Tasks as JSON array

### TaskOutputTool.hpp
Retrieves the current output buffer of a running sub-agent.

- `TaskOutputTool` — is_read_only=true, requires_confirmation=false; run() calls AgentSupervisor::snapshot and extracts last_5_lines for the target agent_id

### TaskStopTool.hpp
Cancels a running sub-agent by agent_id.

- `TaskStopTool` — is_read_only=false, requires_confirmation=false; run() calls AgentSupervisor::cancel for the target agent_id

### TaskTool.hpp
Spawns a new sub-agent task via AgentSupervisor.

- `TaskTool` — is_read_only=false, requires_confirmation=false; run() resolves subagent_type to AgentSpec, calls AgentSupervisor::spawn, returns agent_id

### TaskUpdateTool.hpp
Partially updates an existing task by id.

- `TaskUpdateTool` — is_read_only=false, requires_confirmation=false; run() calls TaskStore::update_task with any provided optional fields

### TeamCreateTool.hpp
Creates a named agent team in the TeamRegistry.

- `TeamCreateTool` — is_read_only=false, requires_confirmation=false; run() calls TeamRegistry::create_team and optionally adds initial members

### TeamDeleteTool.hpp
Deletes a named agent team from the TeamRegistry.

- `TeamDeleteTool` — is_read_only=false, requires_confirmation=false; run() calls TeamRegistry::delete_team by name

### TodoWriteTool.hpp
Writes a structured todo/checklist to the conversation session state.

- `TodoWriteTool` — is_read_only=false, requires_confirmation=false; run() serialises the todo JSON and stores it in the session's metadata

### ToolSearchTool.hpp
Fuzzy-searches the ToolRegistry by name and description.

- `ToolSearchTool` — is_read_only=true, requires_confirmation=false; run() calls ToolRegistry fuzzy matching and returns ranked results

### VerifyPlanExecutionTool.hpp
Checks whether all plan steps have been executed and reports gaps.

- `VerifyPlanExecutionTool` — is_read_only=true, requires_confirmation=false; run() compares PlanMode plan_text against completed tool calls and returns a gap report

### WebFetchTool.hpp
Fetches a URL via the Scrapling sidecar and returns Markdown-rendered content.

- `WebFetchTool` — is_read_only=true, requires_confirmation=false; run() calls SidecarManager::request<FetchRequest, FetchResponse>("/fetch", ...) and returns the markdown field

### WebSearchTool.hpp
Searches the web via the Scrapling sidecar and returns ranked results.

- `WebSearchTool` — is_read_only=true, requires_confirmation=false; run() calls SidecarManager::request<SearchRequest, SearchResponse>("/search", ...) and returns formatted results

### WorkflowTool.hpp
Executes a declarative agent workflow DAG.

- `WorkflowTool` — is_read_only=false, requires_confirmation=false; run() parses workflow steps from args, constructs a Workflow, calls Workflow::execute, returns step outputs as JSON

### WriteTool.hpp
Creates or overwrites a file with provided content.

- `WriteTool` — is_read_only=false, requires_confirmation=true; run() writes content to path atomically via tmp+rename; returns byte count or error
