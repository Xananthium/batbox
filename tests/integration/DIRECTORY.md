# tests/integration

Integration tests covering cross-component behaviour with real subprocess spawning, fake HTTP servers, and filesystem operations. Tests run with fake network services (fake_openai_server.py, fake_mcp_*.py, fake_scrapling_server.py) to avoid external dependencies.

All test files are named `test_<feature>.cpp` and use doctest. Registered with CTest under the "integration" label.

## Files (representative selection)

### test_agent_loader.cpp — AgentLoader scans real .md files; dependency cycle detection
### test_agent_planning_commands.cpp — /agents slash command output format
### test_agent_supervision_bounded.cpp — AgentSupervisor 4-slot semaphore blocks fifth spawn
### test_agent_supervision_integration.cpp — spawn/cancel/wait_all lifecycle with real SubAgents
### test_app_init.cpp — App::run() startup sequence; all subsystems initialise without error
### test_app_shutdown.cpp — App::shutdown() reverse-of-init teardown; idempotency guard
### test_ask_user_question.cpp — AskUserQuestionTool dispatches QuestionCard and returns selections
### test_bash_tool.cpp — BashTool forkpty execution; timeout; cancellation; output cap
### test_compactor.cpp — Compactor::compact() sends summarization request and installs result
### test_config_reload.cpp — reload_config() diffs changed fields; fires ConfigReloadBus
### test_config_tool.cpp — ConfigTool get/set subcommands read/write live Config
### test_conversation_basic.cpp — Conversation::run_turn() sends messages and returns assistant response
### test_conversation_tool_loop.cpp — multi-turn tool call loop with fake OpenAI server
### test_copy_command.cpp — /copy retrieves last assistant message
### test_cron_scheduler.cpp — CronScheduler fires callback on matching wall-clock minute
### test_e2e_cli_smoke.cpp — full CLI invocation with fake OpenAI server; verifies stdout output
### test_edit_tool.cpp — EditTool string replacement; atomic write; diff output
### test_grep_glob.cpp — GrepTool and GlobTool on real filesystem fixtures
### test_headless_print.cpp — --print mode produces correct stdout output format
### test_inference_smoke_openai_compat.cpp — Client::chat() and stream_chat() against fake_openai_server
### test_mcp_*.cpp — MCP transport integration tests using fake_mcp_*.py servers
### test_nuclear_mode.cpp — Nuclear permission mode bypasses all confirmation prompts
### test_permission_gate.cpp — PermissionGate seven-step decision flow integration
### test_plan_mode.cpp — full plan mode lifecycle: enter → approve → execute → exit
### test_plugin_loader_e2e.cpp — PluginLoader scans sample_plugins_e2e fixtures
### test_repl.cpp — Repl::handle_input prefix dispatch; multi-line continuation
### test_session_resume.cpp — SessionStore new_session → append_message → load → resume_for_cwd
### test_sidecar_lifecycle.cpp — SidecarManager ensure_started/shutdown lifecycle
### test_subagent.cpp — SubAgent jthread execution; cancel; snapshot
### test_task_family.cpp — TaskCreateTool/TaskListTool/TaskGetTool/TaskUpdateTool CRUD
### test_team_blackboard.cpp — Team::set_kv/get_kv/cas_kv/broadcast correctness
### test_webfetch_via_sidecar.cpp — WebFetchTool via fake_scrapling_server.py
### test_websearch_via_sidecar.cpp — WebSearchTool via fake_scrapling_server.py
### test_workflow_dag.cpp — Workflow topological sort and parallel execution

### SubAgentStub.cpp
Helper implementation for integration tests that need a controllable agent without running real inference.

### cli_smoke_helpers.hpp
Shared helpers for CLI smoke tests: process spawn, stdout capture, fake server port allocation.

### perf_probes.hpp
Helper macros for measuring wall-clock time in performance budget tests (test_perf_budget.cpp).

### CMakeLists.txt
Links integration test binary; sets up Python path for fake server scripts; registers with CTest.
