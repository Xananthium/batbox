// src/agents/AgentSupervisor.cpp
//
// batbox::agents::AgentSupervisor — full implementation (CPP 6.5).
//
// Blueprint contract:
//   - std::counting_semaphore<MAX_SEM_BOUND> slots — bounded parallelism
//   - std::unordered_map<string, unique_ptr<SubAgent>> agents under shared_mutex
//   - std::deque<std::string> pending_ids — pending agent IDs ordered by spawn
//   - spawn() — try_acquire slot or enqueue; create SubAgent; optionally start
//   - cancel() — find SubAgent by id; call its cancel()
//   - enqueue_message() — find SubAgent; call enqueue_message()
//   - snapshot() — shared_lock agents; collect SubAgent::snapshot() in order
//   - wait_all() — condition_variable wait until active_count reaches 0
//
// Semaphore design:
//   std::counting_semaphore<N> requires a compile-time N (least-maximum-value).
//   We use MAX_SEM_BOUND=64 as the compile-time bound and initialise the
//   semaphore with the runtime limit.  slots_limit_ stores the runtime cap for
//   pending-queue accounting and Queued-event hint strings.
//
// Pending-queue design:
//   When try_acquire() fails, the agent is registered in agents_ (status=queued)
//   and its agent_id is pushed to pending_ids_.  On each on_exit() callback, the
//   front of pending_ids_ is popped and that agent is started (reusing the
//   released slot).  This gives FIFO ordering to the queue.
//
// active_count design:
//   Incremented by spawn() before any slot acquisition.
//   Decremented by on_exit() for the agent that just finished.
//   When dequeuing a pending agent, on_exit() transfers the slot directly to the
//   new agent (no release+acquire cycle) and does NOT decrement for the new agent
//   (it was already counted when it was spawned).
//   wait_all() wakes when active_count reaches 0.

#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentEvent.hpp>
#include <batbox/agents/SubAgent.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Uuid.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace batbox::agents {

// =============================================================================
// Constants
// =============================================================================

/// Compile-time semaphore upper bound.  Must be >= any runtime max_concurrent.
static constexpr int MAX_SEM_BOUND = 64;

/// Default runtime parallelism limit when no explicit value is given.
static constexpr int DEFAULT_PARALLEL_LIMIT = 4;

/// Default LRU bound on the standing-subagent pool (S2/S3, DIS-988).
static constexpr int DEFAULT_STANDING_LIMIT = 4;

namespace {
/// First line of @p s, trimmed of a trailing '\r', truncated to @p max chars
/// (with an ellipsis when truncated).  Used for the one-line standing-status.
std::string first_line_truncated(const std::string& s, std::size_t max) {
    std::size_t nl = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (max == 0) return std::string{};
    if (line.size() > max) {
        line.resize(max - 1);
        line += "…";
    }
    return line;
}
} // namespace

// =============================================================================
// AgentSupervisor::Impl
// =============================================================================

struct AgentSupervisor::Impl {
    // -------------------------------------------------------------------------
    // Semaphore — bounded parallelism token pool.
    // The compile-time template arg is the least-maximum-value; the semaphore
    // is initialised with the runtime limit so it starts fully "available".
    // -------------------------------------------------------------------------
    std::counting_semaphore<MAX_SEM_BOUND> slots;

    /// Runtime parallelism cap (stored for pending-queue accounting).
    int slots_limit;

    // -------------------------------------------------------------------------
    // Agent registry.
    // Guarded by agents_mutex (shared_mutex):
    //   - snapshot() acquires a shared lock (concurrent reads are fine).
    //   - spawn() registration and any write acquires an exclusive lock.
    //
    // insertion_order preserves spawn sequence for deterministic snapshot output.
    // -------------------------------------------------------------------------
    mutable std::shared_mutex                          agents_mutex;
    std::unordered_map<std::string,
                       std::unique_ptr<SubAgent>>      agents;
    std::vector<std::string>                           insertion_order;

    // -------------------------------------------------------------------------
    // Pending-spawn queue.
    // When try_acquire() fails, the spawned agent's id is pushed here.
    // on_exit() pops the front id and starts that agent (reusing the slot).
    // Guarded by pending_mutex (plain mutex; brief critical sections only).
    // -------------------------------------------------------------------------
    std::mutex            pending_mutex;
    std::deque<std::string> pending_ids;

    // -------------------------------------------------------------------------
    // Shared event queue — all SubAgent jthreads push() here.
    // The TUI SubAgentPanel drains it at 10 Hz via dirty_seq() polling.
    // -------------------------------------------------------------------------
    AgentEventQueue event_queue;

    // -------------------------------------------------------------------------
    // Lifecycle counter for wait_all().
    // Incremented by spawn() for every agent (including pending ones).
    // Decremented by on_exit() exactly once per agent.
    // wait_all() blocks on active_cv until active_count == 0.
    // -------------------------------------------------------------------------
    std::mutex              active_mutex;
    std::condition_variable active_cv;
    int                     active_count{0};

    // -------------------------------------------------------------------------
    // Standing-subagent pool (S2/S3, DIS-988).
    //   standing_lru  — order of warm windows, most-recently-interrogated FRONT,
    //                   least-recently-interrogated BACK (the eviction target).
    //   slot_released — agents whose active-work semaphore slot was already
    //                   handed back at promote time, so on_exit() must NOT
    //                   release a second slot for them.
    //   max_standing  — hard LRU bound on the pool (no unbounded growth, AC5).
    // All three are guarded by standing_mutex.  Lock ordering invariant: never
    // hold standing_mutex while acquiring agents_mutex/pending_mutex (the
    // standing methods take standing_mutex, release it, THEN touch the others).
    // -------------------------------------------------------------------------
    mutable std::mutex                standing_mutex;
    std::list<std::string>            standing_lru;
    std::unordered_set<std::string>   slot_released;
    int                               max_standing{DEFAULT_STANDING_LIMIT};

    // -------------------------------------------------------------------------
    // Default Config — kept alive for the duration of this Impl so that
    // SubAgent::cfg_ (a const Config&) never becomes a dangling reference.
    // SubAgent stores cfg_ as a reference; we must ensure the referred-to
    // Config object outlives every SubAgent registered in this supervisor.
    // -------------------------------------------------------------------------
    batbox::config::Config default_cfg_;

    // -------------------------------------------------------------------------
    // Constructor
    // -------------------------------------------------------------------------
    explicit Impl(int limit)
        : slots(limit)
        , slots_limit(limit)
        , default_cfg_(batbox::config::Config::load_default())
    {}

    // -------------------------------------------------------------------------
    // increment_active / decrement_active — update lifecycle counter.
    // -------------------------------------------------------------------------
    void increment_active() {
        std::lock_guard<std::mutex> lk(active_mutex);
        ++active_count;
    }

    void decrement_active() {
        std::lock_guard<std::mutex> lk(active_mutex);
        if (active_count > 0) {
            --active_count;
        }
        active_cv.notify_all();
    }

    // -------------------------------------------------------------------------
    // start_pending_or_release — dequeue next pending agent (if any) and start
    // it, OR release the slot back to the semaphore.
    //
    // Called from on_exit() after locking pending_mutex.
    // Returns true if a pending agent was dequeued and started.
    // -------------------------------------------------------------------------
    bool start_pending_or_release() {
        std::string next_id;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            if (pending_ids.empty()) {
                // No pending agents — release slot.
                slots.release();
                return false;
            }
            next_id = std::move(pending_ids.front());
            pending_ids.pop_front();
        }

        // Find the queued SubAgent and start it (reusing the current slot).
        // IMPORTANT: call start() while still holding the shared_lock to prevent
        // a race where the destructor moves the SubAgent out of the map between
        // the lock release and the start() call.
        bool started = false;
        {
            std::shared_lock<std::shared_mutex> lk(agents_mutex);
            auto it = agents.find(next_id);
            if (it != agents.end()) {
                it->second->start();
                started = true;
            }
        }

        if (!started) {
            // Agent was cancelled while queued — release slot and decrement active
            // (it was counted at spawn time but will never exit naturally).
            slots.release();
            decrement_active();
        }

        return started;
    }

    // -------------------------------------------------------------------------
    // on_exit — callback fired by each SubAgent's jthread on exit.
    //
    // Dequeues the next pending agent (reusing the slot) or releases the slot,
    // UNLESS this agent already handed its active slot back at promote time (a
    // standing subagent) — in that case the slot was reused/released long ago,
    // so we must only decrement the active counter (no second release).
    // Decrements the active counter and notifies wait_all() waiters.
    // -------------------------------------------------------------------------
    void on_exit(const std::string& id) {
        bool slot_already_released = false;
        {
            std::lock_guard<std::mutex> lk(standing_mutex);
            auto it = slot_released.find(id);
            if (it != slot_released.end()) {
                slot_already_released = true;
                slot_released.erase(it);
            }
            standing_lru.remove(id);  // no-op if not standing
        }
        if (!slot_already_released) {
            start_pending_or_release();
        }
        decrement_active();
    }

    // -------------------------------------------------------------------------
    // evict_standing — fire a standing subagent's stop_token so its parked run
    // loop wakes, discards its window, and exits.  Lossless by construction: the
    // gold is already in the notepad (S6).  Caller must NOT hold standing_mutex.
    // -------------------------------------------------------------------------
    void evict_standing(const std::string& id) {
        std::shared_lock<std::shared_mutex> lk(agents_mutex);
        auto it = agents.find(id);
        if (it != agents.end()) {
            it->second->cancel();  // fires child_token → park wait wakes → exit
        }
    }
};

// =============================================================================
// Construction / destruction
// =============================================================================

AgentSupervisor::AgentSupervisor()
    : impl_(new Impl(DEFAULT_PARALLEL_LIMIT))
{}

AgentSupervisor::AgentSupervisor(int max_concurrent)
    : impl_(new Impl(std::clamp(max_concurrent, 1, MAX_SEM_BOUND)))
{}

AgentSupervisor::~AgentSupervisor() {
    // Step 1: Cancel all running agents (cooperative stop request).
    {
        std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
        for (const auto& [id, agent] : impl_->agents) {
            agent->cancel();
        }
    }

    // Step 2: Join all jthreads by destroying the SubAgent unique_ptrs.
    // This MUST happen before deleting impl_ because SubAgent on_exit callbacks
    // capture impl_ by raw pointer; those callbacks fire during jthread join.
    //
    // We clear the agents map under exclusive lock so that no new spawns race
    // with the destruction (though spawning during destruction is UB anyway).
    {
        // Extract all SubAgents out of the map so we can join outside the lock.
        std::vector<std::unique_ptr<SubAgent>> dying;
        {
            std::unique_lock<std::shared_mutex> lk(impl_->agents_mutex);
            dying.reserve(impl_->agents.size());
            for (auto& [id, agent] : impl_->agents) {
                dying.push_back(std::move(agent));
            }
            impl_->agents.clear();
            impl_->insertion_order.clear();
        }
        // Destroy each SubAgent (joins its jthread) while impl_ is still alive.
        // The on_exit callbacks may fire here and access impl_->active_mutex.
        dying.clear();
    }

    // Step 3: Now that all jthreads are joined and no callbacks can fire,
    // it is safe to delete impl_.
    delete impl_;
}

// =============================================================================
// spawn
// =============================================================================

std::string AgentSupervisor::spawn(const AgentSpec&  spec,
                                    std::string_view  prompt,
                                    std::string_view  /*parent_id*/,
                                    CancelToken       ct)
{
    // Generate a cryptographically random RFC 4122 v4 UUID for this agent.
    const std::string agent_id = batbox::Uuid::v4().to_string();

    // Count this agent as active immediately so wait_all() never returns before
    // the agent has been fully registered and started (or queued).
    impl_->increment_active();

    // Build the on_exit callback that the SubAgent jthread fires on termination.
    // Capture agent_id by value so on_exit() can consult the standing-pool
    // slot_released set (a promoted agent already handed its slot back).
    Impl* pimpl = impl_;
    auto on_exit_cb = [pimpl, agent_id]() {
        pimpl->on_exit(agent_id);
    };

    // Construct the SubAgent (status = queued; jthread not yet started).
    // Pass impl_->default_cfg_ (a long-lived member of Impl) rather than a
    // temporary returned by Config::load_default().  SubAgent stores cfg_ as a
    // const Config&; using a temporary would produce a dangling reference once
    // the full-expression that calls make_unique completes.  impl_->default_cfg_
    // outlives every SubAgent registered in this supervisor.
    auto agent = std::make_unique<SubAgent>(
        agent_id,
        spec,
        std::string(prompt),
        std::move(ct),
        impl_->event_queue,
        impl_->default_cfg_,
        std::move(on_exit_cb)
    );

    // Register in the agents map and preserve insertion order.
    {
        std::unique_lock<std::shared_mutex> lk(impl_->agents_mutex);
        impl_->agents.emplace(agent_id, std::move(agent));
        impl_->insertion_order.push_back(agent_id);
    }

    // Attempt a non-blocking slot acquisition.
    if (impl_->slots.try_acquire()) {
        // Slot acquired — start the agent immediately.
        std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
        impl_->agents.at(agent_id)->start();
    } else {
        // All slots busy — push to the pending queue.
        {
            std::lock_guard<std::mutex> lk(impl_->pending_mutex);
            impl_->pending_ids.push_back(agent_id);

            // Post a Queued event for the TUI (shows "queued, N/limit").
            const std::string hint =
                "queued, " + std::to_string(impl_->pending_ids.size()) +
                "/" + std::to_string(impl_->slots_limit);
            impl_->event_queue.push(AgentEvent::make_queued(agent_id, hint));
        }
    }

    return agent_id;
}

// =============================================================================
// snapshot
// =============================================================================

std::vector<AgentSnapshot> AgentSupervisor::snapshot() const {
    std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);

    std::vector<AgentSnapshot> result;
    result.reserve(impl_->insertion_order.size());

    for (const auto& id : impl_->insertion_order) {
        auto it = impl_->agents.find(id);
        if (it != impl_->agents.end()) {
            // SubAgent::snapshot() is const and acquires its own internal mutex.
            result.push_back(it->second->snapshot());
        }
    }

    return result;
}

// =============================================================================
// cancel
// =============================================================================

void AgentSupervisor::cancel(std::string_view agent_id) {
    std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
    const auto it = impl_->agents.find(std::string(agent_id));
    if (it != impl_->agents.end()) {
        // SubAgent::cancel() is thread-safe and non-blocking.
        it->second->cancel();
    }
    // No-op if agent_id is unknown or the agent has already terminated.
}

// =============================================================================
// enqueue_message
// =============================================================================

void AgentSupervisor::enqueue_message(std::string_view agent_id,
                                       std::string_view message)
{
    std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
    const auto it = impl_->agents.find(std::string(agent_id));
    if (it != impl_->agents.end()) {
        // SubAgent::enqueue_message() is thread-safe and non-blocking.
        it->second->enqueue_message(message);
    }
    // No-op if agent_id is unknown or the agent has already terminated.
}

// =============================================================================
// wait_all
// =============================================================================

void AgentSupervisor::wait_all() {
    std::unique_lock<std::mutex> lk(impl_->active_mutex);
    impl_->active_cv.wait(lk, [this] {
        return impl_->active_count == 0;
    });
}

// =============================================================================
// Standing-subagent registry (S2/S3, DIS-988)
// =============================================================================

void AgentSupervisor::promote(std::string_view agent_id) {
    const std::string id(agent_id);

    // 1. Mark the SubAgent standing.  Safe no-op on an unknown handle (AC5).
    {
        std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
        auto it = impl_->agents.find(id);
        if (it == impl_->agents.end()) {
            return;
        }
        it->second->promote();
    }

    // 2. Register in the LRU pool.  Decide everything under standing_mutex, then
    //    act on the slot/eviction OUTSIDE it (lock-ordering invariant: never hold
    //    standing_mutex while touching agents_mutex/pending_mutex).
    bool        newly_standing = false;
    std::string victim;
    {
        std::lock_guard<std::mutex> lk(impl_->standing_mutex);
        auto existing = std::find(impl_->standing_lru.begin(),
                                  impl_->standing_lru.end(), id);
        if (existing != impl_->standing_lru.end()) {
            // Idempotent re-promote: just refresh LRU recency (move to front).
            impl_->standing_lru.splice(impl_->standing_lru.begin(),
                                       impl_->standing_lru, existing);
        } else {
            impl_->slot_released.insert(id);
            impl_->standing_lru.push_front(id);
            newly_standing = true;
            // Enforce the hard LRU bound (AC3/AC5): evict the least-recently-
            // interrogated (back) when the pool exceeds max_standing.
            if (impl_->max_standing >= 0 &&
                static_cast<int>(impl_->standing_lru.size()) > impl_->max_standing) {
                victim = std::move(impl_->standing_lru.back());
                impl_->standing_lru.pop_back();
            }
        }
    }

    if (newly_standing) {
        // Hand this agent's active-work slot back to the pool: it no longer
        // counts against max_concurrent (it is now bounded by max_standing), so
        // parked windows never starve new spawns.  start_pending_or_release()
        // either starts a queued agent on the freed slot or releases it.
        impl_->start_pending_or_release();
    }
    if (!victim.empty()) {
        impl_->evict_standing(victim);
    }
}

std::string AgentSupervisor::interrogate(std::string_view agent_id,
                                          std::string_view question) {
    const std::string id(agent_id);

    // Refresh LRU recency (most-recently-interrogated → front) so this agent is
    // the LAST to be evicted under pressure.
    {
        std::lock_guard<std::mutex> lk(impl_->standing_mutex);
        auto existing = std::find(impl_->standing_lru.begin(),
                                  impl_->standing_lru.end(), id);
        if (existing != impl_->standing_lru.end()) {
            impl_->standing_lru.splice(impl_->standing_lru.begin(),
                                       impl_->standing_lru, existing);
        }
    }

    // Obtain the answer future WITHOUT holding any lock across the blocking get()
    // (the warm window runs on its own jthread).  Unknown handle → "" (AC5).
    std::future<std::string> fut;
    {
        std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
        auto it = impl_->agents.find(id);
        if (it == impl_->agents.end()) {
            return std::string{};
        }
        fut = it->second->interrogate(std::string(question));
    }
    return fut.get();  // SubAgent always fulfils (reaper) — never hangs.
}

void AgentSupervisor::set_max_standing_subagents(int n) {
    std::vector<std::string> victims;
    {
        std::lock_guard<std::mutex> lk(impl_->standing_mutex);
        impl_->max_standing = (n < 0) ? 0 : n;
        // Shrink the pool to the new bound, evicting least-recently-interrogated.
        while (static_cast<int>(impl_->standing_lru.size()) > impl_->max_standing) {
            victims.push_back(std::move(impl_->standing_lru.back()));
            impl_->standing_lru.pop_back();
        }
    }
    for (const auto& v : victims) {
        impl_->evict_standing(v);
    }
}

std::vector<AgentSupervisor::StandingStatus>
AgentSupervisor::standing_status() const {
    // Snapshot the LRU order under standing_mutex, then resolve names/results
    // under agents_mutex (never both held at once — lock-ordering invariant).
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(impl_->standing_mutex);
        ids.assign(impl_->standing_lru.begin(), impl_->standing_lru.end());
    }

    std::vector<StandingStatus> out;
    out.reserve(ids.size());
    std::shared_lock<std::shared_mutex> lk(impl_->agents_mutex);
    for (const auto& id : ids) {
        auto it = impl_->agents.find(id);
        if (it == impl_->agents.end()) {
            continue;
        }
        StandingStatus s;
        s.id          = id;
        s.name        = it->second->name();
        s.status_line = first_line_truncated(it->second->last_result(), 80);
        out.push_back(std::move(s));
    }
    return out;
}

std::size_t AgentSupervisor::standing_count() const {
    std::lock_guard<std::mutex> lk(impl_->standing_mutex);
    return impl_->standing_lru.size();
}

} // namespace batbox::agents
