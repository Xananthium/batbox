// src/repl/Autocomplete.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::repl::Autocomplete — context-aware multi-source
// completion engine.
//
// Source dispatch summary:
//   SlashCommand → source_slash_commands()
//   AtMention    → source_at_mentions() + source_filesystem()
//   BashCommand  → source_filesystem()
//   FilePath     → source_filesystem()
//   None         → empty
//
// Fuzzy scoring mirrors HistorySearch::score_match() exactly so that the
// user experiences a consistent ranking algorithm across Ctrl+R and
// autocomplete.
// ---------------------------------------------------------------------------

#include "batbox/repl/Autocomplete.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace batbox::repl {

// ===========================================================================
// Constructor
// ===========================================================================

Autocomplete::Autocomplete(
    const batbox::commands::SlashCommandRegistry& slash_registry,
    const batbox::plugins::PluginRegistry&        plugin_registry,
    std::function<fs::path()>                     cwd_fn,
    std::vector<fs::path>                         extra_dirs)
    : slash_registry_{slash_registry}
    , plugin_registry_{plugin_registry}
    , cwd_fn_{std::move(cwd_fn)}
    , extra_dirs_{std::move(extra_dirs)}
{}

// ===========================================================================
// set_extra_dirs
// ===========================================================================

void Autocomplete::set_extra_dirs(std::vector<fs::path> dirs) {
    extra_dirs_ = std::move(dirs);
}

// ===========================================================================
// to_lower — ASCII case fold
// ===========================================================================

/*static*/ std::string Autocomplete::to_lower(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (const unsigned char c : s) {
        result.push_back(static_cast<char>(std::tolower(c)));
    }
    return result;
}

// ===========================================================================
// score_match
//
// Identical algorithm to HistorySearch::score_match():
//   1.0  — exact substring hit (case-insensitive)
//   (0,1) — fuzzy: all query chars appear in order; gap-penalty applied
//   0.0  — no match
//
// Pre-condition: both args are already lowercase.
// ===========================================================================

/*static*/ float Autocomplete::score_match(
    std::string_view entry_lower,
    std::string_view query_lower) noexcept
{
    if (query_lower.empty()) {
        return 1.0f;   // empty prefix matches everything
    }
    if (entry_lower.empty()) {
        return 0.0f;
    }

    // --- Substring check (O(N·M)) --
    if (entry_lower.find(query_lower) != std::string_view::npos) {
        return 1.0f;
    }

    // --- Fuzzy check: all query chars in order --
    const auto qlen = static_cast<int>(query_lower.size());
    const auto elen = static_cast<int>(entry_lower.size());

    int qi               = 0;
    int first_match_pos  = -1;
    int last_match_pos   = -1;

    for (int ei = 0; ei < elen && qi < qlen; ++ei) {
        if (entry_lower[static_cast<std::size_t>(ei)] ==
            query_lower[static_cast<std::size_t>(qi)])
        {
            if (first_match_pos == -1) {
                first_match_pos = ei;
            }
            last_match_pos = ei;
            ++qi;
        }
    }

    if (qi < qlen) {
        return 0.0f;   // not all query chars found
    }

    const int   span      = last_match_pos - first_match_pos + 1;
    const int   gaps      = span - qlen;
    const float gap_ratio = static_cast<float>(gaps) / static_cast<float>(elen);
    const float quality   = 1.0f - 0.4f * gap_ratio;

    return (quality < 0.01f) ? 0.01f : (quality > 1.0f ? 1.0f : quality);
}

// ===========================================================================
// source_slash_commands
// ===========================================================================

void Autocomplete::source_slash_commands(
    std::string_view        prefix_lower,
    std::vector<Candidate>& out) const
{
    const auto all_cmds = slash_registry_.all();
    for (const auto* cmd : all_cmds) {
        if (!cmd) {
            continue;
        }
        const std::string name_lower = to_lower(cmd->name());
        const float score = score_match(name_lower, prefix_lower);
        if (score > 0.0f) {
            // Boost prefix-match commands: if name starts with prefix, give
            // an extra 0.2 bonus (clamped to 1.0) to prefer them over fuzzy
            // matches in the middle of the name.
            float boosted = score;
            if (!prefix_lower.empty() &&
                name_lower.size() >= prefix_lower.size() &&
                name_lower.substr(0, prefix_lower.size()) == prefix_lower)
            {
                boosted = std::min(1.0f, score + 0.2f);
            }
            out.push_back(Candidate{
                std::string(cmd->name()),
                std::string(cmd->description()),
                boosted
            });
        }
    }
}

// ===========================================================================
// source_at_mentions
//
// Combines:
//   1. All active plugin skills (type tag "skill")
//   2. All active plugin agents (type tag "agent")
// Prefix is matched against name; description is carried through.
// ===========================================================================

void Autocomplete::source_at_mentions(
    std::string_view        prefix_lower,
    std::vector<Candidate>& out) const
{
    // Snapshot so we are safe even if a background reload runs.
    const auto snapshot = plugin_registry_.get_snapshot();
    if (!snapshot) {
        return;
    }

    for (const auto& plugin : *snapshot) {
        if (plugin.disabled) {
            continue;
        }

        // Skills
        for (const auto& skill : plugin.skills) {
            const std::string name_lower = to_lower(skill.name);
            const float score = score_match(name_lower, prefix_lower);
            if (score > 0.0f) {
                const std::string desc = skill.description.empty()
                    ? ("skill from " + plugin.name)
                    : skill.description;
                out.push_back(Candidate{skill.name, desc, score});
            }
        }

        // Agents
        for (const auto& agent : plugin.agents) {
            const std::string name_lower = to_lower(agent.name);
            const float score = score_match(name_lower, prefix_lower);
            if (score > 0.0f) {
                const std::string desc = agent.description.empty()
                    ? ("agent from " + plugin.name)
                    : agent.description;
                out.push_back(Candidate{agent.name, desc, score});
            }
        }
    }
}

// ===========================================================================
// source_filesystem
//
// Enumerates the cwd (via cwd_fn_ or fs::current_path()) and each directory
// in extra_dirs_.  Only the filename stem of each entry is matched against
// prefix_lower.  Directories are marked with a trailing '/' in their
// description.
//
// To stay within the 50 ms budget for 10 000 files, we stop after collecting
// kFilesystemScanLimit entries per directory and then rank what we have.
// ===========================================================================

void Autocomplete::source_filesystem(
    std::string_view        prefix_lower,
    std::vector<Candidate>& out) const
{
    static constexpr std::size_t kFilesystemScanLimit = 5000;

    // Collect directories to scan.
    std::vector<fs::path> dirs;
    {
        fs::path cwd;
        if (cwd_fn_) {
            cwd = cwd_fn_();
        } else {
            std::error_code ec;
            cwd = fs::current_path(ec);
            if (ec) {
                cwd = ".";
            }
        }
        dirs.push_back(std::move(cwd));
    }
    for (const auto& d : extra_dirs_) {
        dirs.push_back(d);
    }

    // Scan each directory.
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) {
            continue;
        }

        std::size_t count = 0;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (count++ >= kFilesystemScanLimit) {
                break;
            }

            const std::string filename  = entry.path().filename().string();
            const std::string fname_low = to_lower(filename);
            const float score = score_match(fname_low, prefix_lower);

            if (score > 0.0f) {
                std::error_code ec2;
                const bool is_dir = entry.is_directory(ec2);
                const std::string desc = is_dir ? "directory" : "file";
                // Append '/' to directory names to match shell convention.
                const std::string text = is_dir ? (filename + "/") : filename;
                out.push_back(Candidate{text, desc, score});
            }
        }
    }
}

// ===========================================================================
// complete_candidates — full Candidate vector
// ===========================================================================

std::vector<Candidate> Autocomplete::complete_candidates(
    std::string_view    prefix,
    AutocompleteContext ctx) const
{
    if (ctx == AutocompleteContext::None) {
        return {};
    }

    const std::string prefix_lower = to_lower(prefix);

    std::vector<Candidate> candidates;
    candidates.reserve(64);

    switch (ctx) {
    case AutocompleteContext::SlashCommand:
        source_slash_commands(prefix_lower, candidates);
        break;

    case AutocompleteContext::AtMention:
        source_at_mentions(prefix_lower, candidates);
        source_filesystem(prefix_lower, candidates);
        break;

    case AutocompleteContext::BashCommand:
    case AutocompleteContext::FilePath:
        source_filesystem(prefix_lower, candidates);
        break;

    case AutocompleteContext::None:
        // Already handled above.
        break;
    }

    // De-duplicate by text (keep highest score).
    {
        // Sort by text so equal entries are adjacent.
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                if (a.text != b.text) {
                    return a.text < b.text;
                }
                return a.score > b.score; // higher score first
            });

        auto new_end = std::unique(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                return a.text == b.text;
            });
        candidates.erase(new_end, candidates.end());
    }

    // Rank: descending score; ties broken alphabetically.
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            return a.text < b.text;
        });

    // Trim to kMaxResults.
    if (candidates.size() > kMaxResults) {
        candidates.resize(kMaxResults);
    }

    return candidates;
}

// ===========================================================================
// complete — primary API (blueprint contract)
//
// Delegates to complete_candidates() and strips the struct down to strings.
// ===========================================================================

std::vector<std::string> Autocomplete::complete(
    std::string_view    prefix,
    AutocompleteContext ctx) const
{
    const auto candidates = complete_candidates(prefix, ctx);

    std::vector<std::string> result;
    result.reserve(candidates.size());
    for (const auto& c : candidates) {
        result.push_back(c.text);
    }
    return result;
}

// ===========================================================================
// detect_context — utility helper for InputBar
// ===========================================================================

ContextDetectResult detect_context(std::string_view line) {
    if (line.empty()) {
        return {AutocompleteContext::None, ""};
    }

    // Leading '/' → slash command completion.
    // Only trigger when the user is still typing the command name
    // (no space yet after the sigil).
    if (line[0] == '/') {
        const std::string_view after = line.substr(1);
        // If there is a space in after, they are typing args — no completion.
        if (after.find(' ') == std::string_view::npos) {
            return {AutocompleteContext::SlashCommand, std::string(after)};
        }
        return {AutocompleteContext::None, ""};
    }

    // Leading '!' → bash command completion (filesystem).
    if (line[0] == '!') {
        // Find the last space-separated token — that is the filesystem prefix.
        const std::string_view after = line.substr(1);
        const auto last_space = after.rfind(' ');
        const std::string prefix = (last_space == std::string_view::npos)
            ? std::string(after)
            : std::string(after.substr(last_space + 1));
        return {AutocompleteContext::BashCommand, prefix};
    }

    // Leading '@' → @-mention completion.
    if (line[0] == '@') {
        const std::string_view after = line.substr(1);
        // If there is a space, they are past the mention — no completion.
        if (after.find(' ') == std::string_view::npos) {
            return {AutocompleteContext::AtMention, std::string(after)};
        }
        return {AutocompleteContext::None, ""};
    }

    // Leading '[[' → agent mention completion (same as @-mention source).
    if (line.size() >= 2 && line[0] == '[' && line[1] == '[') {
        const std::string_view after = line.substr(2);
        // Unclosed bracket: still completing.
        if (after.find("]]") == std::string_view::npos &&
            after.find(' ')  == std::string_view::npos)
        {
            return {AutocompleteContext::AtMention, std::string(after)};
        }
        return {AutocompleteContext::None, ""};
    }

    // Last word looks like a path → filesystem completion.
    {
        const auto last_space = line.rfind(' ');
        const std::string_view last_token = (last_space == std::string_view::npos)
            ? line
            : line.substr(last_space + 1);

        if (!last_token.empty() &&
            (last_token[0] == '.' || last_token[0] == '/' || last_token[0] == '~' ||
             last_token.find('/') != std::string_view::npos))
        {
            return {AutocompleteContext::FilePath, std::string(last_token)};
        }
    }

    return {AutocompleteContext::None, ""};
}

} // namespace batbox::repl
