#pragma once

#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/autonomous/Engine.hpp>
#include <batbox/permissions/PermissionMode.hpp>

namespace batbox::app {

void configure_worker_runtime(agents::AgentSupervisor& supervisor,
    const autonomous::Options& options);
void configure_worker_runtime(agents::AgentSupervisor& supervisor,
    permissions::PermissionMode mode);

} // namespace batbox::app
