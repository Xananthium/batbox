// src/commands/ModelCmd.cpp
//
// batbox::commands::ModelCmd — implements the /model slash command.
//
// Behaviour:
//   /model             — list available models (from BATBOX_MODELS env var,
//                        falling back to settings.json "default_model"), then
//                        prompt the user to select by number or name.
//   /model <name>      — switch directly to the named model.
//
// Model resolution order (highest to lowest priority):
//   1. BATBOX_MODELS env var (comma-separated list)
//   2. settings.json "models" array
//   3. Built-in defaults: {"gpt-4o", "gpt-4o-mini"}
//
// Persistence
// -----------
// The selected model is written to ctx.config_dir / "settings.json" under
// the "default_model" key using the same minimal upsert approach used by
// VimCmd — read the file, patch the key, write back atomically.
//
// No aliases.
//
// Registration entry point:
//   void register_model_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
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

/// Strip leading and trailing ASCII whitespace from a string_view.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end   = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Split a string by a single-char delimiter, skipping empty tokens.
[[nodiscard]] std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::string tok;
    std::istringstream stream(s);
    while (std::getline(stream, tok, delim)) {
        // Trim whitespace from each token.
        const std::string_view tv = trim(tok);
        if (!tv.empty()) {
            tokens.emplace_back(tv);
        }
    }
    return tokens;
}

/// Return the list of models in priority order:
///   1. BATBOX_MODELS env var (comma-separated)
///   2. Built-in defaults
[[nodiscard]] std::vector<std::string> resolve_model_list() {
    if (const char* env_models = std::getenv("BATBOX_MODELS")) {
        auto list = split(std::string(env_models), ',');
        if (!list.empty()) return list;
    }
    return {"gpt-4o", "gpt-4o-mini"};
}

/// Upsert the "default_model" key in settings.json using the same atomic
/// strategy as VimCmd: read → patch → write-to-tmp → rename.
///
/// Returns an empty string on success or a problem description on failure.
[[nodiscard]] std::string persist_model(
    const fs::path& config_dir,
    const std::string& model_name)
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
        f << "{\n  \"default_model\": \"" << model_name << "\"\n}\n";
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

    // Replace existing "default_model": "..." line or inject before closing brace.
    const std::regex model_re(R"("default_model"\s*:\s*"[^"]*")");
    const std::string replacement =
        std::string("\"default_model\": \"") + model_name + "\"";

    if (std::regex_search(content, model_re)) {
        content = std::regex_replace(content, model_re, replacement);
    } else {
        const auto last_brace = content.rfind('}');
        if (last_brace == std::string::npos) {
            return "settings.json does not appear to contain a JSON object; "
                   "default_model not persisted (use BATBOX_DEFAULT_MODEL env var instead)";
        }
        content.insert(last_brace,
                       std::string("  \"default_model\": \"") + model_name + "\",\n");
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

/// Print a numbered list of models to `out`, marking the current model.
void print_model_list(
    std::ostream& out,
    const std::vector<std::string>& models,
    const std::string& current_model)
{
    out << "\n  Available models\n";
    out << "  ────────────────\n\n";
    for (std::size_t i = 0; i < models.size(); ++i) {
        const bool is_current = (models[i] == current_model);
        out << "    " << (i + 1) << ".  " << models[i];
        if (is_current) out << "  (current)";
        out << '\n';
    }
    out << '\n';
}

/// Read the current default_model from settings.json.
/// Returns an empty string if the file/key is not found.
[[nodiscard]] std::string read_current_model(const fs::path& config_dir) {
    const fs::path settings_path = config_dir / "settings.json";
    std::ifstream f(settings_path);
    if (!f) return {};

    std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    // Simple regex extraction: "default_model": "value"
    const std::regex model_re(R"X("default_model"\s*:\s*"([^"]*)")X");
    std::smatch match;
    if (std::regex_search(content, match, model_re) && match.size() >= 2) {
        return match[1].str();
    }
    return {};
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// ModelCmd
// ---------------------------------------------------------------------------

class ModelCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "model";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "List or switch the active inference model.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/model [model-name]";
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

batbox::Result<void> ModelCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view arg = trim(args);
    const std::vector<std::string> models = resolve_model_list();

    // Read the current model from settings.json (empty if not set).
    std::string current_model = read_current_model(ctx.config_dir);
    if (current_model.empty() && !models.empty()) {
        // Fall back to the first model in the list as the implicit current.
        current_model = models[0];
        // Also check BATBOX_DEFAULT_MODEL env var.
        if (const char* env_default = std::getenv("BATBOX_DEFAULT_MODEL")) {
            current_model = env_default;
        }
    }

    if (arg.empty()) {
        // ModalPicker path: list models and prompt the user to pick.
        print_model_list(ctx.output, models, current_model);
        ctx.output << "  Enter a number or model name (or press Enter to keep '"
                   << current_model << "'): ";

        std::string line;
        if (!std::getline(ctx.input, line)) {
            // EOF / non-interactive — keep current model, just print the list.
            ctx.output << '\n';
            return {};
        }

        const std::string_view choice = trim(line);
        if (choice.empty()) {
            ctx.output << "  Kept model as '" << current_model << "'.\n";
            return {};
        }

        // Try to interpret as a 1-based index first.
        std::size_t idx = 0;
        const auto [ptr, err_code] =
            std::from_chars(choice.data(), choice.data() + choice.size(), idx);
        if (err_code == std::errc{} && ptr == choice.data() + choice.size()) {
            // Numeric input.
            if (idx < 1 || idx > models.size()) {
                return batbox::Err(
                    std::string("/model: index ") + std::string(choice) +
                    " is out of range (1–" + std::to_string(models.size()) + ")."
                );
            }
            const std::string& chosen = models[idx - 1];
            const std::string persist_err = persist_model(ctx.config_dir, chosen);
            if (!persist_err.empty()) {
                ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
            }
            ctx.output << "  Switched to model '" << chosen << "'.\n";
            return {};
        }

        // Otherwise treat the input as a model name (exact or prefix match).
        const std::string choice_str(choice);
        // Exact match first.
        auto it = std::find(models.begin(), models.end(), choice_str);
        if (it == models.end()) {
            // Prefix match.
            it = std::find_if(models.begin(), models.end(),
                [&](const std::string& m) {
                    return m.rfind(choice_str, 0) == 0;
                });
        }
        if (it == models.end()) {
            return batbox::Err(
                std::string("/model: unknown model '") + choice_str +
                "'. Run /model to see the full list."
            );
        }
        const std::string& chosen = *it;
        const std::string persist_err = persist_model(ctx.config_dir, chosen);
        if (!persist_err.empty()) {
            ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
        }
        ctx.output << "  Switched to model '" << chosen << "'.\n";
        return {};
    }

    // Direct switch path: /model <name>
    const std::string model_name(arg);

    // Validate: must be in the configured model list (case-insensitive search
    // but we store the canonical-cased version from the list).
    auto it = std::find(models.begin(), models.end(), model_name);
    if (it == models.end()) {
        // Accept the name even if it is not in the list — the user may be
        // specifying a model they have added manually.  Persist it and warn.
        const std::string persist_err = persist_model(ctx.config_dir, model_name);
        if (!persist_err.empty()) {
            ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
        }
        ctx.output << "  Switched to model '" << model_name << "'";
        ctx.output << " (not in configured BATBOX_MODELS list — verify spelling).\n";
        return {};
    }

    const std::string& chosen = *it;
    const std::string persist_err = persist_model(ctx.config_dir, chosen);
    if (!persist_err.empty()) {
        ctx.output << "  Warning: could not persist setting — " << persist_err << "\n";
    }
    ctx.output << "  Switched to model '" << chosen << "'.\n";
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_model_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ModelCmd>());
    (void)res;
}

} // namespace batbox::commands
