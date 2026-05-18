# include/batbox/app

Application wiring headers: MCP health polling, and the three wire-up functions that register all slash commands, tools, and TUI components.

## Files

### McpStatusPoller.hpp
Background thread that polls MCP server health and fires a callback on state change.

- `McpStatusPoller::McpStatusPoller(registry, on_change, tick_interval=1s)` — constructs and immediately starts the polling jthread; calls on_change(server_name, new_health_state) whenever a server's health changes
- `McpStatusPoller::~McpStatusPoller()` — requests jthread stop, joins the polling thread

### WireCommands.hpp
Registers all 38 slash commands into the SlashCommandRegistry.

- `wire_commands(registry, supervisor, plan_mode, skill_loader, plugin_loader, plugin_registry, demon_panel) -> void` — constructs and registers all slash command objects; sets cross-cutting dependencies (supervisor for /task, plan_mode for /plan, etc.)

### WireTools.hpp
Registers all 39 tool implementations into the ToolRegistry.

- `wire_tools(registry, cfg, cfg_mutex, sidecar, mcp_registry, supervisor, team_registry, skill_loader, plan_mode, confirm_fn, prompt_fn) -> void` — constructs every ITool subclass with its required dependencies and calls registry.register_tool() for each

### WireTui.hpp
Wires the FTXUI component tree: chat view, input bar, permission gate, plan card, question card, and agent panel.

- `wire_tui(screen_mgr, supervisor, queue, theme, history, keybindings, model_name, on_submit_override, slash_registry, permission_card, plan_approval_card, question_card, mcp_registry, permission_gate, on_interrupt_cb) -> void` — assembles the root FTXUI component, sets event callbacks, starts the 10Hz ticker, and installs the root into ScreenManager
