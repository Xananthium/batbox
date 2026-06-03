#pragma once
// =============================================================================
// batbox/agents/SubAgent.hpp — per-agent thread with its own ConversationEngine
//
// SubAgent
//   Each SubAgent owns:
//     - Its own batbox::conversation::Conversation instance
//     - Its own stop_source_ (linked to a parent CancelToken)
//     - A std::jthread that runs the agent's conversation loop
//     - A thread-safe input queue for injected messages (enqueue_message)
//     - A rolling last-5-lines output buffer for AgentSnapshot
//
//   Lifecycle:
//     queued    → SubAgent constructed, not yet started (waiting for semaphore)
//     running   → jthread started, conversation loop active
//     done      → conversation completed successfully (final event posted)
//     failed    → conversation ended with an error
//     cancelled → cancel() was called; thread exits on next checkpoint
//
//   run() loop (executes in the jthread):
//     1. Post AgentEvent::Started
//     2. Call conv_.user_message(initial_prompt_)
//     3. Loop:
//        a. Call conv_.run_turn(child_token) with streaming callback that posts
//           TokenAppended events and appends to last_5_lines_ buffer
//        b. On cancellation: post Cancelled event, exit
//        c. On error: post Errored event, exit
//        d. On stop_requested: post Cancelled event, exit
//        e. Drain pending_messages_ queue; if any → user_message + loop again
//        f. If no pending messages and finish_reason == "stop": post Completed, exit
//     4. Release semaphore slot (via on_exit callback)
//
//   Thread safety:
//     - cancel() and enqueue_message() are the only public methods safe to call
//       from any thread after construction.
//     - snapshot() reads fields under snapshot_mutex_ (lock-free path for TUI).
//     - All other methods (run, constructor) must be called from the owning thread.
//
// Blueprint contract: batbox::agents::SubAgent (blueprints rows 16760–16763)
// =============================================================================

#include <batbox/agents/AgentEvent.hpp>
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>    // for AgentSnapshot
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/session/SessionStore.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace batbox::agents {

// =============================================================================
// SubAgent::Status — lifecycle state enum
// =============================================================================
enum class SubAgentStatus {
    queued,     ///< Constructed, waiting for a semaphore slot before starting
    running,    ///< jthread active, conversation loop in progress
    done,       ///< Completed successfully
    failed,     ///< Terminated with an inference or tool error
    cancelled,  ///< Cancelled via cancel() or parent token
};

// =============================================================================
// SubAgent — per-agent jthread with owned ConversationEngine
// =============================================================================

/// Owns one conversation context running in a dedicated std::jthread.
///
/// Constructed by AgentSupervisor::spawn() with an AgentSpec, an initial
/// prompt, and a parent CancelToken.  The jthread is started immediately
/// by calling start(); callers (AgentSupervisor) may defer start() until
/// a semaphore slot is available.
///
/// Blueprint contract: batbox::agents::SubAgent (blueprints row 16760)
class SubAgent {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // agent_id      — opaque UUID string assigned by AgentSupervisor
    // spec          — configuration (name, model, allowed_tools, prompt_body)
    // initial_prompt— first user-turn message delivered to the conversation
    // parent_ct     — parent cancellation token; when fired, child also cancels
    // event_queue   — shared MPSC queue; SubAgent pushes all lifecycle events here
    // cfg           — batbox runtime config (model, API key, etc.)
    // on_exit       — callback invoked once when the jthread finishes; used by
    //                 AgentSupervisor to release the semaphore slot
    // -------------------------------------------------------------------------
    SubAgent(std::string             agent_id,
             AgentSpec               spec,
             std::string             initial_prompt,
             batbox::CancelToken     parent_ct,
             AgentEventQueue&        event_queue,
             const batbox::config::Config& cfg,
             std::function<void()>   on_exit);

    // Non-copyable, non-movable (owns jthread and mutexes).
    SubAgent(const SubAgent&)            = delete;
    SubAgent& operator=(const SubAgent&) = delete;
    SubAgent(SubAgent&&)                 = delete;
    SubAgent& operator=(SubAgent&&)      = delete;

    /// Destructor: requests stop and joins the jthread (jthread does this by
    /// default, but we also call cancel() to ensure cooperative exit).
    ~SubAgent();

    // -------------------------------------------------------------------------
    // start() — launch the jthread and begin the conversation loop
    //
    // Changes status from queued → running.
    // Safe to call exactly once after construction.
    // -------------------------------------------------------------------------
    void start();

    // -------------------------------------------------------------------------
    // cancel() — request cooperative cancellation
    //
    // Signals stop_source_.request_stop().  The running conversation will
    // exit at its next CancelToken checkpoint (stream_chat cancellation,
    // or inter-turn check).  Thread-safe; may be called from any thread.
    //
    // Blueprint contract: batbox::agents::SubAgent::cancel (row 16762)
    // -------------------------------------------------------------------------
    void cancel();

    // -------------------------------------------------------------------------
    // enqueue_message() — inject a peer message into the agent's input queue
    //
    // The message will be delivered to the conversation as a new user-turn
    // after the current inference call completes.  Thread-safe.
    // -------------------------------------------------------------------------
    void enqueue_message(std::string_view message);

    // -------------------------------------------------------------------------
    // snapshot() — produce a point-in-time AgentSnapshot for the TUI
    //
    // Acquires snapshot_mutex_ briefly; designed to be fast for the 10Hz ticker.
    // -------------------------------------------------------------------------
    [[nodiscard]] AgentSnapshot snapshot() const;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] const std::string& id()   const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return spec_.name; }
    [[nodiscard]] SubAgentStatus     status() const noexcept {
        return status_.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // Standing mode — the warm, interrogable window (S2/S3, DIS-988)
    // -------------------------------------------------------------------------

    // promote() — mark this subagent STANDING.
    //
    // A standing subagent does NOT collapse its conversation to a string and
    // exit when it reaches quiescence (no pending work) — it PARKS with its
    // Conversation still alive on the jthread stack, waiting for follow-up
    // interrogations.  This is the goose deferred-closed fix: the parent talks
    // to a warm window, not a re-spawned one.  Idempotent; thread-safe.
    void promote() noexcept;

    // is_standing() — true once promote() has been called.  Thread-safe.
    [[nodiscard]] bool is_standing() const noexcept {
        return standing_.load(std::memory_order_acquire);
    }

    // interrogate() — issue a follow-up user turn against the still-engulfed
    // context and get the answer.
    //
    // Returns a future that resolves to the visible output of the follow-up
    // turn.  The source is NOT re-engulfed: the question runs against the warm
    // Conversation built by the original run.  Thread-safe; may be called from
    // any thread (the parent).
    //
    // SAFETY (AC5): never hangs, never throws.  If the agent is not standing,
    // has terminated, or has been cancelled/evicted, the returned future is
    // already satisfied with an empty string (the "no warm window" sentinel).
    // Any interrogation in flight when the agent exits for ANY reason is
    // fulfilled with the sentinel by the run loop's reaper, so a caller blocked
    // on .get() is always released.
    [[nodiscard]] std::future<std::string> interrogate(std::string question);

    // last_result() — the most recent quiescent result summary (the string the
    // status line surfaces).  Empty until the first turn completes.  Thread-safe.
    [[nodiscard]] std::string last_result() const;

private:
    // -------------------------------------------------------------------------
    // run() — conversation loop, executes inside the jthread
    //
    // Blueprint contract: batbox::agents::SubAgent::run (row 16761)
    // -------------------------------------------------------------------------
    void run(std::stop_token st);

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /// Append a line to the rolling last-5-lines output buffer (under lock).
    void append_output_line(std::string_view line);

    /// Drain and return all pending injected messages (under lock); clears queue.
    [[nodiscard]] std::vector<std::string> drain_pending_messages();

    /// Post a status-transition event to the shared queue and update status_.
    void set_status(SubAgentStatus s);

    /// A queued follow-up turn for a standing subagent and the promise that
    /// receives its answer.  The promise is shared so interrogate() can hold the
    /// future end while the run loop fulfils the other end.
    struct PendingInterrogation {
        std::string                               question;
        std::shared_ptr<std::promise<std::string>> answer;
    };

    /// Close the interrogation channel on run-loop exit (ANY path): mark
    /// terminated_, fulfil @p current (the in-flight question, if any) and every
    /// still-queued interrogation with the empty sentinel so no caller blocked on
    /// .get() ever hangs.  Idempotent (guarded by terminated_).  Called by the
    /// run loop's RAII reaper.
    void terminate_interrogations(
        const std::shared_ptr<std::promise<std::string>>& current) noexcept;

    // -------------------------------------------------------------------------
    // Members
    // -------------------------------------------------------------------------

    const std::string   id_;
    const AgentSpec     spec_;
    const std::string   initial_prompt_;

    // Shared event queue (non-owning reference; AgentSupervisor owns it).
    AgentEventQueue&    event_queue_;

    // Runtime config (non-owning reference; outlives this SubAgent).
    const batbox::config::Config& cfg_;

    // Called once when the jthread exits (releases supervisor semaphore slot).
    std::function<void()> on_exit_;

    // Per-agent cancellation: child of the parent token so that both the
    // parent conversation and this agent's own cancel() can stop the agent.
    batbox::CancelSource child_source_;
    batbox::CancelToken  child_token_;

    // Lifecycle status; updated atomically from the jthread.
    std::atomic<SubAgentStatus> status_{SubAgentStatus::queued};

    // Input queue for peer messages (guarded by msg_mutex_).
    mutable std::mutex          msg_mutex_;
    std::deque<std::string>     pending_messages_;

    // Rolling last-5-lines output buffer for AgentSnapshot (guarded by snapshot_mutex_).
    mutable std::mutex          snapshot_mutex_;
    std::vector<std::string>    last_5_lines_;
    std::string                 current_step_;
    std::size_t                 token_count_{0};

    // Most recent quiescent result summary (guarded by snapshot_mutex_); the
    // string the standing-status line surfaces for this warm window.
    std::string                 last_result_;

    // -- Standing mode (S2/S3, DIS-988) --------------------------------------
    // standing_: set by promote(); makes the run loop park instead of exit.
    std::atomic<bool>           standing_{false};

    // Interrogation channel between the parent (interrogate()) and the run loop.
    // interrogate() pushes a PendingInterrogation and notifies; the parked run
    // loop pops it, runs a follow-up turn, and fulfils its promise.
    mutable std::mutex                    interrogate_mutex_;
    std::condition_variable               interrogate_cv_;
    std::deque<PendingInterrogation>      interrogations_;
    // terminated_: once the run loop has left for good, interrogate() must
    // fulfil its own promise with the sentinel rather than enqueue (no consumer
    // would ever pop it).  Guarded by interrogate_mutex_.
    bool                                  terminated_{false};

    // The jthread; declared last so that all other members are initialised
    // before the thread can access them via the run() lambda.
    std::jthread                thread_;
};

// =============================================================================
// status_label() — human-readable string for SubAgentStatus
// =============================================================================

[[nodiscard]] inline const char* status_label(SubAgentStatus s) noexcept {
    switch (s) {
        case SubAgentStatus::queued:    return "queued";
        case SubAgentStatus::running:   return "running";
        case SubAgentStatus::done:      return "completed";
        case SubAgentStatus::failed:    return "errored";
        case SubAgentStatus::cancelled: return "cancelled";
    }
    return "unknown";
}

} // namespace batbox::agents
