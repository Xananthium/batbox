// src/commands/PluginCmd.cpp
//
// batbox::commands::PluginCmd — implements the /plugin slash command.
//
// Sub-commands:
//   /plugin list            — list all plugins with enabled/disabled state
//   /plugin enable <name>   — enable the named plugin (updates settings.json)
//   /plugin disable <name>  — disable the named plugin (updates settings.json)
//   /plugin reload          — rescan all plugin roots and refresh the registry
//   /plugin add <path>      — copy local plugin directory tree into ~/.batbox/plugins/
//   /plugin remove <name>   — remove plugin from ~/.batbox/plugins/ (prompts confirmation)
//
// CommandContext dependencies:
//   ctx.mcp_registry — currently not used by /plugin; this command operates
//                      through a PluginLoader/PluginRegistry owned by the App.
//
// Design:
//   PluginCmd holds raw pointers to a PluginLoader and PluginRegistry.  These
//   are injected at registration time via register_plugin_cmd().  When pointers
//   are null (headless/test mode) the command degrades gracefully by printing
//   "(plugin subsystem not available)".
//
//   enable/disable call PluginRegistry::enable()/disable() in-memory then write
//   the new disabled-names set to settings.json via persist_disabled_names().
//   The helper writes {"plugins":{"disabled":[...]}} into ~/.batbox/settings.json,
//   merging with any existing content.
//
//   add and remove delegate directly to PluginLoader::add_local() and
//   PluginLoader::remove(); these methods handle the filesystem copy/delete and
//   trigger an atomic reload of the registry automatically.
//
//   remove requires explicit user confirmation ("yes") read from ctx.input.
//
//   Remote install is explicitly NOT supported.  If the path argument to add
//   starts with "http://" or "https://" an error is returned immediately.
//
// Registration entry points:
//   void register_plugin_cmd(SlashCommandRegistry&,
//                            batbox::plugins::PluginLoader*,
//                            batbox::plugins::PluginRegistry*);
//   void register_plugin_cmd(SlashCommandRegistry&);  // headless overload

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/plugins/Plugin.hpp>
#include <batbox/plugins/PluginLoader.hpp>
#include <batbox/plugins/PluginRegistry.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Strip leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Split `s` at the first whitespace boundary.
/// Returns {first_word, remainder_after_whitespace}.
[[nodiscard]] std::pair<std::string_view, std::string_view>
split_first(std::string_view s) noexcept {
    const auto space = s.find_first_of(" \t");
    if (space == std::string_view::npos) {
        return {s, {}};
    }
    const auto rest_start = s.find_first_not_of(" \t", space);
    const std::string_view rest =
        (rest_start == std::string_view::npos) ? std::string_view{} : s.substr(rest_start);
    return {s.substr(0, space), rest};
}

/// Unicode indicators.
constexpr std::string_view kEnabled  = "\xe2\x9c\x93";  // ✓ U+2713
constexpr std::string_view kDisabled = "\xe2\x9c\x97";  // ✗ U+2717

/// Return the path to ~/.batbox/settings.json.
[[nodiscard]] fs::path default_settings_path() {
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') {
        return ".batbox/settings.json";
    }
    return fs::path(home) / ".batbox" / "settings.json";
}

/// Read the current disabled plugin names from settings.json.
/// Returns an empty vector if the file is missing or the key is absent.
[[nodiscard]] std::vector<std::string> read_disabled_names() {
    const fs::path settings = default_settings_path();
    std::vector<std::string> disabled;

    std::ifstream f(settings);
    if (!f.is_open()) return disabled;

    // Minimal hand-rolled JSON parse for the plugins.disabled array.
    // Reads the entire file and locates "disabled":[...].
    // This avoids pulling nlohmann::json into PluginCmd.cpp directly;
    // settings.json is tiny so a simple scan is safe.
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    const auto key_pos = content.find("\"disabled\"");
    if (key_pos == std::string::npos) return disabled;

    const auto arr_start = content.find('[', key_pos);
    if (arr_start == std::string::npos) return disabled;

    const auto arr_end = content.find(']', arr_start);
    if (arr_end == std::string::npos) return disabled;

    // Extract string tokens between arr_start and arr_end.
    std::size_t pos = arr_start + 1;
    while (pos < arr_end) {
        const auto q1 = content.find('"', pos);
        if (q1 == std::string::npos || q1 >= arr_end) break;
        const auto q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= arr_end) break;
        disabled.push_back(content.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }

    return disabled;
}

/// Write the disabled names list back to settings.json.
/// Performs a read-modify-write preserving other existing keys as a JSON blob.
/// If settings.json does not exist, creates it with a minimal structure.
[[nodiscard]] batbox::Result<void> persist_disabled_names(
    const std::vector<std::string>& names)
{
    const fs::path settings = default_settings_path();
    std::error_code ec;
    fs::create_directories(settings.parent_path(), ec);
    if (ec) {
        return batbox::Err(
            std::string("settings.json: cannot create directory: ") + ec.message());
    }

    // Read current content (if any) so we can preserve other keys.
    std::string content;
    {
        std::ifstream f(settings);
        if (f.is_open()) {
            content = std::string((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        }
    }

    // Build the new disabled array JSON fragment.
    std::string disabled_json = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) disabled_json += ',';
        disabled_json += '"';
        for (char c : names[i]) {
            if (c == '"' || c == '\\') disabled_json += '\\';
            disabled_json += c;
        }
        disabled_json += '"';
    }
    disabled_json += ']';

    // Strategy: if content already has a "plugins" key, splice in the new
    // "disabled" value.  Otherwise write a fresh minimal JSON.
    const auto plugins_pos = content.find("\"plugins\"");
    if (plugins_pos == std::string::npos) {
        // Write fresh.
        content = "{\n  \"plugins\": {\n    \"disabled\": " +
                  disabled_json + "\n  }\n}\n";
    } else {
        const auto disabled_pos = content.find("\"disabled\"", plugins_pos);
        if (disabled_pos == std::string::npos) {
            // "plugins" object exists but has no "disabled" key.
            // Insert before the closing brace of the plugins object.
            const auto brace = content.find('}', plugins_pos);
            if (brace == std::string::npos) {
                content = "{\n  \"plugins\": {\n    \"disabled\": " +
                          disabled_json + "\n  }\n}\n";
            } else {
                content.insert(brace, ",\n    \"disabled\": " + disabled_json);
            }
        } else {
            // Replace the existing disabled value.
            const auto colon = content.find(':', disabled_pos);
            if (colon == std::string::npos) {
                return batbox::Err(std::string("settings.json: malformed (no colon after \"disabled\")"));
            }
            // Find the extent of the old array value.
            const auto old_start = content.find('[', colon);
            if (old_start == std::string::npos) {
                return batbox::Err(std::string("settings.json: malformed (no '[' after disabled)"));
            }
            const auto old_end = content.find(']', old_start);
            if (old_end == std::string::npos) {
                return batbox::Err(std::string("settings.json: malformed (no ']' in disabled array)"));
            }
            content.replace(old_start, old_end - old_start + 1, disabled_json);
        }
    }

    std::ofstream out(settings, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        return batbox::Err(
            std::string("settings.json: cannot write to ") + settings.string());
    }
    out << content;
    if (!out) {
        return batbox::Err(
            std::string("settings.json: write error for ") + settings.string());
    }
    return {};
}

// ---------------------------------------------------------------------------
// Sub-command handlers (all require non-null loader and registry)
// ---------------------------------------------------------------------------

/// /plugin list — show all plugins with enabled/disabled state.
batbox::Result<void> do_list(std::ostream& out,
                              batbox::plugins::PluginRegistry& registry)
{
    const auto& plugins = registry.all_plugins();

    out << "\n  Plugins";
    if (plugins.empty()) {
        out << " (none installed)\n\n";
        out << "  Plugin roots:\n";
        out << "    ~/.claude/plugins/        (claude-code compat)\n";
        out << "    ./.claude/plugins/        (project compat)\n";
        out << "    ~/.batbox/plugins/        (user global)\n";
        out << "    ./.batbox/plugins/        (project local)\n";
        out << "\n";
        out << "  Add a plugin: /plugin add <path-to-plugin-dir>\n\n";
        return {};
    }

    auto sorted = plugins;
    std::sort(sorted.begin(), sorted.end(),
              [](const batbox::plugins::Plugin& a,
                 const batbox::plugins::Plugin& b) {
                  return a.name < b.name;
              });

    out << " (" << sorted.size() << ")\n\n";

    for (const auto& p : sorted) {
        const bool enabled = !p.disabled;
        out << "  " << (enabled ? kEnabled : kDisabled) << "  "
            << p.name;
        if (!p.version.empty()) {
            out << "  v" << p.version;
        }
        if (!p.description.empty()) {
            out << "\n       " << p.description;
        }
        // Asset summary.
        const std::size_t n_skills   = p.skills.size();
        const std::size_t n_agents   = p.agents.size();
        const std::size_t n_commands = p.commands.size();
        const std::size_t n_mcp      = p.mcp_servers.size();
        if (n_skills + n_agents + n_commands + n_mcp > 0) {
            out << "\n       [";
            bool first = true;
            auto emit = [&](std::size_t n, const char* label) {
                if (n == 0) return;
                if (!first) out << ", ";
                out << n << " " << label;
                if (n != 1) out << "s";
                first = false;
            };
            emit(n_skills,   "skill");
            emit(n_agents,   "agent");
            emit(n_commands, "command");
            emit(n_mcp,      "MCP server");
            out << "]";
        }
        if (p.disabled) {
            out << "  (disabled)";
        }
        out << "\n\n";
    }

    out << "  /plugin enable <name>    Enable a disabled plugin\n";
    out << "  /plugin disable <name>   Disable an enabled plugin\n";
    out << "  /plugin reload           Rescan all plugin roots\n";
    out << "\n";
    return {};
}

/// /plugin enable <name>
batbox::Result<void> do_enable(std::string_view name,
                                std::ostream& out,
                                batbox::plugins::PluginRegistry& registry)
{
    if (name.empty()) {
        return batbox::Err(std::string(
            "/plugin enable: plugin name required.\n"
            "Usage: /plugin enable <name>"));
    }

    const bool changed = registry.enable(name);
    if (!changed) {
        const batbox::plugins::Plugin* p = registry.get(name);
        if (p == nullptr) {
            return batbox::Err(
                std::string("/plugin enable: plugin not found: \"") +
                std::string(name) + "\".\nRun \"/plugin list\" to see installed plugins.");
        }
        out << "\n  Plugin \"" << name << "\" is already enabled.\n\n";
        return {};
    }

    // Persist the updated disabled list.
    auto disabled = read_disabled_names();
    disabled.erase(std::remove(disabled.begin(), disabled.end(), std::string(name)),
                   disabled.end());
    if (auto r = persist_disabled_names(disabled); !r) {
        return batbox::Err(
            std::string("/plugin enable: enabled in memory but failed to write settings: ") +
            r.error());
    }

    out << "\n  " << kEnabled << "  Plugin \"" << name << "\" enabled.\n\n";
    return {};
}

/// /plugin disable <name>
batbox::Result<void> do_disable(std::string_view name,
                                 std::ostream& out,
                                 batbox::plugins::PluginRegistry& registry)
{
    if (name.empty()) {
        return batbox::Err(std::string(
            "/plugin disable: plugin name required.\n"
            "Usage: /plugin disable <name>"));
    }

    const bool changed = registry.disable(name);
    if (!changed) {
        const batbox::plugins::Plugin* p = registry.get(name);
        if (p == nullptr) {
            return batbox::Err(
                std::string("/plugin disable: plugin not found: \"") +
                std::string(name) + "\".\nRun \"/plugin list\" to see installed plugins.");
        }
        out << "\n  Plugin \"" << name << "\" is already disabled.\n\n";
        return {};
    }

    // Persist the updated disabled list.
    auto disabled = read_disabled_names();
    if (std::find(disabled.begin(), disabled.end(), std::string(name)) == disabled.end()) {
        disabled.push_back(std::string(name));
    }
    if (auto r = persist_disabled_names(disabled); !r) {
        return batbox::Err(
            std::string("/plugin disable: disabled in memory but failed to write settings: ") +
            r.error());
    }

    out << "\n  " << kDisabled << "  Plugin \"" << name << "\" disabled.\n\n";
    return {};
}

/// /plugin reload
batbox::Result<void> do_reload(std::ostream& out,
                                batbox::plugins::PluginLoader& loader,
                                batbox::plugins::PluginRegistry& registry)
{
    out << "\n  Rescanning plugin roots...\n";
    if (auto r = loader.reload(registry); !r) {
        return batbox::Err(
            std::string("/plugin reload: failed: ") + r.error());
    }
    const std::size_t count = registry.size();
    out << "  " << kEnabled << "  Reload complete. " << count << " plugin"
        << (count == 1 ? "" : "s") << " found.\n\n";
    return {};
}

/// /plugin add <path>
batbox::Result<void> do_add(std::string_view path_arg,
                             std::ostream& out,
                             batbox::plugins::PluginLoader& loader)
{
    if (path_arg.empty()) {
        return batbox::Err(std::string(
            "/plugin add: path required.\n"
            "Usage: /plugin add <local-path-to-plugin-dir>\n"
            "Note: Remote install is not supported."));
    }

    // Reject remote URLs explicitly.
    if (path_arg.substr(0, 7) == "http://" ||
        path_arg.substr(0, 8) == "https://")
    {
        return batbox::Err(std::string(
            "/plugin add: remote install is not supported.\n"
            "Only local directory paths are accepted.\n"
            "Usage: /plugin add <local-path-to-plugin-dir>"));
    }

    const fs::path source(path_arg);
    out << "\n  Adding plugin from: " << source.string() << "\n";

    if (auto r = loader.add_local(source); !r) {
        return batbox::Err(
            std::string("/plugin add: ") + r.error());
    }

    out << "  " << kEnabled << "  Plugin installed and loaded.\n\n";
    return {};
}

/// /plugin remove <name>
batbox::Result<void> do_remove(std::string_view name,
                                std::ostream& out,
                                std::istream& in,
                                batbox::plugins::PluginLoader& loader,
                                batbox::plugins::PluginRegistry& registry)
{
    if (name.empty()) {
        return batbox::Err(std::string(
            "/plugin remove: plugin name required.\n"
            "Usage: /plugin remove <name>"));
    }

    // Verify the plugin exists.
    if (registry.get(name) == nullptr) {
        return batbox::Err(
            std::string("/plugin remove: plugin not found: \"") +
            std::string(name) + "\".\nRun \"/plugin list\" to see installed plugins.");
    }

    // Prompt for confirmation.
    out << "\n  WARNING: This will permanently delete plugin \"" << name
        << "\" from ~/.batbox/plugins/.\n";
    out << "  Type \"yes\" to confirm, or anything else to cancel: ";
    out.flush();

    std::string answer;
    if (!std::getline(in, answer)) {
        out << "\n  Removal cancelled (no input).\n\n";
        return {};
    }
    // Trim the answer.
    const std::string_view ans_view = trim(answer);
    if (ans_view != "yes") {
        out << "\n  Removal cancelled.\n\n";
        return {};
    }

    out << "\n  Removing plugin \"" << name << "\"...\n";
    if (auto r = loader.remove(name); !r) {
        return batbox::Err(
            std::string("/plugin remove: ") + r.error());
    }

    out << "  " << kEnabled << "  Plugin \"" << name << "\" removed.\n\n";
    return {};
}

/// Print usage help for /plugin.
void print_plugin_usage(std::ostream& out) {
    out << "\n  /plugin — Manage batbox plugins\n\n";
    out << "  Sub-commands:\n";
    out << "    /plugin list              List installed plugins with state\n";
    out << "    /plugin enable <name>     Enable a disabled plugin\n";
    out << "    /plugin disable <name>    Disable an enabled plugin\n";
    out << "    /plugin reload            Rescan all plugin roots\n";
    out << "    /plugin add <path>        Install from local directory (no remote)\n";
    out << "    /plugin remove <name>     Permanently delete from ~/.batbox/plugins/\n";
    out << "\n";
    out << "  Plugin roots (ascending priority):\n";
    out << "    ~/.claude/plugins/        (claude-code compat, read-only)\n";
    out << "    ./.claude/plugins/        (project compat, read-only)\n";
    out << "    ~/.batbox/plugins/        (user global, writable)\n";
    out << "    ./.batbox/plugins/        (project local, writable)\n";
    out << "\n";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PluginCmd
// ---------------------------------------------------------------------------

class PluginCmd final : public ISlashCommand {
public:
    /// Construct with optional live subsystem pointers.
    /// When both are null the command degrades gracefully.
    explicit PluginCmd(batbox::plugins::PluginLoader*   loader   = nullptr,
                       batbox::plugins::PluginRegistry* registry = nullptr) noexcept
        : loader_(loader), registry_(registry) {}

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "plugin";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Manage plugins: list, enable, disable, reload, add, remove.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/plugin [list|enable <name>|disable <name>|reload|add <path>|remove <name>]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;

private:
    batbox::plugins::PluginLoader*   loader_;    ///< may be null in headless mode
    batbox::plugins::PluginRegistry* registry_;  ///< may be null in headless mode
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> PluginCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view trimmed = trim(args);
    const auto [sub, rest] = split_first(trimmed);

    // Default to "list" when called with no arguments.
    const std::string_view subcmd = sub.empty() ? std::string_view{"list"} : sub;

    if (subcmd == "help" || subcmd == "--help" || subcmd == "-h") {
        print_plugin_usage(ctx.output);
        return {};
    }

    // Graceful degradation when subsystems are unavailable.
    if (loader_ == nullptr || registry_ == nullptr) {
        ctx.output << "\n  Plugin subsystem is not available in this context.\n\n";
        return {};
    }

    if (subcmd == "list") {
        return do_list(ctx.output, *registry_);
    }

    if (subcmd == "enable") {
        return do_enable(trim(rest), ctx.output, *registry_);
    }

    if (subcmd == "disable") {
        return do_disable(trim(rest), ctx.output, *registry_);
    }

    if (subcmd == "reload") {
        return do_reload(ctx.output, *loader_, *registry_);
    }

    if (subcmd == "add") {
        return do_add(trim(rest), ctx.output, *loader_);
    }

    if (subcmd == "remove") {
        return do_remove(trim(rest), ctx.output, ctx.input, *loader_, *registry_);
    }

    return batbox::Err(
        std::string("/plugin: unknown subcommand '") + std::string(subcmd) +
        "'.\nUsage: " + std::string(usage()) +
        "\nRun \"/plugin help\" for details.");
}

// ---------------------------------------------------------------------------
// Registration functions
// ---------------------------------------------------------------------------

void register_plugin_cmd(SlashCommandRegistry&            registry,
                          batbox::plugins::PluginLoader*   loader,
                          batbox::plugins::PluginRegistry* plugin_registry)
{
    auto res = registry.register_command(
        std::make_shared<PluginCmd>(loader, plugin_registry));
    (void)res;
}

void register_plugin_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<PluginCmd>(nullptr, nullptr));
    (void)res;
}

} // namespace batbox::commands
