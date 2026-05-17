# =============================================================================
# Component C6 — Sub-agent Supervisor (batbox_agents)
# Task family tools, thread pool, bounded parallelism (4 parallel).
# AgentEvent stream, /demon easter-egg controller.
# Depends on: batbox_core, batbox_config, batbox_permissions
# =============================================================================

add_library(batbox_agents STATIC)

set_target_properties(batbox_agents PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_agents
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_agents
    PUBLIC
        batbox_core
        batbox_config
        batbox_permissions
        batbox_session
        batbox_conversation
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP 6.x task lands:
#   AgentSupervisor.cpp  (CPP 6.5) — semaphore-bounded spawn + snapshot
#   SubAgent.cpp         (CPP 6.4) — per-agent execution context + lifecycle
#   AgentSpec.cpp        (CPP 6.2) — spec struct, JSON deserialisation
#   AgentLoader.cpp      (CPP 6.3) — scan ~/.batbox/agents/ for agent .md files
#   AgentEvent.cpp                 — AgentEvent variants + MPSC queue
#   Team.cpp             (CPP 6.6) — TeamCreate / TeamDelete grouping
#   Workflow.cpp         (CPP 6.7) — Workflow tool DAG execution
#   Demon.cpp            (CPP 6.8) — /demon controller (5Hz panel + direct cmds)
# ---------------------------------------------------------------------------
target_sources(batbox_agents
    PRIVATE
        .placeholder.cpp
        AgentSupervisor.cpp   # CPP 6.5 — semaphore-bounded spawn + snapshot (full impl)
        SubAgent.cpp          # CPP 6.4 — per-agent jthread + ConversationEngine
        AgentSpec.cpp         # CPP 6.2 — frontmatter-driven agent config
        AgentLoader.cpp       # CPP 6.3 — scan ~/.batbox/agents/ for agent .md files
        AgentEvent.cpp
        Team.cpp          # CPP 6.6 — named team groups + KV blackboard
        Workflow.cpp
        Demon.cpp            # CPP 6.8 — Party Monster easter-egg agent spec + rate limiter
        # FrontmatterParser.cpp is compiled directly here (not via batbox_plugins)
        # to avoid a batbox_agents ↔ batbox_plugins circular dependency.
        # batbox_plugins (C11) depends on batbox_agents (C6); so C6 must not
        # link C11.  AgentSpec needs the parser at C6 level, so we include the
        # parser source directly here.  batbox_plugins compiles it again for the
        # plugin layer — OBJECT library duplication is intentional.
        ${PROJECT_SOURCE_DIR}/src/plugins/FrontmatterParser.cpp
)
