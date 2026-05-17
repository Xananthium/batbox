// =============================================================================
// src/agents/SubAgent.cpp — per-agent jthread + ConversationEngine
//
// SubAgent owns one Conversation instance running in a dedicated std::jthread.
// The conversation loop:
//   1. Posts AgentEvent::Started
//   2. Delivers initial_prompt_ as the first user message
//   3. Loops: run_turn → check cancellation → drain pending messages → repeat
//   4. Posts Completed / Cancelled / Errored on exit
//   5. Fires on_exit_ to release the supervisor's semaphore slot
//
// Blueprint contract: batbox::agents::SubAgent (blueprints rows 16760–16763)
// =============================================================================

#include <batbox/agents/SubAgent.hpp>
#include <batbox/conversation/Conversation.hpp>
#include <batbox/core/CancelToken.hpp>

#include <string>
#include <utility>
#include <vector>

namespace batbox::agents {

// =============================================================================
// Construction
// =============================================================================

SubAgent::SubAgent(std::string              agent_id,
                   AgentSpec                spec,
                   std::string              initial_prompt,
                   batbox::CancelToken      parent_ct,
                   AgentEventQueue&         event_queue,
                   const batbox::config::Config& cfg,
                   std::function<void()>    on_exit)
    : id_(std::move(agent_id))
    , spec_(std::move(spec))
    , initial_prompt_(std::move(initial_prompt))
    , event_queue_(event_queue)
    , cfg_(cfg)
    , on_exit_(std::move(on_exit))
{
    // Create a child token linked to the parent: fires when parent fires OR
    // when child_source_.request_stop() is called (via cancel()).
    auto [child_src, child_tok] = parent_ct.child();
    child_source_ = std::move(child_src);
    child_token_  = std::move(child_tok);

    // Status begins at queued; jthread not yet started.
    status_.store(SubAgentStatus::queued, std::memory_order_release);
}

// =============================================================================
// Destructor
// =============================================================================

SubAgent::~SubAgent() {
    // Request stop so the run() loop can exit cooperatively, then jthread
    // destructor joins.  Calling cancel() here is safe if already cancelled.
    cancel();
    // jthread joins in its own destructor (requests stop + joins).
}

// =============================================================================
// start() — launch the jthread
// =============================================================================

void SubAgent::start() {
    // Transition status: queued → running.
    status_.store(SubAgentStatus::running, std::memory_order_release);

    thread_ = std::jthread([this](std::stop_token st) {
        run(std::move(st));
    });
}

// =============================================================================
// cancel() — request cooperative cancellation
// =============================================================================

void SubAgent::cancel() {
    child_source_.request_stop();
}

// =============================================================================
// enqueue_message() — inject a peer message into the input queue
// =============================================================================

void SubAgent::enqueue_message(std::string_view message) {
    std::lock_guard<std::mutex> lock{msg_mutex_};
    pending_messages_.emplace_back(std::string(message));
}

// =============================================================================
// snapshot() — point-in-time read for the TUI 10Hz ticker
// =============================================================================

AgentSnapshot SubAgent::snapshot() const {
    AgentSnapshot snap;
    snap.id   = id_;
    snap.name = spec_.name;
    snap.status = status_label(status_.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock{snapshot_mutex_};
        snap.current_step = current_step_;
        snap.last_5_lines = last_5_lines_;
        snap.token_count  = token_count_;
    }
    return snap;
}

// =============================================================================
// Private helpers
// =============================================================================

void SubAgent::append_output_line(std::string_view line) {
    std::lock_guard<std::mutex> lock{snapshot_mutex_};
    last_5_lines_.emplace_back(std::string(line));
    if (last_5_lines_.size() > 5) {
        last_5_lines_.erase(last_5_lines_.begin());
    }
}

std::vector<std::string> SubAgent::drain_pending_messages() {
    std::lock_guard<std::mutex> lock{msg_mutex_};
    std::vector<std::string> out;
    out.reserve(pending_messages_.size());
    for (auto& m : pending_messages_) {
        out.push_back(std::move(m));
    }
    pending_messages_.clear();
    return out;
}

void SubAgent::set_status(SubAgentStatus s) {
    status_.store(s, std::memory_order_release);
}

// =============================================================================
// run() — conversation loop; called from the jthread
//
// Blueprint contract: batbox::agents::SubAgent::run (row 16761)
// =============================================================================

void SubAgent::run(std::stop_token /*st*/) {
    // -------------------------------------------------------------------------
    // Guard: ensure on_exit_ fires when we leave, regardless of path.
    // -------------------------------------------------------------------------
    struct ExitGuard {
        std::function<void()>& fn;
        ~ExitGuard() { if (fn) fn(); }
    } exit_guard{on_exit_};

    try {
    // -------------------------------------------------------------------------
    // Early cancellation check — avoid constructing expensive infrastructure if
    // the parent already cancelled the token before the jthread even started.
    // -------------------------------------------------------------------------
    if (child_token_.is_cancelled()) {
        set_status(SubAgentStatus::cancelled);
        event_queue_.push(AgentEvent::make_cancelled(id_, "pre_start"));
        return;
    }

    // -------------------------------------------------------------------------
    // Post Started event.
    // -------------------------------------------------------------------------
    event_queue_.push(AgentEvent::make_started(id_, spec_.name));

    // -------------------------------------------------------------------------
    // Build per-agent inference dependencies.
    // Apply the agent's model override if specified.
    // -------------------------------------------------------------------------
    batbox::config::Config agent_cfg = cfg_;
    if (spec_.model.has_value()) {
        agent_cfg.api.default_model =
            batbox::config::resolve_model_alias(spec_.model.value(), cfg_);
    }

    batbox::inference::Client   client{agent_cfg};
    batbox::session::SessionStore store{}; // uses default sessions dir

    // Accumulate the current turn's output into a line buffer so we can
    // split on newlines and update last_5_lines_ correctly.
    std::string line_buffer;

    // on_delta callback: called by Conversation::run_turn for each SSE token.
    auto on_delta = [this, &line_buffer](std::string_view chunk) {
        // Push TokenAppended event to the shared queue.
        event_queue_.push(AgentEvent::make_token_appended(
            id_, std::string(chunk)));

        // Accumulate into line_buffer; flush complete lines to last_5_lines_.
        for (char c : chunk) {
            if (c == '\n') {
                if (!line_buffer.empty()) {
                    append_output_line(line_buffer);
                    line_buffer.clear();
                }
            } else {
                line_buffer += c;
            }
        }

        // Update token count under the snapshot lock.
        {
            std::lock_guard<std::mutex> lock{snapshot_mutex_};
            ++token_count_;
        }
    };

    // -------------------------------------------------------------------------
    // Construct the Conversation: owns messages_ and drives inference turns
    // on this thread only.
    // -------------------------------------------------------------------------
    batbox::conversation::Conversation conv{
        client,
        store,
        agent_cfg,
        /*working_dir=*/{},
        on_delta
    };

    // -------------------------------------------------------------------------
    // Deliver the initial user prompt.
    // -------------------------------------------------------------------------
    conv.user_message(initial_prompt_);

    // -------------------------------------------------------------------------
    // Conversation loop.
    // -------------------------------------------------------------------------
    while (true) {
        // Check for cancellation before starting a new turn.
        if (child_token_.is_cancelled()) {
            if (!line_buffer.empty()) {
                append_output_line(line_buffer);
                line_buffer.clear();
            }
            set_status(SubAgentStatus::cancelled);
            event_queue_.push(AgentEvent::make_cancelled(id_, "stop_requested"));
            return;
        }

        // Update current_step for snapshot.
        {
            std::lock_guard<std::mutex> lock{snapshot_mutex_};
            current_step_ = "inference";
        }

        // Run one inference turn.
        // Vend a fresh linked child token for this turn (CancelToken is move-only;
        // child_token_ must remain valid for is_cancelled() checks).
        auto [turn_src, turn_tok] = child_token_.child();
        (void)turn_src;  // turn_src kept alive until end of loop iteration
        auto turn_result = conv.run_turn(std::move(turn_tok));

        if (!turn_result.has_value()) {
            const std::string& err = turn_result.error();

            // Flush partial line.
            if (!line_buffer.empty()) {
                append_output_line(line_buffer);
                line_buffer.clear();
            }

            if (err == "cancelled" || child_token_.is_cancelled()) {
                set_status(SubAgentStatus::cancelled);
                event_queue_.push(AgentEvent::make_cancelled(id_, err));
            } else {
                set_status(SubAgentStatus::failed);
                event_queue_.push(AgentEvent::make_errored(id_, err));
            }
            return;
        }

        // Flush any remaining partial line from this turn.
        if (!line_buffer.empty()) {
            append_output_line(line_buffer);
            line_buffer.clear();
        }

        // Check for cancellation after turn completes.
        if (child_token_.is_cancelled()) {
            set_status(SubAgentStatus::cancelled);
            event_queue_.push(AgentEvent::make_cancelled(id_, "stop_requested"));
            return;
        }

        // Drain any peer messages enqueued during this turn.
        auto pending = drain_pending_messages();

        if (pending.empty()) {
            // No pending messages: agent work is complete.
            std::string summary;
            {
                std::lock_guard<std::mutex> lock{snapshot_mutex_};
                for (const auto& line : last_5_lines_) {
                    if (!summary.empty()) summary += '\n';
                    summary += line;
                }
                current_step_ = "done";
            }

            set_status(SubAgentStatus::done);
            event_queue_.push(AgentEvent::make_completed(id_, summary));
            return;
        }

        // Deliver each injected message as a new user turn and continue the loop.
        for (const auto& msg : pending) {
            conv.user_message(msg);
            event_queue_.push(AgentEvent::make_step_began(
                id_, "peer_message", msg.substr(0, 80)));
        }

        // Loop back to run_turn.
    }

    } catch (const std::exception& ex) {
        // Catch any uncaught exception from inference/session/conversation setup.
        // Post an Errored event so the TUI and supervisor see the failure.
        // The ExitGuard will fire on_exit_ after this catch block returns.
        set_status(SubAgentStatus::failed);
        event_queue_.push(AgentEvent::make_errored(id_, ex.what()));
    } catch (...) {
        set_status(SubAgentStatus::failed);
        event_queue_.push(AgentEvent::make_errored(id_, "unknown exception"));
    }
}

} // namespace batbox::agents
