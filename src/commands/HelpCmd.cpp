// src/commands/HelpCmd.cpp
//
// batbox::commands::HelpCmd — implements the /help slash command.
//
// Walks the SlashCommandRegistry, groups commands by the categories defined
// in curated-surface.md, and writes a formatted listing to ctx.output.
//
// Aliases: /? → /help
//
// Registration entry point:
//   void register_help_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Category table
// ---------------------------------------------------------------------------
// Mirrors the groupings from curated-surface.md exactly.
// Commands not found in any group land in "Other".

struct CategoryEntry {
    std::string_view category;
    std::vector<std::string_view> names;  // primary names, lowercase
};

static const CategoryEntry kCategories[] = {
    { "Core UX",                { "help", "exit", "clear", "init" } },
    { "Model & Config",         { "model", "config", "effort" } },
    { "Display & Theme",        { "theme", "output-style" } },
    { "Status & Stats",         { "status", "stats", "usage", "cost" } },
    { "Session Lifecycle",      { "resume", "session", "compact" } },
    { "Memory & Context",       { "memory", "context" } },
    { "Project Filesystem",     { "add-dir", "files", "diff" } },
    { "Code Review",            { "review", "security-review" } },
    { "Agents / Planning",      { "agents", "plan", "tasks", "skills" } },
    { "Permissions & Hooks",    { "permissions", "hooks", "advisor" } },
    { "Plugins & MCP",          { "mcp", "plugin" } },
    { "IDE & Editor",           { "ide", "vim", "keybindings", "terminal-setup" } },
    { "Misc",                   { "copy" } },
    { "Easter Egg",             { "demon" } },
};

// ---------------------------------------------------------------------------
// HelpCmd
// ---------------------------------------------------------------------------

class HelpCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "help";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "List all available slash commands grouped by category.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/help";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return { "?" };
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   /*args*/,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> HelpCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    std::ostream& out = ctx.output;
    const SlashCommandRegistry& registry = ctx.registry;

    // Collect all registered commands sorted by primary name.
    const std::vector<ISlashCommand*> all_cmds = registry.all();

    // Track which commands have been printed (to catch "Other" category).
    std::vector<bool> printed(all_cmds.size(), false);

    // Map name → index for O(1) membership test.
    auto find_by_name = [&](std::string_view n) -> std::size_t {
        for (std::size_t i = 0; i < all_cmds.size(); ++i) {
            if (all_cmds[i]->name() == n) return i;
        }
        return static_cast<std::size_t>(-1);
    };

    out << "\n  Slash commands\n";
    out << "  ──────────────\n\n";

    // Emit each category in definition order.
    for (const auto& cat : kCategories) {
        bool header_written = false;

        for (std::string_view cmd_name : cat.names) {
            const std::size_t idx = find_by_name(cmd_name);
            if (idx == static_cast<std::size_t>(-1)) continue;

            ISlashCommand* cmd = all_cmds[idx];
            printed[idx] = true;

            if (!header_written) {
                out << "  " << cat.category << "\n";
                header_written = true;
            }

            const bool is_phase2 = (cmd->phase() == CommandPhase::Phase2);

            // "    /name" padded to column 24.
            out << "    /" << cmd->name();
            const std::size_t name_col = 5 + cmd->name().size();  // "    /" + name
            if (name_col < 24) {
                out << std::string(24 - name_col, ' ');
            } else {
                out << ' ';
            }

            out << cmd->description();
            if (is_phase2) out << "  (coming soon)";
            out << '\n';

            // Print aliases on the next line indented to the description column.
            const auto als = cmd->aliases();
            if (!als.empty()) {
                out << "                        aliases: ";
                for (std::size_t ai = 0; ai < als.size(); ++ai) {
                    if (ai > 0) out << ", ";
                    out << '/' << als[ai];
                }
                out << '\n';
            }
        }

        if (header_written) out << '\n';
    }

    // Catch-all: commands not present in any category group.
    bool other_header = false;
    for (std::size_t i = 0; i < all_cmds.size(); ++i) {
        if (printed[i]) continue;
        if (!other_header) {
            out << "  Other\n";
            other_header = true;
        }
        out << "    /" << all_cmds[i]->name()
            << "  " << all_cmds[i]->description() << '\n';
    }
    if (other_header) out << '\n';

    out << "  " << all_cmds.size() << " command"
        << (all_cmds.size() == 1 ? "" : "s") << " registered.\n\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_help_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<HelpCmd>());
    (void)res;
}

} // namespace batbox::commands
