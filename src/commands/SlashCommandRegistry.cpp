// src/commands/SlashCommandRegistry.cpp
//
// Implementation of batbox::commands::SlashCommandRegistry.
//
// See include/batbox/commands/SlashCommandRegistry.hpp for the full API
// documentation and scoring rubric.

#include <batbox/commands/SlashCommandRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/*static*/
std::string SlashCommandRegistry::to_lower(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    for (unsigned char c : sv) {
        result.push_back(static_cast<char>(std::tolower(c)));
    }
    return result;
}

/*static*/
int SlashCommandRegistry::score_command(const ISlashCommand& cmd,
                                         const std::string&   lq) {
    // Empty query: include all commands with a neutral score of 1.
    if (lq.empty()) {
        return 1;
    }

    const std::string lname = to_lower(cmd.name());
    const std::string ldesc = to_lower(cmd.description());

    // --- Primary name checks -------------------------------------------------

    // Exact primary name match.
    if (lname == lq) {
        return 100;
    }
    // Primary name starts with query.
    if (lname.size() >= lq.size() &&
        lname.compare(0, lq.size(), lq) == 0) {
        return 80;
    }

    // --- Alias checks --------------------------------------------------------

    for (const auto& alias : cmd.aliases()) {
        const std::string la = to_lower(alias);
        // Alias starts with query.
        if (la.size() >= lq.size() && la.compare(0, lq.size(), lq) == 0) {
            return 70;
        }
    }

    // --- Substring checks ----------------------------------------------------

    // Primary name contains query.
    if (lname.find(lq) != std::string::npos) {
        return 60;
    }

    // Any alias contains query.
    for (const auto& alias : cmd.aliases()) {
        const std::string la = to_lower(alias);
        if (la.find(lq) != std::string::npos) {
            return 50;
        }
    }

    // Description contains query.
    if (ldesc.find(lq) != std::string::npos) {
        return 30;
    }

    // No match.
    return 0;
}

// ---------------------------------------------------------------------------
// register_command
// ---------------------------------------------------------------------------

batbox::Result<void> SlashCommandRegistry::register_command(
    std::shared_ptr<ISlashCommand> cmd)
{
    if (!cmd) {
        return batbox::Err(std::string("register_command: null command pointer"));
    }

    const std::string primary = to_lower(cmd->name());

    if (primary.empty()) {
        return batbox::Err(std::string("register_command: command name must not be empty"));
    }

    // Check primary name collision.
    if (commands_.count(primary) > 0) {
        return batbox::Err(
            "register_command: duplicate primary name '" + primary + "'"
        );
    }
    if (alias_map_.count(primary) > 0) {
        return batbox::Err(
            "register_command: name '" + primary +
            "' is already registered as an alias for '" +
            alias_map_.at(primary) + "'"
        );
    }

    // Check alias collisions.
    for (const auto& alias_raw : cmd->aliases()) {
        const std::string alias = to_lower(alias_raw);
        if (alias.empty()) {
            return batbox::Err(
                std::string("register_command: command '") + primary +
                "' has an empty alias"
            );
        }
        if (commands_.count(alias) > 0) {
            return batbox::Err(
                "register_command: alias '" + alias +
                "' for command '" + primary +
                "' conflicts with primary name of another command"
            );
        }
        if (alias_map_.count(alias) > 0) {
            return batbox::Err(
                "register_command: alias '" + alias +
                "' for command '" + primary +
                "' is already used by command '" +
                alias_map_.at(alias) + "'"
            );
        }
    }

    // All checks passed — insert.
    for (const auto& alias_raw : cmd->aliases()) {
        const std::string alias = to_lower(alias_raw);
        alias_map_.emplace(alias, primary);
    }
    commands_.emplace(primary, std::move(cmd));
    return {};  // Result<void> default-constructs to ok.
}

// ---------------------------------------------------------------------------
// lookup
// ---------------------------------------------------------------------------

ISlashCommand* SlashCommandRegistry::lookup(std::string_view name_or_alias) const noexcept {
    const std::string key = to_lower(name_or_alias);

    // Check primary names first.
    {
        auto it = commands_.find(key);
        if (it != commands_.end()) {
            return it->second.get();
        }
    }

    // Check alias map.
    {
        auto alias_it = alias_map_.find(key);
        if (alias_it != alias_map_.end()) {
            auto cmd_it = commands_.find(alias_it->second);
            if (cmd_it != commands_.end()) {
                return cmd_it->second.get();
            }
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// fuzzy_find
// ---------------------------------------------------------------------------

std::vector<ISlashCommand*> SlashCommandRegistry::fuzzy_find(
    std::string_view query,
    std::size_t      k) const
{
    const std::string lq = to_lower(query);

    // Build (score, primary_name, ptr) triples.
    struct Candidate {
        int          score;
        std::string  primary_name;
        ISlashCommand* ptr;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(commands_.size());

    for (const auto& [pname, sp] : commands_) {
        const int s = score_command(*sp, lq);
        if (s > 0) {
            candidates.push_back({s, pname, sp.get()});
        }
    }

    // Sort: highest score first; tie-break by primary name ascending.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.primary_name < b.primary_name;
              });

    // Extract pointers, capped at k.
    const std::size_t out_size = std::min(k, candidates.size());
    std::vector<ISlashCommand*> result;
    result.reserve(out_size);
    for (std::size_t i = 0; i < out_size; ++i) {
        result.push_back(candidates[i].ptr);
    }
    return result;
}

// ---------------------------------------------------------------------------
// all
// ---------------------------------------------------------------------------

std::vector<ISlashCommand*> SlashCommandRegistry::all() const {
    // Collect all (primary_name, ptr) pairs and sort by name.
    std::vector<std::pair<std::string, ISlashCommand*>> entries;
    entries.reserve(commands_.size());
    for (const auto& [name, sp] : commands_) {
        entries.emplace_back(name, sp.get());
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<ISlashCommand*> result;
    result.reserve(entries.size());
    for (auto& [name, ptr] : entries) {
        result.push_back(ptr);
    }
    return result;
}

// ---------------------------------------------------------------------------
// names
// ---------------------------------------------------------------------------

std::vector<std::string> SlashCommandRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(commands_.size());
    for (const auto& [name, _] : commands_) {
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace batbox::commands
