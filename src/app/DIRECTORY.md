# src/app

Application wiring implementations: MCP health polling thread and the three registration functions that connect tools, commands, and TUI components.

## Files

### McpStatusPoller.cpp
jthread polling loop: calls McpServerRegistry health state for each server at tick_interval; compares to previous state; fires on_change callback on transitions.

### WireCommands.cpp
Constructs all 38 slash command objects with their dependencies and calls SlashCommandRegistry::register_command(); handles ordering of phase-1 vs phase-2 commands.

### WireTools.cpp
Constructs all 39 ITool subclasses with injected dependencies (cfg, sidecar, mcp_registry, supervisor, etc.) and registers them via ToolRegistry::register_tool().

### WireTui.cpp
Assembles the FTXUI component tree: Container::Vertical(ChatView, InputBar, PermissionBanner); attaches SubAgentPanel, DemonPanel, and overlay cards; installs root into ScreenManager; starts 10 Hz ticker thread.
