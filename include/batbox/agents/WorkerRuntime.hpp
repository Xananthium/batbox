#pragma once

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/config/Config.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>

namespace batbox::tools { class ToolRegistry; }
namespace batbox::permissions { class PermissionGate; }
namespace batbox::conversation { class PlanMode; }

namespace batbox::agents {

// The factory lives at the application boundary; the agent library does not
// depend on plugins or application wiring. Ownership keeps every referenced
// dependency alive until the worker's Conversation has been destroyed.
struct WorkerRuntime {
    std::shared_ptr<void> owner;
    tools::ToolRegistry* registry = nullptr;
    permissions::PermissionGate* gate = nullptr;
    conversation::PlanMode* plan_mode = nullptr;
    std::mutex* config_mutex = nullptr;
};

using WorkerRuntimeFactory =
    std::function<WorkerRuntime(config::Config&, const AgentSpec&)>;

} // namespace batbox::agents
