# src/tools

Tool implementations: all 39 ITool subclasses, the ToolRegistry, TaskStore, and CronScheduler.

## Files

### ToolRegistry.cpp
`register_tool()` implementation: inserts into tools_ map and tracks insertion_order_; throws on null or duplicate. `find_by_name()`: unordered_map lookup. `available_tool_schemas()`: filters by allowed names; wraps each schema_json() in {"type":"function","function":...}. `dispatch()`: finds tool, checks plan-mode and allowed_tools constraints, calls run(), catches exceptions.

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
### ToolSearchTool.cpp — `run()` fuzzy-searches ToolRegistry by name and description; returns ranked results.
### VerifyPlanExecutionTool.cpp — `run()` compares PlanMode plan_text against tool call history; reports unexecuted steps.
### WebFetchTool.cpp — `run()` calls SidecarManager::request<FetchRequest,FetchResponse>("/fetch",...); returns FetchResponse.markdown.
### WebSearchTool.cpp — `run()` calls SidecarManager::request<SearchRequest,SearchResponse>("/search",...); formats results.
### WorkflowTool.cpp — `run()` parses step definitions from args; constructs Workflow; calls Workflow::execute(); returns step outputs.
### WriteTool.cpp — `run()` validates path; writes content to tmp file; renames atomically; returns byte count.

### CMakeLists.txt
Build rules for the tools static library.
