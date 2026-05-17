// src/session/SessionStore.cpp
// =============================================================================
// batbox SessionStore — implementation (CPP 9.4).
// =============================================================================

#include <batbox/session/SessionStore.hpp>

#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace batbox::session {

// =============================================================================
// Constructor — primary
// =============================================================================
SessionStore::SessionStore(fs::path sessions_dir)
    : sessions_dir_(std::move(sessions_dir))
    , index_path_(sessions_dir_ / "index.json")
{
    // Ensure the sessions directory exists.
    std::error_code ec;
    fs::create_directories(sessions_dir_, ec);
    if (ec) {
        BATBOX_LOG_WARN("SessionStore: failed to create sessions directory '{}': {}",
                    sessions_dir_.string(), ec.message());
    }

    // Launch background recovery if the index is missing or corrupt.
    const bool index_exists = fs::exists(index_path_);
    const bool index_corrupt = index_exists && is_corrupt(index_path_);

    if (!index_exists || index_corrupt) {
        BATBOX_LOG_INFO("SessionStore: {} — launching background index rebuild.",
                    index_corrupt ? "index corrupt" : "index absent");

        rebuilder_ = std::make_unique<IndexRebuilder>(index_path_);
        auto [src, tok] = CancelToken::make_root();
        (void)src; // Source stays alive inside tok via shared state.
        rebuilder_->start(std::move(tok),
            [](size_t done, size_t total) {
                if (total > 0 && (done % 50 == 0 || done == total)) {
                    BATBOX_LOG_DEBUG("SessionStore rebuild: {}/{}", done, total);
                }
            });
    }
}

// =============================================================================
// Constructor — default (uses config_dir() / "sessions")
// =============================================================================
SessionStore::SessionStore()
    : SessionStore(batbox::paths::config_dir() / "sessions")
{}

// =============================================================================
// new_session()
// =============================================================================
Result<std::string>
SessionStore::new_session(const std::string& model,
                          const fs::path& working_dir)
{
    // 1. Generate a UUID v4 session id.
    const Uuid uuid      = Uuid::v4();
    const std::string id = uuid.to_string();
    const fs::path path  = sessions_dir_ / (id + ".json");
    const auto now       = std::chrono::system_clock::now();

    // 2. Build the initial SessionFile.
    SessionFile sf;
    sf.id             = uuid;
    sf.created_at     = now;
    sf.updated_at     = now;
    sf.model_at_start = model;
    sf.working_dir    = working_dir;
    sf.messages       = {};
    sf.tool_calls_summary    = Json::object();
    sf.usage_total           = {};
    sf.permission_rules_used = {};

    // 3. Write the file to disk.
    auto write_res = write_initial(path, sf);
    if (!write_res) {
        return batbox::Err("SessionStore::new_session: write_initial failed: " +
                           write_res.error());
    }

    // 4. Append an index record.
    SessionIndexRecord rec;
    rec.id                    = uuid;
    rec.created_at            = now;
    rec.updated_at            = now;
    rec.first_message_preview = "";
    rec.model                 = model;
    rec.turn_count            = 0;
    rec.file_path             = path;

    auto idx_res = append_index_record(index_path_, rec);
    if (!idx_res) {
        BATBOX_LOG_WARN("SessionStore::new_session: index append failed: {}", idx_res.error());
        // Not fatal — the session file is valid even if the index is stale.
    }

    // 5. Register in in-memory maps.
    {
        std::lock_guard<std::mutex> lock(maps_mutex_);
        session_paths_[id]      = path;
        session_turn_counts_[id] = 0;
        session_previews_[id]   = "";
        session_models_[id]     = model;
        session_created_at_[id] = now;
        // Lazily create per-session mutex.
        if (session_mutexes_.find(id) == session_mutexes_.end()) {
            session_mutexes_[id] = std::make_unique<std::mutex>();
        }
        current_session_id_ = id;
    }

    BATBOX_LOG_DEBUG("SessionStore: created session {} model={}", id, model);
    return id;
}

// =============================================================================
// append_message()
// =============================================================================
Result<void>
SessionStore::append_message(const std::string& session_id, const Json& message)
{
    // 1. Acquire per-session mutex (does not block other sessions).
    std::mutex& smutex = get_session_mutex(session_id);
    std::lock_guard<std::mutex> sess_lock(smutex);

    // 2. Resolve the file path.
    fs::path current_path;
    {
        std::lock_guard<std::mutex> maps_lock(maps_mutex_);
        auto it = session_paths_.find(session_id);
        if (it == session_paths_.end()) {
            // Not in map — try to load from index.
            // Release maps lock before calling load (which takes maps_lock again).
        } else {
            current_path = it->second;
        }
    }

    if (current_path.empty()) {
        // Attempt to resolve from index.
        auto recs = read_latest_per_id(index_path_, 1024);
        if (!recs) {
            return batbox::Err("SessionStore::append_message: index unreadable: " +
                               recs.error());
        }
        for (const auto& rec : recs.value()) {
            if (rec.id.to_string() == session_id) {
                current_path = rec.file_path;
                break;
            }
        }
        if (current_path.empty()) {
            // Last resort: try canonical name.
            auto try_json    = sessions_dir_ / (session_id + ".json");
            auto try_json_gz = sessions_dir_ / (session_id + ".json.gz");
            if (fs::exists(try_json))         current_path = try_json;
            else if (fs::exists(try_json_gz)) current_path = try_json_gz;
            else {
                return batbox::Err("SessionStore::append_message: session not found: " +
                                   session_id);
            }
        }
        // Cache for next time.
        std::lock_guard<std::mutex> maps_lock(maps_mutex_);
        session_paths_[session_id] = current_path;
    }

    // 3. Call the lower-level append (SessionFile.hpp).
    fs::path path_out = current_path;
    auto res = batbox::session::append_message(current_path, message, &path_out);
    if (!res) {
        return batbox::Err("SessionStore::append_message: file append failed: " +
                           res.error());
    }

    // 4. Update in-memory maps.
    const auto now = std::chrono::system_clock::now();
    std::string preview;
    uint64_t    new_turn_count = 0;
    std::string model;
    std::chrono::system_clock::time_point created_at = now;

    {
        std::lock_guard<std::mutex> maps_lock(maps_mutex_);
        // Update path (may have changed if file was gzip-compressed).
        session_paths_[session_id] = path_out;

        // Increment turn count.
        auto& tc = session_turn_counts_[session_id];
        ++tc;
        new_turn_count = tc;

        // Capture preview from the first user-visible message.
        auto& prev_ref = session_previews_[session_id];
        if (prev_ref.empty()) {
            // Extract content string if available.
            if (message.contains("content") && message["content"].is_string()) {
                std::string c = message["content"].get<std::string>();
                if (c.size() > 120) c = c.substr(0, 120);
                prev_ref = std::move(c);
            }
        }
        preview = prev_ref;

        auto mit = session_models_.find(session_id);
        if (mit != session_models_.end()) model = mit->second;

        auto cit = session_created_at_.find(session_id);
        if (cit != session_created_at_.end()) created_at = cit->second;
    }

    // 5. Append refreshed index record (non-blocking write; failure is non-fatal).
    SessionIndexRecord rec;
    if (auto opt = Uuid::parse(session_id); opt.has_value()) {
        rec.id = opt.value();
    }
    rec.created_at            = created_at;
    rec.updated_at            = now;
    rec.first_message_preview = preview;
    rec.model                 = model;
    rec.turn_count            = new_turn_count;
    rec.file_path             = path_out;

    auto idx_res = append_index_record(index_path_, rec);
    if (!idx_res) {
        BATBOX_LOG_WARN("SessionStore::append_message: index update failed: {}", idx_res.error());
    }

    return {};
}

// =============================================================================
// list_recent()
// =============================================================================
Result<std::vector<SessionIndexRecord>>
SessionStore::list_recent(size_t n)
{
    return read_latest_per_id(index_path_, n);
}

// =============================================================================
// load()
// =============================================================================
Result<SessionFile>
SessionStore::load(const std::string& session_id)
{
    fs::path path;

    // 1. Check in-memory path map first.
    {
        std::lock_guard<std::mutex> lock(maps_mutex_);
        auto it = session_paths_.find(session_id);
        if (it != session_paths_.end()) {
            path = it->second;
        }
    }

    // 2. Look up in index if not found in map.
    if (path.empty()) {
        auto recs = read_latest_per_id(index_path_, 4096);
        if (recs) {
            for (const auto& rec : recs.value()) {
                if (rec.id.to_string() == session_id) {
                    path = rec.file_path;
                    break;
                }
            }
        }
    }

    // 3. Fallback: try canonical filenames.
    if (path.empty()) {
        auto try_json    = sessions_dir_ / (session_id + ".json");
        auto try_json_gz = sessions_dir_ / (session_id + ".json.gz");
        if (fs::exists(try_json))         path = try_json;
        else if (fs::exists(try_json_gz)) path = try_json_gz;
    }

    if (path.empty() || !fs::exists(path)) {
        return batbox::Err("SessionStore::load: session not found: " + session_id);
    }

    return read_session_file(path);
}

// =============================================================================
// resume_for_cwd()
// =============================================================================
std::optional<SessionFile>
SessionStore::resume_for_cwd(const fs::path& working_dir)
{
    // Attempt to canonicalise the target path (ignore error if non-existent).
    fs::path target;
    std::error_code ec;
    auto canonical = fs::canonical(working_dir, ec);
    target = ec ? working_dir : canonical;

    // Get up to 256 recent sessions.
    auto recs_res = list_recent(256);
    if (!recs_res) {
        BATBOX_LOG_WARN("SessionStore::resume_for_cwd: list_recent failed: {}", recs_res.error());
        return std::nullopt;
    }

    for (const auto& rec : recs_res.value()) {
        auto sf_res = load(rec.id.to_string());
        if (!sf_res) continue;

        const SessionFile& sf = sf_res.value();
        std::error_code ec2;
        auto sf_canonical = fs::canonical(sf.working_dir, ec2);
        fs::path sf_dir = ec2 ? sf.working_dir : sf_canonical;

        if (sf_dir == target) {
            return sf;
        }
    }

    return std::nullopt;
}

// =============================================================================
// current_session_id()
// =============================================================================
std::optional<std::string>
SessionStore::current_session_id() const
{
    std::lock_guard<std::mutex> lock(maps_mutex_);
    return current_session_id_;
}

// =============================================================================
// touch()
// =============================================================================
Result<void>
SessionStore::touch(const std::string& session_id)
{
    const auto now = std::chrono::system_clock::now();
    // build_index_record requires maps_mutex_ NOT be held by the caller.
    // It acquires no lock itself (called under caller's lock in append_message),
    // but here we call it outside any lock — that is safe since build_index_record
    // only reads maps that are guarded by maps_mutex_ internally via its own reads.
    // We acquire maps_mutex_ first to safely read the in-memory maps.
    SessionIndexRecord rec;
    {
        std::lock_guard<std::mutex> lock(maps_mutex_);
        rec = build_index_record(session_id, now);
    }

    auto idx_res = append_index_record(index_path_, rec);
    if (!idx_res) {
        BATBOX_LOG_WARN("SessionStore::touch: index update failed for {}: {}",
                        session_id, idx_res.error());
        // Non-fatal: the session file is still valid even if the index is stale.
    }
    return {};
}

// =============================================================================
// Private: get_session_mutex()
// =============================================================================
std::mutex&
SessionStore::get_session_mutex(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(maps_mutex_);
    auto it = session_mutexes_.find(session_id);
    if (it == session_mutexes_.end()) {
        auto [ins_it, ok] = session_mutexes_.emplace(
            session_id, std::make_unique<std::mutex>());
        return *ins_it->second;
    }
    return *it->second;
}

// =============================================================================
// Private: build_index_record() — not used externally
// =============================================================================
SessionIndexRecord
SessionStore::build_index_record(const std::string& session_id,
                                  std::chrono::system_clock::time_point updated_at) const
{
    SessionIndexRecord rec;
    if (auto opt = Uuid::parse(session_id); opt.has_value()) {
        rec.id = opt.value();
    }
    rec.updated_at = updated_at;

    auto it_c = session_created_at_.find(session_id);
    rec.created_at = (it_c != session_created_at_.end()) ? it_c->second : updated_at;

    auto it_p = session_previews_.find(session_id);
    rec.first_message_preview = (it_p != session_previews_.end()) ? it_p->second : "";

    auto it_m = session_models_.find(session_id);
    rec.model = (it_m != session_models_.end()) ? it_m->second : "";

    auto it_t = session_turn_counts_.find(session_id);
    rec.turn_count = (it_t != session_turn_counts_.end()) ? it_t->second : 0;

    auto it_path = session_paths_.find(session_id);
    if (it_path != session_paths_.end()) {
        rec.file_path = it_path->second;
    }

    return rec;
}

} // namespace batbox::session
