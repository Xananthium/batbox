// include/batbox/session/SessionRecovery.hpp
// =============================================================================
// batbox SessionRecovery — non-blocking background index rebuild.
//
// Design:
//   When the session index at ~/.batbox/sessions/index.json is missing or
//   corrupt (as detected by batbox::session::is_corrupt()), the application
//   launches an IndexRebuilder rather than blocking the startup path.
//
//   IndexRebuilder::start() spawns a std::jthread.  The thread calls
//   rebuild_from_dir() (from SessionIndex.hpp) which walks the sessions
//   directory, reads each .json / .json.gz file header, and writes a fresh
//   index.json atomically via tmp+rename.
//
//   Progress is reported via a user-supplied callback invoked once per file
//   processed: callback(done, total) where done ≤ total.
//
//   Cancellation is cooperative via batbox::CancelToken: the background
//   thread checks tok.is_cancelled() between files.  When the token fires,
//   the thread exits without writing a partial index (the existing, possibly
//   corrupt index is left in place; the backup, if created, is kept).
//
// Corrupt-index backup:
//   Before launching the rebuild thread, if the index file exists and
//   is_corrupt() returns true, the caller should move it to index.json.bak.
//   IndexRebuilder::start() does this atomically using
//   std::filesystem::rename so the original is preserved for diagnostics.
//
// Usage (typical — non-blocking):
//
//   #include "batbox/session/SessionRecovery.hpp"
//
//   namespace sess = batbox::session;
//
//   auto idx = sess::default_index_path();
//   if (!std::filesystem::exists(idx) || sess::is_corrupt(idx)) {
//       auto sessions_dir = idx.parent_path();
//       auto rebuilder    = std::make_shared<sess::IndexRebuilder>(idx);
//       rebuilder->start(
//           std::move(my_cancel_token),
//           [](size_t done, size_t total) {
//               // update status line, e.g. "(rebuilding 47/210)"
//           });
//       // start() returns immediately; rebuild runs in background.
//   }
//
// Thread-safety:
//   IndexRebuilder is NOT thread-safe with respect to concurrent start()
//   calls.  Call start() exactly once per IndexRebuilder instance.
//   The progress callback is invoked from the background thread; the caller
//   must ensure any shared state it touches is properly synchronised.
//
// Build (standalone, from repo root):
//   c++ -std=c++20 \
//       -Iinclude \
//       -Ibuild/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_session_recovery.cpp \
//       src/session/SessionRecovery.cpp \
//       src/session/SessionIndex.cpp \
//       src/core/Uuid.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/core/Json.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_session_recovery && /tmp/test_session_recovery
// =============================================================================

#pragma once

#include <batbox/core/CancelToken.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <thread>

namespace batbox::session {

// =============================================================================
// IndexRebuilder
//
// Launches a detached background thread that:
//   1. Optionally renames a corrupt index to index.json.bak.
//   2. Walks sessions_dir for *.json and *.json.gz files.
//   3. Reads each file's metadata header.
//   4. Writes a fresh index_path atomically (tmp + rename).
//
// start() returns immediately (non-blocking).
// Progress is published via callback(done, total).
// Cancellation is cooperative via the supplied CancelToken.
// =============================================================================
class IndexRebuilder {
public:
    // -------------------------------------------------------------------------
    // Constructor
    //
    // index_path   — absolute path to the index file to rebuild
    //                (parent directory = sessions_dir for the scan).
    // -------------------------------------------------------------------------
    explicit IndexRebuilder(std::filesystem::path index_path);

    // Non-copyable; the jthread is move-only anyway.
    IndexRebuilder(const IndexRebuilder&)            = delete;
    IndexRebuilder& operator=(const IndexRebuilder&) = delete;

    // Movable.
    IndexRebuilder(IndexRebuilder&&)            noexcept = default;
    IndexRebuilder& operator=(IndexRebuilder&&) noexcept = default;

    // Destructor joins the background thread (jthread semantics: requests
    // stop + joins automatically on destruction).
    ~IndexRebuilder() = default;

    // -------------------------------------------------------------------------
    // start()
    //
    // Launches the background rebuild thread.  Returns immediately.
    //
    // ct           — CancelToken; when cancelled the background thread exits
    //                cooperatively between file scans (no partial index written).
    // progress_cb  — called from the background thread each time a session file
    //                is processed: progress_cb(files_processed, total_files).
    //                May be nullptr (progress is then silently suppressed).
    //
    // Precondition: start() must be called at most once per IndexRebuilder.
    //               Calling it a second time is undefined behaviour.
    //
    // Blueprint contract:
    //   void start(CancelToken ct,
    //              std::function<void(size_t done, size_t total)> progress_cb)
    // -------------------------------------------------------------------------
    void start(CancelToken ct,
               std::function<void(size_t done, size_t total)> progress_cb);

private:
    std::filesystem::path index_path_;
    std::jthread          worker_;
};

} // namespace batbox::session
