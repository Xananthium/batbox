// src/commands/StatusCmd.cpp
//
// batbox::commands::StatusCmd — implements the /status slash command.
//
// /status aggregates health information from four subsystems and prints a
// formatted dashboard to ctx.output:
//
//   Model           — model name from ctx.conversation.get_model_name()
//   Session         — active session UUID or "(none)"
//   Permission mode — from nullable ctx.permission_mode_str
//   Sidecar         — state from nullable ctx.sidecar_manager (cold/running/…)
//   MCP Servers     — health table from nullable ctx.mcp_registry
//   Tool deps       — rg (ripgrep) and python3 availability via PATH scan
//
// All subsystem pointers are nullable — the command degrades gracefully with
// "(n/a)" when any is absent.
//
// Registration entry point:
//   void register_status_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/mcp/IMcpTransport.hpp>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>   // access(2)

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Return true if `program` is found as an executable on the PATH.
[[nodiscard]] bool program_on_path(std::string_view program) noexcept {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;

    std::string_view path_view(path_env);
    while (!path_view.empty()) {
        const auto colon = path_view.find(':');
        const std::string_view dir = (colon == std::string_view::npos)
                                     ? path_view
                                     : path_view.substr(0, colon);
        path_view = (colon == std::string_view::npos)
                    ? std::string_view{}
                    : path_view.substr(colon + 1);

        if (dir.empty()) continue;

        std::string full;
        full.reserve(dir.size() + 1 + program.size());
        full += dir;
        full += '/';
        full += program;

        if (::access(full.c_str(), X_OK) == 0) {
            return true;
        }
    }
    return false;
}

/// Unicode checkmark / cross mark for status indicators.
constexpr std::string_view kOk  = "✓";   // U+2713
constexpr std::string_view kErr = "✗";   // U+2717

/// Format a single status row: "  <mark>  <label padded to 20>  <detail>"
[[nodiscard]] std::string status_row(bool              ok,
                                     std::string_view  label,
                                     std::string_view  detail = {})
{
    constexpr std::size_t kLabelWidth = 20;
    std::string row;
    row += "  ";
    row += (ok ? kOk : kErr);
    row += "  ";
    row += label;
    if (!detail.empty()) {
        if (label.size() < kLabelWidth) {
            row += std::string(kLabelWidth - label.size(), ' ');
        } else {
            row += ' ';
        }
        row += detail;
    }
    row += '\n';
    return row;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// StatusCmd
// ---------------------------------------------------------------------------

class StatusCmd final : public ISlashCommand {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "status";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show model, sidecar health, MCP servers, permission mode, session info.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/status";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> StatusCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    std::ostream& out = ctx.output;

    out << "\n  Status\n";
    out << "  ──────\n\n";

    // ---- Model --------------------------------------------------------------
    {
        const std::string model = ctx.conversation.get_model_name();
        const bool ok = !model.empty();
        out << status_row(ok, "Model", ok ? model : "(none)");
    }

    // ---- Session ------------------------------------------------------------
    {
        const std::string sid = ctx.conversation.get_session_id();
        const bool ok = !sid.empty();
        const std::string detail = ok
            ? (sid.size() > 8 ? sid.substr(0, 8) + "\xe2\x80\xa6" : sid)  // U+2026 ELLIPSIS
            : "(none)";
        out << status_row(ok, "Session", detail);
    }

    // ---- Permission mode ----------------------------------------------------
    {
        const bool ok = (ctx.permission_mode_str != nullptr);
        out << status_row(ok, "Permission mode",
                          ok ? ctx.permission_mode_str : "(n/a)");
    }

    // ---- Sidecar ------------------------------------------------------------
    {
        if (ctx.sidecar_manager != nullptr) {
            const batbox::sidecar::SidecarState st =
                ctx.sidecar_manager->current_state();
            const bool ok = (st == batbox::sidecar::SidecarState::Running);
            out << status_row(ok, "Sidecar",
                              std::string(batbox::sidecar::to_string(st)));
        } else {
            out << status_row(false, "Sidecar", "(n/a)");
        }
    }

    // ---- MCP Servers --------------------------------------------------------
    {
        if (ctx.mcp_registry != nullptr) {
            const auto names = ctx.mcp_registry->server_names();
            if (names.empty()) {
                out << status_row(true, "MCP servers", "(none configured)");
            } else {
                out << "  \xe2\x84\xb9  MCP servers (" << names.size() << ")\n";  // ℹ
                for (const auto& srv_name : names) {
                    const batbox::mcp::IMcpTransport* t =
                        ctx.mcp_registry->get(srv_name);
                    const bool healthy = (t != nullptr) && t->healthy();
                    out << "       ";
                    out << (healthy ? kOk : kErr);
                    out << "  " << srv_name << '\n';
                }
            }
        } else {
            out << status_row(false, "MCP servers", "(n/a)");
        }
    }

    // ---- Tool dependencies --------------------------------------------------
    out << "\n  Tool dependencies\n";
    out << status_row(program_on_path("rg"),      "rg (ripgrep)");
    out << status_row(program_on_path("python3"), "python3");

    out << '\n';
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_status_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<StatusCmd>());
    (void)res;
}

} // namespace batbox::commands
