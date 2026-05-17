// src/commands/ResumeCmd.cpp
//
// batbox::commands::ResumeCmd — implements the /resume slash command.
//
// /resume lists the 20 most-recently-updated sessions via SessionStore::list_recent(20),
// presents them as a numbered picker, waits for the user to select one, then loads
// the session file and restores the conversation via ConversationHandle::set_messages_json().
//
// Argument forms:
//   /resume          — interactive numbered picker over last 20 sessions
//   /resume last     — silently loads the most recent session (no picker)
//   /resume <id>     — loads the session with the given UUID prefix
//   /resume cwd      — loads the most recent session for the current working directory
//
// Read time contract: < 50 ms for index reads (inherited from SessionStore, CPP 9.4).
// Selection loads + replays history via ConversationHandle::set_messages_json().
//
// No aliases.
//
// Registration entry point:
//   void register_resume_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/session/SessionFile.hpp>
#include <batbox/session/SessionIndex.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

namespace {

// ---------------------------------------------------------------------------
// format_session_age
//
// Converts a std::chrono::system_clock::time_point to a human-readable age
// string relative to now: "5m ago", "2h ago", "3d ago", "2026-04-10".
// ---------------------------------------------------------------------------
std::string format_session_age(
    std::chrono::system_clock::time_point updated_at)
{
    const auto now = std::chrono::system_clock::now();
    const auto age = now - updated_at;

    const auto secs  = std::chrono::duration_cast<std::chrono::seconds>(age).count();
    const auto mins  = secs / 60;
    const auto hours = mins / 60;
    const auto days  = hours / 24;

    if (secs < 60)        return std::to_string(secs) + "s ago";
    if (mins  < 60)       return std::to_string(mins) + "m ago";
    if (hours < 24)       return std::to_string(hours) + "h ago";
    if (days  < 30)       return std::to_string(days) + "d ago";

    // Fall back to calendar date.
    const std::time_t t = std::chrono::system_clock::to_time_t(updated_at);
    std::tm tm_val{};
#if defined(_WIN32)
    ::gmtime_s(&tm_val, &t);
#else
    ::gmtime_r(&t, &tm_val);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y-%m-%d");
    return oss.str();
}

// ---------------------------------------------------------------------------
// truncate_preview
//
// Truncates `s` to at most `max_len` characters, appending "…" if shortened.
// ---------------------------------------------------------------------------
std::string truncate_preview(const std::string& s, std::size_t max_len = 60)
{
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len - 1) + "\xe2\x80\xa6"; // UTF-8 ellipsis U+2026
}

// ---------------------------------------------------------------------------
// strip_leading_whitespace
// ---------------------------------------------------------------------------
std::string_view strip(std::string_view sv)
{
    const auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = sv.find_last_not_of(" \t\r\n");
    return sv.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// print_session_list
//
// Writes a numbered table of SessionIndexRecords to `out`.
// Format:
//   [1]  <id_prefix>  <age>  <turn_count> turns  <preview>
// ---------------------------------------------------------------------------
void print_session_list(
    std::ostream& out,
    const std::vector<batbox::session::SessionIndexRecord>& records)
{
    out << "\nRecent sessions:\n\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        const std::string id_str = r.id.to_string();
        const std::string id_prefix = id_str.size() >= 8 ? id_str.substr(0, 8) : id_str;
        const std::string age = format_session_age(r.updated_at);
        const std::string preview = truncate_preview(r.first_message_preview);

        out << "  [" << (i + 1) << "]  "
            << id_prefix << "  "
            << age << "  "
            << r.turn_count << " turn" << (r.turn_count == 1 ? "" : "s") << "  ";
        if (!preview.empty()) {
            out << "  \"" << preview << "\"";
        }
        out << "\n";
    }
    out << "\n";
}

} // namespace

// ---------------------------------------------------------------------------
// ResumeCmd
// ---------------------------------------------------------------------------

class ResumeCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "resume";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Resume a previous session from the session picker (last 20 sessions).";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/resume [last | cwd | <session-id>]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;
};

// ---------------------------------------------------------------------------
// execute
//
// Dispatch on the args string:
//   "" or whitespace-only → interactive numbered picker over last 20 sessions
//   "last"               → silently load the most recent session
//   "cwd"                → load the most recent session for ctx.cwd
//   any other string     → treat as session id prefix, load matching session
// ---------------------------------------------------------------------------

batbox::Result<void> ResumeCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view trimmed = strip(args);

    // Build the SessionStore pointed at the config_dir sessions subdirectory.
    const std::filesystem::path sessions_dir = ctx.config_dir / "sessions";
    batbox::session::SessionStore store(sessions_dir);

    // ------------------------------------------------------------------
    // cwd mode: resume the most recent session for the current directory.
    // ------------------------------------------------------------------
    if (trimmed == "cwd") {
        std::optional<batbox::session::SessionFile> maybe =
            store.resume_for_cwd(ctx.cwd);
        if (!maybe.has_value()) {
            return batbox::Err(
                std::string("/resume cwd: no previous session found for ") +
                ctx.cwd.string());
        }
        ctx.conversation.set_messages_json(
            batbox::Json(maybe->messages));
        ctx.output << "Resumed session " << maybe->id.to_string()
                   << " (" << maybe->messages.size() << " messages).\n";
        return {};
    }

    // ------------------------------------------------------------------
    // "last" mode: silently load the single most-recent session.
    // ------------------------------------------------------------------
    if (trimmed == "last") {
        auto list_res = store.list_recent(1);
        if (!list_res.has_value()) {
            return batbox::Err("/resume last: failed to read session index: " +
                               list_res.error());
        }
        if (list_res.value().empty()) {
            return batbox::Err(std::string("/resume last: no sessions found."));
        }
        const std::string sid = list_res.value().front().id.to_string();
        auto load_res = store.load(sid);
        if (!load_res.has_value()) {
            return batbox::Err("/resume last: failed to load session: " +
                               load_res.error());
        }
        ctx.conversation.set_messages_json(
            batbox::Json(load_res.value().messages));
        ctx.output << "Resumed session " << sid
                   << " (" << load_res.value().messages.size() << " messages).\n";
        return {};
    }

    // ------------------------------------------------------------------
    // id-prefix mode: find the first session whose UUID starts with trimmed.
    // ------------------------------------------------------------------
    if (!trimmed.empty()) {
        const std::string prefix(trimmed);
        // List up to 256 sessions to find a match.
        auto list_res = store.list_recent(256);
        if (!list_res.has_value()) {
            return batbox::Err("/resume: failed to read session index: " +
                               list_res.error());
        }
        std::string matched_id;
        for (const auto& rec : list_res.value()) {
            const std::string id_str = rec.id.to_string();
            if (id_str.rfind(prefix, 0) == 0) {
                matched_id = id_str;
                break;
            }
        }
        if (matched_id.empty()) {
            return batbox::Err("/resume: no session found with id prefix \"" +
                               prefix + "\".");
        }
        auto load_res = store.load(matched_id);
        if (!load_res.has_value()) {
            return batbox::Err("/resume: failed to load session " +
                               matched_id + ": " + load_res.error());
        }
        ctx.conversation.set_messages_json(
            batbox::Json(load_res.value().messages));
        ctx.output << "Resumed session " << matched_id
                   << " (" << load_res.value().messages.size() << " messages).\n";
        return {};
    }

    // ------------------------------------------------------------------
    // Interactive picker: list last 20, prompt for index, load selected.
    // ------------------------------------------------------------------
    auto list_res = store.list_recent(20);
    if (!list_res.has_value()) {
        return batbox::Err("/resume: failed to read session index: " +
                           list_res.error());
    }

    const auto& records = list_res.value();
    if (records.empty()) {
        ctx.output << "No previous sessions found.\n";
        return {};
    }

    print_session_list(ctx.output, records);
    ctx.output << "Enter session number (1-" << records.size()
               << ") or 'q' to cancel: ";
    ctx.output.flush();

    // Read the user's choice.
    std::string line;
    if (!std::getline(ctx.input, line)) {
        return batbox::Err(std::string("/resume: no input received."));
    }

    const std::string_view choice = strip(line);
    if (choice == "q" || choice == "Q") {
        ctx.output << "Cancelled.\n";
        return {};
    }

    // Parse the 1-based index.
    std::size_t idx = 0;
    const auto [ptr, ec] = std::from_chars(
        choice.data(), choice.data() + choice.size(), idx);
    if (ec != std::errc{} || ptr != choice.data() + choice.size() ||
        idx < 1 || idx > records.size()) {
        return batbox::Err(
            std::string("/resume: invalid selection '") + std::string(choice) +
            "'. Enter a number between 1 and " + std::to_string(records.size()) +
            " or 'q'.");
    }

    const std::string selected_id = records[idx - 1].id.to_string();
    auto load_res = store.load(selected_id);
    if (!load_res.has_value()) {
        return batbox::Err("/resume: failed to load session " +
                           selected_id + ": " + load_res.error());
    }

    ctx.conversation.set_messages_json(
        batbox::Json(load_res.value().messages));
    ctx.output << "Resumed session " << selected_id
               << " (" << load_res.value().messages.size() << " messages).\n";
    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_resume_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<ResumeCmd>());
    (void)res;
}

} // namespace batbox::commands
