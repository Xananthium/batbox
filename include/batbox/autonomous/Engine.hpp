#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <filesystem>
#include <string>

namespace batbox::agents { class AgentSupervisor; }

namespace batbox::autonomous {

// Autonomous mode is a persistent owner of the same Conversation used by the
// terminal client. No separate model/provider implementation lives here.
struct Options {
    std::filesystem::path state_dir;
    std::filesystem::path letter_file;
    std::filesystem::path project_root;
    std::string memory_url = "http://127.0.0.1:8771";
    std::string goal;
    int interval_seconds = 60;
    unsigned long long max_cycles = 0; // zero: continue until SIGINT/SIGTERM
    std::size_t worker_count = 3; // coordinator + three persistent workers
};

void register_engine_tools(tools::ToolRegistry& registry, const Options& options);

int run(const Options& options, config::Config& config, tools::ToolRegistry& registry,
        agents::AgentSupervisor& supervisor);

} // namespace batbox::autonomous
