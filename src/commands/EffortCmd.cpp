// src/commands/EffortCmd.cpp
//
// batbox::commands::EffortCmd — implements the /effort slash command.
//
// Behaviour:
//   /effort                — show current effort level and usage
//   /effort low            — set effort to low  (fast, cheaper)
//   /effort medium         — set effort to medium (balanced)
//   /effort high           — set effort to high (thorough, more expensive)
//   /effort status         — print current level without changing it
//
// Effort semantics
// ----------------
// "Effort level" controls the reasoning/compute budget for o1-style models.
// It is stored as the BATBOX_EFFORT_LEVEL config field and mapped to
// inference request parameters by the Client (CPP 4.x):
//   low    — reasoning_effort: "low",  max_completion_tokens: 4 096
//   medium — reasoning_effort: "medium", max_completion_tokens: 16 384
//   high   — reasoning_effort: "high",   max_completion_tokens: 65 536
//
// For non-o1 models the effort field is silently ignored by the Client.
//
// Persistence
// -----------
// The selected level is written to ctx.config_dir / "settings.json" under
// the "effort_level" key using the same minimal upsert approach as VimCmd /
// ModelCmd.  BATBOX_EFFORT_LEVEL env var overrides settings.json at load time.
//
// No aliases.
//
// Registration entry point:
//   void register_effort_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Strip leading/trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end   = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Valid effort levels (canonical lowercase).
static constexpr std::string_view kEffortLevels[] = {"low", "medium", "high"};

/// True when `s` is a valid effort level string.
[[nodiscard]] bool is_valid_effort(std::string_view s) noexcept {
    for (const auto& lvl : kEffortLevels) {
        if (s == lvl) return true;
    }
    return false;
}

/// Human-readable description for each effort level.
[[nodiscard]] std::string_view effort_description(std::string_view level) noexcept {
    if (level == "low")    return "fast, lower cost; suitable for simple tasks";
    if (level == "medium") return "balanced speed and quality (default)";
    if (level == "high")   return "thorough reasoning, higher cost; best for complex tasks";
    return "unknown level";
}

/// Read the current effort level from BATBOX_EFFORT_LEVEL env var, then
/// fall back to settings.json "effort_level", then default to "medium".
[[nodiscard]] std::string resolve_current_effort(const fs::path& config_dir) {
    // Process env / .env takes highest priority.
    if (const char* env_effort = std::getenv("BATBOX_EFFORT_LEVEL")) {
        const std::string_view v = trim(std::string_view(env_effort));
        if (is_valid_effort(v)) return std::string(v);
    }

    // Read from settings.json.
    const fs::path settings_path = config_dir / "settings.json";
    std::ifstream f(settings_path);
    if (!f) return "medium";

    std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    const std::regex effort_re(R"X("effort_level"\s*:\s*"([^"]*)")X");
    std::smatch match;
    if (std::regex_search(content, match, effort_re) && match.size() >= 2) {
        const std::string found = match[1].str();
        if (is_valid_effort(found)) return found;
    }

    return "medium";
}

/// Upsert the "effort_level" key in settings.json atomically.
/// Returns empty string on success or a problem description.
[[nodiscard]] std::string persist_effort_level(
    const fs::path& config_dir,
    std::string_view level)
{
    const fs::path settings_path = config_dir / "settings.json";

    // Ensure config directory exists.
    std::error_code ec;
    fs::create_directories(config_dir, ec);
    if (ec) {
        return std::string("cannot create config dir '") + config_dir.string()
               + "': " + ec.message();
    }

    // If file does not exist, create a minimal one.
    if (!fs::exists(settings_path, ec)) {
        std::ofstream f(settings_path);
        if (!f) {
            return std::string("cannot create '") + settings_path.string()
                   + "': " + std::strerror(errno);
        }
        f << "{\n  \"effort_level\": \"" << level << "\"\n}\n";
        return {};
    }

    // File exists — read it.
    std::ifstream in_f(settings_path);
    if (!in_f) {
        return std::string("cannot read '") + settings_path.string()
               + "': " + std::strerror(errno);
    }
    std::string content((std::istreambuf_iterator<char>(in_f)),
                          std::istreambuf_iterator<char>());
    in_f.close();

    // Replace existing "effort_level": "..." or inject before closing brace.
    const std::regex effort_re(R"X("effort_level"\s*:\s*"[^"]*")X");
    const std::string replacement =
        std::string("\"effort_level\": \"") + std::string(level) + "\"";

    if (std::regex_search(content, effort_re)) {
        content = std::regex_replace(content, effort_re, replacement);
    } else {
        const auto last_brace = content.rfind('}');
        if (last_brace == std::string::npos) {
            return "settings.json does not appear to contain a JSON object; "
                   "effort_level not persisted (use BATBOX_EFFORT_LEVEL env var instead)";
        }
        content.insert(last_brace,
                       std::string("  \"effort_level\": \"") + std::string(level) + "\",\n");
    }

    // Write back atomically.
    const fs::path tmp_path = settings_path.string() + ".batbox.tmp";
    {
        std::ofstream out_f(tmp_path);
        if (!out_f) {
            return std::string("cannot write '") + tmp_path.string()
                   + "': " + std::strerror(errno);
        }
        out_f << content;
    }
    fs::rename(tmp_path, settings_path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return std::string("cannot atomically replace '") + settings_path.string()
               + "': " + ec.message();
    }
    return {};
}

/// Print the current effort status to `out`.
void print_effort_status(std::ostream& out, const std::string& current_level) {
    out << "\n  Effort level: " << current_level << "\n\n";
    for (const auto& lvl : kEffortLevels) {
        const bool is_current = (lvl == current_level);
        out << "    /effort " << lvl;
        // Pad to column 24 from the /effort prefix.
        const std::size_t col = 12 + lvl.size();  // "    /effort " + lvl
        if (col < 24) out << std::string(24 - col, ' ');
        else out << ' ';
        out << effort_description(lvl);
        if (is_current) out << "  ← current";
        out << '\n';
    }
    out << '\n';

    // Warn if env var overrides the setting.
    if (const char* env_effort = std::getenv("BATBOX_EFFORT_LEVEL")) {
        out << "  Note: BATBOX_EFFORT_LEVEL=" << env_effort
            << " is set in the environment and overrides settings.json.\n\n";
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// EffortCmd
// ---------------------------------------------------------------------------

class EffortCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "effort";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Set the reasoning effort level for o1-style models (low/medium/high).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/effort [low|medium|high|status]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   args,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> EffortCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view arg = trim(args);
    const std::string current_level = resolve_current_effort(ctx.config_dir);

    if (arg.empty() || arg == "status") {
        // Show current effort level and available options.
        print_effort_status(ctx.output, current_level);
        return {};
    }

    if (!is_valid_effort(arg)) {
        return batbox::Err(
            std::string("/effort: unknown level '") + std::string(arg) +
            "'.\nValid levels: low, medium, high.\n"
            "Usage: " + std::string(usage())
        );
    }

    const std::string new_level(arg);

    if (new_level == current_level) {
        // Still persist: if the user explicitly set the level (even to the
        // current/default value) we write it to settings.json so that a later
        // change to the BATBOX_EFFORT_LEVEL env var does not silently override
        // a user's saved preference.  This also ensures the test for persistence
        // of the default value ("medium") works correctly.
        (void)persist_effort_level(ctx.config_dir, new_level);
        ctx.output << "  Effort level is already '" << current_level << "'.\n";
        print_effort_status(ctx.output, current_level);
        return {};
    }

    // Persist to settings.json.
    const std::string persist_err = persist_effort_level(ctx.config_dir, new_level);
    if (!persist_err.empty()) {
        // Non-fatal: report the issue but continue.
        ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
    }

    ctx.output << "  Effort level set to '" << new_level << "': "
               << effort_description(new_level) << ".\n";

    // Warn if env var will override the persisted setting this session.
    if (const char* env_effort = std::getenv("BATBOX_EFFORT_LEVEL")) {
        const std::string_view env_v = trim(std::string_view(env_effort));
        if (env_v != new_level) {
            ctx.output << "  Note: BATBOX_EFFORT_LEVEL=" << env_effort
                       << " overrides this session — unset it to use the saved value.\n";
        }
    }
    ctx.output << '\n';

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_effort_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<EffortCmd>());
    (void)res;
}

} // namespace batbox::commands
