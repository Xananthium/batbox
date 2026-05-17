// src/tools/ReadTool.cpp
//
// Implementation of batbox::tools::ReadTool.
//
// Reads a text file and returns its content with cat -n style line numbers.
// Supports optional 1-based offset and limit parameters to read a slice.
// Hard cap: 5 MB total file size; default line cap: 2000 lines.
//
// Blueprint contract: batbox::tools::ReadTool (CPP 5.3)

#include <batbox/tools/ReadTool.hpp>

#include <batbox/core/Json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// Image extension detection — returns true for known image formats.
// These files are returned as a text placeholder instead of raw bytes.
// ---------------------------------------------------------------------------
bool is_image_extension(const std::filesystem::path& p) {
    const std::string ext = [&] {
        std::string s = p.extension().string();
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();

    static constexpr std::array<std::string_view, 9> kImageExts = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg", ".ico", ".tiff"
    };
    for (auto e : kImageExts) {
        if (ext == e) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Binary detection heuristic — probes the first 8 KiB for null bytes.
// Returns true when the file looks like binary data.
// ---------------------------------------------------------------------------
bool looks_binary(const std::filesystem::path& path) {
    constexpr std::size_t kProbeBytes = 8192;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    std::array<char, kProbeBytes> buf{};
    f.read(buf.data(), static_cast<std::streamsize>(kProbeBytes));
    const auto n = static_cast<std::size_t>(f.gcount());
    for (std::size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// cat -n line numbering:
//   Each line is formatted as "%6d→%s" where the arrow is U+2192 (→).
//   Line numbers are 1-based; startLine is the number of the first line
//   in the slice being formatted.
// ---------------------------------------------------------------------------
std::string add_line_numbers(const std::string& content, std::size_t start_line) {
    if (content.empty()) return {};

    std::ostringstream out;
    std::size_t line_no = start_line;
    std::size_t pos     = 0;

    while (pos <= content.size()) {
        auto nl = content.find('\n', pos);
        const bool at_end = (nl == std::string::npos);
        const std::string_view line = at_end
            ? std::string_view(content).substr(pos)
            : std::string_view(content).substr(pos, nl - pos);

        // Pad line number to 6 digits, then U+2192 (→ = 3 bytes UTF-8: 0xE2 0x86 0x92).
        // Format exactly as the TS addLineNumbers() function does.
        char num_buf[16];
        std::snprintf(num_buf, sizeof(num_buf), "%6zu", line_no);
        out << num_buf << "\xe2\x86\x92" << line;

        if (at_end) break;
        out << '\n';
        pos = nl + 1;
        ++line_no;
    }

    return out.str();
}

} // anonymous namespace

// =============================================================================
// ITool identity
// =============================================================================

std::string_view ReadTool::name() const {
    return "Read";
}

std::string_view ReadTool::description() const {
    return "Read a file and return its contents with line numbers (cat -n style). "
           "Supports optional 1-based offset and limit parameters to read a slice of the file.";
}

Json ReadTool::schema_json() const {
    return Json{
        {"name",        "Read"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"file_path", Json{
                    {"type",        "string"},
                    {"description", "Absolute or cwd-relative path of the file to read."}
                }},
                {"offset", Json{
                    {"type",        "integer"},
                    {"minimum",     1},
                    {"description", "1-based line number at which to start reading. Defaults to 1."}
                }},
                {"limit", Json{
                    {"type",        "integer"},
                    {"minimum",     1},
                    {"description", "Maximum number of lines to return. Defaults to 2000."}
                }}
            }},
            {"required", Json::array({"file_path"})}
        }}
    };
}

// =============================================================================
// ITool execution
// =============================================================================

ToolResult ReadTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check — bail out fast.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Extract and validate arguments.
    // ------------------------------------------------------------------
    if (!args.contains("file_path") || !args["file_path"].is_string()) {
        return ToolResult::error("Read: required argument 'file_path' is missing or not a string.");
    }
    const std::string raw_path = args["file_path"].get<std::string>();

    std::size_t offset = 1;
    if (args.contains("offset") && !args["offset"].is_null()) {
        if (!args["offset"].is_number_integer()) {
            return ToolResult::error("Read: 'offset' must be an integer >= 1.");
        }
        const auto v = args["offset"].get<long long>();
        if (v < 1) {
            return ToolResult::error("Read: 'offset' must be >= 1.");
        }
        offset = static_cast<std::size_t>(v);
    }

    std::size_t limit = kDefaultLineLimit;
    if (args.contains("limit") && !args["limit"].is_null()) {
        if (!args["limit"].is_number_integer()) {
            return ToolResult::error("Read: 'limit' must be a positive integer.");
        }
        const auto v = args["limit"].get<long long>();
        if (v < 1) {
            return ToolResult::error("Read: 'limit' must be >= 1.");
        }
        limit = static_cast<std::size_t>(v);
    }

    // ------------------------------------------------------------------
    // 2. Resolve path against ctx.cwd; reject symlinks that escape cwd.
    // ------------------------------------------------------------------
    std::filesystem::path requested(raw_path);
    if (requested.is_relative()) {
        requested = ctx.cwd / requested;
    }

    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(requested, ec);
    if (ec) {
        // weakly_canonical can fail on broken symlinks pointing outside cwd.
        // Fall back to lexical normalization.
        resolved = requested.lexically_normal();
    }

    // Symlink escape check: if the resolved canonical path is a symlink,
    // verify that its target also falls within ctx.cwd.
    if (std::filesystem::is_symlink(resolved, ec) && !ec) {
        std::filesystem::path link_target = std::filesystem::read_symlink(resolved, ec);
        if (!ec) {
            if (link_target.is_relative()) {
                link_target = resolved.parent_path() / link_target;
            }
            std::filesystem::path canonical_target = std::filesystem::weakly_canonical(link_target, ec);
            if (!ec) {
                // Reject if the symlink target escapes cwd.
                const std::filesystem::path cwd_canonical = std::filesystem::weakly_canonical(ctx.cwd, ec);
                if (!ec) {
                    const std::string target_str  = canonical_target.string();
                    const std::string cwd_str     = cwd_canonical.string();
                    // Check: target must be cwd or a subdirectory of cwd.
                    if (target_str != cwd_str &&
                        target_str.substr(0, cwd_str.size()) != cwd_str &&
                        target_str.size() > cwd_str.size() &&
                        target_str[cwd_str.size()] != std::filesystem::path::preferred_separator) {
                        // Not a subdirectory match; do a proper prefix check.
                        const std::string sep_str(1, static_cast<char>(std::filesystem::path::preferred_separator));
                        const std::string cwd_with_sep = cwd_str + sep_str;
                        if (target_str != cwd_str && target_str.substr(0, cwd_with_sep.size()) != cwd_with_sep) {
                            return ToolResult::error(
                                "Read: symlink '" + raw_path + "' points outside the working directory.");
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 3. Existence and type checks.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) return ToolResult::error("cancelled");

    if (!std::filesystem::exists(resolved, ec) || ec) {
        return ToolResult::error(
            "Read: file not found: '" + raw_path + "'. "
            "Check the path and ensure the file exists.");
    }

    if (std::filesystem::is_directory(resolved, ec)) {
        return ToolResult::error(
            "Read: '" + raw_path + "' is a directory, not a file. "
            "Use Glob to list directory contents.");
    }

    // ------------------------------------------------------------------
    // 4. Size cap (5 MB hard limit on total file size).
    // ------------------------------------------------------------------
    const auto file_size = std::filesystem::file_size(resolved, ec);
    if (ec) {
        return ToolResult::error(
            "Read: cannot stat '" + raw_path + "': " + ec.message());
    }
    if (file_size > kMaxFileSizeBytes) {
        return ToolResult::error(
            "Read: file '" + raw_path + "' is " +
            std::to_string(file_size / 1024 / 1024) + " MB, which exceeds the 5 MB limit. "
            "Use offset and limit parameters to read specific portions of the file.");
    }

    // ------------------------------------------------------------------
    // 5. Image extension check — return placeholder.
    // ------------------------------------------------------------------
    if (is_image_extension(resolved)) {
        const std::string ext = resolved.extension().string();
        return ToolResult::ok(
            "(image: " + ext + " file — use a vision-capable model to inspect this file)");
    }

    // ------------------------------------------------------------------
    // 6. Open the file for reading.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) return ToolResult::error("cancelled");

    std::ifstream file(resolved, std::ios::binary);
    if (!file.is_open()) {
        // Try to distinguish permission denied from other errors.
        std::error_code open_ec;
        std::filesystem::file_status st = std::filesystem::status(resolved, open_ec);
        if (!open_ec) {
            const auto perms = st.permissions();
            const bool owner_read = (perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none;
            if (!owner_read) {
                return ToolResult::error(
                    "Read: permission denied: '" + raw_path + "'.");
            }
        }
        return ToolResult::error("Read: cannot open '" + raw_path + "'.");
    }

    // ------------------------------------------------------------------
    // 7. Binary heuristic — return placeholder for non-text files.
    // ------------------------------------------------------------------
    {
        constexpr std::size_t kProbeBytes = 8192;
        std::array<char, kProbeBytes> probe{};
        file.read(probe.data(), static_cast<std::streamsize>(kProbeBytes));
        const auto n = static_cast<std::size_t>(file.gcount());
        for (std::size_t i = 0; i < n; ++i) {
            if (probe[i] == '\0') {
                return ToolResult::ok(
                    "(binary file: '" + raw_path + "' — contains non-text data and cannot be displayed as text)");
            }
        }
        // Seek back to the beginning for the actual read.
        // Clear eofbit (set when file is smaller than kProbeBytes) before seeking.
        file.clear();
        file.seekg(0, std::ios::beg);
        if (!file) {
            return ToolResult::error("Read: seek failed on '" + raw_path + "'.");
        }
    }

    if (ctx.is_cancelled()) return ToolResult::error("cancelled");

    // ------------------------------------------------------------------
    // 8. Read lines with offset/limit applied.
    //    offset is 1-based; we skip (offset-1) lines, then collect up to
    //    `limit` lines.
    // ------------------------------------------------------------------
    const std::size_t skip  = offset - 1;   // number of lines to skip
    std::size_t total_lines = 0;            // total lines in file (counted during scan)
    std::size_t lines_read  = 0;
    std::ostringstream content_buf;
    bool first_kept_line = true;

    std::string line;
    while (std::getline(file, line)) {
        ++total_lines;

        if (total_lines <= skip) {
            // Still skipping prefix lines.
            continue;
        }
        if (lines_read >= limit) {
            // Already collected enough; keep scanning to count total_lines
            // but don't accumulate content.
            continue;
        }

        if (!first_kept_line) {
            content_buf << '\n';
        }
        content_buf << line;
        first_kept_line = false;
        ++lines_read;

        if (ctx.is_cancelled()) return ToolResult::error("cancelled");
    }

    // Handle the case where the file ends without a newline on the last line.
    // std::getline handles this correctly above — no special casing needed.

    // While scanning remaining lines after limit, we need to count them:
    // (already counted in the loop above with the continue branch)

    // If offset is beyond the file length, report it helpfully.
    if (offset > total_lines + 1 && total_lines > 0) {
        return ToolResult::error(
            "Read: offset " + std::to_string(offset) +
            " is beyond the end of '" + raw_path + "' (file has " +
            std::to_string(total_lines) + " lines).");
    }

    // ------------------------------------------------------------------
    // 9. Format with cat -n line numbers.
    // ------------------------------------------------------------------
    const std::string raw_content = content_buf.str();
    const std::string numbered    = add_line_numbers(raw_content, offset);

    // ------------------------------------------------------------------
    // 10. Build result with structured payload.
    // ------------------------------------------------------------------
    Json payload = Json{
        {"file_path",   resolved.string()},
        {"start_line",  offset},
        {"lines_read",  lines_read},
        {"total_lines", total_lines},
        {"file_size",   static_cast<std::size_t>(file_size)}
    };

    return ToolResult::ok(numbered, std::move(payload));
}

} // namespace batbox::tools
