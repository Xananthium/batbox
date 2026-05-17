// include/batbox/repl/Autocomplete.hpp
// ---------------------------------------------------------------------------
// batbox::repl::Autocomplete — context-aware multi-source completion engine
// for the batbox REPL InputBar.
//
// Design overview
// ---------------
// The Autocomplete class is constructed once at App::init time with non-owning
// references to the three live registries:
//   • batbox::commands::SlashCommandRegistry  — slash command names + descriptions
//   • batbox::plugins::PluginRegistry         — skill + agent names (via active_plugins())
//
// A current-working-directory functor and an optional list of extra search
// directories are set at construction; they drive the filesystem source.
//
// Prefix dispatch
// ---------------
// The caller sets AutocompleteContext to tell Autocomplete which source(s) to
// query:
//
//   AutocompleteContext::SlashCommand  — triggered after the user types '/'
//       → returns slash command names (fuzzy-ranked by prefix)
//
//   AutocompleteContext::AtMention     — triggered after the user types '@'
//       → returns skills + agents + matching file paths (fuzzy-ranked)
//
//   AutocompleteContext::BashCommand   — triggered after the user types '!'
//       → returns file paths from the filesystem source
//
//   AutocompleteContext::FilePath      — triggered when the current token
//       looks like a path (contains '/' or starts with '.', '~')
//       → returns filesystem completions only
//
//   AutocompleteContext::None          — no autocomplete (returns empty)
//
// Fuzzy matching
// --------------
// The same scoring algorithm used by HistorySearch is applied:
//   • Exact substring hit  → quality 1.0
//   • Fuzzy (all chars in order) → quality based on gap-penalty formula
//   • Recency for filesystem: alphabetical order used as tiebreak
//
// Performance
// -----------
// Filesystem completions use std::filesystem::directory_iterator (non-recursive)
// over the cwd and extra dirs.  For cwd with up to 10 000 entries the scan
// completes in <50 ms on modern hardware (macOS / Linux SSD).
//
// Candidates are returned as plain strings (name only, no leading sigil).
// Use complete_candidates() for the richer Candidate struct that includes a
// description field for TUI rendering.
//
// No FTXUI dependency — pure logic.
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/plugins/PluginRegistry.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::repl {

// ---------------------------------------------------------------------------
// AutocompleteContext — which source to consult
// ---------------------------------------------------------------------------

enum class AutocompleteContext {
    None,         ///< No completion — return empty list.
    SlashCommand, ///< After '/': complete against slash command names.
    AtMention,    ///< After '@': skills + agents + file paths.
    BashCommand,  ///< After '!': file paths (bash-command-style).
    FilePath,     ///< Token looks like a path: file paths only.
};

// ---------------------------------------------------------------------------
// Candidate — a ranked completion entry with an optional description.
//
// Returned by complete_candidates(); the description is shown by InputBar in
// the autocomplete popup.  `text` is the raw completion string (no sigil).
// ---------------------------------------------------------------------------

struct Candidate {
    std::string text;         ///< The completion text (no leading @, /, !, [[).
    std::string description;  ///< One-line hint shown in the TUI popup; may be empty.
    float       score;        ///< Ranking score in (0, 1]; higher = better match.

    bool operator==(const Candidate&) const = default;
};

// ---------------------------------------------------------------------------
// Autocomplete
// ---------------------------------------------------------------------------

class Autocomplete {
public:
    // -----------------------------------------------------------------------
    // Constructor
    //
    // slash_registry — reference to the live SlashCommandRegistry.
    //                  Must outlive this Autocomplete instance.
    //
    // plugin_registry — reference to the live PluginRegistry.
    //                   Must outlive this Autocomplete instance.
    //
    // cwd_fn — nullary callable that returns the current working directory;
    //          called each time complete() is invoked so the caller can
    //          change directories between completions.  If nullptr is
    //          provided, fs::current_path() is used.
    //
    // extra_dirs — additional directories searched by the filesystem source
    //              (in addition to the cwd).  Typically the user's configured
    //              "add-dir" extras from settings.json.
    // -----------------------------------------------------------------------
    explicit Autocomplete(
        const batbox::commands::SlashCommandRegistry& slash_registry,
        const batbox::plugins::PluginRegistry&        plugin_registry,
        std::function<fs::path()>                     cwd_fn    = nullptr,
        std::vector<fs::path>                         extra_dirs = {});

    // Non-copyable (reference members).
    Autocomplete(const Autocomplete&)            = delete;
    Autocomplete& operator=(const Autocomplete&) = delete;

    // Movable.
    Autocomplete(Autocomplete&&)            = default;
    Autocomplete& operator=(Autocomplete&&) = delete;

    ~Autocomplete() = default;

    // -----------------------------------------------------------------------
    // complete(prefix, ctx) — primary API matching the blueprint contract.
    //
    // Dispatches to the appropriate source(s) based on ctx, applies fuzzy
    // filtering against prefix, and returns up to kMaxResults candidates
    // sorted by descending score (best match first).
    //
    // Returns a vector of plain strings (no sigil prefix).
    // Empty prefix → returns all candidates from the selected source(s),
    //               alphabetically sorted.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<std::string> complete(
        std::string_view    prefix,
        AutocompleteContext ctx) const;

    // -----------------------------------------------------------------------
    // complete_candidates(prefix, ctx) — richer API for TUI rendering.
    //
    // Same dispatch and ranking as complete(), but returns Candidate structs
    // that include a description field and score.  InputBar uses this to
    // render the autocomplete popup with hints.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<Candidate> complete_candidates(
        std::string_view    prefix,
        AutocompleteContext ctx) const;

    // -----------------------------------------------------------------------
    // set_extra_dirs — update the list of extra filesystem search directories.
    // Replaces any previously set list.
    // -----------------------------------------------------------------------
    void set_extra_dirs(std::vector<fs::path> dirs);

    // -----------------------------------------------------------------------
    // Maximum number of candidates returned by complete() / complete_candidates().
    // -----------------------------------------------------------------------
    static constexpr std::size_t kMaxResults = 20;

private:
    // ---- Sources ------------------------------------------------------------

    /// Populate candidates from registered slash commands.
    /// prefix is matched against command name and aliases.
    void source_slash_commands(
        std::string_view        prefix_lower,
        std::vector<Candidate>& out) const;

    /// Populate candidates from plugin skills and agents.
    /// prefix is matched against name and description.
    void source_at_mentions(
        std::string_view        prefix_lower,
        std::vector<Candidate>& out) const;

    /// Populate candidates from the filesystem (cwd + extra_dirs).
    /// prefix is matched against the entry filename (not full path).
    void source_filesystem(
        std::string_view        prefix_lower,
        std::vector<Candidate>& out) const;

    // ---- Scoring helpers (shared with HistorySearch algorithm) -------------

    /// Compute match quality in [0, 1]:
    ///   1.0  — exact substring
    ///   (0,1) — fuzzy (all query chars in order)
    ///   0.0  — no match
    /// Both args must be pre-lowercased.
    [[nodiscard]] static float score_match(
        std::string_view entry_lower,
        std::string_view query_lower) noexcept;

    /// ASCII case fold to a new string.
    [[nodiscard]] static std::string to_lower(std::string_view s);

    // ---- State --------------------------------------------------------------

    const batbox::commands::SlashCommandRegistry& slash_registry_;
    const batbox::plugins::PluginRegistry&        plugin_registry_;
    std::function<fs::path()>                     cwd_fn_;
    std::vector<fs::path>                         extra_dirs_;
};

// ---------------------------------------------------------------------------
// detect_context(line) — utility helper used by InputBar.
//
// Inspects the current input line (up to the cursor) and returns the
// AutocompleteContext that should be used for completion, along with the
// prefix fragment the user has typed after the sigil.
//
// Returns {None, ""} when the line does not trigger any completion.
// ---------------------------------------------------------------------------

struct ContextDetectResult {
    AutocompleteContext context;
    std::string         prefix;  ///< Text after the sigil (may be empty).
};

[[nodiscard]] ContextDetectResult detect_context(std::string_view line);

} // namespace batbox::repl
