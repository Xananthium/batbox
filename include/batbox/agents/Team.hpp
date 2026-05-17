// include/batbox/agents/Team.hpp
//
// batbox::agents::Team — named group of agent IDs with a shared KV blackboard.
// batbox::agents::TeamRegistry — singleton holding all named teams.
//
// Blueprint contract (task CPP 6.6):
//   struct Team { name, member_ids, blackboard (shared_mutex-guarded) }
//   class TeamRegistry { create_team, get_team, delete_team, team_names }
//
// Blackboard operations:
//   set_kv(key, value)           — unconditional store
//   get_kv(key)                  → std::optional<Json>
//   cas_kv(key, expected, value) → bool (compare-and-swap)
//
// Team membership:
//   add_member(id)               — idempotent
//   remove_member(id)            — no-op if not present
//   members()                    → snapshot vector<string>
//   broadcast(message)           — placeholder for enqueue to all members
//                                  (wires into AgentSupervisor when live)
//
// Blueprint rows: 16770–16772

#pragma once

#include <batbox/core/Json.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace batbox::agents {

// =============================================================================
// Team — named agent group with a shared mutex-guarded KV blackboard
// =============================================================================

/// A named group of sub-agent IDs sharing a KV blackboard.
///
/// The blackboard is an `unordered_map<string, Json>` guarded by a
/// `shared_mutex` — concurrent reads proceed without blocking each other;
/// writes are exclusive.  compare-and-swap (cas_kv) provides optimistic
/// concurrency for agents that need atomic conditional updates.
///
/// Blueprint contract: batbox::agents::Team (blueprints row 16771)
struct Team {
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a new (empty) team with the given name.
    explicit Team(std::string name);

    /// Non-copyable (owns a shared_mutex which is not movable/copyable).
    Team(const Team&)            = delete;
    Team& operator=(const Team&) = delete;
    Team(Team&&)                 = delete;
    Team& operator=(Team&&)      = delete;

    // -------------------------------------------------------------------------
    // Membership
    // -------------------------------------------------------------------------

    /// Add agent_id to the team.  Idempotent — no-op if already a member.
    ///
    /// @param agent_id  Opaque agent identifier returned by AgentSupervisor::spawn().
    void add_member(std::string_view agent_id);

    /// Remove agent_id from the team.  No-op if not currently a member.
    ///
    /// @param agent_id  The agent to remove.
    void remove_member(std::string_view agent_id);

    /// Return a point-in-time snapshot of the current member list.
    ///
    /// @returns Vector of agent_id strings, in insertion order.
    [[nodiscard]] std::vector<std::string> members() const;

    // -------------------------------------------------------------------------
    // Broadcast
    // -------------------------------------------------------------------------

    /// Enqueue a message to all current members.
    ///
    /// Implementation note: when AgentSupervisor is fully live (CPP 6.5), this
    /// calls supervisor->enqueue_message(member_id, message) for each member.
    /// The message is stored in a per-team pending queue here so that Team is
    /// self-contained and usable without an AgentSupervisor reference.
    ///
    /// @param message  UTF-8 text to deliver to each member agent.
    void broadcast(std::string_view message);

    /// Return and drain all pending broadcast messages (consumed by the supervisor
    /// when it processes outbound queues).
    [[nodiscard]] std::vector<std::string> drain_pending_broadcasts();

    // -------------------------------------------------------------------------
    // Blackboard — KV operations
    // -------------------------------------------------------------------------

    /// Store value under key, replacing any existing entry.
    ///
    /// @param key    String key (non-empty).
    /// @param value  Any JSON value.
    void set_kv(std::string_view key, const Json& value);

    /// Retrieve the value stored under key, or nullopt if absent.
    ///
    /// @param key  String key to look up.
    /// @returns    std::optional<Json>; nullopt when key not found.
    [[nodiscard]] std::optional<Json> get_kv(std::string_view key) const;

    /// Compare-and-swap: atomically replace value only when the current value
    /// equals expected.  Comparison uses nlohmann::json operator==.
    ///
    /// @param key       String key.
    /// @param expected  Value that must currently be stored for the swap to occur.
    /// @param desired   New value to store on success.
    /// @returns         true if the swap was performed; false otherwise (key absent
    ///                  or current value != expected).
    [[nodiscard]] bool cas_kv(std::string_view key,
                               const Json&      expected,
                               const Json&      desired);

    /// Remove a key from the blackboard.  No-op if the key is not present.
    ///
    /// @param key  String key to erase.
    void erase_kv(std::string_view key);

    /// Return the number of entries currently in the blackboard.
    [[nodiscard]] std::size_t blackboard_size() const;

    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------

    /// The team's unique name (set at construction, immutable).
    const std::string name;

private:
    // Member list — guarded by members_mtx_.
    mutable std::mutex              members_mtx_;
    std::vector<std::string>        member_ids_;

    // Pending broadcasts — guarded by broadcast_mtx_.
    mutable std::mutex              broadcast_mtx_;
    std::vector<std::string>        pending_broadcasts_;

    // Blackboard — guarded by blackboard_mtx_.
    mutable std::shared_mutex                            blackboard_mtx_;
    std::unordered_map<std::string, Json>                blackboard_;
};

// =============================================================================
// TeamRegistry — singleton holding all named teams
// =============================================================================

/// Thread-safe registry of named Team objects.
///
/// Access via TeamRegistry::instance().
///
/// All methods are safe to call from multiple agent threads concurrently.
class TeamRegistry {
public:
    // -------------------------------------------------------------------------
    // Singleton access
    // -------------------------------------------------------------------------

    /// Return the process-wide TeamRegistry singleton.
    [[nodiscard]] static TeamRegistry& instance() noexcept;

    // -------------------------------------------------------------------------
    // Team lifecycle
    // -------------------------------------------------------------------------

    /// Create a new team with the given name and return a pointer to it.
    ///
    /// If a team with this name already exists, the existing team is returned
    /// (idempotent create).
    ///
    /// @param name  Unique team name.
    /// @returns     Non-owning pointer to the Team (lifetime: until delete_team).
    Team* create_team(std::string name);

    /// Look up an existing team by name.
    ///
    /// @param name  Team name to find.
    /// @returns     Non-owning pointer, or nullptr if not found.
    [[nodiscard]] Team* get_team(std::string_view name) const;

    /// Delete a named team, freeing its memory.  No-op if the name is unknown.
    ///
    /// @param name  Team name to delete.
    void delete_team(std::string_view name);

    /// Return a snapshot of all current team names.
    [[nodiscard]] std::vector<std::string> team_names() const;

    /// Return the number of teams currently registered.
    [[nodiscard]] std::size_t size() const;

    // -------------------------------------------------------------------------
    // Non-copyable / non-movable (singleton)
    // -------------------------------------------------------------------------

    TeamRegistry(const TeamRegistry&)            = delete;
    TeamRegistry& operator=(const TeamRegistry&) = delete;
    TeamRegistry(TeamRegistry&&)                 = delete;
    TeamRegistry& operator=(TeamRegistry&&)      = delete;

private:
    TeamRegistry()  = default;
    ~TeamRegistry() = default;

    mutable std::shared_mutex                                registry_mtx_;
    std::unordered_map<std::string, std::unique_ptr<Team>>   teams_;
};

} // namespace batbox::agents
