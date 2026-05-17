// src/commands/SkillsCmd.cpp
//
// batbox::commands::SkillsCmd — implements the /skills slash command.
//
// Behaviour:
//   /skills                  — list all loaded skills with name + description
//   /skills run <name>       — invoke skill by name: outputs its prompt_body
//   /skills info <name>      — show full metadata for a single skill
//
// Skill listing
// -------------
// When a live SkillLoader is wired in via the constructor the command uses it
// directly (reflecting any runtime-loaded skills).  When no SkillLoader is
// available the command constructs a fresh one, calls load_user_dirs(), and
// uses that ephemeral loader for the duration of the execute() call.
//
// The /skills run <name> subcommand writes the skill's prompt_body to
// ctx.output.  Actual inference execution is not triggered here; that is
// deferred to SkillTool (CPP 5.19).  This is consistent with the design in
// ned-cpp.md §2.C11 which delegates execution to the AI layer.
//
// Registration entry point:
//   void register_skills_cmd(SlashCommandRegistry&, batbox::plugins::SkillLoader*);
//   void register_skills_cmd(SlashCommandRegistry&);   // skill_loader = nullptr

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <iomanip>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

/// Truncate a string to at most `n` characters, appending "..." when truncated.
[[nodiscard]] std::string truncate(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    if (n <= 3) return s.substr(0, n);
    return s.substr(0, n - 3) + "...";
}

/// Render a list of all skills to `out`.
void render_skill_list(std::ostream& out,
                       const batbox::plugins::SkillLoader& loader)
{
    const std::vector<std::string> names = loader.names();

    if (names.empty()) {
        out << "  No skills loaded.\n";
        out << "  Skills are .md files in ~/.batbox/skills/, ./.batbox/skills/,\n";
        out << "  ~/.claude/skills/, or ./.claude/skills/\n";
        return;
    }

    out << '\n';
    out << "  Available skills (" << names.size() << "):\n\n";

    constexpr std::size_t kNameWidth = 24;
    constexpr std::size_t kDescWidth = 52;
    constexpr std::size_t kSrcWidth  = 12;

    // Header.
    out << "  " << std::left
        << std::setw(kNameWidth) << "NAME"
        << "  " << std::setw(kDescWidth) << "DESCRIPTION"
        << "  " << std::setw(kSrcWidth)  << "SOURCE"
        << '\n';
    out << "  " << std::string(kNameWidth, '-')
        << "  " << std::string(kDescWidth, '-')
        << "  " << std::string(kSrcWidth,  '-')
        << '\n';

    for (const auto& n : names) {
        const batbox::plugins::Skill* skill = loader.find(n);
        if (!skill) continue;  // should not happen

        out << "  " << std::left
            << std::setw(kNameWidth) << truncate(skill->name, kNameWidth)
            << "  " << std::setw(kDescWidth) << truncate(skill->description, kDescWidth)
            << "  " << std::setw(kSrcWidth)  << truncate(skill->source, kSrcWidth)
            << '\n';
    }

    out << '\n';
    out << "  Use \"/skills run <name>\" to invoke a skill.\n";
    out << "  Use \"/skills info <name>\" for full metadata.\n";
    out << '\n';
}

/// Render full metadata for a single skill.
void render_skill_info(std::ostream& out,
                       const batbox::plugins::Skill& skill)
{
    out << '\n';
    out << "  Name:        " << skill.name        << '\n';
    out << "  Description: " << skill.description << '\n';
    out << "  Source:      " << skill.source       << '\n';

    if (skill.model.has_value()) {
        out << "  Model:       " << *skill.model << '\n';
    }

    if (!skill.allowed_tools.empty()) {
        out << "  Tools:      ";
        for (const auto& t : skill.allowed_tools) {
            out << " " << t;
        }
        out << '\n';
    }

    if (skill.script_path.has_value()) {
        out << "  Script:      " << skill.script_path->string() << '\n';
    }

    if (!skill.prompt_body.empty()) {
        out << '\n';
        out << "  Prompt body:\n";
        // Indent each line.
        const std::string& body = skill.prompt_body;
        std::size_t pos = 0;
        while (pos < body.size()) {
            const auto nl = body.find('\n', pos);
            const std::string_view line =
                (nl == std::string::npos)
                ? std::string_view(body).substr(pos)
                : std::string_view(body).substr(pos, nl - pos);
            out << "    " << line << '\n';
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    out << '\n';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SkillsCmd
// ---------------------------------------------------------------------------

class SkillsCmd final : public ISlashCommand {
public:
    /// Construct with an optional live SkillLoader pointer.
    /// When skill_loader is nullptr, the command constructs an ephemeral loader
    /// at execute() time using load_user_dirs() so the output reflects the
    /// current on-disk skill files.
    explicit SkillsCmd(batbox::plugins::SkillLoader* skill_loader = nullptr)
        : skill_loader_(skill_loader) {}

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "skills";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "List available skills; \"/skills run <name>\" to invoke one.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/skills [run <name> | info <name>]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   args,
        CommandContext&    ctx) override;

private:
    batbox::plugins::SkillLoader* skill_loader_;  ///< Non-owning; may be null.

    /// Return a reference to the live loader if wired in, or load a fresh one.
    /// The `ephemeral` parameter receives a newly-constructed SkillLoader
    /// when skill_loader_ is null; it must outlive the returned reference.
    [[nodiscard]] const batbox::plugins::SkillLoader& resolve_loader(
        std::unique_ptr<batbox::plugins::SkillLoader>& ephemeral) const;
};

// ---------------------------------------------------------------------------
// resolve_loader
// ---------------------------------------------------------------------------

const batbox::plugins::SkillLoader& SkillsCmd::resolve_loader(
    std::unique_ptr<batbox::plugins::SkillLoader>& ephemeral) const
{
    if (skill_loader_ != nullptr) {
        return *skill_loader_;
    }

    // Construct an ephemeral loader and scan user directories.
    ephemeral = std::make_unique<batbox::plugins::SkillLoader>();
    ephemeral->load_user_dirs();
    return *ephemeral;
}

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> SkillsCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view trimmed = trim(args);

    // Resolve the loader (may create an ephemeral instance).
    std::unique_ptr<batbox::plugins::SkillLoader> ephemeral;
    const batbox::plugins::SkillLoader& loader = resolve_loader(ephemeral);

    // /skills (no args) — list all skills.
    if (trimmed.empty()) {
        render_skill_list(ctx.output, loader);
        return {};
    }

    // Split into subcommand + remainder.
    const auto space_pos = trimmed.find(' ');
    const std::string_view sub  = (space_pos == std::string_view::npos)
                                  ? trimmed
                                  : trimmed.substr(0, space_pos);
    const std::string_view rest = (space_pos == std::string_view::npos)
                                  ? std::string_view{}
                                  : trim(trimmed.substr(space_pos + 1));

    // /skills run <name> — output the skill's prompt body.
    if (sub == "run") {
        if (rest.empty()) {
            return batbox::Err(
                std::string("/skills run: skill name required.\n"
                            "Usage: /skills run <name>"));
        }

        const std::string skill_name(rest);
        const batbox::plugins::Skill* skill = loader.find(skill_name);
        if (!skill) {
            return batbox::Err(
                std::string("/skills run: skill '") + skill_name +
                "' not found.\nUse /skills to list available skills.");
        }

        ctx.output << '\n';
        ctx.output << "  Skill: " << skill->name << '\n';
        ctx.output << "  " << skill->description << '\n';
        ctx.output << '\n';
        if (!skill->prompt_body.empty()) {
            ctx.output << skill->prompt_body << '\n';
        } else {
            ctx.output << "  (empty prompt body)\n";
        }

        if (skill->script_path.has_value()) {
            ctx.output << '\n';
            ctx.output << "  Note: this skill has a companion script at:\n";
            ctx.output << "    " << skill->script_path->string() << '\n';
            ctx.output << "  Script execution is handled by SkillTool (CPP 5.19).\n";
        }
        ctx.output << '\n';
        return {};
    }

    // /skills info <name> — show full metadata.
    if (sub == "info") {
        if (rest.empty()) {
            return batbox::Err(
                std::string("/skills info: skill name required.\n"
                            "Usage: /skills info <name>"));
        }

        const std::string skill_name(rest);
        const batbox::plugins::Skill* skill = loader.find(skill_name);
        if (!skill) {
            return batbox::Err(
                std::string("/skills info: skill '") + skill_name +
                "' not found.\nUse /skills to list available skills.");
        }

        render_skill_info(ctx.output, *skill);
        return {};
    }

    return batbox::Err(
        std::string("/skills: unknown subcommand '") + std::string(sub) +
        "'.\nUsage: " + std::string(usage()));
}

// ---------------------------------------------------------------------------
// Registration functions
// ---------------------------------------------------------------------------

void register_skills_cmd(SlashCommandRegistry&        registry,
                          batbox::plugins::SkillLoader* skill_loader)
{
    auto res = registry.register_command(
        std::make_shared<SkillsCmd>(skill_loader));
    (void)res;
}

void register_skills_cmd(SlashCommandRegistry& registry) {
    register_skills_cmd(registry, nullptr);
}

} // namespace batbox::commands
