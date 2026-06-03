# include/batbox/agents

Public headers for the sub-agent execution subsystem: event bus, agent specs, supervisor, team/blackboard, and workflow DAG.

## Files

### AgentEvent.hpp
MPSC event queue for inter-agent communication.

- `AgentEvent::make_started(agent_id)` — constructs a Started event for agent lifecycle tracking
- `AgentEvent::make_step_began(agent_id, step)` — constructs a StepBegan event carrying the step description
- `AgentEvent::make_token_appended(agent_id, token)` — constructs a TokenAppended event for streaming output
- `AgentEvent::make_tool_call_began(agent_id, name, args)` — constructs a ToolCallBegan event
- `AgentEvent::make_tool_call_ended(agent_id, name, result)` — constructs a ToolCallEnded event
- `AgentEvent::make_completed(agent_id, result)` — constructs a Completed event with final output
- `AgentEvent::make_errored(agent_id, msg)` — constructs an Errored event with error message
- `AgentEvent::make_cancelled(agent_id)` — constructs a Cancelled event
- `AgentEvent::make_parent_message_observed(agent_id, msg)` — constructs a ParentMessageObserved event
- `AgentEvent::make_queued(agent_id)` — constructs a Queued event (semaphore wait)
- `AgentEventQueue::push(event)` — enqueues an AgentEvent; increments dirty_seq atomically
- `AgentEventQueue::try_pop() -> optional<AgentEvent>` — pops next event; returns nullopt when empty, never blocks
- `AgentEventQueue::wait_pop(stop_token) -> optional<AgentEvent>` — blocks until event available or stop_token fires
- `AgentEventQueue::drain() -> vector<AgentEvent>` — drains all pending events without blocking
- `AgentEventQueue::dirty_seq() -> uint64_t` — returns monotonic counter incremented on every push; TUI polls this for change detection
- `AgentEventQueue::size() -> size_t` — returns approximate queue depth
- `AgentEventQueue::empty() -> bool` — returns true when queue is empty

### AgentLoader.hpp
Scans user directories for agent .md files and loads them as AgentSpec objects.

- `AgentLoader::load() -> Result<void>` — scans default agent dirs, parses frontmatter, populates the registry
- `AgentLoader::load_from(dir) -> Result<void>` — scans a single directory for agent .md files
- `AgentLoader::reload() -> Result<void>` — rescans all registered dirs, replaces existing entries
- `AgentLoader::names() -> vector<string>` — returns names of all loaded agents
- `AgentLoader::get(name) -> optional<AgentSpec>` — looks up agent by name; returns nullopt if not found
- `AgentLoader::size() -> size_t` — returns number of loaded agent specs
- `AgentLoader::has_cycle_error() -> bool` — returns true when a dependency cycle was detected during load

### AgentSpec.hpp
Agent specification parsed from a .md file's YAML frontmatter.

- `AgentSpec::from_file(path) -> Result<AgentSpec>` — reads .md file, parses YAML frontmatter, returns populated AgentSpec or error
- `AgentSpec::from_type(subagent_type) -> AgentSpec` — looks up built-in agent spec by subagent_type string (e.g. "database-agent")
- `EndpointOverride` (struct) — optional per-agent inference-endpoint override (AC1, DIS-988). `use_distill_endpoint` reuses `cfg.distill.*` (the local 3090), or set `base_url`/`api_key`/`model` explicitly. Carried on `AgentSpec::endpoint` (nullopt → target `cfg.api` unchanged). Lets a standing subagent run on local free compute; threaded through `SubAgent::run()` via the S8 `ProviderRegistry`.

### AgentSupervisor.hpp
Semaphore-based agent execution supervisor + LRU-bounded standing-subagent registry (S2/S3, DIS-988).

- `AgentSupervisor::AgentSupervisor()` — constructs with default max_concurrent=4
- `AgentSupervisor::AgentSupervisor(max_concurrent)` — constructs with explicit concurrency limit
- `AgentSupervisor::spawn(spec, prompt, parent_id, ct) -> string` — spawns a SubAgent in a jthread, acquires semaphore slot, returns agent_id UUID
- `AgentSupervisor::snapshot() -> vector<AgentSnapshot>` — returns current status snapshot for all live agents
- `AgentSupervisor::cancel(agent_id)` — fires the cancel token for a specific agent
- `AgentSupervisor::enqueue_message(agent_id, message)` — injects a message into a running agent's queue
- `AgentSupervisor::wait_all()` — blocks until all spawned agents reach terminal state
- `AgentSupervisor::promote(agent_id)` — marks a subagent STANDING (warm window kept alive at quiescence) and registers it in the LRU pool; hands its active-work slot back so parked windows don't starve spawns; evicts least-recently-interrogated over `max_standing`. Idempotent; safe no-op on unknown handle (AC2/AC3)
- `AgentSupervisor::interrogate(agent_id, question) -> string` — issues a follow-up turn against a standing subagent's still-engulfed context and returns the answer (blocking); refreshes LRU recency; "" on unknown/non-standing/evicted handle; never hangs/throws (AC2/AC5)
- `AgentSupervisor::set_max_standing_subagents(n)` — sets the hard LRU bound on the standing pool; lowering it evicts down to fit (AC5)
- `AgentSupervisor::standing_status() -> vector<StandingStatus>` — bounded {id,name,status_line} list of warm subagents, most-recently-interrogated first; source for the AC4 status line
- `AgentSupervisor::standing_count() -> size_t` — number of subagents currently in the standing pool
- `AgentSupervisor::set_agent_config(cfg)` — override the Config handed to subsequently-spawned SubAgents (default is `Config::load_default()` pure defaults); lets callers/hermetic tests point spawned agents at a specific endpoint. Affects only later spawns; not concurrent with spawn()
- `StandingStatus` (struct) — `{id, name, status_line}` one-line-per-warm-window status record (AC4)

### Demon.hpp
The Demon advisor agent spec and rate limiter.

- `DemonRateLimiter::is_allowed(session_token_budget) -> bool` — returns true when token budget permits a new Demon comment
- `DemonRateLimiter::record_comment(tokens)` — records a comment's token cost against the budget
- `Demon::spec() -> AgentSpec` — returns the static AgentSpec for the built-in Demon advisor agent
- `demon_make_spec() -> AgentSpec` — free function returning the Demon AgentSpec; used by WireCommands

### SubAgent.hpp
Single agent execution unit running in a jthread.

- `SubAgent::SubAgent(agent_id, spec, initial_prompt, parent_ct, event_queue, cfg, on_exit)` — constructs but does not start the agent thread
- `SubAgent::start()` — launches the jthread, begins inference loop
- `SubAgent::cancel()` — requests cooperative cancellation via stop_token
- `SubAgent::enqueue_message(message)` — pushes a message into the agent's pending input queue
- `SubAgent::snapshot() -> AgentSnapshot` — returns current status, step, last 5 output lines, token count
- `SubAgent::id() -> string` — returns the agent's UUID string
- `SubAgent::name() -> string` — returns the agent's display name (from AgentSpec)
- `SubAgent::status() -> SubAgentStatus` — returns current lifecycle state (queued/running/done/failed/cancelled)
- `SubAgent::promote()` — marks the agent STANDING: at quiescence the run loop PARKS (conv kept alive on the jthread) instead of collapsing to a string and exiting (AC2, DIS-988)
- `SubAgent::is_standing() -> bool` — true once promoted
- `SubAgent::interrogate(question) -> future<string>` — queues a follow-up turn against the warm window; future resolves to the answer, or the empty sentinel if not standing/terminated/cancelled (never hangs, AC5)
- `SubAgent::last_result() -> string` — most recent quiescent result summary (the status-line source)
- `status_label(SubAgentStatus) -> string_view` — returns human-readable label for the status enum

### Team.hpp
Shared-memory blackboard and broadcast bus for cooperating agents.

- `Team::add_member(agent_id)` — registers an agent as a member of this team
- `Team::remove_member(agent_id)` — unregisters an agent from this team
- `Team::members() -> vector<string>` — returns current member agent_ids
- `Team::broadcast(message)` — enqueues message to all members' input queues
- `Team::drain_pending_broadcasts() -> vector<string>` — returns and clears pending broadcast messages
- `Team::set_kv(key, value)` — writes a key-value pair to the blackboard; wakes waiters
- `Team::get_kv(key) -> optional<string>` — reads a key from the blackboard; returns nullopt if absent
- `Team::cas_kv(key, expected, desired) -> bool` — atomic compare-and-swap on blackboard entry; returns true on success
- `Team::erase_kv(key)` — removes a key from the blackboard
- `Team::blackboard_size() -> size_t` — returns number of entries in the blackboard
- `TeamRegistry::instance() -> TeamRegistry&` — returns the process-global singleton
- `TeamRegistry::create_team(name) -> shared_ptr<Team>` — creates and registers a named team; throws if name already exists
- `TeamRegistry::get_team(name) -> shared_ptr<Team>` — looks up team by name; returns nullptr if not found
- `TeamRegistry::delete_team(name)` — removes a team from the registry
- `TeamRegistry::team_names() -> vector<string>` — lists all registered team names
- `TeamRegistry::size() -> size_t` — returns count of registered teams

### Workflow.hpp
Declarative DAG-style agent workflow executor using Kahn's topological sort.

- `Workflow::Workflow()` — constructs with default StopOnFirst failure policy
- `Workflow::Workflow(policy)` — constructs with explicit FailurePolicy
- `Workflow::add_step(step)` — appends a WorkflowStep to the DAG
- `Workflow::add_step(name, agent, prompt, deps)` — convenience overload; constructs and appends WorkflowStep
- `Workflow::execute(supervisor, ct) -> Result<map<string,string>, string>` — topologically sorts steps, spawns agents via supervisor, returns map of step_name -> output or first error
- `Workflow::steps() -> vector<WorkflowStep>` — returns the registered step list
- `Workflow::failure_policy() -> FailurePolicy` — returns current failure policy
- `Workflow::set_failure_policy(policy)` — updates the failure policy
