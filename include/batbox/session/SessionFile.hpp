// include/batbox/session/SessionFile.hpp
// =============================================================================
// batbox Session File — per-session JSON shape and crash-safe I/O.
//
// Design (ned-cpp.md §2.C9 + pmdraft.md F5):
//
//   On-disk format:
//     ~/.batbox/sessions/<uuid>.json       — plain JSON, up to 1 MB
//     ~/.batbox/sessions/<uuid>.json.gz    — gzip-compressed above 1 MB
//
//   JSON schema (top-level fields):
//     {
//       "id":                  "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
//       "created_at":          "2026-05-15T12:00:00Z",   // ISO 8601 UTC
//       "updated_at":          "2026-05-15T12:05:00Z",
//       "model_at_start":      "gpt-4o",
//       "working_dir":         "/home/user/project",
//       "messages":            [ ... ],
//       "tool_calls_summary":  { ... },
//       "usage_total":         { "prompt_tokens": N, "completion_tokens": N },
//       "permission_rules_used": [ ... ]
//     }
//
//   Append strategy (crash-safe):
//     write_initial() writes the full skeleton with an empty messages array.
//     append_message() opens in "r+" mode, seeks to just before the closing
//     "]\n}" sequence at the end of the file, overwrites with ",<msg>\n]\n}"
//     (first message omits the leading comma), then fsyncs.
//     Recovery: on read, if the tail is malformed, the file is truncated back
//     to the last valid "]\n}" position.
//
//   Gzip:
//     write_initial() + append_message() always write plain JSON.
//     After the file exceeds GZIP_THRESHOLD_BYTES (1 MB), the caller should
//     compact via gzip; this is done automatically inside save_compressed()
//     which rewrites the whole file as <uuid>.json.gz and removes the .json.
//     read_session_file() auto-detects the extension.
//
// Thread-safety:
//   None — callers must serialise concurrent access per session.
//
// Build (standalone, no CMake needed — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_session_file.cpp src/session/SessionFile.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libz.a \
//       -o /tmp/test_session_file && /tmp/test_session_file
// =============================================================================

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace batbox::session {

// =============================================================================
// Constants
// =============================================================================

/// Files at or above this byte count are gzip-compressed on save.
inline constexpr std::uintmax_t GZIP_THRESHOLD_BYTES = 1'000'000; // 1 MB

// =============================================================================
// UsageTotal — token accounting for the whole session.
// =============================================================================
struct UsageTotal {
    long long prompt_tokens     = 0;
    long long completion_tokens = 0;

    [[nodiscard]] Json to_json() const;
    [[nodiscard]] static UsageTotal from_json(const Json& j);
};

// =============================================================================
// SessionFile struct — the in-memory representation of a session file.
// =============================================================================
struct SessionFile {
    Uuid        id;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    std::string model_at_start;
    std::filesystem::path                  working_dir;
    std::vector<Json>                      messages;
    Json                                   tool_calls_summary;  // object
    UsageTotal                             usage_total;
    std::vector<Json>                      permission_rules_used;

    /// Serialise the whole struct to a nlohmann::json object.
    [[nodiscard]] Json to_json() const;

    /// Deserialise from a nlohmann::json object.
    /// Returns Err if required fields are absent or malformed.
    [[nodiscard]] static Result<SessionFile> from_json(const Json& j);
};

// =============================================================================
// Free functions — the public I/O API
// =============================================================================

/// Create a new session file at `path` with `sf.messages` written as an empty
/// array (or with whatever messages are already in `sf`).
///
/// Writes atomically: renders to a temp file alongside `path`, then renames.
/// Returns Err on any I/O or serialisation failure.
[[nodiscard]] Result<void>
write_initial(const std::filesystem::path& path, const SessionFile& sf);

/// Incrementally append one message JSON object to an existing session file.
///
/// Strategy (ned-cpp.md §2.C9):
///   1. Open the file for reading and writing.
///   2. Scan backwards from EOF to find the last valid "]\n}" marker.
///   3. Seek to that position; write ",<msg>\n]\n}" (no leading comma for the
///      first message, i.e. the messages array was previously empty "[]").
///   4. Truncate any bytes that were beyond the insertion point.
///   5. fsync, close.
///
/// After a crash the file may be left with a partial write beyond the last
/// valid marker; read_session_file() recovers by truncating to that marker.
///
/// If after the append the file size exceeds GZIP_THRESHOLD_BYTES, the file
/// is automatically compacted via save_compressed(): the plain .json is
/// replaced by a .json.gz and the original is removed.  The caller's `path`
/// variable is updated to the new .json.gz path via the out-parameter
/// `path_out` (set even on no-compression; set to original path in that case).
///
/// Returns Err on any I/O failure.
[[nodiscard]] Result<void>
append_message(const std::filesystem::path& path,
               const Json& message,
               std::filesystem::path* path_out = nullptr);

/// Read and deserialise a session file.
///
/// Auto-detects format:
///   - `.json`    — plain JSON read, parse
///   - `.json.gz` — decompress via zlib, then parse
///
/// On malformed tail (crash recovery): scans backwards for "]\n}" and
/// truncates the file to that position before parsing.  Returns the recovered
/// data as Ok; does NOT return Err merely due to a truncated tail.
///
/// Returns Err on: file not found, decompression failure, JSON parse failure,
/// or the `id` field being absent/malformed.
[[nodiscard]] Result<SessionFile>
read_session_file(const std::filesystem::path& path);

/// Rewrite the entire session as a gzip-compressed file.
///
/// Writes to `<stem>.json.gz` atomically (temp + rename), then removes the
/// original `path` (which may be `.json` or `.json.gz`).
/// Returns the new .json.gz path on success.
[[nodiscard]] Result<std::filesystem::path>
save_compressed(const std::filesystem::path& path, const SessionFile& sf);

} // namespace batbox::session
