// src/tools/WriteTool.cpp
//
// Implementation of batbox::tools::WriteTool.
//
// Atomic write strategy:
//   Write to <parent_dir>/.batbox_write_tmp_XXXXXX, then std::filesystem::rename
//   over the destination.  Both files are in the same directory, guaranteeing
//   that the rename is atomic on POSIX (same filesystem / same mount point).
//
// Unified diff generation:
//   A minimal Myers LCS-based line-level diff is computed in-process.
//   Output format mirrors `diff -u` (--- a/path, +++ b/path, @@ hunks).
//   No external process or third-party diff library is required.
//
// Blueprint contract: batbox::tools::WriteTool (blueprints table rows 16630-16632)

#include <batbox/tools/WriteTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::tools {

// =============================================================================
// Internal helpers (anonymous namespace — not part of the ABI)
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// split_lines — splits a string into lines, keeping newlines as part of each
// line.  An empty trailing segment (from a trailing newline) is included so
// that empty-file diffs work correctly.
// ---------------------------------------------------------------------------
static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.push_back(text.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        lines.push_back(text.substr(start)); // no trailing newline
    }
    return lines;
}

// ---------------------------------------------------------------------------
// lcs_lengths — computes the DP table for longest common subsequence between
// vectors a and b.  Returns an (m+1) x (n+1) table where table[i][j] is the
// LCS length of a[0..i) and b[0..j).
// ---------------------------------------------------------------------------
static std::vector<std::vector<int>>
lcs_lengths(const std::vector<std::string>& a,
            const std::vector<std::string>& b) {
    const int m = static_cast<int>(a.size());
    const int n = static_cast<int>(b.size());
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (int i = 1; i <= m; ++i) {
        for (int j = 1; j <= n; ++j) {
            if (a[static_cast<std::size_t>(i - 1)] ==
                b[static_cast<std::size_t>(j - 1)]) {
                dp[static_cast<std::size_t>(i)]
                  [static_cast<std::size_t>(j)] =
                    dp[static_cast<std::size_t>(i - 1)]
                      [static_cast<std::size_t>(j - 1)] + 1;
            } else {
                dp[static_cast<std::size_t>(i)]
                  [static_cast<std::size_t>(j)] =
                    std::max(dp[static_cast<std::size_t>(i - 1)]
                               [static_cast<std::size_t>(j)],
                             dp[static_cast<std::size_t>(i)]
                               [static_cast<std::size_t>(j - 1)]);
            }
        }
    }
    return dp;
}

// ---------------------------------------------------------------------------
// DiffOp — represents a single operation in an edit script.
// ---------------------------------------------------------------------------
enum class DiffOp { Equal, Insert, Delete };

struct EditEntry {
    DiffOp  op;
    std::string line; // the line text (with any trailing newline)
};

// ---------------------------------------------------------------------------
// build_edit_script — walks the LCS table back-tracking to produce an ordered
// list of Equal / Insert / Delete operations.
// ---------------------------------------------------------------------------
static std::vector<EditEntry>
build_edit_script(const std::vector<std::string>& a,
                  const std::vector<std::string>& b,
                  const std::vector<std::vector<int>>& dp) {
    std::vector<EditEntry> edits;
    int i = static_cast<int>(a.size());
    int j = static_cast<int>(b.size());
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 &&
            a[static_cast<std::size_t>(i - 1)] ==
            b[static_cast<std::size_t>(j - 1)]) {
            edits.push_back({DiffOp::Equal, a[static_cast<std::size_t>(i - 1)]});
            --i;
            --j;
        } else if (j > 0 &&
                   (i == 0 ||
                    dp[static_cast<std::size_t>(i)]
                      [static_cast<std::size_t>(j - 1)] >=
                    dp[static_cast<std::size_t>(i - 1)]
                      [static_cast<std::size_t>(j)])) {
            edits.push_back({DiffOp::Insert, b[static_cast<std::size_t>(j - 1)]});
            --j;
        } else {
            edits.push_back({DiffOp::Delete, a[static_cast<std::size_t>(i - 1)]});
            --i;
        }
    }
    std::reverse(edits.begin(), edits.end());
    return edits;
}

// ---------------------------------------------------------------------------
// make_unified_diff — builds a unified diff string (--- /+++ / @@ hunks)
// from the edit script.  context_lines controls how many equal lines
// are shown around each hunk (matches `diff -u` default of 3).
// ---------------------------------------------------------------------------
static std::string
make_unified_diff(const std::string& old_content,
                  const std::string& new_content,
                  const std::string& path_label,
                  int context_lines = 3) {
    const auto old_lines = split_lines(old_content);
    const auto new_lines = split_lines(new_content);

    if (old_lines == new_lines) {
        return "(no changes)";
    }

    const auto dp     = lcs_lengths(old_lines, new_lines);
    const auto edits  = build_edit_script(old_lines, new_lines, dp);

    // -----------------------------------------------------------------------
    // Collect hunk ranges.  A hunk is a contiguous run of non-Equal edits
    // expanded by context_lines on each side.
    // We need positions in the old file (old_line) and new file (new_line)
    // for the @@ header.
    // -----------------------------------------------------------------------
    struct HunkRange {
        int start_idx; // index in edits[] where this hunk begins
        int end_idx;   // index in edits[] one past the hunk end
        int old_start; // 1-based start line in old file
        int new_start; // 1-based start line in new file
        int old_count; // lines from old file in this hunk
        int new_count; // lines from new file in this hunk
    };

    // Step 1: find which edit-entry indices are "changed" (Insert / Delete).
    std::vector<int> changed_idx;
    for (int k = 0; k < static_cast<int>(edits.size()); ++k) {
        if (edits[static_cast<std::size_t>(k)].op != DiffOp::Equal)
            changed_idx.push_back(k);
    }

    if (changed_idx.empty()) {
        return "(no changes)";
    }

    // Step 2: group changed indices into hunk spans (merge nearby changes).
    std::vector<std::pair<int,int>> spans; // [lo, hi] in edits[]
    int lo = std::max(0, changed_idx.front() - context_lines);
    int hi = std::min(static_cast<int>(edits.size()) - 1,
                      changed_idx.front() + context_lines);
    for (std::size_t ci = 1; ci < changed_idx.size(); ++ci) {
        int next_lo = std::max(0, changed_idx[ci] - context_lines);
        int next_hi = std::min(static_cast<int>(edits.size()) - 1,
                               changed_idx[ci] + context_lines);
        if (next_lo <= hi + 1) {
            hi = next_hi; // merge
        } else {
            spans.emplace_back(lo, hi);
            lo = next_lo;
            hi = next_hi;
        }
    }
    spans.emplace_back(lo, hi);

    // Step 3: For each span, compute old/new line numbers.
    // We'll pre-compute old_line and new_line at each edit index.
    std::vector<int> old_at(edits.size(), 0), new_at(edits.size(), 0);
    {
        int oi = 1, ni = 1;
        for (std::size_t k = 0; k < edits.size(); ++k) {
            old_at[k] = oi;
            new_at[k] = ni;
            if (edits[k].op == DiffOp::Equal ||
                edits[k].op == DiffOp::Delete) ++oi;
            if (edits[k].op == DiffOp::Equal ||
                edits[k].op == DiffOp::Insert) ++ni;
        }
    }

    std::ostringstream out;
    out << "--- a/" << path_label << "\n"
        << "+++ b/" << path_label << "\n";

    for (const auto& [span_lo, span_hi] : spans) {
        // Count old_count and new_count within the span.
        int old_count = 0, new_count = 0;
        for (int k = span_lo; k <= span_hi; ++k) {
            const auto& e = edits[static_cast<std::size_t>(k)];
            if (e.op == DiffOp::Equal || e.op == DiffOp::Delete) ++old_count;
            if (e.op == DiffOp::Equal || e.op == DiffOp::Insert) ++new_count;
        }

        const int hunk_old_start = old_at[static_cast<std::size_t>(span_lo)];
        const int hunk_new_start = new_at[static_cast<std::size_t>(span_lo)];

        out << "@@ -" << hunk_old_start << "," << old_count
            << " +" << hunk_new_start << "," << new_count << " @@\n";

        for (int k = span_lo; k <= span_hi; ++k) {
            const auto& e = edits[static_cast<std::size_t>(k)];
            if (e.op == DiffOp::Equal)  out << ' ' << e.line;
            else if (e.op == DiffOp::Delete) out << '-' << e.line;
            else                             out << '+' << e.line;
            // Ensure each line ends with a newline in the diff output.
            if (!e.line.empty() && e.line.back() != '\n') out << '\n';
        }
    }

    return out.str();
}

// ---------------------------------------------------------------------------
// read_file_or_empty — reads a file into a string; returns empty string if
// the file does not exist (new file).  Throws std::runtime_error on other
// errors.
// ---------------------------------------------------------------------------
static std::string read_file_or_empty(const fs::path& p) {
    if (!fs::exists(p)) return {};

    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file for reading: " + p.string());
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// ---------------------------------------------------------------------------
// resolve_path — resolves the given path string:
//   1. Expand a leading "~/" to the user home dir.
//   2. If relative, resolve against ctx.cwd.
// ---------------------------------------------------------------------------
static fs::path resolve_path(const std::string& raw_path, const fs::path& cwd) {
    std::string p = raw_path;

    // Expand ~ prefix.
    if (!p.empty() && p[0] == '~') {
        if (p.size() == 1 || p[1] == '/') {
            const char* home = std::getenv("HOME");
            std::string home_str;
            if (home && home[0] != '\0') {
                home_str = home;
            } else {
                // Fallback: leave ~ unexpanded (unlikely but safe).
                home_str = "~";
            }
            p = home_str + p.substr(1);
        }
    }

    fs::path result(p);
    if (result.is_relative()) {
        result = cwd / result;
    }
    return result;
}

// ---------------------------------------------------------------------------
// atomic_write — writes `content` to `dst` atomically:
//   1. Create a temp file in `dst`'s parent directory.
//   2. Write content to the temp file.
//   3. Rename temp file over `dst`.
// Throws std::runtime_error on any failure.
// ---------------------------------------------------------------------------
static void atomic_write(const fs::path& dst, const std::string& content) {
    const fs::path parent = dst.parent_path();

    // Create parent dirs if they don't exist.
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("Cannot create parent directories for '" +
                                     dst.string() + "': " + ec.message());
        }
    }

    // Build a unique temp-file path in the same directory.
    // Use <parent>/.batbox_write_XXXXXX pattern via mkstemp.
    std::string tmpl = (parent / ".batbox_write_XXXXXX").string();
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');

    int fd = ::mkstemp(tmpl_buf.data());
    if (fd == -1) {
        throw std::runtime_error(
            std::string("Cannot create temp file near '") +
            dst.string() + "': " + std::strerror(errno));
    }

    const fs::path tmp_path(tmpl_buf.data());

    // Write content.
    {
        std::size_t written = 0;
        while (written < content.size()) {
            ssize_t n = ::write(fd,
                                content.data() + written,
                                content.size() - written);
            if (n == -1) {
                const std::string msg = std::strerror(errno);
                ::close(fd);
                fs::remove(tmp_path);
                throw std::runtime_error(
                    "Write to temp file failed: " + msg);
            }
            written += static_cast<std::size_t>(n);
        }
    }
    ::close(fd);

    // Atomic rename.
    std::error_code ec;
    fs::rename(tmp_path, dst, ec);
    if (ec) {
        fs::remove(tmp_path);
        throw std::runtime_error(
            "Atomic rename failed for '" + dst.string() + "': " + ec.message());
    }
}

} // anonymous namespace

// =============================================================================
// WriteTool — ITool virtual implementations
// =============================================================================

std::string_view WriteTool::name() const {
    return "Write";
}

std::string_view WriteTool::description() const {
    return "Create or overwrite a file with the given content; "
           "uses an atomic temp-file + rename strategy and creates parent directories.";
}

Json WriteTool::schema_json() const {
    return Json{
        {"name",        "Write"},
        {"description", "Create or overwrite a file with the given content; "
                        "uses an atomic temp-file + rename strategy and creates parent directories."},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"path", Json{
                    {"type",        "string"},
                    {"description", "Absolute or relative path of the file to write. "
                                    "A leading ~/ is expanded to the home directory."}
                }},
                {"content", Json{
                    {"type",        "string"},
                    {"description", "Full content to write to the file. "
                                    "The existing content is replaced entirely."}
                }}
            }},
            {"required", Json::array({"path", "content"})}
        }}
    };
}

ToolResult WriteTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 1. Plan-mode guard — WriteTool is a mutating operation.
        // ------------------------------------------------------------------
        if (ctx.is_plan_mode()) {
            return ToolResult::error(
                "Write: refused — plan mode is active. "
                "Approve the plan before writing files.");
        }

        // ------------------------------------------------------------------
        // 2. Validate arguments.
        // ------------------------------------------------------------------
        if (!args.contains("path") || !args.at("path").is_string()) {
            return ToolResult::error(
                "Write: missing or non-string argument 'path'.");
        }
        if (!args.contains("content") || !args.at("content").is_string()) {
            return ToolResult::error(
                "Write: missing or non-string argument 'content'.");
        }

        const std::string raw_path = args.at("path").get<std::string>();
        const std::string content  = args.at("content").get<std::string>();

        if (raw_path.empty()) {
            return ToolResult::error("Write: 'path' must not be empty.");
        }

        // ------------------------------------------------------------------
        // 3. Resolve path.
        // ------------------------------------------------------------------
        const fs::path dst = resolve_path(raw_path, ctx.cwd);

        // ------------------------------------------------------------------
        // 4. Cancellation checkpoint before I/O.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("Write: cancelled before write.");
        }

        // ------------------------------------------------------------------
        // 5. Read old content for diff (empty string if file does not exist).
        // ------------------------------------------------------------------
        std::string old_content;
        try {
            old_content = read_file_or_empty(dst);
        } catch (const std::exception& e) {
            return ToolResult::error(
                std::string("Write: cannot read existing file for diff: ") + e.what());
        }

        // ------------------------------------------------------------------
        // 6. Atomic write.
        // ------------------------------------------------------------------
        atomic_write(dst, content);

        // ------------------------------------------------------------------
        // 7. Cancellation checkpoint after write (best-effort).
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            // The write already happened — note it in the result but still ok.
            return ToolResult::ok("Write: file written (cancelled after write): " +
                                  dst.string());
        }

        // ------------------------------------------------------------------
        // 8. Generate unified diff for the body.
        // ------------------------------------------------------------------
        // Use a path label relative to ctx.cwd when possible.
        std::string path_label;
        std::error_code ec;
        const fs::path rel = fs::relative(dst, ctx.cwd, ec);
        path_label = (!ec && !rel.empty()) ? rel.string() : dst.string();

        const std::string diff = make_unified_diff(old_content, content, path_label);

        // Build a structured payload with the path and operation metadata.
        Json payload = Json{
            {"type",      "diff_card"},
            {"path",      dst.string()},
            {"operation", old_content.empty() ? "create" : "overwrite"},
            {"diff",      diff}
        };

        return ToolResult::ok(diff, std::move(payload));

    } catch (const std::exception& e) {
        return ToolResult::error(
            std::string("Write: unexpected error: ") + e.what());
    } catch (...) {
        return ToolResult::error("Write: unknown error.");
    }
}

} // namespace batbox::tools
