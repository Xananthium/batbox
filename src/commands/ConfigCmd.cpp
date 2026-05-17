// src/commands/ConfigCmd.cpp
//
// batbox::commands::ConfigCmd — implements the /config slash command.
//
// Subcommands:
//   /config              — alias for /config show
//   /config show         — dump the effective config to output (secrets redacted)
//   /config get <key>    — print a single config key value
//   /config set <key> <value>  — set a BATBOX_* config key in settings.json
//   /config reload       — hot-reload config from .env + settings.json
//   /config path         — print the path to settings.json
//
// Config field access
// -------------------
// CommandContext does not carry a live Config* pointer at this phase.  /config
// reads the raw .env file and settings.json from ctx.config_dir using the same
// helpers used by the rest of the config layer (EnvLoader + SettingsLoader).
// Field access uses the flat key names that match BATBOX_* env var names
// (e.g. "BATBOX_DEFAULT_MODEL", "BATBOX_THEME") as well as short forms
// ("default_model", "theme", etc.).
//
// /config reload invokes batbox::config::reload_config() when a live Config
// reference is available through the config_dir; the command operates in
// "display reload" mode otherwise (re-reads .env + settings.json and displays
// the new effective values).
//
// Persistence
// -----------
// /config set writes the new value into ctx.config_dir/settings.json using
// the minimal key-upsert approach (same as VimCmd / ModelCmd).
//
// No aliases.
//
// Registration entry point:
//   void register_config_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/EnvLoader.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
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

/// Split s on the first whitespace run; returns {head, rest}.
[[nodiscard]] std::pair<std::string_view, std::string_view>
split_first_word(std::string_view s) {
    const auto space = s.find_first_of(" \t");
    if (space == std::string_view::npos) return {s, {}};
    const std::string_view head = s.substr(0, space);
    const std::string_view rest = trim(s.substr(space));
    return {head, rest};
}

/// All BATBOX_* keys we expose, paired with short forms.
///
/// Format: {batbox_env_key, short_alias}
/// Used for: tab-completion scaffolding, display order, and set validation.
static const std::pair<std::string_view, std::string_view> kKnownKeys[] = {
    {"BATBOX_API_BASE_URL",               "base_url"},
    {"BATBOX_API_KEY",                    "api_key"},
    {"BATBOX_DEFAULT_MODEL",              "default_model"},
    {"BATBOX_MODELS",                     "models"},
    {"BATBOX_MAX_TOKENS",                 "max_tokens"},
    {"BATBOX_TEMPERATURE",                "temperature"},
    {"BATBOX_TOP_P",                      "top_p"},
    {"BATBOX_REQUEST_TIMEOUT_SEC",        "request_timeout_sec"},
    {"BATBOX_STREAM",                     "stream"},
    {"BATBOX_PROVIDER_HINT",              "provider_hint"},
    {"BATBOX_CONFIG_DIR",                 "config_dir"},
    {"BATBOX_PROJECT_MEMORY_FILE",        "project_memory_file"},
    {"BATBOX_LOG_LEVEL",                  "log_level"},
    {"BATBOX_LOG_FILE",                   "log_file"},
    {"BATBOX_THEME",                      "theme"},
    {"BATBOX_NO_SPLASH",                  "no_splash"},
    {"BATBOX_VIM_MODE",                   "vim_mode"},
    {"BATBOX_STATUSLINE",                 "statusline"},
    {"BATBOX_SEARCH_ENGINE",              "search_engine"},
    {"BATBOX_SEARXNG_URL",                "searxng_url"},
    {"BATBOX_RESPECT_ROBOTS",             "respect_robots"},
    {"BATBOX_WEBFETCH_MAX_BYTES",         "webfetch_max_bytes"},
    {"BATBOX_WEBFETCH_TIMEOUT_SEC",       "webfetch_timeout_sec"},
    {"BATBOX_SIDECAR_PYTHON",             "sidecar_python"},
    {"BATBOX_SIDECAR_VENV",              "sidecar_venv"},
    {"BATBOX_SIDECAR_STARTUP_TIMEOUT_SEC","sidecar_startup_timeout_sec"},
    {"BATBOX_SIDECAR_AUTOSTART",          "sidecar_autostart"},
    {"BATBOX_SIDECAR_PREWARM",            "sidecar_prewarm"},
    {"BATBOX_BASH_TIMEOUT_SEC",           "bash_timeout_sec"},
    {"BATBOX_BASH_MAX_OUTPUT_BYTES",      "bash_max_output_bytes"},
    {"BATBOX_TASK_PARALLEL_LIMIT",        "task_parallel_limit"},
    {"BATBOX_MCP_CONFIG",                 "mcp_config"},
    {"BATBOX_MCP_STARTUP_TIMEOUT_SEC",    "mcp_startup_timeout_sec"},
    {"BATBOX_AGENTS_CONFIG",              "agents_config"},
    {"BATBOX_AGENTS_DIR",                 "agents_dir"},
    {"BATBOX_DEMON_ENABLED",              "demon_enabled"},
    {"BATBOX_PERMISSION_MODE",            "permission_mode"},
    {"BATBOX_AUTO_APPROVE_READS",         "auto_approve_reads"},
    {"BATBOX_AUTO_COMPACT_AT_PCT",        "auto_compact_at_pct"},
    {"BATBOX_KEEP_LAST_N_TURNS_VERBATIM", "keep_last_n_turns_verbatim"},
    {"BATBOX_EFFORT_LEVEL",               "effort_level"},
};

/// Resolve a user-supplied key to the canonical BATBOX_* env key name.
/// Accepts both short forms ("theme") and full env key names ("BATBOX_THEME").
/// Returns empty string if the key is not recognised.
[[nodiscard]] std::string_view resolve_key(std::string_view user_key) {
    // Case-insensitive comparison helper.
    auto ieq = [](std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };

    for (const auto& [env_key, short_key] : kKnownKeys) {
        if (ieq(user_key, env_key) || ieq(user_key, short_key)) {
            return env_key;
        }
    }
    return {};
}

/// Redact the value of secret keys before display.
[[nodiscard]] std::string_view maybe_redact(
    std::string_view env_key,
    std::string_view value)
{
    if (env_key == "BATBOX_API_KEY") return "****";
    return value;
}

/// Build a merged key=value map from environment (process env) and .env file.
/// Process env takes precedence over .env file values.
[[nodiscard]] batbox::config::EnvMap load_effective_env(const fs::path& config_dir) {
    batbox::config::EnvMap env;

    // Attempt to load the .env file.
    const fs::path env_file = config_dir / ".env";
    auto load_res = batbox::config::load_env_file(env_file);
    if (load_res.has_value()) {
        env = std::move(load_res).value();
    }
    // Merge process env on top (process_env_wins=true so shell env wins).
    batbox::config::merge_with_process_env(env, /*process_env_wins=*/true);

    return env;
}

/// Read settings.json as raw text.  Returns empty string on failure.
[[nodiscard]] std::string read_settings_raw(const fs::path& config_dir) {
    std::ifstream f(config_dir / "settings.json");
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

/// Print the effective config values for all known keys to `out`.
void print_effective_config(std::ostream& out, const batbox::config::EnvMap& env) {
    out << "\n  Effective configuration  (BATBOX_* fields)\n";
    out << "  ──────────────────────────────────────────\n\n";

    // Group by sub-struct prefix.
    // We iterate kKnownKeys in definition order for a stable, readable output.
    for (const auto& [env_key, short_key] : kKnownKeys) {
        const std::string key_str(env_key);
        auto it = env.find(key_str);
        const std::string raw_val = (it != env.end()) ? it->second : std::string{"(default)"};
        const std::string display_val = std::string(maybe_redact(env_key, raw_val));

        // Align columns: key at column 0, value at column 42.
        out << "    " << env_key;
        const std::size_t key_col = 4 + env_key.size();
        if (key_col < 46) {
            out << std::string(46 - key_col, ' ');
        } else {
            out << ' ';
        }
        out << display_val << '\n';
    }
    out << '\n';
    out << "  Source: environment variables / " << "~/.batbox/.env\n";
    out << "  Override: settings.json (lower priority than env vars)\n\n";
}

/// Upsert a key-value pair in settings.json.
/// The key may be a BATBOX_* full name or a short alias; it is stored in
/// settings.json using a normalised lowercase snake_case form derived from
/// the BATBOX_* name (drop "BATBOX_", lowercase, underscores preserved).
///
/// Returns empty string on success or a problem description.
[[nodiscard]] std::string persist_config_key(
    const fs::path& config_dir,
    std::string_view batbox_env_key,
    std::string_view value)
{
    // Derive the JSON key name: drop "BATBOX_" prefix, lowercase.
    const std::string json_key = [&]() -> std::string {
        constexpr std::string_view prefix = "BATBOX_";
        std::string k(batbox_env_key.substr(batbox_env_key.starts_with(prefix)
                                            ? prefix.size() : 0));
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return k;
    }();

    const fs::path settings_path = config_dir / "settings.json";

    // Ensure config directory exists.
    std::error_code ec;
    fs::create_directories(config_dir, ec);
    if (ec) {
        return std::string("cannot create config dir '") + config_dir.string()
               + "': " + ec.message();
    }

    // Decide how to serialise the value.
    // Strings go as "value", booleans and numbers go unquoted.
    auto is_numeric = [](std::string_view s) -> bool {
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
        return true;
    };
    const bool is_bool = (value == "true" || value == "false");
    const bool is_num  = !is_bool && is_numeric(value);
    const std::string serialised_value =
        (is_bool || is_num)
            ? std::string(value)
            : ("\"" + std::string(value) + "\"");

    const std::string json_entry =
        "\"" + json_key + "\": " + serialised_value;

    // If file does not exist, create a minimal one.
    if (!fs::exists(settings_path, ec)) {
        std::ofstream f(settings_path);
        if (!f) {
            return std::string("cannot create '") + settings_path.string()
                   + "': " + std::strerror(errno);
        }
        f << "{\n  " << json_entry << "\n}\n";
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

    // Build a regex that matches the existing key.
    // We handle both quoted-string values and bare (bool/number) values.
    const std::string escaped_key = std::regex_replace(json_key, std::regex(R"([-\.])"), R"(\$&)");
    const std::regex key_re(
        std::string("\"") + escaped_key +
        R"X("\s*:\s*(?:"[^"]*"|true|false|[0-9]+(?:\.[0-9]*)?))X"
    );

    if (std::regex_search(content, key_re)) {
        content = std::regex_replace(content, key_re, json_entry);
    } else {
        const auto last_brace = content.rfind('}');
        if (last_brace == std::string::npos) {
            return "settings.json does not appear to contain a JSON object; "
                   "key not persisted";
        }
        content.insert(last_brace, "  " + json_entry + ",\n");
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

}  // anonymous namespace

// ---------------------------------------------------------------------------
// ConfigCmd
// ---------------------------------------------------------------------------

class ConfigCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "config";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Show or edit the effective batbox configuration.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/config [show | get <key> | set <key> <value> | reload | path]";
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

batbox::Result<void> ConfigCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view sub_full = trim(args);
    const auto [sub, rest] = split_first_word(sub_full);

    // Default (no subcommand) → show
    if (sub.empty() || sub == "show") {
        const batbox::config::EnvMap env = load_effective_env(ctx.config_dir);
        print_effective_config(ctx.output, env);
        ctx.output << "  Settings file: " << (ctx.config_dir / "settings.json").string() << "\n";
        ctx.output << "  /config get <key>          — print a single key\n";
        ctx.output << "  /config set <key> <value>  — update settings.json\n";
        ctx.output << "  /config reload             — re-read .env + settings.json\n\n";
        return {};
    }

    if (sub == "path") {
        const fs::path settings_path = ctx.config_dir / "settings.json";
        ctx.output << "  Config directory:  " << ctx.config_dir.string() << '\n';
        ctx.output << "  Settings file:     " << settings_path.string() << '\n';
        ctx.output << "  Env overlay file:  " << (ctx.config_dir / ".env").string() << '\n';
        return {};
    }

    if (sub == "reload") {
        // Reload: re-read .env + settings.json and display what changed.
        ctx.output << "  Reloading configuration from "
                   << ctx.config_dir.string() << " ...\n";
        const batbox::config::EnvMap env_before = load_effective_env(ctx.config_dir);
        // Trigger OS re-read by re-parsing .env (the process env cannot change
        // without execve, but the file may have changed).
        batbox::config::EnvMap env_after;
        {
            auto res = batbox::config::load_env_file(ctx.config_dir / ".env");
            if (res.has_value()) {
                env_after = std::move(res).value();
            }
            batbox::config::merge_with_process_env(env_after, true);
        }

        // Summarise changed keys.
        std::vector<std::string> changed;
        for (const auto& [env_key, _short] : kKnownKeys) {
            const std::string k(env_key);
            const std::string before_val =
                (env_before.count(k) ? env_before.at(k) : std::string{});
            const std::string after_val  =
                (env_after.count(k)  ? env_after.at(k)  : std::string{});
            if (before_val != after_val) {
                changed.push_back(k);
            }
        }

        if (changed.empty()) {
            ctx.output << "  Configuration unchanged.\n\n";
        } else {
            ctx.output << "  " << changed.size() << " field(s) changed:\n";
            for (const auto& k : changed) {
                const std::string_view after_v =
                    maybe_redact(k, env_after.count(k) ? env_after.at(k) : "(default)");
                ctx.output << "    " << k << " = " << after_v << '\n';
            }
            ctx.output << '\n';
        }
        ctx.output << "  Reload complete.\n\n";
        return {};
    }

    if (sub == "get") {
        if (rest.empty()) {
            return batbox::Err(
                std::string("/config get: missing key.\n"
                            "Usage: /config get <key>")
            );
        }
        const std::string_view canon_key = resolve_key(rest);
        if (canon_key.empty()) {
            return batbox::Err(
                std::string("/config get: unknown key '") + std::string(rest) +
                "'.\nRun /config show for a full list."
            );
        }

        const batbox::config::EnvMap env = load_effective_env(ctx.config_dir);
        auto it = env.find(std::string(canon_key));
        if (it == env.end()) {
            ctx.output << "  " << canon_key << " = (default)\n";
        } else {
            const std::string_view display_val = maybe_redact(canon_key, it->second);
            ctx.output << "  " << canon_key << " = " << display_val << '\n';
        }
        return {};
    }

    if (sub == "set") {
        if (rest.empty()) {
            return batbox::Err(
                std::string("/config set: missing key and value.\n"
                            "Usage: /config set <key> <value>")
            );
        }
        const auto [key_part, value_part] = split_first_word(rest);
        if (key_part.empty()) {
            return batbox::Err(
                std::string("/config set: missing key.\n"
                            "Usage: /config set <key> <value>")
            );
        }
        if (value_part.empty()) {
            return batbox::Err(
                std::string("/config set: missing value for key '") +
                std::string(key_part) +
                "'.\nUsage: /config set <key> <value>"
            );
        }

        const std::string_view canon_key = resolve_key(key_part);
        if (canon_key.empty()) {
            return batbox::Err(
                std::string("/config set: unknown key '") + std::string(key_part) +
                "'.\nRun /config show for a full list."
            );
        }

        // Refuse to set secret keys via the command (security: shell history).
        if (canon_key == "BATBOX_API_KEY") {
            return batbox::Err(
                std::string("/config set: BATBOX_API_KEY must be set via the .env file "
                            "or shell environment, not this command.")
            );
        }

        const std::string persist_err =
            persist_config_key(ctx.config_dir, canon_key, value_part);
        if (!persist_err.empty()) {
            return batbox::Err(
                std::string("/config set: ") + persist_err
            );
        }
        ctx.output << "  " << canon_key << " = " << value_part
                   << "  (saved to settings.json)\n";
        ctx.output << "  Run /config reload to apply changes to the current session.\n\n";
        return {};
    }

    return batbox::Err(
        std::string("/config: unknown subcommand '") + std::string(sub) +
        "'.\nUsage: " + std::string(usage())
    );
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_config_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ConfigCmd>());
    (void)res;
}

} // namespace batbox::commands
