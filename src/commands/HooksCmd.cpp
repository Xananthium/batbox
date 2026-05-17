// src/commands/HooksCmd.cpp
//
// batbox::commands::HooksCmd — implements the /hooks slash command.
//
// Behaviour:
//   /hooks          — list all configured hook events from settings.json
//   /hooks <event>  — show matchers and hooks for a specific event
//
// settings.json hooks schema (mirrors claude-code wire format):
//   {
//     "hooks": {
//       "PreToolUse": [
//         { "matcher": "Bash", "hooks": [{ "type": "command", "command": "echo pre" }] }
//       ],
//       "PostToolUse": [ ... ]
//     }
//   }
//
// HooksCmd reads the raw "hooks" object from settings.json via nlohmann::json
// directly (the Settings struct does not model the hooks section) and presents
// a formatted read-only view.  To add or modify hooks the user edits
// settings.json directly or uses the TUI hooks menu.
//
// No aliases.
//
// Registration entry point:
//   void register_hooks_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Known hook event names (mirrors src/entrypoints/sdk/coreTypes.ts HOOK_EVENTS)
// ---------------------------------------------------------------------------

static constexpr std::string_view kHookEvents[] = {
    "PreToolUse",
    "PostToolUse",
    "PostToolUseFailure",
    "Notification",
    "UserPromptSubmit",
    "SessionStart",
    "SessionEnd",
    "Stop",
    "StopFailure",
    "SubagentStart",
    "SubagentStop",
    "PreCompact",
    "PostCompact",
    "PermissionRequest",
    "PermissionDenied",
    "Setup",
    "TeammateIdle",
    "TaskCreated",
    "TaskCompleted",
    "Elicitation",
    "ElicitationResult",
    "ConfigChange",
    "WorktreeCreate",
    "WorktreeRemove",
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Return the human-readable description for a hook type string.
[[nodiscard]] std::string_view hook_type_label(std::string_view type) noexcept {
    if (type == "command") return "shell command";
    if (type == "prompt")  return "prompt";
    if (type == "agent")   return "agent";
    if (type == "http")    return "HTTP";
    return "unknown";
}

/// Extract a display string for a single hook object.
[[nodiscard]] std::string hook_display(const batbox::Json& hook) {
    if (!hook.is_object()) return "(invalid hook)";

    const auto type_it = hook.find("type");
    if (type_it == hook.end() || !type_it->is_string()) return "(no type)";
    const std::string type = type_it->get<std::string>();

    std::string display;
    display += "[";
    display += std::string(hook_type_label(type));
    display += "] ";

    if (type == "command") {
        const auto cmd_it = hook.find("command");
        if (cmd_it != hook.end() && cmd_it->is_string()) {
            display += cmd_it->get<std::string>();
        }
        const auto shell_it = hook.find("shell");
        if (shell_it != hook.end() && shell_it->is_string()) {
            display += "  (shell: " + shell_it->get<std::string>() + ")";
        }
    } else if (type == "prompt" || type == "agent") {
        const auto prompt_it = hook.find("prompt");
        if (prompt_it != hook.end() && prompt_it->is_string()) {
            const std::string prompt = prompt_it->get<std::string>();
            // Truncate long prompts at 60 chars.
            if (prompt.size() > 60) {
                display += prompt.substr(0, 57) + "...";
            } else {
                display += prompt;
            }
        }
    } else if (type == "http") {
        const auto url_it = hook.find("url");
        if (url_it != hook.end() && url_it->is_string()) {
            display += url_it->get<std::string>();
        }
    }

    // Optional "if" condition.
    const auto if_it = hook.find("if");
    if (if_it != hook.end() && if_it->is_string()) {
        display += "  if: " + if_it->get<std::string>();
    }

    return display;
}

/// Load and return the raw "hooks" JSON object from `settings_path`.
/// Returns an empty JSON object when the file is absent or has no hooks key.
[[nodiscard]] batbox::Json load_hooks_json(const fs::path& settings_path) {
    std::error_code ec;
    if (!fs::exists(settings_path, ec)) {
        return batbox::Json::object();
    }

    std::ifstream ifs(settings_path);
    if (!ifs.is_open()) {
        return batbox::Json::object();
    }

    std::ostringstream buf;
    buf << ifs.rdbuf();
    const std::string content = buf.str();
    ifs.close();

    batbox::Json root;
    try {
        root = batbox::Json::parse(content);
    } catch (...) {
        return batbox::Json::object();
    }

    if (!root.is_object()) return batbox::Json::object();

    const auto hooks_it = root.find("hooks");
    if (hooks_it == root.end() || !hooks_it->is_object()) {
        return batbox::Json::object();
    }

    return *hooks_it;
}

/// Print a summary listing of all configured hook events to `out`.
void print_hooks_summary(std::ostream& out, const batbox::Json& hooks_obj) {
    // Collect events that have at least one configured matcher.
    std::vector<std::string> configured_events;
    for (auto it = hooks_obj.begin(); it != hooks_obj.end(); ++it) {
        if (it->is_array() && !it->empty()) {
            configured_events.push_back(it.key());
        }
    }

    std::sort(configured_events.begin(), configured_events.end());

    if (configured_events.empty()) {
        out << "  No hooks configured.\n\n";
        out << "  Add hooks via settings.json under the \"hooks\" key:\n";
        out << "    {\n";
        out << "      \"hooks\": {\n";
        out << "        \"PreToolUse\": [\n";
        out << "          { \"matcher\": \"Bash\",\n";
        out << "            \"hooks\": [{ \"type\": \"command\", \"command\": \"echo pre\" }] }\n";
        out << "        ]\n";
        out << "      }\n";
        out << "    }\n\n";
        out << "  Supported events:\n";
        for (std::string_view ev : kHookEvents) {
            out << "    " << ev << '\n';
        }
        return;
    }

    out << '\n';
    for (const auto& event : configured_events) {
        const auto& matchers = hooks_obj[event];

        // Count total hooks across all matchers.
        std::size_t hook_count = 0;
        for (const auto& matcher_obj : matchers) {
            if (matcher_obj.is_object()) {
                const auto h_it = matcher_obj.find("hooks");
                if (h_it != matcher_obj.end() && h_it->is_array()) {
                    hook_count += h_it->size();
                }
            }
        }

        out << "  " << event
            << "  (" << matchers.size()
            << " matcher" << (matchers.size() == 1 ? "" : "s")
            << ", " << hook_count
            << " hook" << (hook_count == 1 ? "" : "s")
            << ")\n";
    }

    out << '\n';
    out << "  " << configured_events.size()
        << " event" << (configured_events.size() == 1 ? "" : "s")
        << " with hooks.\n\n";
    out << "  /hooks <EventName>  to inspect a specific event.\n";
    out << "  Edit settings.json directly to add or remove hooks.\n";
}

/// Print detail for a single hook event to `out`.
void print_event_detail(std::ostream& out,
                        const std::string& event,
                        const batbox::Json& hooks_obj)
{
    const auto ev_it = hooks_obj.find(event);
    if (ev_it == hooks_obj.end() || !ev_it->is_array() || ev_it->empty()) {
        out << "  No hooks configured for event: " << event << '\n';
        return;
    }

    const batbox::Json& matchers = *ev_it;
    out << '\n';
    out << "  " << event << '\n';
    out << "  " << std::string(event.size(), '-') << '\n';
    out << '\n';

    std::size_t matcher_idx = 0;
    for (const auto& matcher_obj : matchers) {
        ++matcher_idx;

        if (!matcher_obj.is_object()) {
            out << "  Matcher " << matcher_idx << ": (invalid — not an object)\n";
            continue;
        }

        // Matcher pattern.
        const auto m_it = matcher_obj.find("matcher");
        const std::string matcher_str =
            (m_it != matcher_obj.end() && m_it->is_string())
            ? m_it->get<std::string>()
            : "(no matcher)";

        out << "  Matcher " << matcher_idx << ": " << matcher_str << '\n';

        // Hooks array.
        const auto h_it = matcher_obj.find("hooks");
        if (h_it == matcher_obj.end() || !h_it->is_array() || h_it->empty()) {
            out << "    (no hooks)\n";
        } else {
            for (const auto& hook : *h_it) {
                out << "    " << hook_display(hook) << '\n';
            }
        }
        out << '\n';
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HooksCmd
// ---------------------------------------------------------------------------

class HooksCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "hooks";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "View configured pre/post tool hooks from settings.json.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/hooks [EventName]";
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

batbox::Result<void> HooksCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    // Derive settings.json path from ctx.config_dir.
    const fs::path settings_path = ctx.config_dir / "settings.json";
    const batbox::Json hooks_obj = load_hooks_json(settings_path);

    // Strip leading/trailing whitespace from args.
    const std::string_view event_arg = [&]() -> std::string_view {
        auto s = args;
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) return {};
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }();

    ctx.output << "\n  Hooks\n";
    ctx.output << "  ─────\n";

    if (event_arg.empty()) {
        // Summary mode: list all configured events.
        print_hooks_summary(ctx.output, hooks_obj);
    } else {
        // Detail mode: show hooks for the specified event.
        const std::string event(event_arg);

        // Validate that it looks like a known event (warn but proceed anyway).
        bool known = false;
        for (std::string_view ev : kHookEvents) {
            if (ev == event_arg) { known = true; break; }
        }
        if (!known) {
            ctx.output << "  Note: '" << event
                       << "' is not a standard hook event name.\n\n";
        }

        print_event_detail(ctx.output, event, hooks_obj);
    }

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_hooks_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<HooksCmd>());
    (void)res;
}

} // namespace batbox::commands
