// include/batbox/session/SessionIndex.hpp
// =============================================================================
// batbox session index — append-only JSONL record at
//   ~/.batbox/sessions/index.json
//
// Design (Karla MAJOR-1 compliant):
//   - One JSON record per line (JSONL / newline-delimited JSON).
//   - append_index_record() opens with O_APPEND | O_WRONLY, writes one line
//     terminated by '\n', then fdsyncs.  Multiple writers on the same file are
//     safe on POSIX because O_APPEND + write() < PIPE_BUF is atomic on local
//     filesystems.  NFS caveat: not atomic; documented here, not guarded.
//   - read_latest_per_id() streams the file line-by-line, keeping the latest
//     record per id in an unordered_map (last-record-wins / highest updated_at
//     wins when there are duplicates or tombstone-style re-appends for updates).
//     Returns the last `n` sessions sorted by updated_at descending.
//   - Corrupt / non-parseable lines are silently skipped with a WARN log entry;
//     a corrupt_line_count is tracked in the returned summary.
//   - read time < 50 ms for 10,000-entry index (contract from acceptance criteria).
//   - rebuild_from_dir() scans a sessions directory, reads every .json and
//     .json.gz session file, and re-populates the index from scratch.  Used by
//     SessionRecovery when the index is missing or too corrupt to be trusted.
//   - is_corrupt() returns true if the last read_latest_per_id call produced any
//     skipped lines OR the file cannot be opened.
//
// Serialisation keys (stable — changing breaks existing indexes):
//   id, created_at, updated_at, first_message_preview, model, turn_count,
//   file_path
//
// Timestamps are ISO-8601 UTC strings: "2026-01-15T10:30:00Z".
//
// Build (standalone, from repo root):
//   c++ -std=c++20 \
//       -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_session_index.cpp \
//       src/session/SessionIndex.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_session_index && /tmp/test_session_index
// =============================================================================

#pragma once

#include <batbox/core/Result.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace batbox::session {

// =============================================================================
// SessionIndexRecord — one row in the JSONL index.
// =============================================================================

struct SessionIndexRecord {
    /// UUID that identifies the session (matches the session .json filename stem).
    Uuid id;

    /// Wall-clock time the session was first created (UTC).
    std::chrono::system_clock::time_point created_at;

    /// Wall-clock time of the most recent update (UTC).
    std::chrono::system_clock::time_point updated_at;

    /// First user-visible message, truncated to ≤ 120 chars.
    std::string first_message_preview;

    /// Model name at session start (e.g. "gpt-4o").
    std::string model;

    /// Number of conversational turns completed in this session.
    uint64_t turn_count{0};

    /// Absolute path to the session .json (or .json.gz) file on disk.
    std::filesystem::path file_path;

    /// DIS-1020 — subagent journaling.  Empty for the main chat session; set to
    /// the SubAgent id for a subagent's log.  This is the lookup key the resume
    /// child (DIS-941 next step) uses to find a closed subagent's session by id.
    /// Soft field: absent in legacy index lines (read as empty, not corrupt).
    std::string agent_id;
};

// =============================================================================
// default_index_path()
//
// Returns the canonical index path: config_dir() / "sessions" / "index.json".
// Calls batbox::paths::config_dir() which honours $BATBOX_CONFIG_DIR.
// Does NOT create the file or its parent directories.
// =============================================================================
[[nodiscard]] std::filesystem::path default_index_path();

// =============================================================================
// append_index_record()
//
// Serialises `rec` to a single JSON line and appends it atomically to
// `index_path` using O_APPEND | O_WRONLY | O_CREAT.  The line is terminated
// with '\n' and the file descriptor is fsync'd before close.
//
// If `index_path`'s parent directory does not exist it is created
// (create_directories) before opening.
//
// Returns:
//   Ok(void)    — line written and synced successfully.
//   Err(string) — errno description on open/write/fsync failure.
//
// Thread / process safety:
//   Safe across threads and processes on local POSIX filesystems because
//   a single write() of the serialised line (including '\n') is < PIPE_BUF
//   bytes as long as the record is under 4096 bytes.  Records are typically
//   < 512 bytes.  NFS is NOT safe (noted caveat only; not guarded).
//
// Blueprint contract:
//   Result<void> append_index_record(const std::filesystem::path& index_path,
//                                    const SessionIndexRecord& rec)
// =============================================================================
[[nodiscard]] Result<void>
append_index_record(const std::filesystem::path& index_path,
                    const SessionIndexRecord& rec);

// =============================================================================
// read_latest_per_id()
//
// Streams `index_path` line-by-line, keeping the record with the highest
// updated_at for each unique id (last-record-wins on equal updated_at).
// Returns the `n` most-recently-updated unique sessions, sorted by updated_at
// descending (most recent first).
//
// Corrupt / blank / non-JSON lines are logged at WARN level and counted in
// `corrupt_lines_skipped` of the returned summary; they never cause a failure.
//
// Performance contract: < 50 ms wall time for an index containing 10 000 entries
// on local SSD (M2-class hardware).  The implementation reads the file in one
// buffered pass and builds the map in O(N) time.
//
// Returns:
//   Ok(vector<SessionIndexRecord>) — empty vector when file does not exist.
//   Err(string)                    — only on hard I/O errors (permission denied,
//                                    IO error mid-read, etc.).  Missing file is
//                                    NOT an error — returns Ok({}).
//
// Blueprint contract:
//   Result<std::vector<SessionIndexRecord>>
//       read_latest_per_id(const std::filesystem::path& index_path, size_t n=20)
// =============================================================================
[[nodiscard]] Result<std::vector<SessionIndexRecord>>
read_latest_per_id(const std::filesystem::path& index_path, size_t n = 20);

// =============================================================================
// rebuild_from_dir()
//
// Scans `sessions_dir` for files matching *.json and *.json.gz, reads each
// one, extracts the minimal metadata needed to build a SessionIndexRecord,
// and writes a fresh index to `index_path`.
//
// Existing content of `index_path` is replaced atomically (write to a
// `.tmp` sibling, then rename).
//
// Files that cannot be opened or parsed are skipped with a WARN log and counted
// in the returned uint64_t (number of files skipped).
//
// This is a potentially slow, blocking operation — call it from a background
// thread (SessionRecovery owns scheduling).
//
// Returns:
//   Ok(uint64_t)  — number of session files successfully indexed.
//   Err(string)   — if the sessions directory cannot be opened.
//
// Blueprint contract:
//   Result<uint64_t> rebuild_from_dir(const std::filesystem::path& sessions_dir,
//                                     const std::filesystem::path& index_path)
// =============================================================================
[[nodiscard]] Result<uint64_t>
rebuild_from_dir(const std::filesystem::path& sessions_dir,
                 const std::filesystem::path& index_path);

// =============================================================================
// is_corrupt()
//
// Returns true if the most recent attempt to open or parse `index_path`
// encountered any error — including:
//   - File open failure (permission denied, I/O error)
//   - At least one skipped (corrupt) line detected by read_latest_per_id
//
// This function is a lightweight stateless heuristic: it calls
// read_latest_per_id(index_path, 0) and checks whether any lines were corrupt.
// The result is NOT cached.
//
// Returns false when:
//   - The file does not exist (absence is not corruption; rebuild_from_dir
//     creates a fresh index).
//   - read_latest_per_id succeeds with zero corrupt lines.
//
// Blueprint contract:
//   bool is_corrupt(const std::filesystem::path& index_path)
// =============================================================================
[[nodiscard]] bool is_corrupt(const std::filesystem::path& index_path);

} // namespace batbox::session
