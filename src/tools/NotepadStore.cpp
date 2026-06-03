// src/tools/NotepadStore.cpp
//
// Implementation of batbox::tools::NotepadStore (DIS-981, S6 — the notepad).
//
// Disk-backed, append-structured, session-keyed working-memory pad.  See
// include/batbox/tools/NotepadStore.hpp for the full design rationale.

#include <batbox/tools/NotepadStore.hpp>
#include <batbox/core/Paths.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::tools {

// =============================================================================
// Process-wide write mutex.
//
// All appends/archives across every NotepadStore instance serialise on this so
// concurrent tool dispatches (which each construct their own NotepadStore but
// point at the same files) cannot interleave a half-written entry.  Reads do
// not take it — a completed append is a consistent view at the OS level.
// =============================================================================
static std::mutex& write_mutex() {
    static std::mutex m;
    return m;
}

// =============================================================================
// Construction / static helpers
// =============================================================================

NotepadStore::NotepadStore(fs::path root)
    : root_(root.empty() ? default_root() : std::move(root)) {}

fs::path NotepadStore::default_root() {
    // Test/override hook: $BATBOX_NOTEPAD_DIR lets tests point the pad at a
    // temp dir without touching the real config dir.
    if (const char* env = std::getenv("BATBOX_NOTEPAD_DIR")) {
        if (env[0] != '\0') {
            return fs::path(env);
        }
    }
    return batbox::paths::config_dir() / "notepads";
}

std::string NotepadStore::session_key(const std::string& session_id,
                                      const std::string& agent_id) {
    if (!session_id.empty()) return session_id;
    if (!agent_id.empty())   return agent_id;
    return "default";
}

std::string NotepadStore::sanitise_key(const std::string& key) {
    if (key.empty()) return "default";
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        const bool safe = std::isalnum(static_cast<unsigned char>(c)) ||
                          c == '.' || c == '_' || c == '-';
        out.push_back(safe ? c : '_');
    }
    if (out.empty()) return "default";
    return out;
}

fs::path NotepadStore::pad_path(const std::string& key) const {
    return root_ / (sanitise_key(key) + ".md");
}

// =============================================================================
// append()
// =============================================================================

batbox::Result<void> NotepadStore::append(const std::string& key,
                                          const std::string& note,
                                          const std::string& section) {
    if (note.empty()) {
        return batbox::Err(std::string("notepad append: 'note' must not be empty"));
    }

    const fs::path path = pad_path(key);

    std::lock_guard<std::mutex> lock(write_mutex());

    // Ensure the pad root exists.
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (ec) {
        return batbox::Err(std::string("notepad append: cannot create pad dir '") +
                           root_.string() + "': " + ec.message());
    }

    // Born header on first append (lazy lifecycle: born at first jot).  No
    // timestamp — keeps the pad deterministic for tests and diffs; the index
    // metadata lives outside the pad body.
    const bool fresh = !fs::exists(path) || fs::file_size(path, ec) == 0;

    std::ofstream out(path, std::ios::app | std::ios::binary);
    if (!out) {
        return batbox::Err(std::string("notepad append: cannot open pad '") +
                           path.string() + "' for writing");
    }

    if (fresh) {
        out << "# Notepad — " << sanitise_key(key) << "\n\n";
    }

    // Append-structured entry with an optional light header.  Each entry is its
    // own blank-line-delimited paragraph block so grep() can return whole
    // nuggets.
    if (!section.empty()) {
        out << "## " << section << "\n";
    }
    out << note;
    if (note.back() != '\n') out << "\n";
    out << "\n";  // blank-line block separator

    out.flush();
    if (!out) {
        return batbox::Err(std::string("notepad append: write failed for '") +
                           path.string() + "'");
    }
    return {};
}

// =============================================================================
// read()
// =============================================================================

std::string NotepadStore::read(const std::string& key) const {
    const fs::path path = pad_path(key);
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};

    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// =============================================================================
// grep()
// =============================================================================

namespace {

// Case-insensitive substring test.
bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

// Split a pad into blank-line-delimited paragraph blocks (entries), preserving
// each block's text (trailing blank line stripped).
std::vector<std::string> split_blocks(const std::string& pad) {
    std::vector<std::string> blocks;
    std::string current;
    std::istringstream in(pad);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            if (!current.empty()) {
                blocks.push_back(current);
                current.clear();
            }
        } else {
            current += line;
            current += '\n';
        }
    }
    if (!current.empty()) blocks.push_back(current);
    return blocks;
}

std::string bounded(std::string s, std::size_t max_chars) {
    if (s.size() <= max_chars) return s;
    s.resize(max_chars);
    s += "\n…(truncated)";
    return s;
}

} // namespace

std::string NotepadStore::grep(const std::string& key,
                               const std::string& query,
                               std::size_t        max_chars) const {
    const std::string pad = read(key);
    if (pad.empty()) return {};
    if (query.empty()) return bounded(pad, max_chars);

    std::ostringstream ss;
    bool first = true;
    for (const auto& block : split_blocks(pad)) {
        if (contains_ci(block, query)) {
            if (!first) ss << "\n";
            ss << block;
            first = false;
        }
    }
    return bounded(ss.str(), max_chars);
}

// =============================================================================
// reinjection_slice()
// =============================================================================

std::string NotepadStore::reinjection_slice(const std::string& key,
                                            std::size_t        max_chars) const {
    const std::string pad = read(key);
    if (pad.empty()) return {};
    if (pad.size() <= max_chars) return pad;

    // Keep the tail (most recent notes).  Snap to a line boundary so we never
    // surface a half line.
    std::size_t start = pad.size() - max_chars;
    const std::size_t nl = pad.find('\n', start);
    if (nl != std::string::npos && nl + 1 < pad.size()) {
        start = nl + 1;
    }
    return std::string("…(earlier notes truncated)\n\n") + pad.substr(start);
}

// =============================================================================
// export_pad() / archive()
// =============================================================================

std::string NotepadStore::export_pad(const std::string& key) const {
    return read(key);
}

batbox::Result<void> NotepadStore::archive(const std::string& key) {
    const fs::path path = pad_path(key);

    std::lock_guard<std::mutex> lock(write_mutex());

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return {};  // nothing to archive
    }

    const fs::path archive_dir = root_ / "archive";
    fs::create_directories(archive_dir, ec);
    if (ec) {
        return batbox::Err(std::string("notepad archive: cannot create '") +
                           archive_dir.string() + "': " + ec.message());
    }

    const std::string stem = sanitise_key(key);
    fs::path dest = archive_dir / (stem + ".md");
    for (int n = 1; fs::exists(dest, ec); ++n) {
        dest = archive_dir / (stem + "-" + std::to_string(n) + ".md");
    }

    fs::rename(path, dest, ec);
    if (ec) {
        // Cross-device or other rename failure: fall back to copy+remove.
        fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return batbox::Err(std::string("notepad archive: cannot move pad to '") +
                               dest.string() + "': " + ec.message());
        }
        fs::remove(path, ec);
    }
    return {};
}

} // namespace batbox::tools
