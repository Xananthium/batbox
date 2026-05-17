#pragma once
// =============================================================================
// batbox/agents/AgentEvent.hpp — Agent lifecycle event type + MPSC queue
//
// AgentEvent
//   Carries a single agent lifecycle notification from a sub-agent thread to
//   the TUI ticker.  Defined as a tagged struct (agent_id, Kind, payload, ts)
//   rather than a variant so all consumers can dispatch on a single enum
//   without visitor boilerplate.
//
//   Kind values (all 10):
//     Started                – agent thread acquired a semaphore slot and began
//     StepBegan              – entering a named reasoning/tool step
//     TokenAppended          – streaming token received from inference
//     ToolCallBegan          – about to execute a tool call
//     ToolCallEnded          – tool call returned
//     Completed              – agent finished successfully
//     Errored                – agent terminated with an error
//     Cancelled              – agent was stopped via stop_token
//     ParentMessageObserved  – parent conversation turn completed (demon hook)
//     Queued                 – agent is waiting for a semaphore slot (all 4 in use)
//
// payload semantics by Kind:
//   Started                → agent display name
//   StepBegan              → "step_name: description"
//   TokenAppended          → the partial output chunk
//   ToolCallBegan          → "tool_name: input_json"
//   ToolCallEnded          → result summary (truncated)
//   Completed              → final output summary
//   Errored                → error message
//   Cancelled              → reason string (may be empty)
//   ParentMessageObserved  → parent turn summary
//   Queued                 → position hint, e.g. "queued, 1/4"
//
// AgentEventQueue
//   Thread-safe MPSC (multi-producer, single-consumer) queue.
//   Multiple sub-agent std::jthreads push(); the TUI ticker calls drain() or
//   wait_pop() from a single consumer thread.
//
//   API:
//     void push(AgentEvent)
//       Appends an event; wakes all threads blocked in wait_pop().
//       Increments the dirty sequence counter atomically.
//
//     std::optional<AgentEvent> try_pop()
//       Non-blocking dequeue from the front.  Returns std::nullopt when empty.
//
//     std::optional<AgentEvent> wait_pop(const std::stop_token&)
//       Blocks until an event is available OR the stop_token fires.
//       Returns std::nullopt on cancellation.
//
//     std::vector<AgentEvent> drain()
//       Removes and returns ALL pending events in one call; intended for the
//       TUI ticker which wants a consistent snapshot then re-renders once.
//       Returns an empty vector when the queue is empty.
//
//     uint64_t dirty_seq() const noexcept
//       Monotonically-increasing counter incremented on every push().
//       The 10Hz TUI ticker compares this against its last-rendered seq to
//       decide whether a re-render is needed (zero CPU when quiet).
//
// Thread safety:
//   push() — safe to call from any number of threads simultaneously.
//   try_pop() / wait_pop() / drain() — intended for a SINGLE consumer thread;
//   concurrent calls from multiple consumers produce undefined behaviour.
//
// =============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace batbox::agents {

// ---------------------------------------------------------------------------
// AgentEvent
// ---------------------------------------------------------------------------
struct AgentEvent {
    // Unique identifier of the sub-agent that emitted this event.
    std::string agent_id;

    // Lifecycle discriminant — drives TUI rendering decisions.
    enum class Kind {
        Started,
        StepBegan,
        TokenAppended,
        ToolCallBegan,
        ToolCallEnded,
        Completed,
        Errored,
        Cancelled,
        ParentMessageObserved,
        Queued,
    } kind;

    // Kind-dependent payload string (see file header for semantics).
    std::string payload;

    // Wall-clock timestamp at the moment the event was constructed.
    std::chrono::system_clock::time_point ts;

    // -----------------------------------------------------------------------
    // Named constructors — produce a fully-initialised AgentEvent with ts=now.
    // -----------------------------------------------------------------------

    [[nodiscard]] static AgentEvent make_started(std::string agent_id,
                                                  std::string display_name);

    [[nodiscard]] static AgentEvent make_step_began(std::string agent_id,
                                                     std::string step_name,
                                                     std::string description);

    [[nodiscard]] static AgentEvent make_token_appended(std::string agent_id,
                                                         std::string chunk);

    [[nodiscard]] static AgentEvent make_tool_call_began(std::string agent_id,
                                                          std::string tool_name,
                                                          std::string input_json);

    [[nodiscard]] static AgentEvent make_tool_call_ended(std::string agent_id,
                                                          std::string result_summary);

    [[nodiscard]] static AgentEvent make_completed(std::string agent_id,
                                                    std::string output_summary);

    [[nodiscard]] static AgentEvent make_errored(std::string agent_id,
                                                  std::string error_message);

    [[nodiscard]] static AgentEvent make_cancelled(std::string agent_id,
                                                    std::string reason = {});

    [[nodiscard]] static AgentEvent make_parent_message_observed(std::string agent_id,
                                                                   std::string turn_summary);

    [[nodiscard]] static AgentEvent make_queued(std::string agent_id,
                                                 std::string position_hint = {});

    // -----------------------------------------------------------------------
    // Utility
    // -----------------------------------------------------------------------

    // Returns a short human-readable label for Kind (e.g. "token_appended").
    [[nodiscard]] static const char* kind_label(Kind k) noexcept;
    [[nodiscard]] const char* kind_label() const noexcept { return kind_label(kind); }
};

// ---------------------------------------------------------------------------
// AgentEventQueue — MPSC queue with dirty-sequence change detection
// ---------------------------------------------------------------------------
class AgentEventQueue {
public:
    AgentEventQueue() = default;

    // Non-copyable, non-movable (mutex + condvar are not movable).
    AgentEventQueue(const AgentEventQueue&)            = delete;
    AgentEventQueue& operator=(const AgentEventQueue&) = delete;
    AgentEventQueue(AgentEventQueue&&)                 = delete;
    AgentEventQueue& operator=(AgentEventQueue&&)      = delete;

    // -----------------------------------------------------------------------
    // push(event)
    // Appends event to the back of the queue, increments dirty_seq_, and
    // notifies one waiting consumer.  Safe to call from any thread.
    // -----------------------------------------------------------------------
    void push(AgentEvent event);

    // -----------------------------------------------------------------------
    // try_pop()
    // Non-blocking; returns the front event (removing it) or std::nullopt.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<AgentEvent> try_pop();

    // -----------------------------------------------------------------------
    // wait_pop(stop_token)
    // Blocks until an event is available or stop_token is cancelled.
    // Returns std::nullopt when cancelled before an event arrived.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<AgentEvent> wait_pop(std::stop_token stop);

    // -----------------------------------------------------------------------
    // drain()
    // Removes and returns ALL currently-queued events in arrival order.
    // Returns an empty vector if the queue is empty.
    // Intended for the TUI ticker: grab everything, render once, sleep.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::vector<AgentEvent> drain();

    // -----------------------------------------------------------------------
    // dirty_seq()
    // Monotonically-increasing counter; incremented on every push().
    // The 10Hz ticker checks (dirty_seq() != last_rendered_seq) to decide
    // whether to wake FTXUI for a re-render.  Load with acquire semantics.
    // -----------------------------------------------------------------------
    [[nodiscard]] uint64_t dirty_seq() const noexcept {
        return dirty_seq_.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    // size() — approximate (snapshot under lock); useful for diagnostics.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t size() const;

    // -----------------------------------------------------------------------
    // empty() — true when no events are pending (snapshot under lock).
    // -----------------------------------------------------------------------
    [[nodiscard]] bool empty() const;

private:
    mutable std::mutex              mutex_;
    std::condition_variable_any     cv_;
    std::deque<AgentEvent>          queue_;
    std::atomic<uint64_t>           dirty_seq_{0};
};

} // namespace batbox::agents
