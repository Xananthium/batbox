// src/tools/GlobTool.cpp
// ---------------------------------------------------------------------------
// Implementation of GlobTool — filesystem glob using
// std::filesystem::recursive_directory_iterator + batbox::permissions::glob_match.
//
// See include/batbox/tools/GlobTool.hpp for the full contract.
// ---------------------------------------------------------------------------

#include <batbox/tools/GlobTool.hpp>
#include <batbox/permissions/PatternMatcher.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// How many directory entries to process between cancellation polls.
constexpr std::size_t kCancelCheckInterval = 64;

// ---------------------------------------------------------------------------
// path_to_forward_slash
//
// Convert a native filesystem path to a forward-slash string so the glob
// pattern written by the user (which always uses '/') matches on all
// platforms (including Windows where native separators are '\\').
// ---------------------------------------------------------------------------
std::string path_to_forward_slash(const fs::path& p) {
    std::string s = p.generic_string();  // generic_string() always uses '/'
    return s;
}

// ---------------------------------------------------------------------------
// collect_matches
//
// Walk `base` recursively, test every regular file's path (relative to base,
// forward-slash) against `pattern`, and collect the results together with
// their last-write-time for later sorting.
// ---------------------------------------------------------------------------
struct MatchEntry {
    fs::path         abs_path;
    fs::file_time_type mtime;
};

std::vector<MatchEntry> collect_matches(const fs::path&  base,
                                         const std::string& pattern,
                                         ToolContext&      ctx) {
    std::vector<MatchEntry> results;

    fs::recursive_directory_iterator it(
        base,
        fs::directory_options::skip_permission_denied
    );
    const fs::recursive_directory_iterator end{};

    std::size_t count = 0;
    for (; it != end; ++it) {
        // Cancellation check every kCancelCheckInterval entries.
        if ((++count % kCancelCheckInterval) == 0 && ctx.is_cancelled()) {
            break;
        }

        // Only match regular files (symlinks to files are included via
        // follow_directory_symlink; we skip directories and other specials).
        std::error_code ec;
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            continue;
        }

        // Build the relative path from base (forward-slash normalised).
        const fs::path rel = entry.path().lexically_relative(base);
        const std::string rel_str = path_to_forward_slash(rel);

        if (!permissions::glob_match(pattern, rel_str)) {
            continue;
        }

        // Retrieve mtime; on error use epoch so the file still appears but
        // sorts last.
        auto mtime = entry.last_write_time(ec);
        if (ec) {
            mtime = fs::file_time_type{};
        }

        results.push_back({entry.path(), mtime});
    }

    return results;
}

} // anonymous namespace

// ===========================================================================
// GlobTool — ITool implementation
// ===========================================================================

std::string_view GlobTool::name() const {
    return "Glob";
}

std::string_view GlobTool::description() const {
    return "Find files matching a glob pattern (supports *, **, ?, [abc]),"
           " sorted by modification time descending.";
}

Json GlobTool::schema_json() const {
    return Json{
        {"name",        "Glob"},
        {"description", "Find files matching a glob pattern (supports *, **, ?, [abc]),"
                        " sorted by modification time descending."},
        {"parameters", Json{
            {"type",     "object"},
            {"properties", Json{
                {"pattern", Json{
                    {"type",        "string"},
                    {"description", "Glob pattern to match against file paths relative"
                                   " to the base directory. Examples: \"*.cpp\","
                                   " \"src/**/*.hpp\", \"**/*_test.cpp\"."}
                }},
                {"path", Json{
                    {"type",        "string"},
                    {"description", "Base directory to search from. Defaults to the"
                                   " current working directory when absent or empty."}
                }}
            }},
            {"required", Json::array({"pattern"})}
        }}
    };
}

ToolResult GlobTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 1. Extract and validate arguments.
        // ------------------------------------------------------------------
        const auto pattern_it = args.find("pattern");
        if (pattern_it == args.end() || !pattern_it->is_string()) {
            return ToolResult::error(
                "Glob: required argument 'pattern' is missing or not a string.");
        }
        const std::string pattern = pattern_it->get<std::string>();
        if (pattern.empty()) {
            return ToolResult::error("Glob: 'pattern' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 2. Resolve the base directory.
        // ------------------------------------------------------------------
        fs::path base;
        const auto path_it = args.find("path");
        if (path_it != args.end()
                && path_it->is_string()
                && !path_it->get<std::string>().empty()) {
            base = fs::path(path_it->get<std::string>());
        } else {
            base = ctx.cwd;
        }

        // Resolve to an absolute path so symlinks and relative bases all work.
        std::error_code ec;
        base = fs::canonical(base, ec);
        if (ec) {
            return ToolResult::error(
                "Glob: cannot resolve base directory '" + base.string()
                + "': " + ec.message());
        }
        if (!fs::is_directory(base, ec) || ec) {
            return ToolResult::error(
                "Glob: base path '" + base.string() + "' is not a directory.");
        }

        // ------------------------------------------------------------------
        // 3. Walk the tree and collect matching entries.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        std::vector<MatchEntry> matches = collect_matches(base, pattern, ctx);

        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 4. Sort by mtime descending (newest first).
        // ------------------------------------------------------------------
        std::sort(matches.begin(), matches.end(),
                  [](const MatchEntry& a, const MatchEntry& b) {
                      return a.mtime > b.mtime;
                  });

        // ------------------------------------------------------------------
        // 5. Build text body and structured payload.
        // ------------------------------------------------------------------
        Json paths_json = Json::array();
        std::ostringstream body;
        for (const auto& m : matches) {
            const std::string abs_str = path_to_forward_slash(m.abs_path);
            body << abs_str << '\n';
            paths_json.push_back(abs_str);
        }

        const std::size_t count = matches.size();

        if (count == 0) {
            body << "(no matches)";
        }

        Json payload{
            {"matches", std::move(paths_json)},
            {"count",   count}
        };

        return ToolResult::ok(body.str(), std::move(payload));

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("Glob: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("Glob: unknown error.");
    }
}

} // namespace batbox::tools
