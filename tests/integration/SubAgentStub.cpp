// SubAgentStub.cpp — minimal linker stub for integration tests that include
// AgentSupervisor but never call spawn() at runtime.

#include <batbox/agents/SubAgent.hpp>
#include <batbox/agents/AgentEvent.hpp>
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/core/CancelToken.hpp>

#include <functional>
#include <stdexcept>
#include <string>

namespace batbox::agents {

namespace {
    // Static dummy objects used only to satisfy the reference member initializers.
    AgentEventQueue   g_dummy_event_queue;
    batbox::config::Config g_dummy_cfg;
}

SubAgent::SubAgent(std::string             agent_id,
                   AgentSpec               spec,
                   std::string             initial_prompt,
                   batbox::CancelToken     parent_ct,
                   AgentEventQueue&        event_queue,
                   const batbox::config::Config& cfg,
                   std::function<void()>   on_exit)
    : id_(std::move(agent_id))
    , spec_(std::move(spec))
    , initial_prompt_(std::move(initial_prompt))
    , event_queue_(event_queue)
    , cfg_(cfg)
    , on_exit_(std::move(on_exit))
    , child_source_{}
    , child_token_(child_source_.token())
{
    throw std::runtime_error("SubAgent stub: real spawn not available in test builds");
}

SubAgent::~SubAgent() = default;

void SubAgent::start() {
    throw std::runtime_error("SubAgent stub: start() not available in test builds");
}

void SubAgent::cancel() {}

void SubAgent::enqueue_message(std::string_view /*message*/) {}

AgentSnapshot SubAgent::snapshot() const {
    AgentSnapshot snap;
    snap.id     = id_;
    snap.name   = spec_.name;
    snap.status = "queued";
    return snap;
}

} // namespace batbox::agents
