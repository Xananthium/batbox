// src/agents/Team.cpp
//
// Implementation of batbox::agents::Team and batbox::agents::TeamRegistry.
//
// Blueprint contract (task CPP 6.6):
//   Team:         named agent group with shared_mutex-guarded KV blackboard.
//   TeamRegistry: singleton map<name, unique_ptr<Team>>, protected by shared_mutex.
//
// Thread-safety model:
//   Team::members_  — protected by members_mtx_ (exclusive mutex, low contention).
//   Team::blackboard_ — protected by blackboard_mtx_ (shared_mutex):
//     * get_kv / blackboard_size take a shared_lock (multiple concurrent readers OK).
//     * set_kv / cas_kv / erase_kv take a unique_lock (exclusive write).
//   TeamRegistry::teams_ — protected by registry_mtx_ (shared_mutex):
//     * get_team / team_names / size take a shared_lock.
//     * create_team / delete_team take a unique_lock.

#include <batbox/agents/Team.hpp>

#include <utility>

namespace batbox::agents {

// =============================================================================
// Team implementation
// =============================================================================

Team::Team(std::string team_name)
    : name(std::move(team_name))
{}

// ---------------------------------------------------------------------------
// Membership
// ---------------------------------------------------------------------------

void Team::add_member(std::string_view agent_id) {
    std::lock_guard<std::mutex> lock{members_mtx_};
    // Idempotent: only add if not already present.
    for (const auto& existing : member_ids_) {
        if (existing == agent_id) {
            return;
        }
    }
    member_ids_.emplace_back(std::string{agent_id});
}

void Team::remove_member(std::string_view agent_id) {
    std::lock_guard<std::mutex> lock{members_mtx_};
    auto it = std::find(member_ids_.begin(), member_ids_.end(), agent_id);
    if (it != member_ids_.end()) {
        member_ids_.erase(it);
    }
}

std::vector<std::string> Team::members() const {
    std::lock_guard<std::mutex> lock{members_mtx_};
    return member_ids_; // snapshot copy
}

// ---------------------------------------------------------------------------
// Broadcast
// ---------------------------------------------------------------------------

void Team::broadcast(std::string_view message) {
    std::lock_guard<std::mutex> lock{broadcast_mtx_};
    pending_broadcasts_.emplace_back(std::string{message});
}

std::vector<std::string> Team::drain_pending_broadcasts() {
    std::lock_guard<std::mutex> lock{broadcast_mtx_};
    std::vector<std::string> result;
    result.swap(pending_broadcasts_);
    return result;
}

// ---------------------------------------------------------------------------
// Blackboard — set_kv
// ---------------------------------------------------------------------------

void Team::set_kv(std::string_view key, const Json& value) {
    std::unique_lock<std::shared_mutex> lock{blackboard_mtx_};
    blackboard_.insert_or_assign(std::string{key}, value);
}

// ---------------------------------------------------------------------------
// Blackboard — get_kv
// ---------------------------------------------------------------------------

std::optional<Json> Team::get_kv(std::string_view key) const {
    std::shared_lock<std::shared_mutex> lock{blackboard_mtx_};
    auto it = blackboard_.find(std::string{key});
    if (it == blackboard_.end()) {
        return std::nullopt;
    }
    return std::optional<Json>{it->second}; // copy-out while holding lock
}

// ---------------------------------------------------------------------------
// Blackboard — cas_kv (compare-and-swap)
// ---------------------------------------------------------------------------

bool Team::cas_kv(std::string_view key,
                   const Json&      expected,
                   const Json&      desired) {
    std::unique_lock<std::shared_mutex> lock{blackboard_mtx_};
    auto it = blackboard_.find(std::string{key});
    if (it == blackboard_.end()) {
        // Key absent — swap only if expected is null (absent == null semantics).
        if (expected.is_null()) {
            blackboard_.emplace(std::string{key}, desired);
            return true;
        }
        return false;
    }
    if (it->second != expected) {
        return false;
    }
    it->second = desired;
    return true;
}

// ---------------------------------------------------------------------------
// Blackboard — erase_kv
// ---------------------------------------------------------------------------

void Team::erase_kv(std::string_view key) {
    std::unique_lock<std::shared_mutex> lock{blackboard_mtx_};
    blackboard_.erase(std::string{key});
}

// ---------------------------------------------------------------------------
// Blackboard — blackboard_size
// ---------------------------------------------------------------------------

std::size_t Team::blackboard_size() const {
    std::shared_lock<std::shared_mutex> lock{blackboard_mtx_};
    return blackboard_.size();
}

// =============================================================================
// TeamRegistry implementation
// =============================================================================

TeamRegistry& TeamRegistry::instance() noexcept {
    static TeamRegistry registry;
    return registry;
}

Team* TeamRegistry::create_team(std::string name) {
    // First try a shared (read) lock to see if the team already exists —
    // the common path after the first creation.
    {
        std::shared_lock<std::shared_mutex> rlock{registry_mtx_};
        auto it = teams_.find(name);
        if (it != teams_.end()) {
            return it->second.get();
        }
    }
    // Not found — acquire exclusive lock to insert.
    std::unique_lock<std::shared_mutex> wlock{registry_mtx_};
    // Re-check after lock upgrade (another thread may have inserted).
    auto it = teams_.find(name);
    if (it != teams_.end()) {
        return it->second.get();
    }
    auto [ins, ok] = teams_.emplace(name, std::make_unique<Team>(name));
    (void)ok;
    return ins->second.get();
}

Team* TeamRegistry::get_team(std::string_view name) const {
    std::shared_lock<std::shared_mutex> lock{registry_mtx_};
    auto it = teams_.find(std::string{name});
    if (it == teams_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void TeamRegistry::delete_team(std::string_view name) {
    std::unique_lock<std::shared_mutex> lock{registry_mtx_};
    teams_.erase(std::string{name});
}

std::vector<std::string> TeamRegistry::team_names() const {
    std::shared_lock<std::shared_mutex> lock{registry_mtx_};
    std::vector<std::string> names;
    names.reserve(teams_.size());
    for (const auto& [k, _] : teams_) {
        names.push_back(k);
    }
    return names;
}

std::size_t TeamRegistry::size() const {
    std::shared_lock<std::shared_mutex> lock{registry_mtx_};
    return teams_.size();
}

} // namespace batbox::agents
