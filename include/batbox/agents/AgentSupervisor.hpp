// include/batbox/agents/AgentSupervisor.hpp
//
// batbox::agents::AgentSupervisor — orchestrator for spawning and managing
// sub-agent threads with semaphore-bounded parallelism.
//
// Blueprint contract (task CPP 6.5):
//   class AgentSupervisor
//     counting_semaphore<4> slots_ — at most 4 concurrent agents
//     map<id, SubAgent>           — live agent registry
//     event deque                 — AgentEventQueue
//     team registry               — named team management
//
//   Methods:
//     spawn(spec, prompt, parent_id, ct) → agent_id string
//     snapshot()                         → vector<AgentSnapshot>
//     cancel(agent_id)                   → void
//     enqueue_message(agent_id, message) → void (for SendMessage tool)
//     wait_all()                         → void (shutdown: block until all agents done)
//
// This header is the forward-declare contract used by the sub-agent tool
// implementations (CPP 5.28).  The full implementation lives in CPP 6.5.
//
// AgentSnapshot:
//   Per-agent rollup for the 10Hz TUI ticker.
//   Fields: id, name, status, current_step, last_5_lines, token_count
//
// Blueprint rows: 16764–16769, 17015

#pragma once

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/core/CancelToken.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::agents {

// =============================================================================
// AgentSnapshot — per-agent rollup for TUI 10Hz ticker render
// =============================================================================

/// Snapshot of a single running or recently-completed sub-agent.
/// Produced by AgentSupervisor::snapshot() for the SubAgentPanel.
///
/// Blueprint contract: batbox::agents::AgentSnapshot (blueprints row 17015)
struct AgentSnapshot {
    /// Opaque agent identifier (UUID string assigned at spawn time).
    std::string id;

    /// Human-readable display name (from AgentSpec::name or the subagent_type).
    std::string name;

    /// Current lifecycle status: "queued", "running", "completed", "cancelled", "errored".
    std::string status;

    /// Label of the current reasoning/tool step, e.g. "calling bash".
    std::string current_step;

    /// Last up-to-5 output lines accumulated from TokenAppended events.
    std::vector<std::string> last_5_lines;

    /// Running token count for this agent (prompt + completion).
    std::size_t token_count = 0;
};

// =============================================================================
// AgentSupervisor — semaphore-bounded sub-agent orchestrator
// =============================================================================

/// Central orchestrator that spawns, monitors, and cancels sub-agent threads.
///
/// Parallelism is bounded to cfg.tools.task_parallel_limit (default 4) simultaneous
/// agents via a counting_semaphore.  Agents beyond the limit are queued and
/// dispatched as slots free.
///
/// Blueprint contract: batbox::agents::AgentSupervisor (blueprints rows 16764–16769)
class AgentSupervisor {
public:
    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------

    /// Construct with default parallelism limit (4 concurrent agents).
    AgentSupervisor();

    /// Construct with an explicit parallelism limit (1..MAX_CONCURRENT_LIMIT).
    /// Clamps values outside the valid range silently.
    explicit AgentSupervisor(int max_concurrent);

    ~AgentSupervisor();

    // Non-copyable, non-movable (owns threads and semaphore).
    AgentSupervisor(const AgentSupervisor&)            = delete;
    AgentSupervisor& operator=(const AgentSupervisor&) = delete;
    AgentSupervisor(AgentSupervisor&&)                 = delete;
    AgentSupervisor& operator=(AgentSupervisor&&)      = delete;

    // -------------------------------------------------------------------------
    // spawn — create and start a sub-agent
    // -------------------------------------------------------------------------

    /// Acquire a semaphore slot (or queue the agent if all slots are busy), create
    /// a SubAgent with the given spec and initial prompt, start it in a jthread,
    /// and return the newly-assigned agent_id immediately.
    ///
    /// @param spec       Agent configuration (name, model, allowed_tools, …).
    /// @param prompt     Initial user-turn text delivered to the agent.
    /// @param parent_id  ID of the calling agent (empty for root conversation).
    /// @param ct         Parent cancel token; cancelling the parent cascades.
    ///
    /// @returns Opaque agent_id string (UUID) for subsequent control calls.
    ///
    /// Blueprint contract: batbox::agents::AgentSupervisor::spawn (row 16766)
    std::string spawn(const AgentSpec&  spec,
                      std::string_view  prompt,
                      std::string_view  parent_id,
                      CancelToken       ct);

    // -------------------------------------------------------------------------
    // snapshot — poll all agent states for the TUI ticker
    // -------------------------------------------------------------------------

    /// Return a rollup snapshot of every known agent (running, queued, and
    /// recently terminated).  Called by SubAgentPanel at 10 Hz; must be fast.
    ///
    /// @returns Vector of AgentSnapshot, one entry per agent, in spawn order.
    ///
    /// Blueprint contract: batbox::agents::AgentSupervisor::snapshot (row 16767)
    std::vector<AgentSnapshot> snapshot() const;

    // -------------------------------------------------------------------------
    // cancel — request cooperative cancellation of a specific agent
    // -------------------------------------------------------------------------

    /// Find the SubAgent with the given id and call its cancel() method, which
    /// signals the agent's stop_source.  The agent's jthread will exit on the
    /// next cancellation checkpoint and post an AgentEvent::Cancelled event.
    ///
    /// A no-op if agent_id does not match any known agent (already completed
    /// or never existed).
    ///
    /// @param agent_id  The id returned by spawn().
    ///
    /// Blueprint contract: batbox::agents::AgentSupervisor::cancel (row 16768)
    void cancel(std::string_view agent_id);

    // -------------------------------------------------------------------------
    // enqueue_message — deliver a peer message to a running agent
    // -------------------------------------------------------------------------

    /// Enqueue a message string into the input queue of the identified agent.
    /// The message is delivered on the agent's next turn boundary (after its
    /// current inference call returns).
    ///
    /// A no-op if agent_id is unknown or the agent has already terminated.
    ///
    /// @param agent_id  The id returned by spawn().
    /// @param message   UTF-8 text to deliver as a new user-turn message.
    ///
    /// Used by SendMessageTool to implement peer-to-peer agent communication.
    void enqueue_message(std::string_view agent_id, std::string_view message);

    // -------------------------------------------------------------------------
    // wait_all — block until all agents finish (for graceful shutdown)
    // -------------------------------------------------------------------------

    /// Block the calling thread until every spawned agent (both currently
    /// running and any in the pending queue) has reached a terminal state
    /// (done, failed, or cancelled).
    ///
    /// Intended for orderly shutdown: call cancel() on all agents first,
    /// then wait_all() to join before the supervisor is destroyed.
    void wait_all();

    // =========================================================================
    // Standing-subagent registry — the warm window (S2/S3, DIS-988)
    //
    // A *standing* subagent keeps its conversation alive after it produces a
    // result (it is NOT collapsed to a string), so the parent can interrogate it
    // with follow-up turns against the still-engulfed context.  The standing set
    // is a SEPARATE, LRU-bounded pool that lives ALONGSIDE the active-work
    // parallelism semaphore: promoting an agent hands its active slot back to the
    // pool, so parked windows never starve new spawns.  Under pressure the
    // least-recently-interrogated standing subagent is evicted (its window is
    // discarded and its stop_token fired) — lossless by construction because the
    // gold is already in the notepad (S6).
    //
    // Step 7 exposes the explicit promote + interrogate API.  WHEN to promote
    // (closed→standing) is the step-8 selection heuristic — NOT decided here.
    // =========================================================================

    /// One warm subagent's line for the parent's status surface (AC4).
    struct StandingStatus {
        std::string id;           ///< Opaque agent handle.
        std::string name;         ///< Display name (AgentSpec::name).
        std::string status_line;  ///< One-line status (truncated last result).
    };

    /// promote — mark a known subagent STANDING and register it in the LRU pool.
    ///
    /// The subagent's run loop will park at quiescence instead of exiting, and
    /// the agent's active-work slot is handed back to the pool.  If promoting
    /// pushes the pool past max_standing_subagents, the least-recently-
    /// interrogated standing subagent is evicted.  Idempotent: re-promoting an
    /// already-standing agent just refreshes its LRU recency.  Safe no-op on an
    /// unknown handle.
    void promote(std::string_view agent_id);

    /// interrogate — issue a follow-up user turn against a standing subagent's
    /// warm window and return its answer (blocking).
    ///
    /// Refreshes the agent's LRU recency, then runs the question against the
    /// still-engulfed context (the source is NOT re-engulfed).  Returns the empty
    /// string on an unknown / non-standing / evicted handle — never hangs, never
    /// throws (the warm window always fulfils, AC5).
    [[nodiscard]] std::string interrogate(std::string_view agent_id,
                                          std::string_view question);

    /// set_max_standing_subagents — set the LRU bound on the standing pool.
    /// Clamped to >= 0.  Lowering it below the current pool size evicts the
    /// least-recently-interrogated agents until the pool fits.
    void set_max_standing_subagents(int n);

    /// standing_status — the bounded list of warm subagents available for
    /// follow-up, most-recently-interrogated first.  Source for the AC4 status
    /// line injected into the parent each turn.
    [[nodiscard]] std::vector<StandingStatus> standing_status() const;

    /// standing_count — number of subagents currently in the standing pool.
    [[nodiscard]] std::size_t standing_count() const;

private:
    struct Impl;
    Impl* impl_; ///< Pimpl — hides semaphore, jthreads, and map from callers.
};

} // namespace batbox::agents
