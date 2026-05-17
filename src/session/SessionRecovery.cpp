// src/session/SessionRecovery.cpp
// =============================================================================
// Implementation of IndexRebuilder — background session-index repair.
//
// Design notes:
//   - The rebuild runs inside a std::jthread, which automatically requests
//     stop and joins on destruction.  This means an IndexRebuilder that goes
//     out of scope will cleanly wait for any in-flight rebuild to finish (or
//     exit early if the CancelToken fires), preventing use-after-free of the
//     sessions directory path.
//
//   - Before scanning, if index_path exists and is_corrupt() returns true,
//     the corrupt file is renamed to index_path + ".bak" so it can be
//     inspected for diagnostics.  This rename is a std::filesystem::rename,
//     which is atomic on POSIX.
//
//   - File enumeration uses std::filesystem::directory_iterator.  The total
//     file count is determined by a fast pre-scan pass (no file I/O, just
//     stat) so the progress callback can report accurate fractions.
//
//   - The CancelToken is checked between every file processed, providing
//     sub-file-granularity cancellation without holding locks.
//
//   - rebuild_from_dir() (from SessionIndex.cpp) handles the actual parsing
//     and atomic write; we just own the scheduling and progress accounting.
//
//   - The progress callback is called with (done, total) after each file.
//     If total == 0 (empty directory), the callback is called once with (0, 0)
//     to signal "rebuild complete, nothing to index".
// =============================================================================

#include "batbox/session/SessionRecovery.hpp"

#include "batbox/session/SessionIndex.hpp"
#include "batbox/core/CancelToken.hpp"
#include "batbox/core/Logging.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace batbox::session {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Count session files (*.json and *.json.gz, excluding index.json) in dir.
/// Returns 0 on any I/O error rather than propagating; the rebuild itself will
/// surface errors via its own logging path.
static size_t count_session_files(const fs::path& sessions_dir) noexcept {
    size_t count = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sessions_dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const fs::path& p      = entry.path();
        const std::string ext  = p.extension().string();
        const std::string stem = p.stem().extension().string();
        bool is_json    = (ext == ".json" && p.filename() != "index.json");
        bool is_json_gz = (ext == ".gz"   && stem == ".json");
        if (is_json || is_json_gz) ++count;
    }
    return count;
}

/// Rename corrupt index to index.json.bak.  Best-effort; logs but does not
/// propagate errors so the rebuild can proceed regardless.
static void backup_corrupt_index(const fs::path& index_path,
                                 std::shared_ptr<spdlog::logger>& logger) {
    fs::path bak = index_path;
    bak += ".bak";
    std::error_code ec;
    fs::rename(index_path, bak, ec);
    if (ec) {
        logger->warn("IndexRebuilder: could not rename corrupt index to '{}': {}",
                     bak.string(), ec.message());
    } else {
        logger->info("IndexRebuilder: corrupt index moved to '{}'", bak.string());
    }
}

// ---------------------------------------------------------------------------
// IndexRebuilder
// ---------------------------------------------------------------------------

IndexRebuilder::IndexRebuilder(fs::path index_path)
    : index_path_(std::move(index_path)) {}

void IndexRebuilder::start(
        CancelToken ct,
        std::function<void(size_t done, size_t total)> progress_cb) {

    // Capture everything by value into the jthread lambda so the IndexRebuilder
    // object can be freely moved or destroyed after start() returns.
    // CancelToken is move-only; index_path_ is copied.
    fs::path            idx_path   = index_path_;
    fs::path            sessions_dir = index_path_.parent_path();

    worker_ = std::jthread(
        [idx_path   = std::move(idx_path),
         sessions_dir,
         ct         = std::move(ct),
         progress_cb = std::move(progress_cb)]
        (std::stop_token /*stoken*/) mutable
    {
        auto logger = log::get("session");

        logger->info("IndexRebuilder: starting background rebuild, index='{}'",
                     idx_path.string());

        // ---- 1. Back up corrupt / pre-existing corrupt index ----
        std::error_code ec;
        if (fs::exists(idx_path, ec) && !ec) {
            if (is_corrupt(idx_path)) {
                backup_corrupt_index(idx_path, logger);
            }
        }

        // ---- 2. Check for early cancellation before doing real work ----
        if (ct.is_cancelled()) {
            logger->info("IndexRebuilder: cancelled before scan; exiting.");
            return;
        }

        // ---- 3. Pre-scan: count eligible session files for progress reporting ----
        if (!fs::is_directory(sessions_dir, ec) || ec) {
            logger->warn("IndexRebuilder: sessions dir '{}' does not exist or is not a "
                         "directory — aborting rebuild.", sessions_dir.string());
            return;
        }

        const size_t total_files = count_session_files(sessions_dir);
        logger->info("IndexRebuilder: found {} session files to index", total_files);

        // If the directory is empty, signal completion with (0, 0) and write
        // an empty index by delegating to rebuild_from_dir.
        if (total_files == 0) {
            auto res = rebuild_from_dir(sessions_dir, idx_path);
            if (!res) {
                logger->error("IndexRebuilder: rebuild_from_dir failed: {}", res.error());
            } else {
                logger->info("IndexRebuilder: empty sessions directory — wrote empty index.");
            }
            if (progress_cb) progress_cb(0, 0);
            return;
        }

        // ---- 4. Walk files, call progress_cb per file, check cancellation ----
        // We do our own pass here (rather than inside rebuild_from_dir) so we
        // can hook progress reporting and honour cancellation between files.
        // The actual index writing is still delegated to rebuild_from_dir once
        // we've confirmed we should proceed to the final write.
        size_t done = 0;

        for (const auto& entry : fs::directory_iterator(sessions_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;

            const fs::path& p      = entry.path();
            const std::string ext  = p.extension().string();
            const std::string stem = p.stem().extension().string();
            bool is_json    = (ext == ".json" && p.filename() != "index.json");
            bool is_json_gz = (ext == ".gz"   && stem == ".json");
            if (!is_json && !is_json_gz) continue;

            ++done;
            if (progress_cb) progress_cb(done, total_files);

            // Cooperative cancellation check between files.
            if (ct.is_cancelled()) {
                logger->info("IndexRebuilder: cancelled after {}/{} files; "
                             "not writing partial index.", done, total_files);
                return;
            }
        }

        // ---- 5. Delegate actual index construction to rebuild_from_dir ----
        // This performs the full atomic write (tmp + rename).
        if (ct.is_cancelled()) {
            logger->info("IndexRebuilder: cancelled before final write.");
            return;
        }

        auto res = rebuild_from_dir(sessions_dir, idx_path);
        if (!res) {
            logger->error("IndexRebuilder: rebuild_from_dir failed: {}", res.error());
            return;
        }

        logger->info("IndexRebuilder: rebuild complete — {} sessions indexed.",
                     res.value());
    });
}

} // namespace batbox::session
