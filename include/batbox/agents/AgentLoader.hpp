// include/batbox/agents/AgentLoader.hpp
// =============================================================================
// batbox::agents::AgentLoader — scan ~/.batbox/agents/*.md, parse frontmatter
// into AgentSpec structs, and maintain an in-memory registry with reload and
// duplicate (mtime-preference) handling.
//
// Design (CPP 6.3):
//   AgentLoader owns one flat directory scan:
//     ~/.batbox/agents/
//   Every .md file found is parsed via batbox::plugins::parse_frontmatter.
//   The frontmatter must contain at least a "name" key.
//   Parsed results are stored in an unordered_map<string, AgentSpec> keyed
//   by agent name.
//
//   Duplicate handling:
//     When two .md files in the same directory produce the same "name" value,
//     the file with the later modification time (mtime) wins.  This makes
//     hand-editing and copy-install predictable: the newest file is canonical.
//
//   [[agent-name]] reference resolution and cycle detection:
//     After all files are parsed, AgentLoader builds a cross-reference graph
//     from [[name]] tokens found in prompt_body.  If a cycle is detected, an
//     error is emitted via BATBOX_LOG_ERROR and the cycle-participating agents
//     are retained but the cycle edges are not followed during any lookup.
//     Callers can query whether any cycles were found via has_cycle_error().
//
//   reload():
//     Atomically re-scans the agents directory and replaces agents_ in-place.
//     Missing directory → empty map (not an error; directory may be created later).
//
// Blueprint contract (task CPP 6.3):
//   blueprints row 16756: class batbox::agents::AgentLoader
//   blueprints row 16757: method names() → vector<string>
//   blueprints row 16755: file agents/AgentLoader.hpp
//   blueprints row 16758: file agents/AgentLoader.cpp
//
// Build standalone (from repo root):
//   ROOT=/path/to/batbox
//   c++ -std=c++20 \
//       -I $ROOT/include \
//       -I $ROOT/build/vcpkg_installed/arm64-osx/include \
//       $ROOT/tests/integration/test_agent_loader.cpp \
//       $ROOT/src/plugins/FrontmatterParser.cpp \
//       $ROOT/src/agents/AgentLoader.cpp \
//       $ROOT/src/core/Logging.cpp \
//       $ROOT/src/core/Paths.cpp \
//       $ROOT/src/core/Json.cpp \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       $ROOT/build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       -o /tmp/test_agent_loader && /tmp/test_agent_loader
// =============================================================================

#pragma once

#include <batbox/agents/AgentSpec.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::agents {

// =============================================================================
// AgentLoader
// =============================================================================

/// Scans ~/.batbox/agents/*.md, parses each file into an AgentSpec, and
/// maintains an in-memory registry with reload and mtime-based deduplication.
///
/// Typical lifecycle:
///   1. Construct an AgentLoader.
///   2. Call load() to scan the default agents directory.
///   3. Call names() for autocomplete or get(name) to retrieve a spec.
///   4. Call reload() to re-scan after the user edits agent files.
///
/// Thread safety: not thread-safe.  Callers that need concurrent access should
/// hold an external mutex around load/reload calls.  names() and get() are safe
/// to call concurrently once loading is complete (no mutation).
///
/// Blueprint contract: batbox::agents::AgentLoader (blueprints row 16756)
class AgentLoader {
public:
    AgentLoader() = default;

    // -------------------------------------------------------------------------
    // Loading API
    // -------------------------------------------------------------------------

    /// Scan the default agents directory (~/.batbox/agents/) for .md files.
    /// Each file is parsed; malformed files and those missing "name" are warned
    /// and skipped.  Duplicates by agent name are resolved via mtime preference
    /// (the file with the later mtime wins).
    ///
    /// After scanning, builds the [[name]] cross-reference graph and runs
    /// cycle detection; any cycle is logged via BATBOX_LOG_ERROR.
    ///
    /// A missing agents directory is not an error — load() returns an empty
    /// map and the loader remains functional (ready for reload()).
    ///
    /// @returns  Vector of all AgentSpec objects successfully loaded, in
    ///           alphabetical name order.
    std::vector<AgentSpec> load();

    /// Scan a specific directory for .md agent files (used in tests or for
    /// plugin-bundled agents that live outside ~/.batbox/agents/).
    ///
    /// Behaviour is identical to load() except the directory is caller-supplied.
    /// Results merge into agents_ (same mtime-preference deduplication applies).
    ///
    /// @param dir  Directory to scan; silently ignored if non-existent.
    /// @returns    Vector of AgentSpec objects loaded from dir.
    std::vector<AgentSpec> load_from(const std::filesystem::path& dir);

    /// Re-scan the default agents directory in-place (clears agents_ first).
    /// Equivalent to calling agents_.clear() then load().
    ///
    /// Blueprint contract: reload() per Deb's blueprint pseudocode.
    void reload();

    // -------------------------------------------------------------------------
    // Query API
    // -------------------------------------------------------------------------

    /// Return a sorted list of all known agent names for autocomplete.
    ///
    /// Blueprint contract: batbox::agents::AgentLoader::names (row 16757)
    [[nodiscard]] std::vector<std::string> names() const;

    /// Look up an agent by name.
    /// Returns std::nullopt if no agent with that name is loaded.
    [[nodiscard]] std::optional<AgentSpec> get(std::string_view name) const;

    /// Return the number of agents currently loaded.
    [[nodiscard]] std::size_t size() const noexcept { return agents_.size(); }

    /// True if the most recent load() / reload() detected a [[name]] cycle.
    /// Callers may warn the user; the agents themselves are still accessible.
    [[nodiscard]] bool has_cycle_error() const noexcept { return has_cycle_; }

private:
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Parse one .md file into an AgentSpec.  Returns std::nullopt on failure;
    /// the caller is responsible for logging.
    [[nodiscard]] static std::optional<AgentSpec> parse_agent_file(
        const std::filesystem::path& path);

    /// Insert or replace an entry in agents_ using mtime preference:
    /// the new spec wins only if it was loaded from a file whose mtime is >=
    /// the mtime of the previously stored spec (or if there was no previous entry).
    void upsert_by_mtime(AgentSpec spec);

    /// Build the [[agent-name]] cross-reference graph from the current agents_
    /// map and run DFS-based cycle detection.  Sets has_cycle_ accordingly.
    void build_and_check_refs();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// Master registry: agent name → AgentSpec.
    std::unordered_map<std::string, AgentSpec> agents_;

    /// Set to true when build_and_check_refs() detects a cycle.
    bool has_cycle_ = false;
};

} // namespace batbox::agents
