// src/migrate.cpp
// =============================================================================
// batbox::cmd::run_migrate — `batbox migrate` subcommand.
//
// Copies known claude-code config artefacts from ~/.claude/ → ~/.batbox/
// according to the path-mapping table in pmdraft.md §"Path Mapping".
//
// Behaviour summary:
//   1. Resolve source root (--from-dir or ~/.claude) and dest root
//      (--to-dir or ~/.batbox).
//   2. Build the migration plan: a list of (src_path, dst_path, is_dir) tuples.
//   3. For each entry that exists on disk, decide whether to copy:
//        - dest does not exist          → copy (always)
//        - dest exists, --force absent  → skip with warning
//        - dest exists, --force present → overwrite
//   4. In dry-run mode (default, no --apply), print the plan and exit 0.
//   5. In --apply mode, execute the copies:
//        - Regular files → std::filesystem::copy_file()
//        - Directories   → std::filesystem::copy(recursive)
//   6. Print a summary: N copied, M skipped.
//   7. Return 0 if every attempted copy succeeded, 1 if any failed.
//
// Source files/dirs are never modified or deleted.
// =============================================================================

#include "migrate.hpp"

#include <batbox/core/Logging.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace batbox::cmd {

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// MigrationEntry — one (source, destination) pair in the migration plan.
// ---------------------------------------------------------------------------
struct MigrationEntry {
    fs::path    src;          ///< absolute source path
    fs::path    dst;          ///< absolute destination path
    bool        is_dir;       ///< true for directory entries (recursive copy)
    std::string display_name; ///< human-readable label for output
};

// ---------------------------------------------------------------------------
// resolve_root — resolve a path string to an absolute filesystem path.
// Expands leading "~/" via $HOME.
// ---------------------------------------------------------------------------
static fs::path resolve_root(const std::string& given, const char* default_name) {
    if (!given.empty()) {
        std::string expanded = given;
        if (expanded.size() >= 2 && expanded[0] == '~' && expanded[1] == '/') {
            const char* home = std::getenv("HOME");
            if (home && *home != '\0') {
                expanded = std::string(home) + expanded.substr(1);
            }
        }
        return fs::path(expanded);
    }
    const char* home = std::getenv("HOME");
    if (!home || *home == '\0') {
        throw std::runtime_error("cannot resolve home directory ($HOME not set)");
    }
    return fs::path(home) / default_name;
}

// ---------------------------------------------------------------------------
// build_migration_plan — construct the list of (src, dst) entries.
//
// The mapping follows pmdraft.md §"Path Mapping":
//
//   ~/.claude/settings.json      → ~/.batbox/settings.json
//   ~/.claude/CLAUDE.md          → ~/.batbox/BATBOX.md        (renamed)
//   ~/.claude/keybindings.json   → ~/.batbox/keybindings.json
//   ~/.claude/sessions/          → ~/.batbox/sessions/
//   ~/.claude/plugins/           → ~/.batbox/plugins/
//   ~/.claude/skills/            → ~/.batbox/skills/
//   ~/.claude/agents/            → ~/.batbox/agents/
//   ~/.claude/mcp.json           → ~/.batbox/mcp.json
//   ~/.claude/history            → ~/.batbox/history
//   ~/.claude/.env               → ~/.batbox/.env
// ---------------------------------------------------------------------------
static std::vector<MigrationEntry> build_migration_plan(const fs::path& src_root,
                                                          const fs::path& dst_root) {
    std::vector<MigrationEntry> plan;
    plan.reserve(10);

    // Helper lambda to push a file entry.
    auto add_file = [&](const char* src_name,
                        const char* dst_name,
                        const char* label) {
        plan.push_back({
            src_root / src_name,
            dst_root / dst_name,
            false,
            std::string(label)
        });
    };

    // Helper lambda to push a directory entry.
    auto add_dir = [&](const char* src_name,
                       const char* dst_name,
                       const char* label) {
        plan.push_back({
            src_root / src_name,
            dst_root / dst_name,
            true,
            std::string(label)
        });
    };

    // Plain file mappings.
    add_file("settings.json",    "settings.json",    "settings.json");
    add_file("CLAUDE.md",        "BATBOX.md",         "CLAUDE.md → BATBOX.md");
    add_file("keybindings.json", "keybindings.json",  "keybindings.json");
    add_file("mcp.json",         "mcp.json",           "mcp.json");
    add_file("history",          "history",            "history");
    add_file(".env",             ".env",               ".env");

    // Directory mappings (recursive copy).
    add_dir("sessions",  "sessions",  "sessions/");
    add_dir("plugins",   "plugins",   "plugins/");
    add_dir("skills",    "skills",    "skills/");
    add_dir("agents",    "agents",    "agents/");

    return plan;
}

// ---------------------------------------------------------------------------
// copy_entry — copy a single MigrationEntry to disk.
//
// Returns true on success, false on failure (error already printed to stderr).
// ---------------------------------------------------------------------------
static bool copy_entry(const MigrationEntry& entry, bool force) {
    std::error_code ec;

    if (entry.is_dir) {
        // Directory: recursive copy.
        // copy_options::overwrite_existing + copy_options::recursive.
        auto opts = fs::copy_options::recursive;
        if (force) {
            opts |= fs::copy_options::overwrite_existing;
        } else {
            opts |= fs::copy_options::skip_existing;
        }
        fs::copy(entry.src, entry.dst, opts, ec);
        if (ec) {
            std::cerr << "  error: failed to copy directory "
                      << entry.src.string() << " → " << entry.dst.string()
                      << ": " << ec.message() << '\n';
            return false;
        }
    } else {
        // Regular file: ensure parent exists.
        fs::create_directories(entry.dst.parent_path(), ec);
        if (ec) {
            std::cerr << "  error: cannot create parent dir for "
                      << entry.dst.string() << ": " << ec.message() << '\n';
            return false;
        }

        auto opts = force
            ? fs::copy_options::overwrite_existing
            : fs::copy_options::none; // will fail if dst exists

        fs::copy_file(entry.src, entry.dst, opts, ec);
        if (ec) {
            std::cerr << "  error: failed to copy "
                      << entry.src.string() << " → " << entry.dst.string()
                      << ": " << ec.message() << '\n';
            return false;
        }
    }

    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// run_migrate
// ---------------------------------------------------------------------------
int run_migrate(const MigrateArgs& args) {
    BATBOX_LOG_INFO("migrate: starting (apply={}, force={})",
                    args.apply, args.force);

    // -- Resolve source and destination roots. --------------------------------
    fs::path src_root, dst_root;
    try {
        src_root = resolve_root(args.from_dir, ".claude");
        dst_root = resolve_root(args.to_dir,   ".batbox");
    } catch (const std::exception& ex) {
        std::cerr << "batbox migrate: " << ex.what() << '\n';
        return 1;
    }

    std::cout << "batbox migrate\n"
              << "  from : " << src_root.string() << '\n'
              << "  to   : " << dst_root.string()  << '\n';

    if (!args.apply) {
        std::cout << "  mode : dry-run  (pass --apply to execute)\n";
    } else {
        std::cout << "  mode : apply\n";
        if (args.force) {
            std::cout << "  force: yes (existing destinations will be overwritten)\n";
        }
    }
    std::cout << '\n';

    // -- Verify source root exists. -------------------------------------------
    std::error_code ec;
    if (!fs::exists(src_root, ec) || ec) {
        std::cout << "  source directory not found: " << src_root.string() << '\n';
        std::cout << "  nothing to migrate.\n";
        BATBOX_LOG_INFO("migrate: source not found, exiting cleanly");
        return 0;
    }

    // -- Build plan. ----------------------------------------------------------
    const auto plan = build_migration_plan(src_root, dst_root);

    // -- Evaluate plan: categorise each entry as copy / skip / absent. --------
    enum class Action { Copy, SkipExists, SkipAbsent };
    struct PlannedAction {
        const MigrationEntry* entry;
        Action                action;
    };

    std::vector<PlannedAction> actions;
    actions.reserve(plan.size());

    for (const auto& entry : plan) {
        if (!fs::exists(entry.src, ec) || ec) {
            actions.push_back({&entry, Action::SkipAbsent});
            continue;
        }
        bool dst_exists = fs::exists(entry.dst, ec);
        if (dst_exists && !args.force) {
            actions.push_back({&entry, Action::SkipExists});
        } else {
            actions.push_back({&entry, Action::Copy});
        }
    }

    // -- Print the plan. -------------------------------------------------------
    int n_to_copy   = 0;
    int n_skipped   = 0;
    int n_absent    = 0;

    for (const auto& pa : actions) {
        switch (pa.action) {
            case Action::Copy:
                std::cout << "  [copy]   " << pa.entry->display_name
                          << "\n           " << pa.entry->src.string()
                          << "\n        -> " << pa.entry->dst.string() << '\n';
                ++n_to_copy;
                break;
            case Action::SkipExists:
                std::cout << "  [skip]   " << pa.entry->display_name
                          << "  (destination exists; use --force to overwrite)\n";
                ++n_skipped;
                break;
            case Action::SkipAbsent:
                std::cout << "  [absent] " << pa.entry->display_name
                          << "  (not present in source — nothing to copy)\n";
                ++n_absent;
                break;
        }
    }

    std::cout << '\n'
              << "  plan: " << n_to_copy << " to copy, "
              << n_skipped << " skipped (exist), "
              << n_absent  << " absent in source\n";

    // -- If dry-run, stop here. -----------------------------------------------
    if (!args.apply) {
        std::cout << "\n  dry-run complete — no files written.\n"
                  << "  re-run with --apply to execute.\n";
        BATBOX_LOG_INFO("migrate: dry-run complete ({} would copy, {} skip, {} absent)",
                        n_to_copy, n_skipped, n_absent);
        return 0;
    }

    if (n_to_copy == 0) {
        std::cout << "\n  nothing to do.\n";
        BATBOX_LOG_INFO("migrate: nothing to copy");
        return 0;
    }

    // -- Create destination root if needed. -----------------------------------
    fs::create_directories(dst_root, ec);
    if (ec) {
        std::cerr << "migrate: cannot create destination directory "
                  << dst_root.string() << ": " << ec.message() << '\n';
        return 1;
    }

    // -- Execute copies. ------------------------------------------------------
    std::cout << "\n  copying ...\n";

    int n_copied  = 0;
    int n_failed  = 0;

    for (const auto& pa : actions) {
        if (pa.action != Action::Copy) {
            continue;
        }
        bool ok = copy_entry(*pa.entry, args.force);
        if (ok) {
            std::cout << "  ok  " << pa.entry->display_name << '\n';
            ++n_copied;
            BATBOX_LOG_DEBUG("migrate: copied {}", pa.entry->display_name);
        } else {
            ++n_failed;
            BATBOX_LOG_ERROR("migrate: failed to copy {}", pa.entry->display_name);
        }
    }

    // -- Summary. -------------------------------------------------------------
    std::cout << '\n'
              << "batbox migrate: done\n"
              << "  copied  : " << n_copied  << '\n'
              << "  skipped : " << n_skipped << '\n'
              << "  failed  : " << n_failed  << '\n';

    if (n_failed > 0) {
        std::cerr << "migrate: " << n_failed
                  << " error(s) occurred — review output above.\n";
        BATBOX_LOG_ERROR("migrate: completed with {} failure(s)", n_failed);
        return 1;
    }

    BATBOX_LOG_INFO("migrate: completed successfully ({} copied)", n_copied);
    return 0;
}

// ---------------------------------------------------------------------------
// migrate — zero-argument trampoline (dry-run defaults)
// ---------------------------------------------------------------------------
int migrate() {
    return run_migrate(MigrateArgs{});
}

} // namespace batbox::cmd
