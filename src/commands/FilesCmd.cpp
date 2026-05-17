// src/commands/FilesCmd.cpp
//
// batbox::commands::FilesCmd — implements the /files slash command.
//
// /files [pattern]
//   Lists files in the current working directory (ctx.cwd) plus any extra
//   directories registered via /add-dir.  Output is a tree view showing
//   relative paths, file sizes, and last-modified timestamps.
//
// Pattern filtering:
//   An optional glob-style pattern argument filters displayed entries.
//   Matching is a simple substring check on the filename component.
//   Full glob expansion is not required (keeps this command dependency-free).
//
// Tree format:
//   Directories are shown with a trailing '/'.
//   Files show size (human-readable: B/KB/MB) and mtime (YYYY-MM-DD HH:MM).
//
// No aliases.
//
// Registration entry point:
//   void register_files_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::commands {

// Forward declare the extra dirs accessor from AddDirCmd.cpp.
std::vector<fs::path> get_extra_dirs();

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Strip leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Format a file size in human-readable form.
[[nodiscard]] std::string format_size(std::uintmax_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << (static_cast<double>(bytes) / 1024.0) << " KB";
        return oss.str();
    } else {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1)
            << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
        return oss.str();
    }
}

/// Format a last_write_time as "YYYY-MM-DD HH:MM".
[[nodiscard]] std::string format_mtime(const fs::file_time_type& mtime) {
    // Convert to system_clock for ctime formatting.
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    const std::time_t t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return std::string(buf);
}

/// Simple substring pattern match on the filename stem+extension.
[[nodiscard]] bool matches_pattern(
        const fs::path& file_path,
        std::string_view pattern)
{
    if (pattern.empty()) return true;
    const std::string fname = file_path.filename().string();
    return fname.find(pattern) != std::string::npos;
}

struct FileEntry {
    fs::path    path;
    bool        is_dir;
    std::uintmax_t size_bytes;
    fs::file_time_type mtime;
};

/// Collect all entries (non-recursive) from a root directory.
void collect_entries(
        const fs::path& root,
        std::string_view pattern,
        std::vector<FileEntry>& out)
{
    std::error_code ec;
    fs::directory_iterator it(root, ec);
    if (ec) return;

    std::vector<FileEntry> local;
    for (const auto& entry : it) {
        std::error_code entry_ec;
        const bool is_dir = entry.is_directory(entry_ec);
        const bool is_reg = entry.is_regular_file(entry_ec);

        if (entry_ec) continue;
        if (!is_dir && !is_reg) continue;

        // Filter by pattern on filename.
        if (!matches_pattern(entry.path(), pattern)) continue;

        FileEntry fe;
        fe.path   = entry.path();
        fe.is_dir = is_dir;

        if (is_reg) {
            fe.size_bytes = entry.file_size(entry_ec);
            if (entry_ec) fe.size_bytes = 0;
        } else {
            fe.size_bytes = 0;
        }

        fe.mtime = entry.last_write_time(entry_ec);
        if (entry_ec) fe.mtime = fs::file_time_type{};

        local.push_back(std::move(fe));
    }

    // Sort: directories first, then alphabetically within each group.
    std::sort(local.begin(), local.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        return a.path.filename() < b.path.filename();
    });

    for (auto& fe : local) {
        out.push_back(std::move(fe));
    }
}

/// Emit a tree section header and its entries to `out`.
void emit_tree_section(
        std::ostream& out,
        const fs::path& root,
        std::string_view pattern,
        std::size_t& total_files,
        std::size_t& total_dirs)
{
    std::vector<FileEntry> entries;
    collect_entries(root, pattern, entries);

    // Section header.
    out << "\n  " << root.string() << "/\n";
    out << "  " << std::string(root.string().size() + 1, '-') << "\n";

    if (entries.empty()) {
        out << "  (empty)\n";
        return;
    }

    // Column widths: name column = 36, size = 10, mtime = 16.
    constexpr int kNameWidth = 36;
    constexpr int kSizeWidth = 10;

    for (const auto& fe : entries) {
        const std::string fname = fe.path.filename().string()
                                  + (fe.is_dir ? "/" : "");

        // Pad name to kNameWidth.
        std::string padded_name = fname;
        if (padded_name.size() < static_cast<std::size_t>(kNameWidth)) {
            padded_name += std::string(kNameWidth - padded_name.size(), ' ');
        } else if (padded_name.size() > static_cast<std::size_t>(kNameWidth)) {
            // Truncate with ellipsis.
            padded_name = padded_name.substr(0, kNameWidth - 3) + "...";
        }

        if (fe.is_dir) {
            out << "  " << padded_name << "\n";
            ++total_dirs;
        } else {
            const std::string sz = format_size(fe.size_bytes);
            const std::string mt = format_mtime(fe.mtime);

            // Right-align size in kSizeWidth.
            std::string padded_sz = sz;
            if (padded_sz.size() < static_cast<std::size_t>(kSizeWidth)) {
                padded_sz = std::string(kSizeWidth - padded_sz.size(), ' ') + padded_sz;
            }

            out << "  " << padded_name
                << "  " << padded_sz
                << "  " << mt << "\n";
            ++total_files;
        }
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// FilesCmd
// ---------------------------------------------------------------------------

class FilesCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "files";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "List files in cwd and /add-dir extras with size and mtime.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/files [pattern]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   args,
        CommandContext&    ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> FilesCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    const std::string_view pattern = trim(args);

    // Build the list of roots to display.
    std::vector<fs::path> roots;
    roots.push_back(ctx.cwd);

    const std::vector<fs::path> extra = get_extra_dirs();
    for (const auto& d : extra) {
        roots.push_back(d);
    }

    // Heading.
    ctx.output << "\n  Files";
    if (!pattern.empty()) {
        ctx.output << " (filter: " << pattern << ")";
    }
    ctx.output << "\n  " << std::string(40, '=') << "\n";

    std::size_t total_files = 0;
    std::size_t total_dirs  = 0;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec) || ec) {
            ctx.output << "\n  " << root.string() << "  (not accessible)\n";
            continue;
        }
        emit_tree_section(ctx.output, root, pattern, total_files, total_dirs);
    }

    // Summary footer.
    ctx.output << "\n  "
               << total_files << " file" << (total_files == 1 ? "" : "s")
               << ", "
               << total_dirs  << " director" << (total_dirs == 1 ? "y" : "ies")
               << " across " << roots.size()
               << " root" << (roots.size() == 1 ? "" : "s")
               << ".\n\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_files_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<FilesCmd>());
    (void)res;
}

} // namespace batbox::commands
