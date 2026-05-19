// src/commands/McpCmd.cpp
//
// batbox::commands::McpCmd — implements the /mcp slash command.
//
// Sub-commands:
//   /mcp list               — list all configured MCP servers with health status
//   /mcp restart <name>     — stop and restart the named server transport
//   /mcp resources [<name>] — list resources across all servers (or one server)
//   /mcp prompts [<name>]   — list prompts across all servers (or one server)
//
// CommandContext dependencies:
//   ctx.mcp_registry   — nullable McpServerRegistry*; degrades gracefully when null.
//
// McpClient is constructed on-demand from mcp_registry when needed for
// resources/prompts sub-commands (those require the MCP RPC layer).
//
// /mcp restart issues a McpServerRegistry::restart() call which stops then
// re-starts the named transport.  It does NOT re-run the initialize handshake;
// callers using McpClient should call initialize_one() separately if needed.
//
// Registration entry point:
//   void register_mcp_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/mcp/IMcpTransport.hpp>
#include <batbox/mcp/McpClient.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/commands/CommandHelpers.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Split `s` at the first whitespace boundary.
/// Returns {first_word, remainder_after_whitespace}.
/// When no whitespace is present, remainder is empty.
/// Unicode checkmark / cross mark for health indicators.
constexpr std::string_view kOk  = "\xe2\x9c\x93";  // ✓ U+2713
constexpr std::string_view kErr = "\xe2\x9c\x97";  // ✗ U+2717

// ---------------------------------------------------------------------------
// Sub-command handlers
// ---------------------------------------------------------------------------

/// /mcp list — enumerate all servers with health status.
batbox::Result<void> do_list(std::ostream& out,
                              batbox::mcp::McpServerRegistry& reg)
{
    const auto names = reg.server_names();

    out << "\n  MCP Servers";
    if (names.empty()) {
        out << " (none configured)\n\n";
        out << "  To add a server, edit ~/.batbox/mcp.json.\n\n";
        return {};
    }

    // Sort for deterministic output.
    auto sorted_names = names;
    std::sort(sorted_names.begin(), sorted_names.end());

    out << " (" << sorted_names.size() << ")\n";
    out << "  " << std::string(40, '\xe2') << "\n";  // decorative separator

    // Use simple dashes for the separator line since the UTF-8 char repetition
    // would not produce a valid sequence; use ASCII instead.
    out << "\n";

    for (const auto& srv_name : sorted_names) {
        const batbox::mcp::IMcpTransport* t = reg.get(srv_name);
        const bool healthy = (t != nullptr) && t->healthy();
        out << "  " << (healthy ? kOk : kErr) << "  " << srv_name;
        out << "   [" << (healthy ? "healthy" : "unhealthy") << "]\n";
    }

    out << "\n";
    out << "  Use \"/mcp restart <name>\" to restart a server.\n";
    out << "  Use \"/mcp resources\" or \"/mcp prompts\" to inspect server capabilities.\n";
    out << "\n";
    return {};
}

/// /mcp list — enumerate all servers with health status (no registry).
batbox::Result<void> do_list_unavailable(std::ostream& out)
{
    out << "\n  MCP Servers\n\n";
    out << "  MCP registry is not available in this context.\n\n";
    return {};
}

/// /mcp restart <name> — stop and start the named server transport.
batbox::Result<void> do_restart(std::string_view server_name,
                                 std::ostream& out,
                                 batbox::mcp::McpServerRegistry& reg)
{
    if (server_name.empty()) {
        return batbox::Err(std::string(
            "/mcp restart: server name required.\n"
            "Usage: /mcp restart <server-name>\n"
            "Use \"/mcp list\" to see available servers."));
    }

    out << "\n  Restarting MCP server \"" << server_name << "\"...\n";

    batbox::CancelToken ct;  // non-cancellable for interactive restart
    auto result = reg.restart(server_name, std::move(ct));

    if (!result) {
        return batbox::Err(
            std::string("/mcp restart: failed to restart \"") +
            std::string(server_name) + "\": " + result.error() + "\n");
    }

    const batbox::mcp::IMcpTransport* t = reg.get(server_name);
    const bool now_healthy = (t != nullptr) && t->healthy();
    out << "  " << (now_healthy ? kOk : kErr)
        << "  \"" << server_name << "\" restarted successfully.\n\n";
    return {};
}

/// /mcp resources [<server>] — list resources via McpClient::resources_list.
batbox::Result<void> do_resources(std::string_view filter_server,
                                   std::ostream& out,
                                   batbox::mcp::McpServerRegistry& reg)
{
    batbox::mcp::McpClient client(reg);
    const auto names = reg.server_names();

    if (names.empty()) {
        out << "\n  No MCP servers configured.\n\n";
        return {};
    }

    // Sort for deterministic output.
    auto sorted_names = names;
    std::sort(sorted_names.begin(), sorted_names.end());

    out << "\n  MCP Resources\n\n";

    bool any_found = false;
    for (const auto& srv_name : sorted_names) {
        if (!filter_server.empty() && srv_name != filter_server) {
            continue;
        }

        batbox::CancelToken ct;
        auto result = client.resources_list(srv_name, std::move(ct));
        if (!result) {
            out << "  " << kErr << "  " << srv_name << ": " << result.error() << "\n";
            continue;
        }

        const auto& json_result = result.value();
        // MCP resources/list response: { "resources": [ { "uri", "name", "description" }, ... ] }
        if (json_result.is_object() && json_result.contains("resources") &&
            json_result["resources"].is_array())
        {
            const auto& resources = json_result["resources"];
            if (resources.empty()) {
                out << "  " << srv_name << ": (no resources)\n";
            } else {
                out << "  " << srv_name << " (" << resources.size() << " resource"
                    << (resources.size() == 1 ? "" : "s") << "):\n";
                for (const auto& r : resources) {
                    const std::string uri  = r.value("uri",  "(no uri)");
                    const std::string name = r.value("name", "");
                    out << "    \xe2\x80\xa2  " << uri;  // U+2022 BULLET
                    if (!name.empty() && name != uri) {
                        out << "  (" << name << ")";
                    }
                    out << "\n";
                }
                any_found = true;
            }
        } else {
            out << "  " << srv_name << ": (unexpected response format)\n";
        }
    }

    if (!any_found && !filter_server.empty()) {
        out << "  Server \"" << filter_server << "\" has no resources or was not found.\n";
    }

    out << "\n";
    return {};
}

/// /mcp prompts [<server>] — list prompts via McpClient::prompts_list.
batbox::Result<void> do_prompts(std::string_view filter_server,
                                 std::ostream& out,
                                 batbox::mcp::McpServerRegistry& reg)
{
    batbox::mcp::McpClient client(reg);
    const auto names = reg.server_names();

    if (names.empty()) {
        out << "\n  No MCP servers configured.\n\n";
        return {};
    }

    // Sort for deterministic output.
    auto sorted_names = names;
    std::sort(sorted_names.begin(), sorted_names.end());

    out << "\n  MCP Prompts\n\n";

    bool any_found = false;
    for (const auto& srv_name : sorted_names) {
        if (!filter_server.empty() && srv_name != filter_server) {
            continue;
        }

        batbox::CancelToken ct;
        auto result = client.prompts_list(srv_name, std::move(ct));
        if (!result) {
            out << "  " << kErr << "  " << srv_name << ": " << result.error() << "\n";
            continue;
        }

        const auto& json_result = result.value();
        // MCP prompts/list response: { "prompts": [ { "name", "description" }, ... ] }
        if (json_result.is_object() && json_result.contains("prompts") &&
            json_result["prompts"].is_array())
        {
            const auto& prompts = json_result["prompts"];
            if (prompts.empty()) {
                out << "  " << srv_name << ": (no prompts)\n";
            } else {
                out << "  " << srv_name << " (" << prompts.size() << " prompt"
                    << (prompts.size() == 1 ? "" : "s") << "):\n";
                for (const auto& p : prompts) {
                    const std::string name = p.value("name", "(unnamed)");
                    const std::string desc = p.value("description", "");
                    out << "    \xe2\x80\xa2  " << name;  // U+2022 BULLET
                    if (!desc.empty()) {
                        out << "  \xe2\x80\x94 " << desc;  // U+2014 EM DASH
                    }
                    out << "\n";
                }
                any_found = true;
            }
        } else {
            out << "  " << srv_name << ": (unexpected response format)\n";
        }
    }

    if (!any_found && !filter_server.empty()) {
        out << "  Server \"" << filter_server << "\" has no prompts or was not found.\n";
    }

    out << "\n";
    return {};
}

/// Print usage help for /mcp.
void print_mcp_usage(std::ostream& out) {
    out << "\n  /mcp — Manage MCP (Model Context Protocol) servers\n\n";
    out << "  Sub-commands:\n";
    out << "    /mcp list               List all configured servers and health status\n";
    out << "    /mcp restart <name>     Stop and restart the named server\n";
    out << "    /mcp resources [name]   List resources from all servers (or one)\n";
    out << "    /mcp prompts [name]     List prompts from all servers (or one)\n";
    out << "\n";
    out << "  When called with no arguments, /mcp defaults to \"/mcp list\".\n";
    out << "\n";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// McpCmd
// ---------------------------------------------------------------------------

class McpCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "mcp";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Manage MCP servers: list health, restart, inspect resources/prompts.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/mcp [list|restart <name>|resources [name]|prompts [name]]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> McpCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view trimmed = trim(args);
    const auto [sub, rest] = split_first(trimmed);

    // Default to "list" when called with no arguments.
    const std::string_view subcmd = sub.empty() ? std::string_view{"list"} : sub;

    // Graceful degradation when no MCP registry is available.
    if (ctx.mcp_registry == nullptr) {
        if (subcmd == "list" || sub.empty()) {
            return do_list_unavailable(ctx.output);
        }
        ctx.output << "\n  MCP registry is not available in this context.\n\n";
        return {};
    }

    batbox::mcp::McpServerRegistry& reg = *ctx.mcp_registry;

    if (subcmd == "list") {
        return do_list(ctx.output, reg);
    }

    if (subcmd == "restart") {
        const std::string_view server_name = trim(rest);
        return do_restart(server_name, ctx.output, reg);
    }

    if (subcmd == "resources") {
        const std::string_view filter = trim(rest);
        return do_resources(filter, ctx.output, reg);
    }

    if (subcmd == "prompts") {
        const std::string_view filter = trim(rest);
        return do_prompts(filter, ctx.output, reg);
    }

    if (subcmd == "help" || subcmd == "--help" || subcmd == "-h") {
        print_mcp_usage(ctx.output);
        return {};
    }

    // Unknown sub-command.
    return batbox::Err(
        std::string("/mcp: unknown subcommand '") + std::string(subcmd) +
        "'.\nUsage: " + std::string(usage()) +
        "\nRun \"/mcp help\" for details.");
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_mcp_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<McpCmd>());
    (void)res;
}

} // namespace batbox::commands
