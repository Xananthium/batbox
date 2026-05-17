// src/commands/TasksCmd.cpp
//
// batbox::commands::TasksCmd — implements the /tasks slash command.
//
// Behaviour:
//   /tasks                           — list all tasks from ~/.batbox/tasks.json
//   /tasks --status <s>              — filter by status (pending|in_progress|completed|failed)
//   /tasks --tag    <t>              — filter by tag
//   /tasks --status <s> --tag <t>    — filter by both
//
// Output format
// -------------
// Each task is rendered as a compact markdown-ish block:
//
//   ● task-id (12 chars)  [STATUS]  title
//     description (if non-empty, truncated at 72 chars)
//     tags: #tag1 #tag2 (if any)
//
// A summary count line is appended at the end.
//
// When tasks.json does not exist or is empty the command reports "No tasks found."
//
// Registration entry point:
//   void register_tasks_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/tools/TaskStore.hpp>

#include <algorithm>
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

/// Map a status string to a short display label with padding.
[[nodiscard]] std::string status_label(const std::string& status) {
    if (status == "pending")     return "[pending    ]";
    if (status == "in_progress") return "[in_progress]";
    if (status == "completed")   return "[completed  ]";
    if (status == "failed")      return "[failed     ]";
    return "[" + status + "]";
}

/// Truncate a string to at most `n` chars, appending "..." when truncated.
[[nodiscard]] std::string truncate(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    if (n <= 3) return s.substr(0, n);
    return s.substr(0, n - 3) + "...";
}

/// Parse a simple --key value argument list from `args`.
/// Returns the value for `key` or an empty string when not found.
/// Keys are "--status" and "--tag".
[[nodiscard]] std::string parse_flag(std::string_view args,
                                     std::string_view flag)
{
    // Build a std::string copy for easy substr operations.
    const std::string a(args);
    const std::string f(flag);

    const auto pos = a.find(f);
    if (pos == std::string::npos) return {};

    // Skip past the flag name.
    std::size_t val_start = pos + f.size();

    // Skip whitespace.
    while (val_start < a.size() && (a[val_start] == ' ' || a[val_start] == '\t'))
        ++val_start;

    if (val_start >= a.size()) return {};

    // Value ends at next '--' or end of string.
    std::size_t val_end = val_start;
    while (val_end < a.size() && !(a[val_end] == '-' && val_end + 1 < a.size() && a[val_end + 1] == '-'))
        ++val_end;

    // Trim trailing whitespace from value.
    while (val_end > val_start && (a[val_end - 1] == ' ' || a[val_end - 1] == '\t'))
        --val_end;

    return a.substr(val_start, val_end - val_start);
}

/// Render the task list to `out`.
void render_tasks(std::ostream& out,
                  const std::vector<batbox::tools::Task>& tasks)
{
    if (tasks.empty()) {
        out << "  No tasks found.\n";
        return;
    }

    out << '\n';
    for (const auto& t : tasks) {
        // Bullet + truncated ID + status + title.
        const std::string id_short = t.id.size() > 12 ? t.id.substr(0, 12) : t.id;
        out << "  \xe2\x97\x8f " // UTF-8 bullet ●
            << id_short << "  "
            << status_label(t.status) << "  "
            << truncate(t.title, 60)
            << '\n';

        // Description (if non-empty).
        if (!t.description.empty()) {
            out << "    " << truncate(t.description, 72) << '\n';
        }

        // Tags (if any).
        if (!t.tags.empty()) {
            out << "    tags:";
            for (const auto& tag : t.tags) {
                out << " #" << tag;
            }
            out << '\n';
        }

        // Parent task (if set).
        if (!t.parent_id.empty()) {
            out << "    parent: " << t.parent_id.substr(0, 12) << '\n';
        }

        out << '\n';
    }

    // Summary counts per status.
    std::size_t pending = 0, in_progress = 0, completed = 0, failed = 0;
    for (const auto& t : tasks) {
        if (t.status == "pending")          ++pending;
        else if (t.status == "in_progress") ++in_progress;
        else if (t.status == "completed")   ++completed;
        else if (t.status == "failed")      ++failed;
    }

    out << "  " << tasks.size() << " task(s)"
        << "  pending: "     << pending
        << "  in_progress: " << in_progress
        << "  completed: "   << completed
        << "  failed: "      << failed
        << '\n';
    out << '\n';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// TasksCmd
// ---------------------------------------------------------------------------

class TasksCmd final : public ISlashCommand {
public:
    TasksCmd() = default;

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "tasks";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "List persistent tasks stored in ~/.batbox/tasks.json.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/tasks [--status <status>] [--tag <tag>]";
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

batbox::Result<void> TasksCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    // Parse optional --status and --tag filters.
    const std::string status_filter = parse_flag(args, "--status");
    const std::string tag_filter    = parse_flag(args, "--tag");

    // Validate status filter value when provided.
    if (!status_filter.empty()) {
        const bool valid = (status_filter == "pending"    ||
                            status_filter == "in_progress" ||
                            status_filter == "completed"   ||
                            status_filter == "failed");
        if (!valid) {
            return batbox::Err(
                std::string("/tasks: invalid status '") + status_filter +
                "'.\nValid values: pending, in_progress, completed, failed.\n"
                "Usage: " + std::string(usage()));
        }
    }

    // Load tasks from the default path (~/.batbox/tasks.json).
    batbox::tools::TaskStore store(batbox::tools::TaskStore::default_path());
    const std::vector<batbox::tools::Task> tasks =
        store.list_tasks(status_filter, tag_filter);

    // Show filter context if filtering is active.
    if (!status_filter.empty() || !tag_filter.empty()) {
        ctx.output << "  Filters:";
        if (!status_filter.empty()) ctx.output << " status=" << status_filter;
        if (!tag_filter.empty())    ctx.output << " tag=" << tag_filter;
        ctx.output << '\n';
    }

    render_tasks(ctx.output, tasks);
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_tasks_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<TasksCmd>());
    (void)res;
}

} // namespace batbox::commands
