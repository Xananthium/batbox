# tests/unit

Unit tests covering individual components in isolation. Each test file corresponds to one class or module; tests run without network I/O using doctest.

All test files follow the naming convention `test_<component>.cpp`. Tests are registered with CTest via DoctestAddTestsPatched and labelled "unit".

## Files (representative selection)

### test_agent_event.cpp — AgentEventQueue push/pop/wait_pop/drain/dirty_seq behaviour
### test_agent_spec.cpp — AgentSpec::from_file and AgentSpec::from_type parsing
### test_agents_config.cpp — load_agents_config() JSON parsing and default handling
### test_autocomplete.cpp — Autocomplete::complete() context dispatch and fuzzy scoring
### test_bash_tool_itool.cpp — BashTool ITool contract: name/description/schema_json/is_read_only
### test_bundled_skills.cpp — BundledSkillsRegistry::all() parses all 13 embedded skills
### test_cancel_token.cpp — CancelToken child/combine/on_cancel behaviour
### test_cancel.cpp — CancelSource::request_stop propagation
### test_changelog.cpp — ChangelogEntry parsing and version comparison
### test_chat_view.cpp — ChatView message append and spinner state
### test_chat_wire_model.cpp — ChatRequest/ChatResponse JSON serialisation round-trip
### test_client_reasoning.cpp — Client::stream_chat reasoning_content delta accumulation
### test_commands_s1.cpp — Phase 1 slash command execution contracts
### test_config_load.cpp — Config::load() env var and settings.json precedence
### test_context_window_estimator.cpp — ContextWindow::estimate_tokens heuristic correctness
### test_conversation_cancel.cpp — Conversation::run_turn cooperative cancellation
### test_cron_expr.cpp — CronExpr parse and match logic for all field types
### test_ctx_inspect.cpp — CtxInspectTool run() output format
### test_demon_panel.cpp — DemonPanel tagline rotation and visibility state
### test_diff_card.cpp — DiffCard row parsing and scroll offset
### test_editor_launch.cpp — resolve_editor() priority and binary_accessible() lookup
### test_env_loader.cpp — load_env_file() KEY=VALUE parsing including quoted values
### test_frontmatter_parser.cpp — parse_frontmatter() scalar/list/bool/int/quoted/no-frontmatter cases
### test_glob_tool.cpp — GlobTool path traversal and glob pattern matching
### test_history_search.cpp — HistorySearch::filter_matches scoring and cycling
### test_history.cpp — History push/previous/next/save/load with cap eviction
### test_input_bar_segments.cpp — InputBar segment vector flatten/reconstruct
### test_input_bar.cpp — InputBar autocomplete popup and VimMode integration
### test_input_queue.cpp — InputQueue depth/empty/flatten_entry
### test_tool_subagent_envelope.cpp — ToolSubagentEnvelope (S7): default pass-through; decision hook gates the distiller; swappable hooks; null-hook fallback / never-null invariant
### test_threshold_decider.cpp — ThresholdEngulfDecider (S1, DIS-980): engulf at/above/below the boundary (strictly-greater); error results never engulfed; size-is-trigger; side-effect-free
### test_report_gold.cpp — ReportGoldTool (S4, DIS-980): schema shape (answer required); `parse()` valid/missing/empty/non-string/non-object + ignored wrong-typed optionals; `run()` surfaces the structured result / errors on invalid
### test_distill_config.cpp — DistillConfig (S1+S4, DIS-980): defaults; `BATBOX_DISTILL_*` + threshold env loading; `validate()` (enabled requires URL base_url + model; thresholds/timeouts > 0)

### CMakeLists.txt
Links unit test binary against all batbox static libraries; registers with CTest.
