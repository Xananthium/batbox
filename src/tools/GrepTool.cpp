// src/tools/GrepTool.cpp
//
// Implementation of batbox::tools::GrepTool.
//
// Strategy:
//   1. find_rg()      — locate the `rg` binary on PATH once per process via a
//                       std::call_once guard.
//   2. run_with_rg()  — build argv, popen, capture output.
//   3. run_fallback() — std::filesystem recursive walk + std::regex.
//
// run() dispatches to (2) when rg is found, otherwise (3).

#include <batbox/tools/GrepTool.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  include <cstdlib>
#  define POPEN  _popen
#  define PCLOSE _pclose
#else
#  include <cstdlib>
#  define POPEN  popen
#  define PCLOSE pclose
#endif

namespace batbox::tools {

namespace {

// =============================================================================
// rg binary detection — done once per process
// =============================================================================

struct RgPath {
    std::string path; // empty string means "not found"
    bool        found = false;
};

static RgPath   s_rg;
static std::once_flag s_rg_once;

/// Search PATH for the `rg` binary.  Sets s_rg once.
static void locate_rg() {
    // Quick: try a known location first, then fall back to PATH scan.
    const char* path_env = ::getenv("PATH");
    if (!path_env) return;

#ifdef _WIN32
    const char sep = ';';
    const char* exe_suffix = ".exe";
#else
    const char sep = ':';
    const char* exe_suffix = "";
#endif

    std::string_view path_sv(path_env);
    while (!path_sv.empty()) {
        auto colon = path_sv.find(sep);
        std::string_view dir = (colon == std::string_view::npos)
                                   ? path_sv
                                   : path_sv.substr(0, colon);
        path_sv = (colon == std::string_view::npos)
                      ? std::string_view{}
                      : path_sv.substr(colon + 1);

        if (dir.empty()) continue;

        std::filesystem::path candidate =
            std::filesystem::path(dir) / ("rg" + std::string(exe_suffix));
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            s_rg.path  = candidate.string();
            s_rg.found = true;
            return;
        }
    }
}

[[nodiscard]] static bool rg_available() {
    std::call_once(s_rg_once, locate_rg);
    return s_rg.found;
}

// =============================================================================
// Shell-quoting helper for building rg command line
// =============================================================================

/// Wrap a string in single-quotes, escaping any embedded single-quotes.
/// Safe for POSIX shells.
[[nodiscard]] static std::string shell_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out += '\'';
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += '\'';
    return out;
}

// =============================================================================
// output_mode enum
// =============================================================================

enum class OutputMode { Text, FilesWithMatches, Count };

[[nodiscard]] static OutputMode parse_output_mode(const std::string& s) {
    if (s == "files_with_matches") return OutputMode::FilesWithMatches;
    if (s == "count")              return OutputMode::Count;
    return OutputMode::Text;
}

// =============================================================================
// fnmatch-style glob for the fallback walker
// =============================================================================

/// Very small fnmatch — only * and ? are supported (no ** needed for filename
/// matching).  Matching is against the filename component only.
[[nodiscard]] static bool glob_match_filename(std::string_view pattern,
                                               std::string_view text) {
    if (pattern.empty()) return text.empty();
    if (pattern[0] == '*') {
        // consume zero or more chars
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (glob_match_filename(pattern.substr(1), text.substr(i))) return true;
        }
        return false;
    }
    if (text.empty()) return false;
    if (pattern[0] == '?' || pattern[0] == text[0]) {
        return glob_match_filename(pattern.substr(1), text.substr(1));
    }
    return false;
}

// =============================================================================
// apply_head_limit
// =============================================================================

/// Trim body to at most head_limit lines.
[[nodiscard]] static std::string apply_head_limit(const std::string& body,
                                                    int head_limit) {
    if (head_limit <= 0) return body;
    std::istringstream ss(body);
    std::string line;
    std::string out;
    int count = 0;
    while (std::getline(ss, line)) {
        if (count >= head_limit) {
            out += "[output truncated at " + std::to_string(head_limit)
                 + " lines]\n";
            break;
        }
        out += line + '\n';
        ++count;
    }
    return out;
}

// =============================================================================
// run_with_rg — ripgrep back-end
// =============================================================================

[[nodiscard]] static ToolResult run_with_rg(
    const std::string&  pattern,
    const std::string&  search_path,
    const std::string&  glob,
    const std::string&  type_filter,
    OutputMode          output_mode,
    bool                case_insensitive,
    bool                line_numbers,
    int                 context_before,
    int                 context_after,
    bool                multiline,
    int                 head_limit)
{
    // -----------------------------------------------------------------
    // Build command string
    // -----------------------------------------------------------------
    std::string cmd;
    cmd.reserve(256);
    cmd += shell_quote(s_rg.path);
    cmd += " --no-heading";

    // Always include filename in output so callers can identify the file.
    cmd += " --with-filename";

    // output mode flags
    switch (output_mode) {
        case OutputMode::FilesWithMatches:
            cmd += " -l";
            break;
        case OutputMode::Count:
            cmd += " --count";
            break;
        case OutputMode::Text:
            if (line_numbers) cmd += " -n";
            break;
    }

    if (case_insensitive) cmd += " -i";
    if (multiline)        cmd += " -U";

    if (context_before > 0 && output_mode == OutputMode::Text) {
        cmd += " -B " + std::to_string(context_before);
    }
    if (context_after > 0 && output_mode == OutputMode::Text) {
        cmd += " -A " + std::to_string(context_after);
    }

    if (!glob.empty()) {
        cmd += " -g " + shell_quote(glob);
    }
    if (!type_filter.empty()) {
        cmd += " -t " + shell_quote(type_filter);
    }

    // colour always off (we format for the model)
    cmd += " --color never";

    // pattern and path
    cmd += " -- " + shell_quote(pattern);
    cmd += " "    + shell_quote(search_path);

    // redirect stderr to /dev/null — we surface the empty result body as a
    // "no matches" rather than an error when rg exits 1 (no matches).
    cmd += " 2>/dev/null";

    // -----------------------------------------------------------------
    // Run
    // -----------------------------------------------------------------
    FILE* fp = POPEN(cmd.c_str(), "r");
    if (!fp) {
        return ToolResult::error("GrepTool: failed to launch rg: " + cmd);
    }

    std::string output;
    output.reserve(4096);
    std::array<char, 4096> buf{};
    while (!std::feof(fp)) {
        if (std::fgets(buf.data(), static_cast<int>(buf.size()), fp)) {
            output += buf.data();
        }
    }
    int rc = PCLOSE(fp);
    (void)rc; // 1 = no matches (normal), 2 = error

    if (output.empty()) {
        return ToolResult::ok("No matches found.");
    }

    output = apply_head_limit(output, head_limit);
    return ToolResult::ok(std::move(output));
}

// =============================================================================
// run_fallback — pure C++ back-end
// =============================================================================

[[nodiscard]] static ToolResult run_fallback(
    const std::string&  pattern,
    const std::string&  search_path,
    const std::string&  glob,
    const std::string&  type_filter,
    OutputMode          output_mode,
    bool                case_insensitive,
    bool                line_numbers,
    int                 /*context_before — not supported, noted below*/,
    int                 /*context_after  — not supported, noted below*/,
    bool                multiline,
    int                 head_limit,
    bool                had_context_flags,
    bool                had_type_flag)
{
    namespace fs = std::filesystem;

    // -----------------------------------------------------------------
    // Compile regex
    // -----------------------------------------------------------------
    auto flags = std::regex_constants::ECMAScript;
    if (case_insensitive) flags |= std::regex_constants::icase;
    if (multiline)        flags |= std::regex_constants::multiline;

    std::regex re;
    try {
        re = std::regex(pattern, flags);
    } catch (const std::regex_error& e) {
        return ToolResult::error(
            std::string("GrepTool: invalid regex: ") + e.what());
    }

    // -----------------------------------------------------------------
    // Enumerate files
    // -----------------------------------------------------------------
    std::error_code ec;
    fs::path root(search_path);

    // Collect files to search
    std::vector<fs::path> files;
    if (fs::is_regular_file(root, ec)) {
        files.push_back(root);
    } else if (fs::is_directory(root, ec)) {
        for (const auto& entry :
                 fs::recursive_directory_iterator(
                     root,
                     fs::directory_options::skip_permission_denied,
                     ec))
        {
            if (ec) { ec.clear(); continue; }
            if (!entry.is_regular_file()) continue;

            // glob filter on filename
            if (!glob.empty()) {
                if (!glob_match_filename(glob, entry.path().filename().string())) {
                    continue;
                }
            }

            files.push_back(entry.path());
        }
    } else {
        return ToolResult::error(
            "GrepTool: path does not exist or is not accessible: " + search_path);
    }

    // Sort for deterministic output
    std::sort(files.begin(), files.end());

    // -----------------------------------------------------------------
    // Search each file
    // -----------------------------------------------------------------
    // For files_with_matches we collect unique paths.
    // For count we collect file → count.
    // For text we accumulate lines.

    std::string          text_out;
    std::vector<std::string> fwm_out;
    std::map<std::string, int> count_out;

    for (const auto& fp : files) {
        std::ifstream ifs(fp);
        if (!ifs.is_open()) continue;

        int         line_no  = 0;
        bool        matched  = false;
        std::string line;

        while (std::getline(ifs, line)) {
            ++line_no;
            if (!std::regex_search(line, re)) continue;

            matched = true;
            if (output_mode == OutputMode::Text) {
                if (line_numbers) {
                    text_out += fp.string() + ':' + std::to_string(line_no)
                              + ':' + line + '\n';
                } else {
                    text_out += fp.string() + ':' + line + '\n';
                }
            } else if (output_mode == OutputMode::Count) {
                count_out[fp.string()]++;
            }
        }

        if (matched && output_mode == OutputMode::FilesWithMatches) {
            fwm_out.push_back(fp.string());
        }
    }

    // -----------------------------------------------------------------
    // Build output
    // -----------------------------------------------------------------
    std::string body;

    switch (output_mode) {
        case OutputMode::Text:
            body = text_out.empty() ? "No matches found." : text_out;
            break;
        case OutputMode::FilesWithMatches:
            if (fwm_out.empty()) {
                body = "No matches found.";
            } else {
                for (const auto& f : fwm_out) {
                    body += f + '\n';
                }
            }
            break;
        case OutputMode::Count:
            if (count_out.empty()) {
                body = "No matches found.";
            } else {
                for (const auto& [f, n] : count_out) {
                    body += std::to_string(n) + "  " + f + '\n';
                }
            }
            break;
    }

    // -----------------------------------------------------------------
    // Append limitation notes when caller used unsupported flags
    // -----------------------------------------------------------------
    std::string notes;
    if (had_context_flags) {
        notes += "NOTE: context flags (-A/-B/-C) are not supported by the "
                 "pure-C++ fallback; context lines were omitted.\n";
    }
    if (had_type_flag) {
        notes += "NOTE: `type` filtering is not supported by the pure-C++ "
                 "fallback; all file types were searched.\n";
    }
    if (!notes.empty()) {
        body = notes + body;
    }

    body = apply_head_limit(body, head_limit);
    return ToolResult::ok(std::move(body));
}

} // anonymous namespace

// =============================================================================
// GrepTool — ITool implementation
// =============================================================================

std::string_view GrepTool::name() const {
    return "Grep";
}

std::string_view GrepTool::description() const {
    return "Search files for a regex pattern. "
           "Uses ripgrep when available; falls back to C++ std::regex walker. "
           "Supports output_mode (text/files_with_matches/count), "
           "glob/type filtering, case_insensitive, line_numbers, "
           "context_before/context_after, multiline, and head_limit.";
}

Json GrepTool::schema_json() const {
    return Json{
        {"name",        "Grep"},
        {"description", description()},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"pattern", Json{
                    {"type",        "string"},
                    {"description", "ECMAScript/Rust regex pattern to search for."}
                }},
                {"path", Json{
                    {"type",        "string"},
                    {"description", "Directory or file to search. Defaults to the working directory."}
                }},
                {"glob", Json{
                    {"type",        "string"},
                    {"description", "File-name glob filter, e.g. '*.cpp'. Applied to filename component."}
                }},
                {"type", Json{
                    {"type",        "string"},
                    {"description", "ripgrep file-type alias, e.g. 'cpp'. Ignored by fallback back-end."}
                }},
                {"output_mode", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"text", "files_with_matches", "count"})},
                    {"description", "'text' (default): file:line:content; 'files_with_matches': deduplicated paths; 'count': count-per-file."}
                }},
                {"case_insensitive", Json{
                    {"type",        "boolean"},
                    {"description", "Case-insensitive matching (-i). Default false."}
                }},
                {"line_numbers", Json{
                    {"type",        "boolean"},
                    {"description", "Include line numbers in text-mode output. Default true."}
                }},
                {"context_before", Json{
                    {"type",        "integer"},
                    {"description", "Lines of context before each match (-B). rg only."}
                }},
                {"context_after", Json{
                    {"type",        "integer"},
                    {"description", "Lines of context after each match (-A). rg only."}
                }},
                {"context", Json{
                    {"type",        "integer"},
                    {"description", "Symmetric context lines (-C). rg only. Overridden by context_before/context_after."}
                }},
                {"head_limit", Json{
                    {"type",        "integer"},
                    {"description", "Cap on output lines. 0 or absent = unlimited."}
                }},
                {"multiline", Json{
                    {"type",        "boolean"},
                    {"description", "Enable multiline matching (-U for rg; std::regex::multiline for fallback). Default false."}
                }}
            }},
            {"required", Json::array({"pattern"})}
        }}
    };
}

ToolResult GrepTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // Extract and validate arguments
        // ------------------------------------------------------------------
        if (!args.contains("pattern") || !args["pattern"].is_string()) {
            return ToolResult::error(
                "GrepTool: required argument 'pattern' is missing or not a string.");
        }
        const std::string pattern = args["pattern"].get<std::string>();
        if (pattern.empty()) {
            return ToolResult::error("GrepTool: 'pattern' must not be empty.");
        }

        // Search path — default to ctx.cwd
        std::string search_path;
        if (args.contains("path") && args["path"].is_string()) {
            search_path = args["path"].get<std::string>();
        }
        if (search_path.empty()) {
            search_path = ctx.cwd.string();
        }
        // Resolve relative paths against cwd
        {
            namespace fs = std::filesystem;
            fs::path p(search_path);
            if (p.is_relative()) {
                p = ctx.cwd / p;
            }
            search_path = p.string();
        }

        const std::string glob = (args.contains("glob") && args["glob"].is_string())
                                      ? args["glob"].get<std::string>()
                                      : "";
        const std::string type_filter =
            (args.contains("type") && args["type"].is_string())
                ? args["type"].get<std::string>()
                : "";

        const OutputMode output_mode = (args.contains("output_mode") && args["output_mode"].is_string())
            ? parse_output_mode(args["output_mode"].get<std::string>())
            : OutputMode::Text;

        const bool case_insensitive =
            (args.contains("case_insensitive") && args["case_insensitive"].is_boolean())
                ? args["case_insensitive"].get<bool>()
                : false;

        const bool line_numbers =
            (args.contains("line_numbers") && args["line_numbers"].is_boolean())
                ? args["line_numbers"].get<bool>()
                : true;

        // Context flags: explicit context_before/after override symmetric context
        int ctx_before = 0;
        int ctx_after  = 0;
        bool had_context_flags = false;

        if (args.contains("context") && args["context"].is_number_integer()) {
            int c = args["context"].get<int>();
            ctx_before = ctx_after = std::max(0, c);
            if (c > 0) had_context_flags = true;
        }
        if (args.contains("context_before") && args["context_before"].is_number_integer()) {
            ctx_before = std::max(0, args["context_before"].get<int>());
            if (ctx_before > 0) had_context_flags = true;
        }
        if (args.contains("context_after") && args["context_after"].is_number_integer()) {
            ctx_after = std::max(0, args["context_after"].get<int>());
            if (ctx_after > 0) had_context_flags = true;
        }

        const int head_limit =
            (args.contains("head_limit") && args["head_limit"].is_number_integer())
                ? std::max(0, args["head_limit"].get<int>())
                : 0;

        const bool multiline =
            (args.contains("multiline") && args["multiline"].is_boolean())
                ? args["multiline"].get<bool>()
                : false;

        // ------------------------------------------------------------------
        // Dispatch
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        if (rg_available()) {
            return run_with_rg(pattern, search_path, glob, type_filter,
                               output_mode, case_insensitive, line_numbers,
                               ctx_before, ctx_after, multiline, head_limit);
        }

        const bool had_type_flag = !type_filter.empty();
        return run_fallback(pattern, search_path, glob, type_filter,
                            output_mode, case_insensitive, line_numbers,
                            ctx_before, ctx_after, multiline, head_limit,
                            had_context_flags, had_type_flag);

    } catch (const std::exception& e) {
        return ToolResult::error(std::string("GrepTool: unexpected error: ") + e.what());
    } catch (...) {
        return ToolResult::error("GrepTool: unknown error");
    }
}

} // namespace batbox::tools
