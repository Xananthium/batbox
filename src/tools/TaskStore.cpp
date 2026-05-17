// src/tools/TaskStore.cpp
//
// Implementation of batbox::tools::TaskStore — persistent task storage.
//
// Blueprint contract: CPP 5.16
//   - Storage: ~/.batbox/tasks.json (JSON array)
//   - Atomic writes: write-to-tmp then fs::rename
//   - Thread-safe: std::mutex guards load/mutate/save cycle
//   - Task fields: id, title, description, status, parent_id, tags,
//                  created_at, updated_at

#include <batbox/tools/TaskStore.hpp>
#include <batbox/core/Uuid.hpp>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::tools {

// =============================================================================
// Task — serialisation helpers
// =============================================================================

Json Task::to_json() const {
    Json tags_arr = Json::array();
    for (const auto& t : tags) {
        tags_arr.push_back(t);
    }
    return Json{
        {"id",          id},
        {"title",       title},
        {"description", description},
        {"status",      status},
        {"parent_id",   parent_id},
        {"tags",        std::move(tags_arr)},
        {"created_at",  created_at},
        {"updated_at",  updated_at},
    };
}

bool Task::from_json(const Json& j, Task& out) {
    if (!j.is_object()) return false;

    // id (required string)
    auto it = j.find("id");
    if (it == j.end() || !it->is_string()) return false;
    out.id = it->get<std::string>();

    // title (required string)
    it = j.find("title");
    if (it == j.end() || !it->is_string()) return false;
    out.title = it->get<std::string>();

    // description (optional string, default "")
    it = j.find("description");
    if (it != j.end() && it->is_string()) {
        out.description = it->get<std::string>();
    } else {
        out.description = {};
    }

    // status (required string)
    it = j.find("status");
    if (it == j.end() || !it->is_string()) return false;
    out.status = it->get<std::string>();

    // parent_id (optional string, default "")
    it = j.find("parent_id");
    if (it != j.end() && it->is_string()) {
        out.parent_id = it->get<std::string>();
    } else {
        out.parent_id = {};
    }

    // tags (optional array of strings)
    out.tags.clear();
    it = j.find("tags");
    if (it != j.end() && it->is_array()) {
        for (const auto& tag_item : *it) {
            if (tag_item.is_string()) {
                out.tags.push_back(tag_item.get<std::string>());
            }
        }
    }

    // created_at (required string)
    it = j.find("created_at");
    if (it == j.end() || !it->is_string()) return false;
    out.created_at = it->get<std::string>();

    // updated_at (required string)
    it = j.find("updated_at");
    if (it == j.end() || !it->is_string()) return false;
    out.updated_at = it->get<std::string>();

    return true;
}

// =============================================================================
// TaskStore — construction
// =============================================================================

TaskStore::TaskStore(fs::path tasks_path)
    : tasks_path_(std::move(tasks_path))
{}

// =============================================================================
// TaskStore — static helpers
// =============================================================================

fs::path TaskStore::default_path() {
    const char* home = std::getenv("HOME");
    fs::path base;
    if (home && home[0] != '\0') {
        base = fs::path(home);
    } else {
        base = fs::temp_directory_path() / "batbox_fallback";
    }
    return base / ".batbox" / "tasks.json";
}

const fs::path& TaskStore::path() const noexcept {
    return tasks_path_;
}

bool TaskStore::is_valid_status(const std::string& status) {
    return status == "pending"
        || status == "in_progress"
        || status == "completed"
        || status == "failed";
}

std::string TaskStore::now_iso8601() {
    const auto now = std::chrono::system_clock::now();
    std::time_t t  = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

bool TaskStore::ensure_dir() const {
    std::error_code ec;
    fs::create_directories(tasks_path_.parent_path(), ec);
    return !ec;
}

// =============================================================================
// TaskStore — load / save
// =============================================================================

std::vector<Task> TaskStore::load() const {
    std::error_code ec;
    if (!fs::exists(tasks_path_, ec) || ec) {
        return {};
    }

    std::ifstream in(tasks_path_, std::ios::in | std::ios::binary);
    if (!in) return {};

    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string raw = buf.str();
    if (raw.empty()) return {};

    Json arr;
    try {
        arr = Json::parse(raw);
    } catch (...) {
        return {};
    }

    if (!arr.is_array()) return {};

    std::vector<Task> result;
    result.reserve(arr.size());
    for (const auto& item : arr) {
        Task t;
        if (Task::from_json(item, t)) {
            result.push_back(std::move(t));
        }
    }
    return result;
}

bool TaskStore::save(const std::vector<Task>& tasks) const {
    if (!ensure_dir()) return false;

    Json arr = Json::array();
    for (const auto& t : tasks) {
        arr.push_back(t.to_json());
    }

    const std::string serialised = arr.dump(2);
    const fs::path tmp = tasks_path_.parent_path()
                       / (tasks_path_.filename().string() + ".tmp");

    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) return false;
        out.write(serialised.data(),
                  static_cast<std::streamsize>(serialised.size()));
        if (!out) return false;
    }

    std::error_code ec;
    fs::rename(tmp, tasks_path_, ec);
    if (ec) {
        // Cross-device fallback: copy + remove tmp.
        fs::copy_file(tmp, tasks_path_,
                      fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        return !ec;
    }
    return true;
}

// =============================================================================
// TaskStore — CRUD operations
// =============================================================================

std::optional<Task> TaskStore::create_task(const TaskCreateParams& params) {
    if (params.title.empty()) return std::nullopt;

    const std::string effective_status =
        params.status.empty() ? "pending" : params.status;
    if (!is_valid_status(effective_status)) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Task> tasks = load();

    const std::string ts = now_iso8601();
    Task t;
    t.id          = batbox::Uuid::v4().to_string();
    t.title       = params.title;
    t.description = params.description;
    t.status      = effective_status;
    t.parent_id   = params.parent_id;
    t.tags        = params.tags;
    t.created_at  = ts;
    t.updated_at  = ts;

    tasks.push_back(t);

    if (!save(tasks)) return std::nullopt;
    return t;
}

std::vector<Task> TaskStore::list_tasks(const std::string& status_filter,
                                         const std::string& tag_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Task> all = load();

    if (status_filter.empty() && tag_filter.empty()) {
        return all;
    }

    std::vector<Task> result;
    for (const auto& t : all) {
        if (!status_filter.empty() && t.status != status_filter) {
            continue;
        }
        if (!tag_filter.empty()) {
            bool has_tag = false;
            for (const auto& tag : t.tags) {
                if (tag == tag_filter) { has_tag = true; break; }
            }
            if (!has_tag) continue;
        }
        result.push_back(t);
    }
    return result;
}

std::optional<Task> TaskStore::get_task(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<Task> all = load();
    for (const auto& t : all) {
        if (t.id == id) return t;
    }
    return std::nullopt;
}

bool TaskStore::update_task(const std::string& id,
                             const TaskUpdateParams& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Task> tasks = load();

    bool found = false;
    for (auto& t : tasks) {
        if (t.id != id) continue;
        found = true;

        if (params.title.has_value())       t.title       = *params.title;
        if (params.description.has_value()) t.description = *params.description;
        if (params.status.has_value())      t.status      = *params.status;
        if (params.parent_id.has_value())   t.parent_id   = *params.parent_id;
        if (params.tags.has_value())        t.tags        = *params.tags;
        t.updated_at = now_iso8601();
        break;
    }

    if (!found) return false;
    return save(tasks);
}

} // namespace batbox::tools
