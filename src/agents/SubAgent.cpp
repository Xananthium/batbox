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
#include <batbox/inference/Provider.hpp>   // AC1 (DIS-988): ProviderRegistry seam

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
// promote() — mark this subagent standing (S2/S3, DIS-988)
// =============================================================================

void SubAgent::promote() noexcept {
    standing_.store(true, std::memory_order_release);
    // Wake the run loop in case it is already parked at quiescence (harmless if
    // it is mid-turn — the flag is re-read at the next quiescence).
    interrogate_cv_.notify_one();
}

// =============================================================================
// interrogate() — follow-up turn against the still-warm window
// =============================================================================

std::future<std::string> SubAgent::interrogate(std::string question) {
    auto prom = std::make_shared<std::promise<std::string>>();
    std::future<std::string> fut = prom->get_future();

    bool enqueued = false;
    {
        std::lock_guard<std::mutex> lock{interrogate_mutex_};
        if (!terminated_
            && standing_.load(std::memory_order_acquire)
            && !child_token_.is_cancelled()) {
            interrogations_.push_back(
                PendingInterrogation{std::move(question), prom});
            enqueued = true;
        }
    }

    if (enqueued) {
        interrogate_cv_.notify_one();
    } else {
        // SAFETY (AC5): no warm window is available to answer this — there is no
        // consumer that would ever pop the request, so fulfil the future now
        // with the empty "no warm window" sentinel.  Never hang the caller.
        prom->set_value(std::string{});
    }
    return fut;
}

// =============================================================================
// last_result() — most recent quiescent result summary (status-line source)
// =============================================================================

std::string SubAgent::last_result() const {
    std::lock_guard<std::mutex> lock{snapshot_mutex_};
    return last_result_;
}

// =============================================================================
// terminate_interrogations() — close the channel on run-loop exit (AC5)
//
// Called by the run loop's RAII reaper on EVERY exit path (normal return,
// cancellation, eviction, exception unwind).  Marks the channel terminated and
// fulfils the in-flight question plus every queued one with the empty sentinel,
// so a parent blocked on interrogate().get() is always released.  Idempotent.
// =============================================================================

void SubAgent::terminate_interrogations(
    const std::shared_ptr<std::promise<std::string>>& current) noexcept {
    std::deque<PendingInterrogation> leftover;
    {
        std::lock_guard<std::mutex> lock{interrogate_mutex_};
        if (terminated_) {
            return;  // idempotent — already closed
        }
        terminated_ = true;
        leftover.swap(interrogations_);
    }

    // Each promise is fulfilled exactly once (quiescence clears `current` before
    // the next pop), but guard defensively: set_value throws only if the promise
    // is already satisfied or has no shared state.
    auto safe_fulfill = [](const std::shared_ptr<std::promise<std::string>>& p) {
        if (!p) return;
        try { p->set_value(std::string{}); } catch (...) { /* already satisfied */ }
    };
    safe_fulfill(current);
    for (auto& pi : leftover) {
        safe_fulfill(pi.answer);
    }
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

    // -------------------------------------------------------------------------
    // AC1 (DIS-988) — optional endpoint override.
    //
    // A standing subagent runs on the local 3090 (free compute), not the single
    // usually-cloud cfg.api endpoint.  AgentSpec::model overrides only the model
    // *name*; this moves the *endpoint*.  Mechanism mirrors tools::SubagentDistiller
    // (DIS-980): overwrite agent_cfg.api.* so the inference client resolves
    // provider identity / canonical model / usage from a self-consistent Config.
    // nullopt → agent_cfg keeps cfg.api unchanged (existing behaviour & tests).
    if (spec_.endpoint.has_value()) {
        const auto& ep = spec_.endpoint.value();
        if (ep.use_distill_endpoint) {
            // "Run on the local distill endpoint" selector — reuse cfg.distill.*.
            agent_cfg.api.base_url            = cfg_.distill.base_url;
            agent_cfg.api.api_key             = cfg_.distill.api_key;
            agent_cfg.api.default_model       = cfg_.distill.model;
            agent_cfg.api.request_timeout_sec = cfg_.distill.request_timeout_sec;
        } else {
            if (!ep.base_url.empty()) agent_cfg.api.base_url      = ep.base_url;
            agent_cfg.api.api_key                                 = ep.api_key;
            if (!ep.model.empty())    agent_cfg.api.default_model = ep.model;
        }
        // Let the provider auto-detect (ollama/openai) from the new base_url.
        agent_cfg.api.provider_hint.clear();
    }

    // Resolve the provider via the S8 ProviderRegistry (not a hand-rolled Client):
    // this is the single seam that maps Config → provider identity, and supplies
    // the S9 context-ownership predicate.  Conversation still consumes a Client&
    // (the Provider abstraction is not yet wired into Conversation — deferred S8
    // work, out of scope here), so we read the predicate and construct the Client
    // from the same agent_cfg the registry resolved against.
    const bool provider_owns_context =
        batbox::inference::ProviderRegistry::create(agent_cfg)->manages_own_context();

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

    // S9 (DIS-988 AC1): if the resolved provider owns its own context window,
    // batbox stands down its compaction for this agent's conversation.  Default
    // false for every current OpenAI-compatible endpoint — no behaviour change.
    conv.set_manages_own_context(provider_owns_context);

    // -------------------------------------------------------------------------
    // Standing-mode interrogation state (S2/S3, DIS-988).
    //   pending_answer — promise to fulfil at the next quiescence when the
    //                    current turn was driven by an interrogation.
    //   interrogation_reaper — fires on ANY exit from run() (return, cancel,
    //                    eviction, exception unwind), closing the interrogation
    //                    channel so no parent blocked on interrogate().get()
    //                    ever hangs (AC5).  Declared after `conv` so it is
    //                    destroyed before `conv`.
    // -------------------------------------------------------------------------
    std::shared_ptr<std::promise<std::string>> pending_answer;
    struct InterrogationReaper {
        SubAgent*                                    self;
        std::shared_ptr<std::promise<std::string>>*  slot;
        ~InterrogationReaper() { self->terminate_interrogations(*slot); }
    } interrogation_reaper{this, &pending_answer};

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
            // -----------------------------------------------------------------
            // Quiescent: no pending peer messages.  Build the result summary.
            // -----------------------------------------------------------------
            const bool standing = standing_.load(std::memory_order_acquire);
            std::string summary;
            {
                std::lock_guard<std::mutex> lock{snapshot_mutex_};
                for (const auto& line : last_5_lines_) {
                    if (!summary.empty()) summary += '\n';
                    summary += line;
                }
                last_result_  = summary;
                current_step_ = standing ? "standing (warm)" : "done";
            }

            // If this turn was driven by an interrogation, answer it now and
            // clear the slot so the reaper never double-fulfils the promise.
            if (pending_answer) {
                try { pending_answer->set_value(summary); } catch (...) {}
                pending_answer.reset();
            }

            // Surface the result either way (the parent / status line reads it).
            event_queue_.push(AgentEvent::make_completed(id_, summary));

            // -----------------------------------------------------------------
            // Closed mode (default, every non-promoted agent): collapse the
            // window and exit.  conv is destroyed here — existing behaviour.
            // Only here do we transition to the terminal `done` status.
            // -----------------------------------------------------------------
            if (!standing) {
                set_status(SubAgentStatus::done);
                return;
            }

            // -----------------------------------------------------------------
            // Standing mode (S2/S3, DIS-988): PARK with the Conversation still
            // alive on this stack — the window is NOT collapsed to a string.
            // Status stays `running` (NOT done): the warm window is alive, which
            // is what lets the supervisor distinguish a parked-and-warm agent
            // from a truly-exited one (so promote() on a corpse stays a safe
            // no-op rather than double-releasing its slot — AC5).
            // Wait for the next interrogation (or cancel/eviction).
            // -----------------------------------------------------------------
            std::string next_question;
            {
                std::unique_lock<std::mutex> lock{interrogate_mutex_};
                interrogate_cv_.wait(lock, [this] {
                    return !interrogations_.empty()
                           || child_token_.is_cancelled();
                });

                if (child_token_.is_cancelled()) {
                    // LRU-evicted or parent-cancelled.  Lossless by
                    // construction: the gold is already in the notepad (S6),
                    // reachable via the re-injected pad after the window dies.
                    // The reaper fulfils any queued interrogations with the
                    // sentinel as this frame unwinds.
                    set_status(SubAgentStatus::done);
                    event_queue_.push(AgentEvent::make_cancelled(id_, "evicted"));
                    return;
                }

                PendingInterrogation pi = std::move(interrogations_.front());
                interrogations_.pop_front();
                next_question  = std::move(pi.question);
                pending_answer = std::move(pi.answer);
            }

            // Resume the warm window: deliver the follow-up turn against the
            // still-engulfed context (the source is NOT re-engulfed) and loop.
            event_queue_.push(AgentEvent::make_step_began(
                id_, "interrogate", next_question.substr(0, 80)));
            conv.user_message(next_question);
            continue;
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
