// src/tools/EditTool.cpp
//
// Implementation of batbox::tools::EditTool.
//
// See include/batbox/tools/EditTool.hpp for the full public API contract.
//
// Internal helpers (file-scope only):
//   read_file_content(path)           — read entire file into std::string
//   write_file_atomic(path, content)  — temp-file + rename
//   count_occurrences(haystack, needle) — exact substring count
//   replace_all_occurrences(s, from, to, count) — in-place replace-all
//   replace_first_occurrence(s, from, to)        — in-place replace-first
//   split_lines(text)                 — split string into line vector
//   generate_unified_diff(path, old_lines, new_lines) — unified diff string
//   resolve_path(raw, cwd)            — expand ~/ and resolve relative paths

#include <batbox/tools/EditTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <cstdlib>       // std::getenv
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::tools {

namespace {

// =============================================================================
// resolve_path
// =============================================================================

/// Expand a leading "~/" to the home directory and resolve the path relative
/// to cwd when it is a relative path (no leading '/').
[[nodiscard]] std::filesystem::path
resolve_path(std::string_view raw, const std::filesystem::path& cwd)
{
    std::string s{raw};

    // Expand leading ~/
    if (s.size() >= 2 && s[0] == '~' && s[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home && *home != '\0') {
            s = std::string{home} + s.substr(1);
        }
    }

    std::filesystem::path p{s};
    if (p.is_relative()) {
        p = cwd / p;
    }
    return p.lexically_normal();
}

// =============================================================================
// read_file_content
// =============================================================================

/// Read the entire file at path into a string.
/// Throws std::runtime_error on I/O failure.
[[nodiscard]] std::string read_file_content(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot open file for reading: " + path.string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        throw std::runtime_error("I/O error reading file: " + path.string());
    }
    return ss.str();
}

// =============================================================================
// write_file_atomic
// =============================================================================

/// Write content to a uniquely named temporary file in the same parent
/// directory as dst, then rename it over dst atomically.
/// Throws std::runtime_error on I/O failure.
void write_file_atomic(const std::filesystem::path& dst,
                       const std::string& content)
{
    namespace fs = std::filesystem;

    // Build a unique temp path: <dir>/<stem>.tmp_<pid>
    fs::path dir    = dst.parent_path();
    fs::path stem   = dst.filename();
    // Use process id as a simple disambiguation suffix.
    std::string unique_suffix = ".tmp_" + std::to_string(
        static_cast<unsigned long>(
#if defined(_WIN32)
            GetCurrentProcessId()
#else
            static_cast<unsigned long>(getpid())
#endif
        )
    );
    fs::path tmp = dir / (stem.string() + unique_suffix);

    // Write to temp file.
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            throw std::runtime_error("cannot open temp file for writing: " + tmp.string());
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) {
            throw std::runtime_error("I/O error writing temp file: " + tmp.string());
        }
    } // f flushed and closed here.

    // Rename temp over destination (atomic on POSIX when same filesystem).
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) {
        // Clean up temp on failure.
        fs::remove(tmp, ec);
        throw std::runtime_error(
            "atomic rename failed for " + dst.string() + ": " + ec.message());
    }
}

// =============================================================================
// count_occurrences
// =============================================================================

/// Count non-overlapping exact occurrences of needle in haystack.
[[nodiscard]] std::size_t count_occurrences(std::string_view haystack,
                                             std::string_view needle)
{
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// =============================================================================
// replace_all_occurrences
// =============================================================================

/// Replace all non-overlapping occurrences of from in s with to.
/// Returns the number of replacements made.
[[nodiscard]] std::size_t replace_all_occurrences(std::string& s,
                                                   std::string_view from,
                                                   std::string_view to)
{
    if (from.empty()) return 0;
    std::string result;
    result.reserve(s.size());
    std::size_t count = 0;
    std::size_t pos   = 0;
    std::size_t found = 0;
    while ((found = s.find(from, pos)) != std::string::npos) {
        result.append(s, pos, found - pos);
        result.append(to);
        pos = found + from.size();
        ++count;
    }
    result.append(s, pos, std::string::npos);
    s = std::move(result);
    return count;
}

// =============================================================================
// replace_first_occurrence
// =============================================================================

/// Replace the first occurrence of from in s with to.
/// Precondition: from appears exactly once in s (caller has already verified).
void replace_first_occurrence(std::string& s,
                               std::string_view from,
                               std::string_view to)
{
    std::size_t pos = s.find(from);
    if (pos == std::string::npos) return;
    s.replace(pos, from.size(), to);
}

// =============================================================================
// split_lines
// =============================================================================

/// Split text into a vector of lines.  Each line includes the newline
/// character if present (so join(lines) == text exactly).  The final line
/// may not end with '\n'.
[[nodiscard]] std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.push_back(text.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        lines.push_back(text.substr(start));
    }
    return lines;
}

// =============================================================================
// lcs_table
// =============================================================================

/// Compute the LCS length table for two line sequences.
/// Returns a 2-D vector of size (n+1) x (m+1).
[[nodiscard]] std::vector<std::vector<int>>
lcs_table(const std::vector<std::string>& a,
          const std::vector<std::string>& b)
{
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            if (a[static_cast<std::size_t>(i - 1)] ==
                b[static_cast<std::size_t>(j - 1)]) {
                dp[static_cast<std::size_t>(i)]
                  [static_cast<std::size_t>(j)] =
                    dp[static_cast<std::size_t>(i - 1)]
                      [static_cast<std::size_t>(j - 1)] + 1;
            } else {
                dp[static_cast<std::size_t>(i)]
                  [static_cast<std::size_t>(j)] =
                    std::max(
                        dp[static_cast<std::size_t>(i - 1)]
                          [static_cast<std::size_t>(j)],
                        dp[static_cast<std::size_t>(i)]
                          [static_cast<std::size_t>(j - 1)]);
            }
        }
    }
    return dp;
}

// =============================================================================
// EditOp
// =============================================================================

enum class EditOp { Keep, Delete, Insert };

struct EditEntry {
    EditOp      op;
    std::string line;
};

/// Backtrack through the LCS table to build the sequence of edit operations.
[[nodiscard]] std::vector<EditEntry>
backtrack(const std::vector<std::vector<int>>& dp,
          const std::vector<std::string>& a,
          const std::vector<std::string>& b,
          int i, int j)
{
    if (i == 0 && j == 0) {
        return {};
    }
    if (i == 0) {
        auto ops = backtrack(dp, a, b, i, j - 1);
        ops.push_back({EditOp::Insert,
                       b[static_cast<std::size_t>(j - 1)]});
        return ops;
    }
    if (j == 0) {
        auto ops = backtrack(dp, a, b, i - 1, j);
        ops.push_back({EditOp::Delete,
                       a[static_cast<std::size_t>(i - 1)]});
        return ops;
    }
    if (a[static_cast<std::size_t>(i - 1)] ==
        b[static_cast<std::size_t>(j - 1)]) {
        auto ops = backtrack(dp, a, b, i - 1, j - 1);
        ops.push_back({EditOp::Keep,
                       a[static_cast<std::size_t>(i - 1)]});
        return ops;
    }
    if (dp[static_cast<std::size_t>(i - 1)]
          [static_cast<std::size_t>(j)] >=
        dp[static_cast<std::size_t>(i)]
          [static_cast<std::size_t>(j - 1)]) {
        auto ops = backtrack(dp, a, b, i - 1, j);
        ops.push_back({EditOp::Delete,
                       a[static_cast<std::size_t>(i - 1)]});
        return ops;
    } else {
        auto ops = backtrack(dp, a, b, i, j - 1);
        ops.push_back({EditOp::Insert,
                       b[static_cast<std::size_t>(j - 1)]});
        return ops;
    }
}

// =============================================================================
// generate_unified_diff
// =============================================================================

static constexpr int kContextLines = 3;

/// Generate a unified diff string in the standard format:
///   --- a/<display_path>
///   +++ b/<display_path>
///   @@ -L,C +L,C @@
///   [context and changed lines]
[[nodiscard]] std::string
generate_unified_diff(std::string_view display_path,
                      const std::string& old_content,
                      const std::string& new_content)
{
    if (old_content == new_content) {
        return std::string{"(no changes)\n"};
    }

    const auto old_lines = split_lines(old_content);
    const auto new_lines = split_lines(new_content);

    // For large files fall back to a simpler approach to avoid O(n*m) LCS
    // stack depth issues.  If either side has more than 2000 lines, produce a
    // minimal diff header only.
    const std::size_t kMaxLinesForLcs = 2000;
    if (old_lines.size() > kMaxLinesForLcs || new_lines.size() > kMaxLinesForLcs) {
        std::ostringstream out;
        out << "--- a/" << display_path << "\n";
        out << "+++ b/" << display_path << "\n";
        out << "@@ -1," << old_lines.size()
            << " +1," << new_lines.size() << " @@\n";
        for (const auto& l : old_lines) {
            out << '-';
            out << l;
            if (!l.empty() && l.back() != '\n') out << '\n';
        }
        for (const auto& l : new_lines) {
            out << '+';
            out << l;
            if (!l.empty() && l.back() != '\n') out << '\n';
        }
        return out.str();
    }

    auto dp  = lcs_table(old_lines, new_lines);
    auto ops = backtrack(dp, old_lines, new_lines,
                         static_cast<int>(old_lines.size()),
                         static_cast<int>(new_lines.size()));

    // Convert ops to hunk regions.
    // Build a combined list of (old_lineno, new_lineno, op, text).
    struct DiffLine {
        int        old_no;  // 1-based; 0 means not present in old
        int        new_no;  // 1-based; 0 means not present in new
        EditOp     op;
        std::string text;
    };

    std::vector<DiffLine> diff;
    diff.reserve(ops.size());
    int old_no = 0;
    int new_no = 0;
    for (const auto& e : ops) {
        DiffLine dl;
        dl.op   = e.op;
        dl.text = e.line;
        if (e.op == EditOp::Keep) {
            ++old_no; ++new_no;
            dl.old_no = old_no;
            dl.new_no = new_no;
        } else if (e.op == EditOp::Delete) {
            ++old_no;
            dl.old_no = old_no;
            dl.new_no = 0;
        } else { // Insert
            ++new_no;
            dl.old_no = 0;
            dl.new_no = new_no;
        }
        diff.push_back(std::move(dl));
    }

    // Identify changed lines (not Keep).
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < diff.size(); ++i) {
        if (diff[i].op != EditOp::Keep) {
            changed.push_back(i);
        }
    }

    if (changed.empty()) {
        return "(no changes)\n";
    }

    // Group changed lines into hunks with kContextLines context on each side.
    struct Hunk {
        std::size_t start; // index into diff[]
        std::size_t end;   // exclusive
    };

    std::vector<Hunk> hunks;
    std::size_t i = 0;
    while (i < changed.size()) {
        std::size_t hunk_start = (changed[i] > static_cast<std::size_t>(kContextLines))
                                     ? changed[i] - static_cast<std::size_t>(kContextLines)
                                     : 0;
        // Extend while adjacent changed lines are within 2*context of each other.
        std::size_t j = i;
        while (j + 1 < changed.size() &&
               changed[j + 1] - changed[j] <= 2 * static_cast<std::size_t>(kContextLines)) {
            ++j;
        }
        std::size_t hunk_end = std::min(
            changed[j] + static_cast<std::size_t>(kContextLines) + 1,
            diff.size());
        hunks.push_back({hunk_start, hunk_end});
        i = j + 1;
    }

    // Emit the unified diff.
    std::ostringstream out;
    out << "--- a/" << display_path << "\n";
    out << "+++ b/" << display_path << "\n";

    for (const auto& hunk : hunks) {
        // Count old/new lines in hunk.
        int old_count = 0, new_count = 0;
        int old_start = 0, new_start = 0;
        bool found_old_start = false, found_new_start = false;
        for (std::size_t k = hunk.start; k < hunk.end; ++k) {
            const auto& dl = diff[k];
            if (dl.op != EditOp::Insert) {
                ++old_count;
                if (!found_old_start) {
                    old_start = dl.old_no;
                    found_old_start = true;
                }
            }
            if (dl.op != EditOp::Delete) {
                ++new_count;
                if (!found_new_start) {
                    new_start = dl.new_no;
                    found_new_start = true;
                }
            }
        }
        if (!found_old_start) old_start = 1;
        if (!found_new_start) new_start = 1;

        out << "@@ -" << old_start << ',' << old_count
            << " +" << new_start << ',' << new_count << " @@\n";

        for (std::size_t k = hunk.start; k < hunk.end; ++k) {
            const auto& dl = diff[k];
            char prefix = ' ';
            if (dl.op == EditOp::Delete) prefix = '-';
            else if (dl.op == EditOp::Insert) prefix = '+';

            out << prefix;
            out << dl.text;
            // Ensure the diff line ends with a newline.
            if (!dl.text.empty() && dl.text.back() != '\n') {
                out << '\n';
            }
        }
    }

    return out.str();
}

} // anonymous namespace

// =============================================================================
// EditTool — ITool interface
// =============================================================================

std::string_view EditTool::name() const {
    return "Edit";
}

std::string_view EditTool::description() const {
    return "Edit a file by replacing an exact string match; requires uniqueness "
           "unless replace_all is true; writes atomically and returns a unified diff.";
}

Json EditTool::schema_json() const {
    return Json{
        {"name",        "Edit"},
        {"description", "Edit a file by replacing an exact string match; requires uniqueness "
                        "unless replace_all is true; writes atomically and returns a unified diff."},
        {"parameters", Json{
            {"type", "object"},
            {"properties", Json{
                {"path", Json{
                    {"type",        "string"},
                    {"description", "Path to the file to edit. Resolved relative to the "
                                    "working directory; a leading ~/ is expanded."}
                }},
                {"old_string", Json{
                    {"type",        "string"},
                    {"description", "Exact bytes to find in the file. Must match exactly "
                                    "once unless replace_all is true."}
                }},
                {"new_string", Json{
                    {"type",        "string"},
                    {"description", "Replacement text for each matched occurrence of old_string."}
                }},
                {"replace_all", Json{
                    {"type",        "boolean"},
                    {"description", "When true, replace every occurrence of old_string; "
                                    "default false, which requires exactly one occurrence."}
                }}
            }},
            {"required", Json::array({"path", "old_string", "new_string"})}
        }}
    };
}

// =============================================================================
// EditTool::run
// =============================================================================

ToolResult EditTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 1. Plan-mode gate.
    // ------------------------------------------------------------------
    if (ctx.is_plan_mode()) {
        return ToolResult::error("plan mode: write tools are not allowed");
    }

    // ------------------------------------------------------------------
    // 2. Cancellation check.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 3. Validate required arguments.
    // ------------------------------------------------------------------
    if (!args.contains("path") || !args["path"].is_string()) {
        return ToolResult::error("Edit: missing required argument \"path\"");
    }
    if (!args.contains("old_string") || !args["old_string"].is_string()) {
        return ToolResult::error("Edit: missing required argument \"old_string\"");
    }
    if (!args.contains("new_string") || !args["new_string"].is_string()) {
        return ToolResult::error("Edit: missing required argument \"new_string\"");
    }

    const std::string raw_path   = args["path"].get<std::string>();
    const std::string old_string = args["old_string"].get<std::string>();
    const std::string new_string = args["new_string"].get<std::string>();
    const bool replace_all       = args.value("replace_all", false);

    if (raw_path.empty()) {
        return ToolResult::error("Edit: \"path\" must not be empty");
    }

    // ------------------------------------------------------------------
    // 4. Resolve path.
    // ------------------------------------------------------------------
    std::filesystem::path resolved;
    try {
        resolved = resolve_path(raw_path, ctx.cwd);
    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string{"Edit: failed to resolve path: "} + ex.what());
    }

    // ------------------------------------------------------------------
    // 5. Read file.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    std::string content;
    try {
        content = read_file_content(resolved);
    } catch (const std::exception& ex) {
        return ToolResult::error(std::string{"Edit: "} + ex.what());
    }

    const std::string original_content = content;

    // ------------------------------------------------------------------
    // 6. Count occurrences.
    // ------------------------------------------------------------------
    const std::size_t match_count = count_occurrences(content, old_string);

    if (match_count == 0) {
        return ToolResult::error(
            "old_string not found in " + resolved.string());
    }

    if (!replace_all && match_count > 1) {
        return ToolResult::error(
            "found " + std::to_string(match_count) +
            " matches; pass replace_all:true or pick a longer unique snippet");
    }

    // ------------------------------------------------------------------
    // 7. Perform replacement.
    // ------------------------------------------------------------------
    std::size_t replacements_made = 0;
    if (replace_all) {
        replacements_made = replace_all_occurrences(content, old_string, new_string);
    } else {
        // match_count == 1 is guaranteed here.
        replace_first_occurrence(content, old_string, new_string);
        replacements_made = 1;
    }

    // ------------------------------------------------------------------
    // 8. Atomic write.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    try {
        write_file_atomic(resolved, content);
    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string{"Edit: atomic write failed: "} + ex.what());
    }

    // ------------------------------------------------------------------
    // 9. Build unified diff and return.
    // ------------------------------------------------------------------
    // Use a display path relative to cwd when possible.
    std::string display_path;
    try {
        display_path = std::filesystem::relative(resolved, ctx.cwd).string();
    } catch (...) {
        display_path = resolved.string();
    }

    std::string diff_body = generate_unified_diff(display_path,
                                                   original_content,
                                                   content);

    // Prepend a summary line.
    std::string body;
    if (replace_all && replacements_made > 1) {
        body = "Replaced " + std::to_string(replacements_made) +
               " occurrences in " + display_path + "\n\n" + diff_body;
    } else {
        body = diff_body;
    }

    Json payload = Json{
        {"path",         resolved.string()},
        {"replacements", static_cast<int>(replacements_made)}
    };

    return ToolResult::ok(std::move(body), std::move(payload));
}

} // namespace batbox::tools
