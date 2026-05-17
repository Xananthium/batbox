# =============================================================================
# Component Commands — Slash Command Registry (batbox_commands)
# SlashCommandRegistry + 38 slash command implementations.
# Depends on: batbox_core, batbox_config, batbox_session, batbox_permissions,
#             batbox_conversation, batbox_skills, batbox_agents
# =============================================================================

add_library(batbox_commands OBJECT)

set_target_properties(batbox_commands PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

target_include_directories(batbox_commands
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(batbox_commands
    PUBLIC
        batbox_core
        batbox_config
        batbox_session
        batbox_permissions
        batbox_conversation
        batbox_skills
        batbox_agents
        batbox_inference
        batbox_sidecar
        batbox_mcp
)

# ---------------------------------------------------------------------------
# Sources — uncomment as each CPP cmd.x task lands:
#   SlashCommandRegistry.cpp    (CPP cmd.1)
#   HelpCmd.cpp                 (CPP cmd.2)   ExitCmd.cpp           (CPP cmd.3)
#   ClearCmd.cpp                (CPP cmd.4)   InitCmd.cpp           (CPP cmd.5)
#   ModelCmd.cpp                (CPP cmd.6)   ConfigCmd.cpp         (CPP cmd.7)
#   EffortCmd.cpp               (CPP cmd.8)   ThemeCmd.cpp          (CPP cmd.9)
#   OutputStyleCmd.cpp          (CPP cmd.10)  StatusCmd.cpp         (CPP cmd.11)
#   StatsCmd.cpp                (CPP cmd.12)  UsageCmd.cpp          (CPP cmd.13)
#   CostCmd.cpp                 (CPP cmd.14)  ResumeCmd.cpp         (CPP cmd.15)
#   SessionCmd.cpp              (CPP cmd.16)  CompactCmd.cpp        (CPP cmd.17)
#   MemoryCmd.cpp               (CPP cmd.18)  ContextCmd.cpp        (CPP cmd.19)
#   AddDirCmd.cpp               (CPP cmd.20)  FilesCmd.cpp          (CPP cmd.21)
#   DiffCmd.cpp                 (CPP cmd.22)  ReviewCmd.cpp         (CPP cmd.23)
#   SecurityReviewCmd.cpp       (CPP cmd.24)  AgentsCmd.cpp         (CPP cmd.25)
#   PlanCmd.cpp                 (CPP cmd.26)  TasksCmd.cpp          (CPP cmd.27)
#   SkillsCmd.cpp               (CPP cmd.28)  PermissionsCmd.cpp    (CPP cmd.29)
#   HooksCmd.cpp                (CPP cmd.30)  AdvisorCmd.cpp        (CPP cmd.31)
#   McpCmd.cpp                  (CPP cmd.32)  PluginCmd.cpp         (CPP cmd.33)
#   IdeCmd.cpp                  (CPP cmd.34)  VimCmd.cpp            (CPP cmd.35)
#   KeybindingsCmd.cpp          (CPP cmd.36)  TerminalSetupCmd.cpp  (CPP cmd.37)
#   CopyCmd.cpp                 (CPP cmd.38)  DemonCmd.cpp          (CPP cmd.39)
# ---------------------------------------------------------------------------
target_sources(batbox_commands
    PRIVATE
        .placeholder.cpp
        SlashCommandRegistry.cpp
        HelpCmd.cpp
        ExitCmd.cpp
        ClearCmd.cpp
        InitCmd.cpp
        ModelCmd.cpp
        ConfigCmd.cpp
        EffortCmd.cpp
        ThemeCmd.cpp
        OutputStyleCmd.cpp
        StatusCmd.cpp
        StatsCmd.cpp
        UsageCmd.cpp
        CostCmd.cpp
        ResumeCmd.cpp
        SessionCmd.cpp
        CompactCmd.cpp
        MemoryCmd.cpp
        ContextCmd.cpp
        AddDirCmd.cpp
        FilesCmd.cpp
        DiffCmd.cpp
        ReviewCmd.cpp
        SecurityReviewCmd.cpp
        AgentsCmd.cpp
        PlanCmd.cpp
        TasksCmd.cpp
        SkillsCmd.cpp
        PermissionsCmd.cpp
        HooksCmd.cpp
        AdvisorCmd.cpp
        McpCmd.cpp
        PluginCmd.cpp
        IdeCmd.cpp
        VimCmd.cpp
        KeybindingsCmd.cpp
        TerminalSetupCmd.cpp
        CopyCmd.cpp
        DemonCmd.cpp
)
