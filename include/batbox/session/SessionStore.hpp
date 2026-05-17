// include/batbox/session/SessionStore.hpp
// =============================================================================
// batbox SessionStore — top-level session facade (CPP 9.4).
//
// SessionStore owns the session directory, the JSONL index, and per-session
// mutexes.  It coordinates SessionFile, SessionIndex, and SessionRecovery
// so no caller needs to touch those lower-level APIs directly.
//
// Typical usage:
//
//   namespace sess = batbox::session;
//
//   // Construct once at startup; pass the sessions directory.
//   sess::SessionStore store(batbox::paths::config_dir() / "sessions");
//
//   // Create a new session (returns the session id string).
//   auto id_res = store.new_session("claude-3-5-sonnet", fs::current_path());
//   std::string sid = id_res.value();
//
//   // Append messages.
//   Json msg = {{"role","user"},{"content","hello"}};
//   store.append_message(sid, msg);
//
//   // List recent sessions.
//   auto recents = store.list_recent(20);
//
//   // Reload a session by id.
//   auto sf = store.load(sid);
//
//   // Resume the most-recently-used session for the current working directory.
//   auto maybe = store.resume_for_cwd(fs::current_path());
//
// Thread-safety:
//   All public methods are thread-safe.  Concurrent calls on DIFFERENT session
//   ids do not block each other.  Concurrent calls on the SAME session id are
//   serialised by a per-session std::mutex inside the mutex map.
//
// Recovery on construction:
//   If the index file is missing or corrupt (batbox::session::is_corrupt()),
//   SessionStore automatically launches an IndexRebuilder in the background.
//   Sessions can still be created and loaded during the rebuild; list_recent()
//   may return incomplete results until the rebuild finishes.
//
// Build (standalone, from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/integration/test_session_resume.cpp \
//       src/session/SessionStore.cpp \
//       src/session/SessionFile.cpp \
//       src/session/SessionIndex.cpp \
//       src/session/SessionRecovery.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/CancelToken.cpp src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libz.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_session_resume && /tmp/test_session_resume
// =============================================================================

#pragma once

#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/core/Uuid.hpp>
#include <batbox/session/SessionFile.hpp>
#include <batbox/session/SessionIndex.hpp>
#include <batbox/session/SessionRecovery.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace batbox::session {

// =============================================================================
// SessionStore — top-level session facade.
// =============================================================================
class SessionStore {
public:
    // -------------------------------------------------------------------------
    // Constructor
    //
    // sessions_dir — absolute path to the directory where session files and the
    //                index live.  Created on demand (create_directories is
    //                called internally).  Defaults to
    //                batbox::paths::config_dir() / "sessions" when the
    //                zero-argument constructor is used.
    //
    // On construction:
    //   1. sessions_dir_ is stored.
    //   2. The directory is created if it does not exist.
    //   3. If the index file is absent or corrupt, an IndexRebuilder is started
    //      in the background (non-blocking).
    // -------------------------------------------------------------------------
    explicit SessionStore(std::filesystem::path sessions_dir);

    /// Default constructor: uses batbox::paths::config_dir() / "sessions".
    SessionStore();

    // Non-copyable; move is allowed (the mutex map stays in the original).
    SessionStore(const SessionStore&)            = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    SessionStore(SessionStore&&)                 = delete;
    SessionStore& operator=(SessionStore&&)      = delete;

    ~SessionStore() = default;

    // -------------------------------------------------------------------------
    // new_session()
    //
    // Creates a new session:
    //   1. Generates a UUID v4 session id.
    //   2. Writes the initial session file (<id>.json) via write_initial().
    //   3. Appends one SessionIndexRecord to the JSONL index.
    //   4. Sets current_session_id_ to the new id.
    //
    // Returns Ok(session_id_string) on success, Err(message) on failure.
    //
    // Blueprint contract:
    //   Result<std::string> new_session(const std::string& model,
    //                                   const std::filesystem::path& working_dir)
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::string>
    new_session(const std::string& model,
                const std::filesystem::path& working_dir);

    // -------------------------------------------------------------------------
    // append_message()
    //
    // Thread-safe per-session message append:
    //   1. Acquires the per-session mutex for session_id.
    //   2. Calls batbox::session::append_message() on the session file.
    //   3. Updates the JSONL index record (increments turn_count, updates
    //      updated_at) by appending a refreshed SessionIndexRecord.
    //
    // The path is tracked internally; if the file was compressed to .json.gz
    // the stored path is updated automatically.
    //
    // Returns Ok(void) on success, Err(message) on any I/O failure.
    //
    // Blueprint contract:
    //   Result<void> append_message(const std::string& session_id,
    //                               const Json& message)
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void>
    append_message(const std::string& session_id, const Json& message);

    // -------------------------------------------------------------------------
    // list_recent()
    //
    // Delegates to batbox::session::read_latest_per_id() on the index file.
    // O(index size): one buffered pass over the JSONL file.
    //
    // Returns Ok(vector<SessionIndexRecord>) sorted by updated_at descending,
    // at most n entries.  Returns Ok({}) when the index does not yet exist.
    //
    // Blueprint contract:
    //   Result<std::vector<SessionIndexRecord>> list_recent(size_t n=20)
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<std::vector<SessionIndexRecord>>
    list_recent(size_t n = 20);

    // -------------------------------------------------------------------------
    // load()
    //
    // Reads and parses the session file for the given session_id.
    //
    // Resolution order:
    //   1. If session_id is tracked in the in-memory path map, use that path.
    //   2. Otherwise look up the most recent index record for session_id and
    //      use its file_path.
    //   3. If the index has no record, attempt <sessions_dir_>/<session_id>.json
    //      and then <sessions_dir_>/<session_id>.json.gz as fallbacks.
    //
    // Returns Ok(SessionFile) on success, Err(message) if not found or corrupt
    // (beyond recoverable crash truncation, which read_session_file handles).
    //
    // Blueprint contract:
    //   Result<SessionFile> load(const std::string& session_id)
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<SessionFile>
    load(const std::string& session_id);

    // -------------------------------------------------------------------------
    // resume_for_cwd()
    //
    // Finds the most recently updated session whose working_dir matches
    // working_dir (exact path comparison after fs::canonical if both exist).
    //
    // Returns std::nullopt if no matching session is found.
    //
    // Implementation:
    //   1. Calls list_recent(256) to get candidates.
    //   2. Iterates from most-recent backwards; returns the first whose
    //      SessionFile::working_dir canonicalises to the same path as
    //      working_dir.  Skips files that cannot be read.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<SessionFile>
    resume_for_cwd(const std::filesystem::path& working_dir);

    // -------------------------------------------------------------------------
    // current_session_id()
    //
    // Returns the session id of the most recently created session via
    // new_session(), or std::nullopt if new_session() has not been called.
    //
    // Blueprint contract:
    //   std::optional<std::string> current_session_id() const
    // -------------------------------------------------------------------------
    [[nodiscard]] std::optional<std::string> current_session_id() const;

    // -------------------------------------------------------------------------
    // touch()
    //
    // Updates the last-accessed timestamp of an existing session by appending
    // a refreshed index record with updated_at = now.  This surfaces the
    // session at the top of list_recent() results after a --resume.
    //
    // The session file itself is NOT modified — only the JSONL index is updated.
    // Returns Ok(void) on success.  Index update failures are logged as warnings
    // and treated as non-fatal (the session is still valid).
    //
    // Blueprint contract:
    //   Result<void> touch(const std::string& session_id)
    // -------------------------------------------------------------------------
    Result<void> touch(const std::string& session_id);

private:
    // Directory where all session files live.
    std::filesystem::path sessions_dir_;

    // Path to the JSONL index file inside sessions_dir_.
    std::filesystem::path index_path_;

    // Most recently created session id (via new_session()); empty = none.
    std::optional<std::string> current_session_id_;

    // Per-session file paths (in case a file was compressed to .json.gz).
    // key: session_id string,  value: current absolute path to the file.
    std::unordered_map<std::string, std::filesystem::path> session_paths_;

    // Per-session turn counts, kept in sync with what we append.
    // Used for accurate index updates without re-reading the file.
    std::unordered_map<std::string, uint64_t> session_turn_counts_;

    // Per-session first-message-preview, captured on first append.
    std::unordered_map<std::string, std::string> session_previews_;

    // Per-session model strings.
    std::unordered_map<std::string, std::string> session_models_;

    // Per-session creation timestamps.
    std::unordered_map<std::string, std::chrono::system_clock::time_point> session_created_at_;

    // Global mutex guarding all in-memory maps above.
    // Per-session file I/O uses the finer-grained per-session mutexes below.
    mutable std::mutex maps_mutex_;

    // Per-session mutexes — separate sessions do NOT block each other.
    // Access via get_session_mutex() which lazily inserts under maps_mutex_.
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> session_mutexes_;

    // Optional background rebuild (launched when index is missing/corrupt).
    std::unique_ptr<IndexRebuilder> rebuilder_;

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    /// Returns (creating if absent) the per-session mutex for session_id.
    /// Caller must NOT hold maps_mutex_ when calling this.
    std::mutex& get_session_mutex(const std::string& session_id);

    /// Builds a SessionIndexRecord from in-memory state for a given session.
    /// Caller must hold the per-session mutex.
    SessionIndexRecord build_index_record(const std::string& session_id,
                                          std::chrono::system_clock::time_point updated_at) const;
};

} // namespace batbox::session
