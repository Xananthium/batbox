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
#include <batbox/core/Uuid.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/inference/Provider.hpp>   // AC1 (DIS-988): ProviderRegistry seam

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace batbox::agents {
namespace {
std::string utc_now() {
    const auto now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return out;
}
}

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

    // Wake a PARKED standing agent on cancellation.  The park wait blocks on
    // interrogate_cv_; cancellation (parent-cascade or self) trips child_token_
    // but does not notify the cv, so register a callback that does.  Without
    // this the loop would sleep through an eviction / parent-cancel and the
    // jthread join would hang.  The handle is kept alive as a member.
    cancel_wake_handle_ = child_token_.on_cancel([this]() {
        interrogate_cv_.notify_all();
    });

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
    if (thread_.joinable()) thread_.join();
    terminate_interrogations(nullptr);
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
// prepare_resume() — DIS-1021: arm run() to restore() from a prior session log
// =============================================================================

void SubAgent::prepare_resume(batbox::session::SessionFile sf) {
    resume_from_ = std::move(sf);
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
    // Publish under the same mutex as the park predicate to prevent a lost
    // wakeup between checking the inbox and entering condition_variable::wait.
    {
        std::lock_guard wake_lock{interrogate_mutex_};
        const auto status = status_.load();
        const bool closed = terminated_ || status == SubAgentStatus::done ||
            status == SubAgentStatus::failed || status == SubAgentStatus::cancelled;
        if (closed && (!journal_ || !journal_->read().value("recoverable", false))) return;
        std::string entry(message);
        if (journal_) {
            entry = "[worker-message:" + Uuid::v4().to_string() + "]\n" + entry;
            journal_->update([&](Json& j) { j["inbox"].push_back(entry); });
        }
        if (closed) return; // recovery will load this durable inbox
        std::lock_guard lock{msg_mutex_};
        pending_messages_.push_back(std::move(entry));
    }
    interrogate_cv_.notify_one();
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
            if (journal_) {
                question = "[worker-message:" + Uuid::v4().to_string() + "]\n" + question;
                journal_->update([&](Json& j) { j["inbox"].push_back(question); });
            }
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
// set_quiescence_hook_for_test() — install the DIS-1001 quiescence seam
// =============================================================================

void SubAgent::set_quiescence_hook_for_test(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock{test_hook_mutex_};
    quiescence_hook_for_test_ = std::move(hook);
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

void SubAgent::configure_runtime(WorkerRuntimeFactory factory,
    std::filesystem::path working_dir, std::filesystem::path sessions_dir,
    std::shared_ptr<AgentJournal> journal, bool continuous,
    std::function<void()> on_park,
    std::function<bool(const CancelToken&)> on_resume) {
    runtime_factory_ = std::move(factory);
    working_dir_ = std::move(working_dir);
    sessions_dir_ = std::move(sessions_dir);
    journal_ = std::move(journal);
    continuous_ = continuous;
    on_park_ = std::move(on_park);
    on_resume_ = std::move(on_resume);
    if (journal_) {
        const auto j = journal_->read();
        for (const auto& entry : j.value("inbox", Json::array()))
            pending_messages_.push_back(entry.get<std::string>());
        if (j.value("standing", false)) promote();
    }
}

void SubAgent::close_durably() {
    if (journal_) journal_->update([](Json& j) { j["recoverable"] = false; });
}

void SubAgent::run(std::stop_token /*st*/) {
    struct ExitGuard {
        SubAgent* self;
        ~ExitGuard() {
            self->terminate_interrogations(nullptr);
            if (self->on_exit_) self->on_exit_();
        }
    } exit_guard{this};
    std::shared_ptr<std::promise<std::string>> pending_answer;
    struct Reaper {
        SubAgent* self;
        std::shared_ptr<std::promise<std::string>>& answer;
        ~Reaper() { self->terminate_interrogations(answer); }
    } reaper{this, pending_answer};

    try {
        if (child_token_.is_cancelled()) {
            set_status(SubAgentStatus::cancelled);
            event_queue_.push(AgentEvent::make_cancelled(id_, "pre_start"));
            return;
        }
        event_queue_.push(AgentEvent::make_started(id_, spec_.name));
        config::Config agent_cfg = cfg_;
        if (spec_.model)
            agent_cfg.api.default_model = config::resolve_model_alias(*spec_.model, cfg_);
        if (spec_.endpoint) {
            const auto& ep = *spec_.endpoint;
            if (ep.use_distill_endpoint) {
                agent_cfg.api.base_url = cfg_.distill.base_url;
                agent_cfg.api.api_key = cfg_.distill.api_key;
                agent_cfg.api.default_model = cfg_.distill.model;
                agent_cfg.api.request_timeout_sec = cfg_.distill.request_timeout_sec;
            } else {
                if (!ep.base_url.empty()) agent_cfg.api.base_url = ep.base_url;
                if (!ep.api_key.empty()) agent_cfg.api.api_key = ep.api_key;
                if (!ep.model.empty()) agent_cfg.api.default_model = ep.model;
            }
            agent_cfg.api.provider_hint.clear();
        }
        inference::Client client{agent_cfg};
        session::SessionStore store{sessions_dir_.empty()
            ? paths::config_dir() / "sessions" : sessions_dir_};
        WorkerRuntime runtime;
        if (runtime_factory_) runtime = runtime_factory_(agent_cfg, spec_);
        if (runtime_factory_ && (!runtime.registry || !runtime.gate))
            throw std::runtime_error("Worker runtime factory did not provide tools and permissions");

        std::string turn_output;
        std::string line_buffer;
        auto progress = [&](std::string_view operation, std::string_view phase) {
            { std::lock_guard lock(snapshot_mutex_); current_step_ = phase; }
            if (journal_) journal_->update([&](Json& j) {
                j["phase"] = phase;
                j["updated_at"] = utc_now();
                j["last_progress_at"] = j["updated_at"];
                j["last_operation"] = operation;
                j["progress_sequence"] = j.value("progress_sequence", 0ULL) + 1;
            });
        };
        auto last_stream_progress = std::chrono::steady_clock::time_point{};
        auto on_delta = [&](std::string_view chunk) {
            turn_output.append(chunk);
            event_queue_.push(AgentEvent::make_token_appended(id_, std::string(chunk)));
            for (const char c : chunk) {
                if (c == '\n') { append_output_line(line_buffer); line_buffer.clear(); }
                else line_buffer += c;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!chunk.empty() && now - last_stream_progress >= std::chrono::seconds(1)) {
                progress("stream_delta_received", "streaming");
                last_stream_progress = now;
            }
            std::lock_guard lock(snapshot_mutex_);
            ++token_count_;
        };
        conversation::Conversation conv{client, store, agent_cfg, working_dir_, on_delta,
            runtime.registry, runtime.gate, runtime.plan_mode, runtime.config_mutex};
        conv.set_allowed_tools(spec_.allowed_tools);
        conv.set_manages_own_context(inference::ProviderRegistry::create(agent_cfg)->manages_own_context());
        conv.set_on_context_progress_cb([&](std::string_view operation) {
            progress(operation, operation == "reasoning_delta" ? "reasoning" :
                (operation.starts_with("compaction_") ? "compacting" : "context_prepared"));
        });
        conv.set_on_tool_running_cb([&](std::string_view, std::string_view, int) {
            progress("tool_batch_started", "tools_running");
        });
        conv.set_on_message_appended_cb([&](std::string_view role, std::string_view,
                                           std::string_view, bool) {
            progress(role == "tool" ? "tool_result_persisted" :
                     (role == "assistant" ? "assistant_message_persisted" : "message_persisted"),
                     role == "tool" ? "tool_result" : "message_recorded");
        });
        conv.set_on_reasoning_started_cb([&] { progress("reasoning_started", "reasoning"); });
        conv.set_on_usage_delta_cb([&](uint32_t, double) {
            progress("model_usage_received", "model_response");
        });
        if (resume_from_) {
            store.adopt_restored(*resume_from_);
            if (auto result = conv.restore(*resume_from_); !result)
                throw std::runtime_error("Worker restore failed: " + result.error());
            if (auto result = conv.recover_interrupted_tools(); !result)
                throw std::runtime_error("Worker tool recovery failed: " + result.error());
            // Route new peer/child work through this live handle while the
            // adopted session header retains the original durable lineage.
            conv.set_session_identity(id_, resume_from_->endpoint);
        } else {
            Json endpoint{{"base_url", agent_cfg.api.base_url}, {"model", agent_cfg.api.default_model},
                {"use_distill_endpoint", spec_.endpoint && spec_.endpoint->use_distill_endpoint},
                {"api_key_ref", spec_.endpoint && spec_.endpoint->use_distill_endpoint ? "cfg.distill" : "cfg.api"}};
            conv.set_session_identity(id_, std::move(endpoint));
            if (auto result = conv.start_session(); !result)
                throw std::runtime_error("Worker journal open failed: " + result.error());
        }
        if (journal_) journal_->update([&](Json& j) {
            j["session_id"] = conv.session_id();
            j["status"] = "running";
            j["phase"] = "restored";
            j["updated_at"] = utc_now();
            j["tool_count"] = runtime.registry ? runtime.registry->size() : 0;
            j["exact_compaction_tokens"] = agent_cfg.compact.exact_token_threshold;
        });

        auto deliver = [&](const std::string& message) {
            // An inbox acknowledgement may have been interrupted after its
            // transcript append. Stable message IDs make that boundary idempotent.
            const auto& history = conv.messages();
            const bool recorded = message.starts_with("[worker-message:") &&
                std::any_of(history.begin(), history.end(), [&](const auto& m) { return m.content == message; });
            if (!recorded) {
                if (auto result = conv.user_message_durable(message); !result)
                    throw std::runtime_error("Worker prompt persistence failed: " + result.error());
            }
            if (journal_) journal_->update([&](Json& j) {
                auto& inbox = j["inbox"];
                auto found = std::find(inbox.begin(), inbox.end(), Json(message));
                if (found != inbox.end()) inbox.erase(found);
            });
        };
        if (!resume_from_ || conv.messages().empty()) {
            std::string prompt;
            if (!spec_.prompt_body.empty()) prompt = "Agent role instructions:\n" + spec_.prompt_body + "\n\n";
            prompt += initial_prompt_.empty() && journal_
                ? journal_->read().value("prompt", "") : initial_prompt_;
            deliver(prompt);
        } else if (!initial_prompt_.empty()) {
            deliver(initial_prompt_);
        }
        auto peer_messages = drain_pending_messages();
        for (const auto& message : peer_messages) deliver(message);

        // Restoring a quiescent standing window must not ask the model to
        // repeat its last completed action merely because the process restarted.
        bool need_turn = !resume_from_ || !initial_prompt_.empty() || !peer_messages.empty();
        if (resume_from_ && !need_turn) {
            const auto& history = conv.messages();
            need_turn = history.empty() || history.back().role != conversation::Role::Assistant
                || (history.back().tool_calls && !history.back().tool_calls->empty());
        }
        int cycles = 0;
        while (!child_token_.is_cancelled()) {
            if (need_turn) {
                if (!continuous_ && agent_cfg.agents.max_subagent_turn_cycles > 0 &&
                    cycles >= agent_cfg.agents.max_subagent_turn_cycles) {
                    event_queue_.push(AgentEvent::make_doom_loop_guard(id_, cycles));
                    event_queue_.push(AgentEvent::make_completed(id_, last_result()));
                    if (journal_) journal_->update([](Json& j) { j["recoverable"] = false; j["status"] = "completed"; });
                    set_status(SubAgentStatus::done);
                    return;
                }
                {
                    std::lock_guard lock(snapshot_mutex_);
                    current_step_ = "inference";
                }
                if (journal_) journal_->update([](Json& j) {
                    j["status"] = "running";
                    j["phase"] = "preparing_turn";
                    j["updated_at"] = utc_now();
                });
                turn_output.clear();
                last_stream_progress = {};
                auto [source, token] = child_token_.child();
                auto result = conv.run_turn(std::move(token));
                if (!result) {
                    if (child_token_.is_cancelled() || result.error() == "cancelled") break;
                    throw std::runtime_error(result.error());
                }
                ++cycles;
                if (!line_buffer.empty()) { append_output_line(line_buffer); line_buffer.clear(); }
                {
                    std::lock_guard lock(snapshot_mutex_);
                    last_result_ = turn_output;
                    current_step_ = standing_.load() ? "standing (warm)" : "done";
                }
                if (journal_) journal_->update([&](Json& j) {
                    j["status"] = standing_.load() ? "standing" : "completed";
                    j["last_result"] = turn_output;
                    j["completed_turns"] = j.value("completed_turns", 0ULL) + 1;
                    j["prompt_tokens"] = conv.last_prompt_tokens();
                    j["compactions"] = conv.compaction_count();
                    j["recovery_failures"] = 0;
                    j["phase"] = "turn_completed";
                    j["last_operation"] = "turn_completed";
                    j["updated_at"] = utc_now();
                    j["last_progress_at"] = j["updated_at"];
                    j["progress_sequence"] = j.value("progress_sequence", 0ULL) + 1;
                    j.erase("retry_delay_seconds");
                });
                if (pending_answer) { pending_answer->set_value(turn_output); pending_answer.reset(); }
                event_queue_.push(AgentEvent::make_completed(id_, turn_output));
            }
            if (child_token_.is_cancelled()) break;
            peer_messages = drain_pending_messages();
            if (!peer_messages.empty()) {
                conv.compact_for_standing_turn();
                for (const auto& message : peer_messages) deliver(message);
                need_turn = true;
                continue;
            }
            const bool standing = standing_.load(std::memory_order_acquire);
            std::function<void()> hook;
            { std::lock_guard lock(test_hook_mutex_); hook = quiescence_hook_for_test_; }
            if (hook) hook();
            if (!standing) {
                // Serialize natural close with enqueue_message: any message
                // accepted before close gets its own turn, never a lost wakeup.
                std::lock_guard close_lock(interrogate_mutex_);
                { std::lock_guard inbox_lock(msg_mutex_);
                  if (!pending_messages_.empty()) { need_turn = false; continue; } }
                if (journal_) journal_->update([](Json& j) { j["recoverable"] = false; });
                set_status(SubAgentStatus::done);
                return;
            }
            {
                std::lock_guard lock(snapshot_mutex_);
                current_step_ = "standing (warm)";
            }
            if (on_park_) on_park_();
            std::string question;
            {
                std::unique_lock lock(interrogate_mutex_);
                auto ready = [&] {
                    std::lock_guard inbox_lock(msg_mutex_);
                    return child_token_.is_cancelled() || !interrogations_.empty() || !pending_messages_.empty();
                };
                if (continuous_) interrogate_cv_.wait_for(lock, std::chrono::seconds(2), ready);
                else interrogate_cv_.wait(lock, ready);
                if (child_token_.is_cancelled()) break;
                if (!interrogations_.empty()) {
                    auto pending = std::move(interrogations_.front());
                    interrogations_.pop_front();
                    question = std::move(pending.question);
                    pending_answer = std::move(pending.answer);
                }
            }
            if (on_resume_ && !on_resume_(child_token_)) break;
            conv.compact_for_standing_turn();
            peer_messages = drain_pending_messages();
            for (const auto& message : peer_messages) deliver(message);
            if (!question.empty()) deliver(question);
            else if (peer_messages.empty() && continuous_)
                deliver("Continue your assigned role with one concrete useful step. Consult shared tasks and memory, "
                        "verify your result, record evidence and the next action, and coordinate through the native task tools. "
                        "Avoid repeating completed work. If blocked, inspect the cause and record what is needed.");
            need_turn = true;
        }
        if (journal_) journal_->update([](Json& j) {
            j["status"] = "interrupted";
            j["phase"] = "interrupted";
            j["updated_at"] = utc_now();
        });
        set_status(SubAgentStatus::cancelled);
        event_queue_.push(AgentEvent::make_cancelled(id_, "stop_requested"));
    } catch (const std::exception& ex) {
        set_status(SubAgentStatus::failed);
        append_output_line(ex.what());
        if (journal_) {
            try { journal_->update([&](Json& j) {
                j["status"] = "errored";
                j["failed_phase"] = j.value("phase", "unknown");
                j["phase"] = "errored";
                j["last_error"] = ex.what();
                j["last_error_at"] = utc_now();
                j["updated_at"] = j["last_error_at"];
            }); }
            catch (...) {}
        }
        event_queue_.push(AgentEvent::make_errored(id_, ex.what()));
    } catch (...) {
        set_status(SubAgentStatus::failed);
        event_queue_.push(AgentEvent::make_errored(id_, "unknown exception"));
    }
}

} // namespace batbox::agents
