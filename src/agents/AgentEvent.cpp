// =============================================================================
// src/agents/AgentEvent.cpp — AgentEvent named constructors + AgentEventQueue
// =============================================================================

#include "batbox/agents/AgentEvent.hpp"

#include <stdexcept>
#include <utility>

namespace batbox::agents {

// ---------------------------------------------------------------------------
// Internal helper — current wall-clock time
// ---------------------------------------------------------------------------
namespace {
[[nodiscard]] inline std::chrono::system_clock::time_point now_ts() noexcept {
    return std::chrono::system_clock::now();
}
} // namespace

// ---------------------------------------------------------------------------
// AgentEvent named constructors
// ---------------------------------------------------------------------------

AgentEvent AgentEvent::make_started(std::string agent_id, std::string display_name) {
    return AgentEvent{
        std::move(agent_id),
        Kind::Started,
        std::move(display_name),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_step_began(std::string agent_id,
                                        std::string step_name,
                                        std::string description) {
    std::string payload;
    payload.reserve(step_name.size() + 2 + description.size());
    payload += step_name;
    payload += ": ";
    payload += description;
    return AgentEvent{
        std::move(agent_id),
        Kind::StepBegan,
        std::move(payload),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_token_appended(std::string agent_id, std::string chunk) {
    return AgentEvent{
        std::move(agent_id),
        Kind::TokenAppended,
        std::move(chunk),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_tool_call_began(std::string agent_id,
                                             std::string tool_name,
                                             std::string input_json) {
    std::string payload;
    payload.reserve(tool_name.size() + 2 + input_json.size());
    payload += tool_name;
    payload += ": ";
    payload += input_json;
    return AgentEvent{
        std::move(agent_id),
        Kind::ToolCallBegan,
        std::move(payload),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_tool_call_ended(std::string agent_id, std::string result_summary) {
    return AgentEvent{
        std::move(agent_id),
        Kind::ToolCallEnded,
        std::move(result_summary),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_completed(std::string agent_id, std::string output_summary) {
    return AgentEvent{
        std::move(agent_id),
        Kind::Completed,
        std::move(output_summary),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_errored(std::string agent_id, std::string error_message) {
    return AgentEvent{
        std::move(agent_id),
        Kind::Errored,
        std::move(error_message),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_cancelled(std::string agent_id, std::string reason) {
    return AgentEvent{
        std::move(agent_id),
        Kind::Cancelled,
        std::move(reason),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_parent_message_observed(std::string agent_id,
                                                      std::string turn_summary) {
    return AgentEvent{
        std::move(agent_id),
        Kind::ParentMessageObserved,
        std::move(turn_summary),
        now_ts(),
    };
}

AgentEvent AgentEvent::make_queued(std::string agent_id, std::string position_hint) {
    return AgentEvent{
        std::move(agent_id),
        Kind::Queued,
        std::move(position_hint),
        now_ts(),
    };
}

const char* AgentEvent::kind_label(Kind k) noexcept {
    switch (k) {
        case Kind::Started:                 return "started";
        case Kind::StepBegan:               return "step_began";
        case Kind::TokenAppended:           return "token_appended";
        case Kind::ToolCallBegan:           return "tool_call_began";
        case Kind::ToolCallEnded:           return "tool_call_ended";
        case Kind::Completed:               return "completed";
        case Kind::Errored:                 return "errored";
        case Kind::Cancelled:               return "cancelled";
        case Kind::ParentMessageObserved:   return "parent_message_observed";
        case Kind::Queued:                  return "queued";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// AgentEventQueue
// ---------------------------------------------------------------------------

void AgentEventQueue::push(AgentEvent event) {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        queue_.push_back(std::move(event));
    }
    // Increment dirty seq AFTER the item is in the queue so that a reader
    // who observes the new seq is guaranteed to find at least one item.
    dirty_seq_.fetch_add(1, std::memory_order_release);
    // Wake exactly one waiter; drain() / wait_pop() will consume the item.
    cv_.notify_one();
}

std::optional<AgentEvent> AgentEventQueue::try_pop() {
    std::lock_guard<std::mutex> lock{mutex_};
    if (queue_.empty()) {
        return std::nullopt;
    }
    AgentEvent event = std::move(queue_.front());
    queue_.pop_front();
    return event;
}

std::optional<AgentEvent> AgentEventQueue::wait_pop(std::stop_token stop) {
    std::unique_lock<std::mutex> lock{mutex_};
    // condition_variable_any natively supports stop_token in C++20.
    cv_.wait(lock, stop, [this] { return !queue_.empty(); });
    if (queue_.empty()) {
        // Woken by stop_token, not by a new event.
        return std::nullopt;
    }
    AgentEvent event = std::move(queue_.front());
    queue_.pop_front();
    return event;
}

std::vector<AgentEvent> AgentEventQueue::drain() {
    std::lock_guard<std::mutex> lock{mutex_};
    if (queue_.empty()) {
        return {};
    }
    std::vector<AgentEvent> result;
    result.reserve(queue_.size());
    for (auto& e : queue_) {
        result.push_back(std::move(e));
    }
    queue_.clear();
    return result;
}

std::size_t AgentEventQueue::size() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return queue_.size();
}

bool AgentEventQueue::empty() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return queue_.empty();
}

} // namespace batbox::agents
