// include/batbox/commands/SlashCommandRegistry.hpp
//
// batbox::commands::SlashCommandRegistry — central directory of all slash
// commands registered at App::init time.
//
// Lifecycle
// ---------
// Commands are registered once at startup (App::init → CPP A.3 / CPP S.15).
// The registry is then read-only during normal operation.  Plugin-defined
// commands are also registered here (CPP 11.6) after the initial set.
//
// Lookup
// ------
// `lookup` resolves a primary name OR an alias to a raw pointer.  The pointer
// remains valid for the lifetime of the registry.  Returns nullptr when the
// name is not found.
//
// Fuzzy search
// ------------
// `fuzzy_find` powers the `/` command palette.  It scores each command
// against the query using a simple substring-based ranking:
//
//   1. Exact match on primary name            → score 100
//   2. Primary name starts with query         → score 80
//   3. Any alias starts with query            → score 70
//   4. Primary name contains query            → score 60
//   5. Any alias contains query               → score 50
//   6. Description contains query             → score 30
//   (all comparisons are case-insensitive)
//
// Commands with score == 0 are excluded.  Results are returned sorted
// descending by score, then ascending by primary name for ties.
// At most `k` results are returned (default 10).
//
// Duplicate registration
// ----------------------
// Registering a command whose primary name (or any alias) collides with an
// already-registered name/alias returns an error Result.  The command is NOT
// inserted.
//
// Thread safety
// -------------
// Registration is not thread-safe and must complete before any thread calls
// lookup / fuzzy_find / all.  Post-startup the registry is read-only and
// safe for concurrent reads.

#pragma once

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/core/Result.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// SlashCommandRegistry
// ---------------------------------------------------------------------------

class SlashCommandRegistry {
public:
    SlashCommandRegistry()  = default;
    ~SlashCommandRegistry() = default;

    // Non-copyable, movable.
    SlashCommandRegistry(const SlashCommandRegistry&)            = delete;
    SlashCommandRegistry& operator=(const SlashCommandRegistry&) = delete;
    SlashCommandRegistry(SlashCommandRegistry&&)                 = default;
    SlashCommandRegistry& operator=(SlashCommandRegistry&&)      = default;

    // ---- Registration -------------------------------------------------------

    /// Register a slash command.
    ///
    /// The primary name and all aliases are indexed.  Registration fails if
    /// any of those strings is already in use by a previously registered
    /// command.
    ///
    /// @returns Ok on success.
    ///          Err(message) describing the collision on failure.
    [[nodiscard]] batbox::Result<void> register_command(
        std::shared_ptr<ISlashCommand> cmd);

    // ---- Lookup -------------------------------------------------------------

    /// Resolve a primary name or alias to the owning command.
    ///
    /// The leading slash must NOT be included in `name_or_alias`.
    ///
    /// @returns Pointer to the command, or nullptr if not found.
    ///          The pointer is valid for the lifetime of the registry.
    [[nodiscard]] ISlashCommand* lookup(std::string_view name_or_alias) const noexcept;

    // ---- Fuzzy search -------------------------------------------------------

    /// Return up to `k` commands ranked by relevance to `query`.
    ///
    /// `query` is typically the text the user has typed after the `/` — it
    /// may be empty (returns all commands sorted alphabetically up to `k`).
    ///
    /// Scoring rubric:
    ///   100 — exact primary name match
    ///    80 — primary name starts with query
    ///    70 — any alias starts with query
    ///    60 — primary name contains query (substring)
    ///    50 — any alias contains query
    ///    30 — description contains query
    ///     0 — no match (excluded from results)
    ///
    /// When `query` is empty every command scores 1 and all are returned
    /// (up to `k`), sorted by primary name.
    ///
    /// Results are sorted: highest score first; ties broken by primary name
    /// ascending.
    [[nodiscard]] std::vector<ISlashCommand*> fuzzy_find(
        std::string_view query,
        std::size_t      k = 10) const;

    // ---- Enumeration --------------------------------------------------------

    /// Return all registered commands in alphabetical order by primary name.
    [[nodiscard]] std::vector<ISlashCommand*> all() const;

    /// Return all primary names in alphabetical order.
    /// Used by autocomplete (Repl / Autocomplete sources).
    [[nodiscard]] std::vector<std::string> names() const;

    /// Number of registered commands.
    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

private:
    // Primary name → owned command.
    std::unordered_map<std::string, std::shared_ptr<ISlashCommand>> commands_;

    // Alias → primary name (so lookup can find the owner).
    std::unordered_map<std::string, std::string> alias_map_;

    // ---- Private helpers ----------------------------------------------------

    /// Lower-case a UTF-8 string (ASCII fast-path; sufficient for command names
    /// and English descriptions which are all ASCII in practice).
    [[nodiscard]] static std::string to_lower(std::string_view sv);

    /// Compute a fuzzy score for `cmd` against lower-cased `lq`.
    /// Returns 0 when the command should be excluded entirely.
    /// Returns 1 when query is empty (include everything with neutral score).
    [[nodiscard]] static int score_command(
        const ISlashCommand& cmd,
        const std::string&   lq);
};

} // namespace batbox::commands
