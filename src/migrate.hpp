// src/migrate.hpp
// =============================================================================
// batbox::cmd::migrate — entry point for the `batbox migrate` subcommand.
//
// Performs a one-time, non-destructive copy of known claude-code config files
// and directories from ~/.claude/ into ~/.batbox/.
//
// Behaviour:
//   - Dry-run by default: prints what would be copied, touches nothing.
//   - --apply  : executes the copies (creates ~/.batbox/ if needed).
//   - --force  : allows overwriting existing destination files/dirs.
//   - --from-dir <path> : override the source root (default: ~/.claude/).
//   - --to-dir <path>   : override the destination root (default: ~/.batbox/).
//
// Path mapping (from pmdraft.md, Path Mapping section):
//   settings.json, CLAUDE.md → BATBOX.md, keybindings.json,
//   sessions/, plugins/, skills/, agents/, mcp.json, history, .env
//
// Source is NEVER modified.
//
// Blueprint contract (blueprints table, task CPP A.2):
//   struct batbox::cmd::MigrateArgs   src/migrate.hpp
//   function batbox::cmd::run_migrate src/migrate.cpp
//   function batbox::cmd::migrate     src/migrate.cpp  (thin trampoline)
// =============================================================================

#pragma once

#include <filesystem>
#include <string>

namespace batbox::cmd {

// ---------------------------------------------------------------------------
// MigrateArgs — parsed flags for the migrate subcommand.
//
// Populated by main() via CLI11 callbacks and passed into run_migrate().
// ---------------------------------------------------------------------------
struct MigrateArgs {
    bool        apply{false};       ///< --apply  : perform actual file copies
    bool        force{false};       ///< --force  : overwrite existing destinations
    std::string from_dir;           ///< --from-dir <path>  (default: ~/.claude)
    std::string to_dir;             ///< --to-dir  <path>   (default: ~/.batbox)
};

// ---------------------------------------------------------------------------
// run_migrate(args)
//
// Main implementation of the migrate subcommand.
//
// Returns:
//   0 — success (or clean dry-run)
//   1 — at least one copy failed (errors printed to stderr)
// ---------------------------------------------------------------------------
int run_migrate(const MigrateArgs& args);

// ---------------------------------------------------------------------------
// migrate() — zero-argument trampoline for legacy call-sites that pass no
// args struct yet.  Constructs a default MigrateArgs (dry-run) and delegates
// to run_migrate().
// ---------------------------------------------------------------------------
int migrate();

} // namespace batbox::cmd
