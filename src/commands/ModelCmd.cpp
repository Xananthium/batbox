// src/commands/ModelCmd.cpp
//
// batbox::commands::ModelCmd — implements the /model slash command.
//
// Behaviour:
//   /model             — list available models (from ctx.cfg->api.models),
//                        then prompt the user to select by number or name.
//   /model <name>      — switch directly to the named model.
//
// Model source (PEXT3 1.2):
//   ctx.cfg->api.models        — live Config populated from BATBOX_MODELS env var
//   ctx.cfg->api.default_model — live Config populated from BATBOX_DEFAULT_MODEL
//
// Persistence
// -----------
// The selected model is written to ctx.config_dir / "settings.json" under
// the "default_model" key using the same minimal upsert approach used by
// VimCmd — read the file, patch the key, write back atomically.
//
// Live mutation (PEXT3 1.3)
// -------------------------
// After persisting to disk, commit_model_switch() also mutates
// ctx.cfg->api.default_model in-process under *ctx.cfg_mutex.
//
// const_cast rationale:
//   ctx.cfg is typed as `const Config*` to prevent casual writes outside the
//   designated mutation path.  The App owns the Config and has declared that
//   writes are legal when holding cfg_mutex; here we are in that legal path.
//   The cast is the single, documented site for runtime model mutation.
//   Reads on the inference thread snapshot cfg under the same mutex
//   (see Conversation::snapshot_config or the App's PEXT2 snapshot path);
//   the lock_guard below covers the assignment only — string parsing and
//   persist_model() run without the lock held.
//
// If ctx.cfg_mutex is nullptr (headless/test mode), the live-cfg mutation is
// skipped silently; the settings.json write still proceeds so the new model
// takes effect on the next process start.
//
// No aliases.
//
// Registration entry point:
//   void register_model_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
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

/// Strip leading and trailing ASCII whitespace from a string_view.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end   = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
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

/// Persist `chosen` to settings.json, mutate ctx.cfg->api.default_model under
/// cfg_mutex, and print the "Switched to model" confirmation.
///
/// This is the single canonical site for model mutation in the command layer.
/// All three switch paths (direct, interactive-numeric, interactive-name) call
/// this function.  `out_of_list` adds the "not in configured BATBOX_MODELS list"
/// qualifier when the name was not found in ctx.cfg->api.models.
///
/// Precondition: ctx.cfg != nullptr  (callers must guard with the nullptr check
/// at the top of execute()).
void commit_model_switch(
    std::ostream&      out,
    const fs::path&    config_dir,
    const CommandContext& ctx,
    const std::string& chosen,
    bool               out_of_list = false)
{
    // Persist to disk (outside the mutex — file I/O does not need the lock).
    const std::string persist_err = persist_model(config_dir, chosen);
    if (!persist_err.empty()) {
        out << "  Warning: could not persist setting — " << persist_err << "\n";
    }

    // Mutate the live Config under cfg_mutex.
    //
    // const_cast rationale (see file-level comment):
    //   ctx.cfg is `const Config*` to prevent accidental writes.  This is the
    //   one authorised write site, executed only when cfg_mutex is available.
    //   The lock_guard covers only the assignment; parsing happens before we
    //   get here.
    if (ctx.cfg_mutex != nullptr) {
        std::lock_guard<std::mutex> lk(*ctx.cfg_mutex);
        const_cast<batbox::config::Config*>(ctx.cfg)->api.default_model = chosen;
    }

    // Confirmation output.
    out << "  Switched to model '" << chosen << "'";
    if (out_of_list) {
        out << " (not in configured BATBOX_MODELS list — verify spelling)";
    }
    out << ".\n";
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
    // Guard: live Config must be wired in (PEXT3 1.2).
    // commit_model_switch() may only be called when ctx.cfg != nullptr.
    if (ctx.cfg == nullptr) {
        return batbox::Err(std::string("/model: no live config available"));
    }

    const std::vector<std::string>& models = ctx.cfg->api.models;
    if (models.empty()) {
        return batbox::Err(
            std::string("/model: no models configured; set BATBOX_MODELS or BATBOX_DEFAULT_MODEL")
        );
    }

    const std::string& current_model = ctx.cfg->api.default_model;
    const std::string_view arg = trim(args);

    if (arg.empty()) {
        // Interactive picker: list models and prompt the user to pick.
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
            commit_model_switch(ctx.output, ctx.config_dir, ctx, models[idx - 1]);
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
        commit_model_switch(ctx.output, ctx.config_dir, ctx, *it);
        return {};
    }

    // Direct switch path: /model <name>
    const std::string model_name(arg);

    // Validate against the configured model list; accept unknown names with a warning.
    auto it = std::find(models.begin(), models.end(), model_name);
    if (it == models.end()) {
        commit_model_switch(ctx.output, ctx.config_dir, ctx, model_name, /*out_of_list=*/true);
        return {};
    }

    commit_model_switch(ctx.output, ctx.config_dir, ctx, *it);
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
