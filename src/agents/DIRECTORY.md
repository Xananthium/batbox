# src/agents

Agent subsystem implementations: event queue, spec loading, supervisor, team registry, workflow executor, and the Demon advisor agent.

## Files

### AgentEvent.cpp
AgentEventQueue implementation: MPSC push/pop with condition variable and dirty_seq atomic.

### AgentLoader.cpp
Scans user agent directories; parses .md frontmatter; detects dependency cycles via DFS.

### AgentSpec.cpp
AgentSpec::from_file() and AgentSpec::from_type() implementations; maps subagent_type strings to built-in specs.

### AgentSupervisor.cpp
Semaphore-based spawn coordinator; tracks agent lifecycle; implements wait_all() with condition variable.

### Demon.cpp
Demon::spec() and demon_make_spec() implementations; DemonRateLimiter budget tracking.

### SubAgent.cpp
jthread-based single agent execution: inference loop, tool dispatch, event publishing, cooperative cancellation.

### Team.cpp
Team blackboard (unordered_map) with mutex; broadcast queue; TeamRegistry singleton with shared_ptr map.

### Workflow.cpp
Kahn's topological sort; parallel step launching via AgentSupervisor; StopOnFirst/ContinueAll failure handling.

### CMakeLists.txt
Build rules for the agents static library target.
