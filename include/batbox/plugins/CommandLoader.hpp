// include/batbox/plugins/CommandLoader.hpp
// =============================================================================
// batbox::plugins::CommandLoader — scans user command directories and
// plugin-bundled commands/ directories, parsing each .md file into a
// UserSlashCommand (implements ISlashCommand) and registering it in a
// SlashCommandRegistry.
//
// Directory layout
// ----------------
// Global user dirs (load_user_commands):
//   ~/.claude/commands/*.md       (claude-code compat)
//   ~/.batbox/commands/*.md       (batbox-native)
//
// Plugin-bundled dir (load_from_dir):
//   <plugin-root>/commands/*.md   (called by PluginLoader in CPP 11.7)
//
// .md file format
// ---------------
//   ---
//   name: my-command          # required; no leading slash; falls back to stem
//   description: One-liner    # optional; shown in /help + palette
//   ---
//   Body text with optional template variables:
//     $ARGS  — the full argument string supplied by the user
//     $1     — first whitespace-separated token from args
//     $2     — second whitespace-separated token from args
//
// Collision policy
// ----------------
// If the primary name already exists in the registry (i.e. a built-in or
// earlier plugin command owns that name), the new command is silently dropped
// and a WARN-level log line is emitted.  Built-ins always win.
//
// Error handling
// --------------
// Malformed frontmatter, unreadable files, and missing 'name' fields are all
// logged at WARN level and skipped — CommandLoader never throws.
//
// Thread safety
// -------------
// CommandLoader is stateless; all methods are const.  Registration itself is
// not thread-safe (see SlashCommandRegistry docs) and must complete before
// any concurrent reads of the registry.
// =============================================================================

#pragma once

#include <batbox/commands/SlashCommandRegistry.hpp>

#include <filesystem>

namespace batbox::plugins {

// ---------------------------------------------------------------------------
// CommandLoader
// ---------------------------------------------------------------------------

class CommandLoader {
public:
    CommandLoader()  = default;
    ~CommandLoader() = default;

    // Non-copyable, movable (stateless, but follow rule-of-five for clarity).
    CommandLoader(const CommandLoader&)            = delete;
    CommandLoader& operator=(const CommandLoader&) = delete;
    CommandLoader(CommandLoader&&)                 = default;
    CommandLoader& operator=(CommandLoader&&)      = default;

    // ---- Public API ---------------------------------------------------------

    /// Scan the two global user command directories:
    ///   ~/.claude/commands/
    ///   ~/.batbox/commands/
    ///
    /// Each .md file found is parsed and (if no name collision) registered
    /// in `registry`.  Missing directories are silently ignored.
    void load_user_commands(batbox::commands::SlashCommandRegistry& registry) const;

    /// Scan a single `commands_dir` directory and register all valid .md
    /// files as user-defined slash commands.
    ///
    /// Called by PluginLoader (CPP 11.7) for each plugin's commands/ dir.
    /// If `commands_dir` does not exist or is not a directory, returns without
    /// error.
    void load_from_dir(const std::filesystem::path&             commands_dir,
                       batbox::commands::SlashCommandRegistry&  registry) const;
};

} // namespace batbox::plugins
