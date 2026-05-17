// src/session/SessionIndex.cpp
// =============================================================================
// Implementation of the append-only JSONL session index.
// =============================================================================
//
// Append strategy: O_APPEND + write() + fsync().
//   On local POSIX filesystems a single write() with O_APPEND is atomic if
//   the payload is ≤ PIPE_BUF (4096 bytes minimum per POSIX).  A session
//   index record is typically < 512 bytes, so this holds on ext4, APFS, etc.
//   NFS is not atomic; we document but do not guard that case.
//
// Update strategy (newest-record-wins):
//   To update an existing record the caller appends a new record with the
//   same id.  read_latest_per_id() keeps only the entry with the highest
//   updated_at for each id, so the newest record wins automatically.
//   No in-place rewriting is needed, preserving the O(1) append guarantee.
//
// Timestamp format: ISO-8601 UTC "2026-01-15T10:30:00Z"
//   strftime / strptime are used for platform-portable conversion.  The 'Z'
//   suffix is written unconditionally because system_clock is always UTC.
//
// JSON keys (stable across releases):
//   "id", "created_at", "updated_at", "first_message_preview",
//   "model", "turn_count", "file_path"
// =============================================================================

#include "batbox/session/SessionIndex.hpp"
#include "batbox/core/Json.hpp"
#include "batbox/core/Logging.hpp"
#include "batbox/core/Paths.hpp"
#include "batbox/core/Uuid.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// POSIX headers
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace batbox::session {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal: timestamp helpers
// ---------------------------------------------------------------------------

/// Serialise a system_clock::time_point to "YYYY-MM-DDTHH:MM:SSZ".
static std::string tp_to_iso8601(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

/// Parse "YYYY-MM-DDTHH:MM:SSZ" (or with fractional seconds) to time_point.
/// Returns epoch on parse failure (treated as oldest possible entry, not fatal).
static std::chrono::system_clock::time_point iso8601_to_tp(const std::string& s) {
    std::tm tm_utc{};
    // strptime handles "2026-01-15T10:30:00Z"; fractional seconds ignored
    const char* p = nullptr;
#if defined(_WIN32)
    // Windows doesn't have strptime; use sscanf
    int yr, mo, dy, hr, mi, sc;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mi, &sc) == 6) {
        tm_utc.tm_year = yr - 1900;
        tm_utc.tm_mon  = mo - 1;
        tm_utc.tm_mday = dy;
        tm_utc.tm_hour = hr;
        tm_utc.tm_min  = mi;
        tm_utc.tm_sec  = sc;
        p = s.c_str() + 1; // non-null sentinel
    }
#else
    p = strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_utc);
#endif
    if (!p) {
        return std::chrono::system_clock::from_time_t(0);
    }
    std::time_t t = timegm(&tm_utc);
    if (t == static_cast<time_t>(-1)) {
        return std::chrono::system_clock::from_time_t(0);
    }
    return std::chrono::system_clock::from_time_t(t);
}

// ---------------------------------------------------------------------------
// Internal: serialise / deserialise one record to/from nlohmann::json
// ---------------------------------------------------------------------------

static nlohmann::json record_to_json(const SessionIndexRecord& rec) {
    nlohmann::json j;
    j["id"]                    = rec.id.to_string();
    j["created_at"]            = tp_to_iso8601(rec.created_at);
    j["updated_at"]            = tp_to_iso8601(rec.updated_at);
    j["first_message_preview"] = rec.first_message_preview;
    j["model"]                 = rec.model;
    j["turn_count"]            = rec.turn_count;
    j["file_path"]             = rec.file_path.string();
    return j;
}

/// Returns true on success, false on any parse / missing-field error.
static bool json_to_record(const nlohmann::json& j, SessionIndexRecord& out) {
    try {
        // id — mandatory, must parse as UUID
        auto id_opt = Uuid::parse(j.at("id").get<std::string>());
        if (!id_opt) return false;
        out.id = *id_opt;

        out.created_at            = iso8601_to_tp(j.at("created_at").get<std::string>());
        out.updated_at            = iso8601_to_tp(j.at("updated_at").get<std::string>());
        out.first_message_preview = j.at("first_message_preview").get<std::string>();
        out.model                 = j.at("model").get<std::string>();
        out.turn_count            = j.at("turn_count").get<uint64_t>();
        out.file_path             = fs::path(j.at("file_path").get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// default_index_path()
// ---------------------------------------------------------------------------

fs::path default_index_path() {
    return paths::config_dir() / "sessions" / "index.json";
}

// ---------------------------------------------------------------------------
// append_index_record()
// ---------------------------------------------------------------------------

Result<void> append_index_record(const fs::path& index_path,
                                 const SessionIndexRecord& rec) {
    // Serialise to a compact one-liner + newline.
    std::string line = record_to_json(rec).dump() + "\n";

    // Ensure parent directory exists.
    std::error_code ec;
    fs::create_directories(index_path.parent_path(), ec);
    if (ec) {
        return Err("append_index_record: cannot create parent dir '"
                   + index_path.parent_path().string() + "': " + ec.message());
    }

    // Open with O_APPEND | O_WRONLY | O_CREAT.
    // Mode 0666 — umask applies at runtime.
    int fd = ::open(index_path.c_str(),
                    O_WRONLY | O_CREAT | O_APPEND,
                    static_cast<mode_t>(0666));
    if (fd < 0) {
        return Err(std::string("append_index_record: open failed: ")
                   + std::strerror(errno));
    }

    // Write the line atomically (single write() call).
    const char* data = line.data();
    ssize_t remaining = static_cast<ssize_t>(line.size());
    while (remaining > 0) {
        ssize_t written = ::write(fd, data, static_cast<size_t>(remaining));
        if (written < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            ::close(fd);
            return Err(std::string("append_index_record: write failed: ")
                       + std::strerror(saved));
        }
        data      += written;
        remaining -= written;
    }

    // fsync to guarantee durability before the caller considers the append done.
    if (::fsync(fd) < 0) {
        int saved = errno;
        ::close(fd);
        return Err(std::string("append_index_record: fsync failed: ")
                   + std::strerror(saved));
    }

    ::close(fd);
    return {};  // Result<void> default-constructs as ok
}

// ---------------------------------------------------------------------------
// read_latest_per_id()
//
// Implementation notes:
//   - Reads the entire file into memory in one pass via ifstream with a large
//     buffer.  For a 10 000-entry index with ~300 bytes/record that is ~3 MB —
//     well within a < 50 ms wall-time budget on any local SSD.
//   - Builds an unordered_map<Uuid, SessionIndexRecord> keyed by session id,
//     keeping the record with the highest updated_at (last-record-wins on tie).
//   - After the full scan, extracts the map values into a vector, sorts by
//     updated_at descending, and returns the first `n`.
// ---------------------------------------------------------------------------

// Internal state passed back to is_corrupt() via thread_local.
// Reset at the start of each read_latest_per_id call.
thread_local uint64_t tl_last_corrupt_line_count = 0;
thread_local bool     tl_last_read_had_io_error   = false;

Result<std::vector<SessionIndexRecord>>
read_latest_per_id(const fs::path& index_path, size_t n) {
    tl_last_corrupt_line_count = 0;
    tl_last_read_had_io_error  = false;

    // Missing file is not an error.
    std::error_code ec;
    if (!fs::exists(index_path, ec)) {
        return std::vector<SessionIndexRecord>{};
    }

    std::ifstream in(index_path, std::ios::binary);
    if (!in.is_open()) {
        tl_last_read_had_io_error = true;
        return Err("read_latest_per_id: cannot open '"
                   + index_path.string() + "'");
    }

    // Large read buffer for throughput.
    constexpr std::streamsize kBufSize = 256 * 1024;
    std::vector<char> buf(kBufSize);
    in.rdbuf()->pubsetbuf(buf.data(), kBufSize);

    auto logger = log::get("session");
    std::unordered_map<Uuid, SessionIndexRecord> latest;
    latest.reserve(256);

    std::string line;
    while (std::getline(in, line)) {
        // Skip blank lines (can appear at file end due to trailing newline).
        if (line.empty()) continue;

        // Attempt to parse as JSON.
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception& ex) {
            ++tl_last_corrupt_line_count;
            logger->warn("read_latest_per_id: skipped corrupt line (json parse): {}",
                         ex.what());
            continue;
        }

        SessionIndexRecord rec;
        if (!json_to_record(j, rec)) {
            ++tl_last_corrupt_line_count;
            logger->warn("read_latest_per_id: skipped line (missing/invalid fields)");
            continue;
        }

        // Keep newest-updated_at per id.
        auto it = latest.find(rec.id);
        if (it == latest.end()) {
            latest.emplace(rec.id, std::move(rec));
        } else if (rec.updated_at > it->second.updated_at) {
            it->second = std::move(rec);
        }
    }

    if (in.bad()) {
        tl_last_read_had_io_error = true;
        return Err("read_latest_per_id: I/O error reading '"
                   + index_path.string() + "'");
    }

    // Collect, sort by updated_at descending, truncate to n.
    std::vector<SessionIndexRecord> results;
    results.reserve(latest.size());
    for (auto& kv : latest) {
        results.push_back(std::move(kv.second));
    }

    std::sort(results.begin(), results.end(),
              [](const SessionIndexRecord& a, const SessionIndexRecord& b) {
                  return a.updated_at > b.updated_at;  // descending
              });

    if (n > 0 && results.size() > n) {
        results.resize(n);
    }

    return results;
}

// ---------------------------------------------------------------------------
// rebuild_from_dir()
// ---------------------------------------------------------------------------

Result<uint64_t>
rebuild_from_dir(const fs::path& sessions_dir,
                 const fs::path& index_path) {
    std::error_code ec;
    if (!fs::is_directory(sessions_dir, ec)) {
        return Err("rebuild_from_dir: not a directory: '"
                   + sessions_dir.string() + "'");
    }

    auto logger = log::get("session");
    std::vector<SessionIndexRecord> records;
    uint64_t indexed_count  = 0;
    uint64_t skipped_count  = 0;

    for (const auto& entry : fs::directory_iterator(sessions_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        const fs::path& fpath = entry.path();
        const std::string ext = fpath.extension().string();
        const std::string stem_ext = fpath.stem().extension().string();

        // Accept *.json and *.json.gz; skip index.json itself.
        bool is_json    = (ext == ".json"  && fpath.filename() != "index.json");
        bool is_json_gz = (ext == ".gz"    && stem_ext == ".json");
        if (!is_json && !is_json_gz) continue;

        // Read the first few bytes to extract id and metadata without pulling
        // the full session file (which can be multi-MB).  We open the raw
        // .json directly; .json.gz is read via ifstream (gzip decompression
        // is delegated to SessionFile — here we do best-effort plain read and
        // flag corrupt on failure).
        std::ifstream f(fpath, std::ios::binary);
        if (!f.is_open()) {
            ++skipped_count;
            logger->warn("rebuild_from_dir: cannot open '{}' — skipped",
                         fpath.string());
            continue;
        }

        // Read up to 8 KB — enough to get the outer object fields.
        constexpr std::streamsize kPreview = 8192;
        std::string head(kPreview, '\0');
        f.read(head.data(), kPreview);
        head.resize(static_cast<size_t>(f.gcount()));

        // If this is a .gz file the head is binary; skip gracefully.
        if (is_json_gz) {
            // We can't decompress gzip here without zlib linkage (that lives in
            // SessionFile.cpp).  Create a minimal record from the filename only.
            const std::string stem = fpath.stem().stem().string(); // strip .json.gz
            auto id_opt = Uuid::parse(stem);
            if (!id_opt) {
                ++skipped_count;
                continue;
            }
            SessionIndexRecord gz_rec;
            gz_rec.id                    = *id_opt;
            gz_rec.created_at            = std::chrono::system_clock::now();
            gz_rec.updated_at            = std::chrono::system_clock::now();
            gz_rec.first_message_preview = "(compressed session)";
            gz_rec.model                 = "unknown";
            gz_rec.turn_count            = 0;
            gz_rec.file_path             = fpath;
            records.push_back(std::move(gz_rec));
            ++indexed_count;
            continue;
        }

        // Parse the JSON head.
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(head, nullptr, false, true); // allow partial
        } catch (...) {
            ++skipped_count;
            logger->warn("rebuild_from_dir: cannot parse '{}' — skipped",
                         fpath.string());
            continue;
        }

        // Extract fields; fall back gracefully on missing keys.
        auto id_str = batbox::get_or<std::string>(j, "id", "");
        auto id_opt = Uuid::parse(id_str);
        if (!id_opt) {
            // Try deriving id from filename stem.
            id_opt = Uuid::parse(fpath.stem().string());
        }
        if (!id_opt) {
            ++skipped_count;
            logger->warn("rebuild_from_dir: no valid id in '{}' — skipped",
                         fpath.string());
            continue;
        }

        SessionIndexRecord rec;
        rec.id         = *id_opt;
        rec.file_path  = fpath;

        // Timestamps
        const std::string created_str = batbox::get_or<std::string>(j, "created_at", "");
        rec.created_at = created_str.empty()
                         ? std::chrono::system_clock::from_time_t(0)
                         : iso8601_to_tp(created_str);

        const std::string updated_str = batbox::get_or<std::string>(j, "updated_at", "");
        rec.updated_at = updated_str.empty()
                         ? rec.created_at
                         : iso8601_to_tp(updated_str);

        rec.model      = batbox::get_or<std::string>(j, "model_at_start", "unknown");
        rec.turn_count = batbox::get_or<uint64_t>(j, "turn_count", uint64_t{0});

        // first_message_preview: pull from messages[0].content if present.
        try {
            if (j.contains("messages") && j["messages"].is_array()
                && !j["messages"].empty()) {
                const auto& msg = j["messages"][0];
                std::string content = batbox::get_or<std::string>(msg, "content", "");
                if (content.size() > 120) content.resize(120);
                rec.first_message_preview = std::move(content);
            }
        } catch (...) {
            // Non-fatal; preview stays empty.
        }

        records.push_back(std::move(rec));
        ++indexed_count;
    }

    if (ec) {
        return Err("rebuild_from_dir: directory iteration error on '"
                   + sessions_dir.string() + "': " + ec.message());
    }

    if (indexed_count == 0 && skipped_count == 0) {
        // Empty directory — write an empty index file.
        std::error_code mkec;
        fs::create_directories(index_path.parent_path(), mkec);
        // Atomic write: tmp file + rename.
        fs::path tmp_path = index_path;
        tmp_path += ".tmp";
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return Err("rebuild_from_dir: cannot create temp index '"
                           + tmp_path.string() + "'");
            }
            // Empty file is a valid empty JSONL index.
        }
        fs::rename(tmp_path, index_path, mkec);
        if (mkec) {
            return Err("rebuild_from_dir: rename tmp index failed: " + mkec.message());
        }
        return uint64_t{0};
    }

    // Sort by updated_at descending so the written index is in a natural order.
    std::sort(records.begin(), records.end(),
              [](const SessionIndexRecord& a, const SessionIndexRecord& b) {
                  return a.updated_at > b.updated_at;
              });

    // Atomic write via tmp + rename.
    std::error_code mkec;
    fs::create_directories(index_path.parent_path(), mkec);
    fs::path tmp_path = index_path;
    tmp_path += ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Err("rebuild_from_dir: cannot create temp index '"
                       + tmp_path.string() + "'");
        }
        for (const auto& r : records) {
            out << record_to_json(r).dump() << '\n';
            if (out.bad()) {
                return Err("rebuild_from_dir: I/O error writing tmp index");
            }
        }
    }

    fs::rename(tmp_path, index_path, mkec);
    if (mkec) {
        return Err("rebuild_from_dir: rename failed: " + mkec.message());
    }

    logger->info("rebuild_from_dir: indexed {} sessions ({} skipped) → '{}'",
                 indexed_count, skipped_count, index_path.string());
    return indexed_count;
}

// ---------------------------------------------------------------------------
// is_corrupt()
// ---------------------------------------------------------------------------

bool is_corrupt(const fs::path& index_path) {
    // Missing file is not corrupt — rebuild_from_dir handles absence.
    std::error_code ec;
    if (!fs::exists(index_path, ec)) return false;

    // Attempt a full read; check thread_local corruption counters.
    auto result = read_latest_per_id(index_path, 0);
    if (!result) {
        // Hard I/O error counts as corrupt.
        return true;
    }
    return tl_last_corrupt_line_count > 0 || tl_last_read_had_io_error;
}

} // namespace batbox::session
