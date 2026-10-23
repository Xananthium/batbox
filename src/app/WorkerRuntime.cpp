#include <batbox/app/WorkerRuntime.hpp>
#include <batbox/app/WireTools.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/skills/BundledSkillsRegistry.hpp>

namespace batbox::app {
namespace {

// A worker gets private mutable tool state. These members are ordered so the
// registry dies before its dependencies; Conversation dies before this owner.
struct NativeWorkerRuntime {
    std::mutex config_mutex;
    conversation::PlanMode plan_mode;
    sidecar::SidecarManager sidecar;
    mcp::McpServerRegistry mcp;
    plugins::SkillLoader skills;
    std::shared_ptr<permissions::PermissionStore> permission_store;
    permissions::PermissionGate gate;
    tools::ToolRegistry registry;

    NativeWorkerRuntime(config::Config& cfg, agents::AgentSupervisor& supervisor,
                        permissions::PermissionMode mode,
                        const std::optional<autonomous::Options>& options)
        : sidecar(cfg.sidecar)
        , permission_store(std::make_shared<permissions::PermissionStore>(
              cfg.general.config_dir / "settings.json"))
        , gate(permission_store, mode, [](std::string_view, const Json&) {
              return permissions::Decision::deny_with_reason(
                  "Worker approval requires the parent; use the parent's authorized permission mode.");
          }) {
        skills.set_bundled_skills(batbox::skills::BundledSkillsRegistry::all());
        skills.load_user_dirs();
        wire_tools(registry, cfg, config_mutex, sidecar, mcp, supervisor,
            agents::TeamRegistry::instance(), skills, plan_mode,
            [mode](const std::string&) { return mode == permissions::PermissionMode::Nuclear; });
        if (options) autonomous::register_engine_tools(registry, *options);
        // MCP transports are worker-owned too, so no callback can outlive them.
        auto [source, token] = CancelToken::make_root();
        (void)mcp.start_all(std::move(token));
    }
};

void configure(agents::AgentSupervisor& supervisor, permissions::PermissionMode mode,
               std::optional<autonomous::Options> options) {
    supervisor.set_worker_runtime_factory([&supervisor, mode, options](
        config::Config& cfg, const agents::AgentSpec&) -> agents::WorkerRuntime {
        auto owner = std::make_shared<NativeWorkerRuntime>(cfg, supervisor, mode, options);
        return {owner, &owner->registry, &owner->gate, &owner->plan_mode, &owner->config_mutex};
    });
}
} // namespace

void configure_worker_runtime(agents::AgentSupervisor& supervisor,
                              const autonomous::Options& options) {
    configure(supervisor, permissions::PermissionMode::Nuclear, options);
}

void configure_worker_runtime(agents::AgentSupervisor& supervisor,
                              permissions::PermissionMode mode) {
    configure(supervisor, mode, std::nullopt);
}

} // namespace batbox::app
