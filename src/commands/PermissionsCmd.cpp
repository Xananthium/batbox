// src/commands/PermissionsCmd.cpp
//
// batbox::commands::PermissionsCmd — implements the /permissions slash command.
//
// Behaviour:
//   /permissions             — list all allow/deny/ask rules grouped by kind
//   /permissions add allow <pattern>  — add pattern to allow list
//   /permissions add deny  <pattern>  — add pattern to deny list
//   /permissions add ask   <pattern>  — add pattern to ask list
//   /permissions remove <pattern>     — remove pattern from all lists
//
// Pattern validation:
//   A valid pattern must be non-empty and non-whitespace-only.
//   The PermissionStore enforces deduplication.
//
// The settings.json path is derived from ctx.config_dir so the command works
// in tests (which inject a temp directory) and in production (which sets
// config_dir to ~/.batbox).
//
// No aliases.
//
// Registration entry point:
//   void register_permissions_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/permissions/PermissionRule.hpp>
#include <batbox/permissions/PermissionStore.hpp>
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

/// Strip leading and trailing ASCII whitespace from s.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Validate that `pattern` is a non-empty, non-whitespace-only string.
[[nodiscard]] bool is_valid_pattern(std::string_view pattern) noexcept {
    return !trim(pattern).empty();
}

/// Print all rules from a PermissionStore, grouped by kind, to `out`.
void print_rules(std::ostream& out,
                 const batbox::permissions::PermissionStore& store)
{
    const auto& allow = store.allow_rules();
    const auto& deny  = store.deny_rules();
    const auto& ask   = store.ask_rules();

    const std::size_t total = allow.size() + deny.size() + ask.size();

    if (total == 0) {
        out << "  No permission rules configured.\n\n";
        out << "  Edit settings.json or use:\n";
        out << "    /permissions add allow <pattern>\n";
        out << "    /permissions add deny  <pattern>\n";
        out << "    /permissions add ask   <pattern>\n";
        return;
    }

    out << '\n';

    if (!allow.empty()) {
        out << "  Allow  (auto-approved without prompt)\n";
        for (const auto& p : allow) {
            out << "    + " << p << '\n';
        }
        out << '\n';
    }

    if (!deny.empty()) {
        out << "  Deny   (always blocked)\n";
        for (const auto& p : deny) {
            out << "    - " << p << '\n';
        }
        out << '\n';
    }

    if (!ask.empty()) {
        out << "  Ask    (always prompt, overrides allow)\n";
        for (const auto& p : ask) {
            out << "    ? " << p << '\n';
        }
        out << '\n';
    }

    out << "  " << total << " rule" << (total == 1 ? "" : "s") << " total.\n\n";
    out << "  /permissions add allow|deny|ask <pattern>  to add\n";
    out << "  /permissions remove <pattern>              to remove from all lists\n";
}

/// Parse "allow", "deny", or "ask" from the front of `s`.
/// Returns the kind string and the remaining text after it (trimmed), or
/// empty kind_str if the prefix is not recognised.
[[nodiscard]] std::pair<std::string_view, std::string_view>
parse_kind_and_rest(std::string_view s)
{
    static constexpr std::string_view kKinds[] = { "allow", "deny", "ask" };
    for (std::string_view k : kKinds) {
        if (s.size() < k.size()) continue;
        if (s.substr(0, k.size()) != k) continue;
        // Remainder after the kind token.
        const std::string_view after = s.substr(k.size());
        // Must be end-of-string or a whitespace separator.
        if (!after.empty() && after[0] != ' ' && after[0] != '\t') continue;
        return { k, trim(after) };
    }
    return { {}, s };
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// PermissionsCmd
// ---------------------------------------------------------------------------

class PermissionsCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "permissions";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "View and edit allow/deny/ask tool permission rules.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/permissions [add allow|deny|ask <pattern>] [remove <pattern>]";
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

batbox::Result<void> PermissionsCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    // Derive settings.json path from ctx.config_dir so tests can inject a
    // temp directory and production uses the real ~/.batbox/ directory.
    const auto settings_path = ctx.config_dir / "settings.json";
    batbox::permissions::PermissionStore store(settings_path);

    // Report any load error as a warning but continue (store starts empty).
    if (!store.last_load_error().empty()) {
        ctx.output << "  Warning: " << store.last_load_error() << '\n';
    }

    // --- Parse subcommand -------------------------------------------------------

    const std::string_view trimmed = trim(args);

    // No args → list rules.
    if (trimmed.empty()) {
        ctx.output << "\n  Permission rules\n";
        ctx.output << "  ────────────────\n";
        print_rules(ctx.output, store);
        return {};
    }

    // "add <kind> <pattern>"
    if (trimmed.size() >= 3 && trimmed.substr(0, 3) == "add") {
        const std::string_view rest = trim(trimmed.substr(3));

        // Parse kind: "allow", "deny", or "ask".
        auto [kind_str, pattern_str] = parse_kind_and_rest(rest);

        if (kind_str.empty()) {
            return batbox::Err(
                std::string("/permissions add: expected 'allow', 'deny', or 'ask' after 'add'.\n"
                            "Usage: ") + std::string(usage())
            );
        }

        if (!is_valid_pattern(pattern_str)) {
            return batbox::Err(
                std::string("/permissions add: pattern must be non-empty.\n"
                            "Example: /permissions add allow \"Bash(git *)\"")
            );
        }

        const std::string pattern(trim(pattern_str));

        batbox::Result<void> result{};
        if (kind_str == "allow") {
            result = store.add_allow_rule(pattern);
        } else if (kind_str == "deny") {
            result = store.add_deny_rule(pattern);
        } else {
            result = store.add_ask_rule(pattern);
        }

        if (!result.has_value()) {
            return batbox::Err("/permissions add: failed to save — " + result.error());
        }

        ctx.output << "  Added " << kind_str << " rule: " << pattern << '\n';
        return {};
    }

    // "remove <pattern>"
    if (trimmed.size() >= 6 && trimmed.substr(0, 6) == "remove") {
        const std::string_view rest = trim(trimmed.substr(6));

        if (!is_valid_pattern(rest)) {
            return batbox::Err(
                std::string("/permissions remove: pattern must be non-empty.\n"
                            "Usage: /permissions remove <pattern>")
            );
        }

        const std::string pattern(trim(rest));
        auto result = store.remove_rule(pattern);

        if (!result.has_value()) {
            return batbox::Err("/permissions remove: failed to save — " + result.error());
        }

        ctx.output << "  Removed rule: " << pattern << " (from all lists)\n";
        return {};
    }

    // Unknown subcommand.
    return batbox::Err(
        std::string("/permissions: unknown subcommand '") + std::string(trimmed) +
        "'.\nUsage: " + std::string(usage())
    );
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_permissions_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<PermissionsCmd>());
    (void)res;
}

} // namespace batbox::commands
