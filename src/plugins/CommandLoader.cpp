// src/plugins/CommandLoader.cpp
// =============================================================================
// Implementation of batbox::plugins::CommandLoader.
//
// Scans plugin/user command directories, parses each .md file as a
// user-defined slash command, and registers it in a SlashCommandRegistry.
// Built-in command names always win: any name that already exists in the
// registry at load time is silently skipped with a WARN log.
// =============================================================================

#include <batbox/plugins/CommandLoader.hpp>

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/plugins/FrontmatterParser.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::plugins {

namespace {

// ---------------------------------------------------------------------------
// Template substitution
// ---------------------------------------------------------------------------

/// Split `sv` on ASCII whitespace; return up to `max_tokens` tokens.
[[nodiscard]] std::vector<std::string> split_tokens(std::string_view sv,
                                                     std::size_t max_tokens = 16) {
    std::vector<std::string> tokens;
    tokens.reserve(std::min(max_tokens, static_cast<std::size_t>(4)));

    std::size_t i = 0;
    while (i < sv.size() && tokens.size() < max_tokens) {
        // Skip whitespace.
        while (i < sv.size() && std::isspace(static_cast<unsigned char>(sv[i])))
            ++i;
        if (i >= sv.size()) break;
        // Collect token.
        const std::size_t start = i;
        while (i < sv.size() && !std::isspace(static_cast<unsigned char>(sv[i])))
            ++i;
        tokens.emplace_back(sv.substr(start, i - start));
    }
    return tokens;
}

/// Perform an in-place string replacement of all occurrences of `from` with `to`.
void replace_all(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

/// Substitute template variables in `body` using `args`.
///
///   $ARGS → full args string
///   $1    → first whitespace-separated word (empty string when absent)
///   $2    → second whitespace-separated word (empty string when absent)
///
/// Variables are substituted in declaration order so that a literal "$ARGS"
/// in the user's args string can't accidentally expand into another variable.
[[nodiscard]] std::string substitute_template(std::string body,
                                              std::string_view args) {
    const std::vector<std::string> tokens = split_tokens(args);

    const std::string_view t1 = tokens.size() >= 1 ? std::string_view(tokens[0]) : std::string_view{};
    const std::string_view t2 = tokens.size() >= 2 ? std::string_view(tokens[1]) : std::string_view{};

    // Order matters: $ARGS first (so its replacement text never re-expands).
    replace_all(body, "$ARGS", args);
    replace_all(body, "$1",    t1);
    replace_all(body, "$2",    t2);

    return body;
}

// ---------------------------------------------------------------------------
// UserSlashCommand
// ---------------------------------------------------------------------------

/// A slash command loaded from a user/plugin .md file.
///
/// On execute():
///   1. Substitutes $ARGS/$1/$2 in the stored body template.
///   2. Calls ctx.conversation.inject_user_message(rendered_body).
///   3. Writes a "[user-command: /name]\n" banner to ctx.output.
class UserSlashCommand final : public batbox::commands::ISlashCommand {
public:
    UserSlashCommand(std::string name, std::string description, std::string body)
        : name_(std::move(name))
        , description_(std::move(description))
        , body_(std::move(body))
    {}

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return name_;
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return description_;
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return usage_cache_;
    }

    // ---- Metadata -----------------------------------------------------------

    /// True when the body template contains at least one substitution variable.
    /// The InputBar uses this to decide whether to open an argument-input prompt.
    [[nodiscard]] bool requires_args() const noexcept override {
        return body_.find("$ARGS") != std::string::npos ||
               body_.find("$1")    != std::string::npos ||
               body_.find("$2")    != std::string::npos;
    }

    // ---- Dispatch -----------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view              args,
        batbox::commands::CommandContext& ctx) override
    {
        const std::string rendered = substitute_template(body_, args);
        ctx.conversation.inject_user_message(rendered);
        ctx.output << "[user-command: /" << name_ << "]\n";
        return {};
    }

    // ---- Post-construction init ----------------------------------------------

    void build_usage_cache() {
        usage_cache_ = "/" + name_;
        if (requires_args()) usage_cache_ += " [args]";
    }

private:
    std::string name_;
    std::string description_;
    std::string body_;
    std::string usage_cache_;
};

// ---------------------------------------------------------------------------
// load_one_file — parse a single .md file and build a UserSlashCommand
// ---------------------------------------------------------------------------

/// Load and register one command file.
///
/// @param path      Path to the .md file.
/// @param registry  Registry to register into.
///
/// On any error (read failure, parse failure, name collision), logs a warning
/// and returns without modifying the registry.
void load_one_file(const fs::path&                          path,
                   batbox::commands::SlashCommandRegistry&  registry)
{
    // ---- Read file ----------------------------------------------------------
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        BATBOX_LOG_WARN("CommandLoader: cannot open '{}' — skipped", path.string());
        return;
    }
    std::ostringstream buf;
    buf << stream.rdbuf();
    const std::string content = buf.str();

    // ---- Parse frontmatter --------------------------------------------------
    auto parse_result = parse_frontmatter(content);
    if (!parse_result.has_value()) {
        BATBOX_LOG_WARN("CommandLoader: '{}' frontmatter parse error: {} — skipped",
                        path.string(), parse_result.error());
        return;
    }
    auto& [fm, body] = *parse_result;

    // ---- Resolve command name -----------------------------------------------
    // Prefer frontmatter "name" field; fall back to the file stem.
    std::string cmd_name;
    if (auto it = fm.find("name"); it != fm.end() && it->second.is_string()) {
        cmd_name = it->second.get<std::string>();
    } else {
        cmd_name = path.stem().string();
    }

    // Validate: must be non-empty, no whitespace, no leading slash.
    if (cmd_name.empty()) {
        BATBOX_LOG_WARN("CommandLoader: '{}' has empty name — skipped", path.string());
        return;
    }
    for (char c : cmd_name) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            BATBOX_LOG_WARN("CommandLoader: '{}' name '{}' contains whitespace — skipped",
                            path.string(), cmd_name);
            return;
        }
    }
    // Strip a leading slash if the author accidentally added one.
    if (cmd_name.front() == '/') cmd_name.erase(cmd_name.begin());

    // ---- Collision check — built-ins win ------------------------------------
    if (registry.lookup(cmd_name) != nullptr) {
        BATBOX_LOG_WARN("CommandLoader: '{}' name '{}' already registered — skipped (built-in wins)",
                        path.string(), cmd_name);
        return;
    }

    // ---- Resolve description ------------------------------------------------
    std::string description;
    if (auto it = fm.find("description"); it != fm.end() && it->second.is_string()) {
        description = it->second.get<std::string>();
    } else {
        description = "User-defined command loaded from " + path.filename().string();
    }

    // ---- Build and register -------------------------------------------------
    auto cmd = std::make_shared<UserSlashCommand>(cmd_name, std::move(description), body);
    cmd->build_usage_cache();

    auto reg_result = registry.register_command(std::move(cmd));
    if (!reg_result.has_value()) {
        // This should not happen after the lookup check above, but guard anyway.
        BATBOX_LOG_WARN("CommandLoader: failed to register '{}': {} — skipped",
                        cmd_name, reg_result.error());
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// CommandLoader::load_from_dir
// ---------------------------------------------------------------------------

void CommandLoader::load_from_dir(
    const fs::path&                          commands_dir,
    batbox::commands::SlashCommandRegistry&  registry) const
{
    std::error_code ec;
    if (!fs::is_directory(commands_dir, ec)) {
        // Directory absent or not a directory — not an error.
        return;
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(commands_dir, ec)) {
        if (ec) {
            BATBOX_LOG_WARN("CommandLoader: directory_iterator error in '{}': {}",
                            commands_dir.string(), ec.message());
            break;
        }
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".md") continue;

        load_one_file(entry.path(), registry);
    }
}

// ---------------------------------------------------------------------------
// CommandLoader::load_user_commands
// ---------------------------------------------------------------------------

void CommandLoader::load_user_commands(
    batbox::commands::SlashCommandRegistry& registry) const
{
    const fs::path home = batbox::paths::home_dir();

    // ~/.claude/commands/ — claude-code compatibility root
    load_from_dir(home / ".claude"  / "commands", registry);

    // ~/.batbox/commands/ — batbox-native root
    load_from_dir(home / ".batbox" / "commands", registry);
}

} // namespace batbox::plugins
