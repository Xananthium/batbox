#pragma once

#include <batbox/core/Json.hpp>
#include <filesystem>
#include <functional>
#include <mutex>

namespace batbox::agents {

// Each worker has a small atomic manifest, separate from its append-only
// Conversation journal. Acknowledged inbox entries survive process restart.
class AgentJournal {
public:
    AgentJournal(std::filesystem::path path, Json state);
    Json read() const;
    void update(const std::function<void(Json&)>& edit);
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    Json state_;
};

} // namespace batbox::agents
