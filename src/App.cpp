// src/App.cpp
// =============================================================================
// batbox::App implementation — CPP B.11 scaffold + CPP A.1 headless mode
//                              + CPP A.3 full interactive init wiring
//                              + CPP A.4 clean shutdown sequence.
//
// This file owns the App::run() entry point which:
//   1. Initialises the spdlog logging subsystem (batbox::log::init_logging).
//   2. Loads the .env config file (batbox::config::load_env_file).
//   3. Prints the splash banner (unless --subagent or --print mode).
//   4. Prints the ☢️ nuclear banner if --nuclear was passed.
//   5a. If --print or --subagent: runs a single-turn headless conversation
//       (CPP A.1). No TUI bootstrap. No FTXUI dependency in this path.
//   5b. Otherwise: TUI + REPL wired in CPP A.3.
//
// CPP A.1 — Headless --print mode
// ================================
// When --print is active, run_headless():
//   1. Reads the prompt from args.prompt (argv/stdin).
//   2. Applies model override if --model was passed.
//   3. Instantiates Client + SessionStore + Conversation with the wired ToolRegistry.
//   4. Calls conversation.user_message(prompt) + run_turn(cancel_token).
//   5. Collects the assistant content via the on_delta callback.
//   6. Emits output to stdout:
//        plain/markdown: content streamed token-by-token (newline appended at end)
//        json:           {"role":"assistant","content":"...","usage":{...}}
//   7. Returns: 0 success, 1 model error, 130 SIGINT.
//
// Tool calls in headless mode:
//   The same ToolRegistry as TUI mode is wired into headless mode.
//   The PermissionGate is configured for non-interactive use (nuclear or AcceptEdits).
//   Tool calls work identically to interactive mode; the sidecar prewarm is skipped
//   (it is an HTTP cost, controlled by BATBOX_SIDECAR_PREWARM separately).
//
// CPP A.3 — Interactive App::init wiring order
// =============================================
//   Step 1:  Load .env + process env (load_env_file + merge_with_process_env).
//   Step 2:  Build Config from env map (Config::load_from_env).
//   Step 3:  Init logging (log::init_logging).
//   Step 4:  Set PermissionMode from --nuclear flag / config.security.
//   Step 5:  Discover + load plugins via PluginLoader → PluginRegistry.
//   Step 6:  Load bundled skills + user-dir skills into SkillLoader.
//   Step 7:  Wire all 39 tools (wire_tools).
//   Step 8:  Wire all 38 slash commands (wire_commands) — now with live plugin ptrs.
//   Step 9:  Initialize SubAgentRegistry / AgentSupervisor (already constructed).
//   Step 10: Spawn Python scrapling sidecar prewarm (async, non-blocking).
//   Step 11: Connect MCP clients from ~/.batbox/mcp.json (start_all — graceful).
//   Step 12: Initialize TUI via wire_tui.
//
// CPP A.4 — Shutdown sequence (REVERSE of init)
// ==============================================
// After screen_mgr.run() returns (user quit, /exit, or SIGTERM), App::shutdown()
// is called with a ShutdownContext that bundles pointers to all live subsystems.
//
// SIGTERM handling:
//   g_sigterm_received is set in the async-signal-safe handler.
//   The main thread calls screen_mgr.stop() after detecting the flag.
//   Teardown always runs on the main thread via App::shutdown().
//
// Shutdown order (reverse of init):
//   1. Stop TUI (ScreenManager::stop — drains FTXUI event queue)
//   2. Disconnect MCP clients (McpServerRegistry::stop_all)
//   3. Terminate sidecar (SidecarManager::shutdown — POST /shutdown + kill)
//   4. Stop AgentSupervisor (cancel all + wait_all)
//   5. Session store flush (SessionStore destructor handles SQLite close)
//   6. Unload plugins (PluginRegistry cleared)
//   7. Flush spdlog (spdlog::shutdown())
// =============================================================================

#include "App.hpp"

#include <batbox/core/Logging.hpp>
#include <batbox/config/EnvLoader.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/tui/PermissionCard.hpp>
#include <batbox/tui/PlanApprovalCard.hpp>
#include <batbox/tui/QuestionCard.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/conversation/Conversation.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/repl/CommandContext.hpp>
#include <batbox/commands/ISlashCommand.hpp>

// CPP 5.30 — ToolRegistry wire-up
#include <batbox/app/WireTools.hpp>

// CPP S.15 — SlashCommandRegistry wire-up
#include <batbox/app/WireCommands.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/plugins/PluginLoader.hpp>
#include <batbox/plugins/PluginRegistry.hpp>
#include <batbox/tui/DemonPanel.hpp>

// Bundled skills (CPP K.0)
#include <batbox/skills/BundledSkillsRegistry.hpp>

// CPP 1.15 — TUI layout wire-up
#include <batbox/app/WireTui.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/Screen.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/repl/Keybindings.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/agents/Team.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/conversation/PlanMode.hpp>

#include <atomic>
#include <mutex>

#include <cstdlib>    // std::getenv
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

// POSIX: signal handling for Ctrl+C and SIGTERM (CPP 7.6, CPP A.4)
#include <signal.h>
#include <csignal>
#include <ctime>
#include <unistd.h>  // isatty, STDIN_FILENO

namespace batbox {

// App::shutdown_called_ is defined in AppShutdown.cpp (CPP A.4).
// App::shutdown() and App::reset_shutdown_flag() are in AppShutdown.cpp.

namespace {

// ---------------------------------------------------------------------------
// Bat ASCII art (Miss Kittin era — lowercase, thin box-drawing chars).
// Designed to sit cleanly in an 80-column terminal.
// ---------------------------------------------------------------------------
constexpr std::string_view kBatAscii = R"(
      ___,  ,___
     (  Y    Y  )
      \ \  / /
   ___/ `--' \___
  /   /  bat  \   \
 /   /  box   \   \
 \  /  0.1.0   \  /
  \/____________\/
)";

// ---------------------------------------------------------------------------
// Miss Kittin track-title taglines (deterministic per day-of-year).
// ---------------------------------------------------------------------------
constexpr std::string_view kTaglines[] = {
    "frank sinatra",
    "1982",
    "requiem for a dead star",
    "stock exchange",
    "professional distortion",
    "the hacker",
    "get it while you can",
    "i am not a robot",
    "grand theft auto",
    "land of the living",
    "the new saint-tropez",
    "batbox · made for the fall of dance music · since 2026",
};
constexpr std::size_t kTaglineCount =
    sizeof(kTaglines) / sizeof(kTaglines[0]);

// Return a tagline deterministically seeded by the day-of-year.
std::string_view daily_tagline() {
    std::time_t t = std::time(nullptr);
    std::tm* lt   = std::localtime(&t);
    int day        = lt ? lt->tm_yday : 0;
    return kTaglines[static_cast<std::size_t>(day) % kTaglineCount];
}

// Resolve the .env file path: --env flag, then $BATBOX_CONFIG_DIR/.env,
// then ~/.batbox/.env.
std::filesystem::path resolve_env_file(const AppArgs& args) {
    if (!args.env_file.empty()) {
        return std::filesystem::path(args.env_file);
    }
    const char* config_dir = std::getenv("BATBOX_CONFIG_DIR");
    if (config_dir && *config_dir != '\0') {
        return std::filesystem::path(config_dir) / ".env";
    }
    const char* home = std::getenv("HOME");
    if (home && *home != '\0') {
        return std::filesystem::path(home) / ".batbox" / ".env";
    }
    return std::filesystem::path(".batbox") / ".env";
}

// ---------------------------------------------------------------------------
// Signal handling globals (CPP 7.6, CPP A.4).
//
// All signal handlers are async-signal-safe: they only write to volatile
// sig_atomic_t variables.  All real work happens on the main thread.
// ---------------------------------------------------------------------------

// Counter incremented in the signal handler — one bump per SIGINT (Ctrl+C).
static volatile sig_atomic_t g_sigint_count = 0;

// Timestamp of the most recent SIGINT, in seconds since epoch.
static volatile sig_atomic_t g_sigint_time_sec = 0;

// Set to 1 when SIGTERM is received — main thread calls screen_mgr.stop().
static volatile sig_atomic_t g_sigterm_received = 0;

// Async-signal-safe SIGINT handler (CPP 7.6).
static void sigint_handler(int /*sig*/) noexcept {
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    g_sigint_time_sec = static_cast<sig_atomic_t>(ts.tv_sec);
    g_sigint_count = g_sigint_count + 1;  // NOLINT — volatile; avoid deprecated ++ on volatile
}

// Async-signal-safe SIGTERM handler (CPP A.4).
// Only sets the flag; the main thread checks it after screen_mgr.run() or
// in a poll loop and then calls screen_mgr.stop() + App::shutdown().
static void sigterm_handler(int /*sig*/) noexcept {
    g_sigterm_received = 1;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// App::print_splash
// ---------------------------------------------------------------------------
void App::print_splash(const AppArgs& args) {
    if (args.subagent || args.print_mode) {
        return; // suppress in headless / sub-agent mode
    }
    std::cout << kBatAscii
              << "\n  batbox 0.1.0\n"
              << "  " << daily_tagline() << "\n\n"
              << std::flush;
}

// ---------------------------------------------------------------------------
// App::print_nuclear_banner
// ---------------------------------------------------------------------------
void App::print_nuclear_banner() {
    // Decision of Record #6: nuclear mode must emit the magenta banner.
    // We use ANSI escape codes here; the TUI layer (CPP 1.X) will replace
    // this with a proper FTXUI-rendered modal/status-bar in interactive mode.
    // For headless --print mode this plain-text banner is the correct output.
    constexpr std::string_view kMagenta = "\033[35m";
    constexpr std::string_view kBold    = "\033[1m";
    constexpr std::string_view kReset   = "\033[0m";

    std::cerr << kBold << kMagenta
              << "\n  ☢️  NUCLEAR MODE — ALL PERMISSIONS BYPASSED\n"
              << kReset << std::flush;
}

// ---------------------------------------------------------------------------
// App::run_headless  (CPP A.1)
// ---------------------------------------------------------------------------
// Single-turn non-TUI conversation.  No FTXUI components are instantiated.
//
// Prompt resolution order:
//   1. args.prompt (set by positional arg or --prompt flag in argv)
//   2. args.prompt_flag (kept for compat; CLI11 merges both into args.prompt)
//   3. stdin (if both are empty and stdin is not a terminal — i.e. piped)
//
// Exit codes:
//   0   — success
//   1   — inference/model/restore error, or no prompt provided
//   2   — --resume <uuid> session not found in the store
//   130 — interrupted by SIGINT
// ---------------------------------------------------------------------------
int App::run_headless(const AppArgs& args, const batbox::config::Config& cfg_in,
                      batbox::tools::ToolRegistry& tool_registry,
                      batbox::permissions::PermissionGate& gate) {
    // ------------------------------------------------------------------
    // 1. Resolve prompt text.
    // ------------------------------------------------------------------
    std::string prompt = args.prompt;
    if (prompt.empty()) {
        prompt = args.prompt_flag;
    }
    if (prompt.empty()) {
        // Read from stdin when it is not a terminal (pipe / here-string).
        // isatty() returns 0 when stdin is redirected.
        if (!::isatty(STDIN_FILENO)) {
            std::ostringstream oss;
            oss << std::cin.rdbuf();
            prompt = oss.str();
            // Trim trailing newline from here-string / echo usage.
            while (!prompt.empty() &&
                   (prompt.back() == '\n' || prompt.back() == '\r')) {
                prompt.pop_back();
            }
        }
    }
    if (prompt.empty()) {
        std::cerr << "batbox: --print requires a prompt "
                     "(positional, --prompt, or piped via stdin)\n";
        return 1;
    }

    // ------------------------------------------------------------------
    // 2. Apply --model override to a copy of the config.
    //    cfg_in already has all .env values; we only patch model here.
    // ------------------------------------------------------------------
    batbox::config::Config cfg = cfg_in;
    if (!args.model.empty()) {
        cfg.api.default_model = args.model;
        BATBOX_LOG_DEBUG("headless: model override → {}", args.model);
    }

    // ------------------------------------------------------------------
    // 3. Install SIGINT handler.
    //
    // We snapshot g_sigint_count before the turn, then check it after.
    // The signal handler only sets the volatile counter (async-signal-safe).
    // The CancelSource is requested to stop on the main thread after reading
    // the counter — no C++ calls inside the signal handler.
    // ------------------------------------------------------------------
    const int sigint_before = static_cast<int>(g_sigint_count);
    ::signal(SIGINT, sigint_handler);

    // ------------------------------------------------------------------
    // 4. Build the component graph:
    //      Client → SessionStore → Conversation (with ToolRegistry + gate)
    //
    //    Headless sessions are persisted to the normal session store so
    //    they appear in /session history (and `--resume` can reload them).
    //    The ToolRegistry and PermissionGate are passed in from App::run()
    //    — they are the same fully-wired objects used in interactive mode.
    // ------------------------------------------------------------------
    batbox::inference::Client client{cfg};
    batbox::session::SessionStore store{};

    // ------------------------------------------------------------------
    // 5. CancelToken pair — the token is passed into run_turn().
    //    The source is checked after the turn completes and on SIGINT.
    // ------------------------------------------------------------------
    auto [cancel_src, cancel_tok] = batbox::CancelToken::make_root();

    // ------------------------------------------------------------------
    // 6. Accumulated assistant content (filled by on_delta_cb).
    //
    //    plain / markdown: tokens are streamed live to stdout via on_delta.
    //    json:             tokens accumulate silently; output emitted at end.
    // ------------------------------------------------------------------
    std::string assistant_content;

    auto on_delta = [&](std::string_view chunk) {
        assistant_content += chunk;
        if (args.print_format != PrintFormat::Json) {
            std::cout << chunk << std::flush;
        }
    };

    // ------------------------------------------------------------------
    // 7. Construct Conversation with the wired ToolRegistry + gate.
    //    Both are passed in from App::run() — same objects TUI mode uses.
    // ------------------------------------------------------------------
    batbox::conversation::Conversation conversation{
        client,
        store,
        cfg,
        std::filesystem::current_path(), // working_dir
        on_delta,
        &tool_registry,   // ToolRegistry — fully wired, same as interactive mode
        &gate             // PermissionGate — non-interactive (nuclear/AcceptEdits)
    };

    // ------------------------------------------------------------------
    // 7b. If --resume <uuid> was provided, seed the conversation with prior
    //     history BEFORE the new user turn.
    //
    //     Exit code 2 if the session is not found (clear diagnostic to stderr).
    //     Touch the session's last-accessed timestamp so it floats to the top
    //     of list_recent() results.
    // ------------------------------------------------------------------
    if (!args.resume_id.empty()) {
        auto sf_res = store.load(args.resume_id);
        if (!sf_res) {
            std::cerr << "Session " << args.resume_id << " not found\n";
            return 2;
        }
        auto restore_res = conversation.restore(sf_res.value());
        if (!restore_res) {
            std::cerr << "batbox: failed to restore session: "
                      << restore_res.error() << '\n';
            return 1;
        }
        // Bump last-accessed so this session surfaces at the top of /session.
        (void)store.touch(args.resume_id);
        std::cerr << "Resumed session " << args.resume_id
                  << " (" << conversation.messages().size() << " messages)\n";
    }

    // ------------------------------------------------------------------
    // 8. Submit user prompt.
    // ------------------------------------------------------------------
    conversation.user_message(prompt);

    // Check for pre-run SIGINT.
    if (static_cast<int>(g_sigint_count) > sigint_before) {
        cancel_src.request_stop();
        ::signal(SIGINT, SIG_DFL);
        std::cerr << "\nbatbox: interrupted\n";
        return 130;
    }

    // ------------------------------------------------------------------
    // 9. Run one inference turn.
    //    The CancelToken is consumed by run_turn (moved in).
    // ------------------------------------------------------------------
    auto turn_result = conversation.run_turn(std::move(cancel_tok));

    // Restore default SIGINT disposition before doing anything else.
    ::signal(SIGINT, SIG_DFL);

    // Check for mid-turn SIGINT (run_turn may have returned Err("cancelled")).
    if (static_cast<int>(g_sigint_count) > sigint_before) {
        std::cerr << "\nbatbox: interrupted\n";
        return 130;
    }

    if (!turn_result) {
        std::cerr << "batbox: inference error: " << turn_result.error() << '\n';
        return 1;
    }

    // ------------------------------------------------------------------
    // 10. Extract usage delta from the last assistant message.
    // ------------------------------------------------------------------
    batbox::conversation::UsageDelta final_usage{};
    const auto& messages = conversation.messages();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == batbox::conversation::Role::Assistant &&
            it->usage.has_value()) {
            final_usage = *it->usage;
            break;
        }
    }

    // ------------------------------------------------------------------
    // 11. Emit output to stdout.
    // ------------------------------------------------------------------
    if (args.print_format == PrintFormat::Json) {
        // JSON output: {"role":"assistant","content":"...","usage":{...}}
        batbox::Json out;
        out["role"]    = "assistant";
        out["content"] = assistant_content;
        out["usage"]   = {
            {"prompt_tokens",     final_usage.prompt_tokens},
            {"completion_tokens", final_usage.completion_tokens},
            {"total_tokens",      final_usage.total_tokens},
            {"cost_usd",          final_usage.cost_usd}
        };
        std::cout << out.dump() << '\n' << std::flush;
    } else {
        // plain / markdown: tokens were already streamed via on_delta_cb.
        // Append a trailing newline if the content did not end with one,
        // so the shell prompt appears on its own line.
        if (!assistant_content.empty() && assistant_content.back() != '\n') {
            std::cout << '\n' << std::flush;
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// App::run
// ---------------------------------------------------------------------------
int App::run(const AppArgs& args) {
    // ------------------------------------------------------------------
    // Step 1 — Initialise logging.
    // ------------------------------------------------------------------
    batbox::log::LogConfig log_cfg;
    if (args.verbose) {
        log_cfg.level = "debug";
    }
    batbox::log::init_logging(log_cfg);

    // ------------------------------------------------------------------
    // Step 2a — Load .env config.
    //    A missing .env is not fatal — many env vars have sensible defaults.
    // ------------------------------------------------------------------
    auto env_path = resolve_env_file(args);
    auto env_result = batbox::config::load_env_file(env_path);
    if (!env_result) {
        BATBOX_LOG_DEBUG("no .env loaded ({}): {}", env_path.string(),
                         env_result.error());
    } else {
        BATBOX_LOG_DEBUG(".env loaded from {}: {} keys",
                         env_path.string(), env_result.value().size());
    }

    // ------------------------------------------------------------------
    // Step 2b — Build typed Config from the env map.
    // ------------------------------------------------------------------
    auto env_map = env_result.value_or(batbox::config::EnvMap{});
    batbox::config::merge_with_process_env(env_map, /*process_env_wins=*/true);
    auto config_result = batbox::config::Config::load_from_env(env_map);
    if (!config_result) {
        // Config parse failure is fatal — we cannot determine any safe defaults.
        std::cerr << "batbox: fatal — config load failed: "
                  << config_result.error() << '\n';
        return 1;
    }
    auto config = std::move(*config_result);
    BATBOX_LOG_INFO("config loaded: model={}", config.api.default_model);

    // ------------------------------------------------------------------
    // Step 3 — Splash banner (TUI + interactive only).
    // ------------------------------------------------------------------
    print_splash(args);

    // ------------------------------------------------------------------
    // Step 4 — PermissionMode from --nuclear / config.security.
    //    Both headless (--print) and interactive paths honour --nuclear
    //    per Decision of Record #6.
    // ------------------------------------------------------------------
    if (args.nuclear) {
        print_nuclear_banner();
        BATBOX_LOG_INFO("permission mode: nuclear — all permissions bypassed");
    } else {
        BATBOX_LOG_INFO("permission mode: {}",
                        batbox::config::Config::to_string(config.security.permission_mode));
    }

    // ------------------------------------------------------------------
    // Startup log.
    // ------------------------------------------------------------------
    BATBOX_LOG_INFO("batbox 0.1.0 starting up");
    if (args.subagent) {
        BATBOX_LOG_DEBUG("subagent mode active");
    }
    if (!args.model.empty()) {
        BATBOX_LOG_DEBUG("model override: {}", args.model);
    }

    // ------------------------------------------------------------------
    // Steps 5–7: Plugin discovery, skill loading, and ToolRegistry wiring.
    //
    // These steps run for BOTH headless and interactive modes so that
    // headless --print has the same tool capabilities as interactive mode.
    // The sidecar prewarm (Step 10) is skipped in headless mode — it is an
    // HTTP cost; BATBOX_SIDECAR_PREWARM controls it separately.
    // ------------------------------------------------------------------

    // Step 5 — Plugin discovery (same as interactive, graceful degradation).
    batbox::plugins::PluginRegistry plugin_registry_hl;
    batbox::plugins::PluginLoader   plugin_loader_hl;
    {
        auto plugins_result = plugin_loader_hl.load_all();
        if (!plugins_result) {
            BATBOX_LOG_WARN("plugin discovery (headless): graceful failure: {}",
                            plugins_result.error());
        } else {
            auto reload_result = plugin_loader_hl.reload(plugin_registry_hl);
            if (!reload_result) {
                BATBOX_LOG_WARN("plugin registry reload (headless): graceful: {}",
                                reload_result.error());
            }
        }
    }

    // Step 6 — Skill loading.
    batbox::plugins::SkillLoader skill_loader_hl;
    {
        auto bundled = batbox::skills::BundledSkillsRegistry::all();
        skill_loader_hl.set_bundled_skills(std::move(bundled));
        skill_loader_hl.load_user_dirs();
    }

    // Subsystems required by wire_tools() — lightweight; no sidecar or MCP.
    std::mutex               cfg_mutex_hl;
    batbox::mcp::McpServerRegistry   mcp_registry_hl;
    batbox::agents::AgentSupervisor  supervisor_hl;
    batbox::agents::TeamRegistry&    team_registry_hl =
        batbox::agents::TeamRegistry::instance();
    batbox::sidecar::SidecarManager  sidecar_mgr_hl(config.sidecar);
    batbox::conversation::PlanMode   plan_mode_hl;

    // Step 7 — Wire all tools into ToolRegistry.
    batbox::tools::ToolRegistry tool_registry_hl;
    batbox::app::wire_tools(
        tool_registry_hl,
        config,
        cfg_mutex_hl,
        sidecar_mgr_hl,
        mcp_registry_hl,
        supervisor_hl,
        team_registry_hl,
        skill_loader_hl,
        plan_mode_hl);
    BATBOX_LOG_INFO("tool registry ready (headless): {} tools", tool_registry_hl.size());

    // PermissionGate for headless mode: non-interactive (no TUI callback).
    // Nuclear mode if --nuclear, otherwise AcceptEdits (approve read + write
    // tools automatically, suitable for scripted/piped use).
    const batbox::permissions::PermissionMode headless_perm_mode =
        args.nuclear
            ? batbox::permissions::PermissionMode::Nuclear
            : batbox::permissions::PermissionMode::AcceptEdits;

    auto perm_store_hl = std::make_shared<batbox::permissions::PermissionStore>(
        batbox::permissions::PermissionStore::default_path());

    // Headless prompt_fn: auto-allow everything (no interactive terminal).
    auto headless_prompt_fn =
        [](std::string_view /*tool_name*/, const batbox::Json& /*args*/)
        -> batbox::permissions::Decision {
            return batbox::permissions::Decision::allow();
        };

    batbox::permissions::PermissionGate gate_hl(
        perm_store_hl, headless_perm_mode, headless_prompt_fn);

    // ------------------------------------------------------------------
    // CPP A.1 — Headless --print mode (and --subagent mode).
    //
    //    When --print or --subagent is active:
    //      - Skip TUI bootstrap (no FTXUI dependency in this path).
    //      - Skip sidecar prewarm (controlled by BATBOX_SIDECAR_PREWARM).
    //      - Skip SIGINT double-tap logic (headless owns its own handler).
    //      - Delegate directly to run_headless() which drives Conversation
    //        with the fully-wired ToolRegistry and PermissionGate.
    // ------------------------------------------------------------------
    if (args.print_mode || args.subagent) {
        BATBOX_LOG_INFO("entering headless --print mode");
        return run_headless(args, config, tool_registry_hl, gate_hl);
    }

    // ==================================================================
    // CPP A.3 — INTERACTIVE INIT SEQUENCE
    // ==================================================================

    // ------------------------------------------------------------------
    // Step 5 — Plugin discovery.
    //
    //    PluginLoader scans the 4 standard roots in ascending priority:
    //      1. ~/.claude/plugins/   (claude-code compat)
    //      2. ./.claude/plugins/
    //      3. ~/.batbox/plugins/   (user global)
    //      4. ./.batbox/plugins/   (project-local)
    //
    //    Load failures are graceful: malformed plugins are warned and
    //    skipped.  An empty result (no plugins installed) is not an error.
    // ------------------------------------------------------------------
    batbox::plugins::PluginRegistry plugin_registry;
    batbox::plugins::PluginLoader   plugin_loader;

    {
        auto plugins_result = plugin_loader.load_all();
        if (!plugins_result) {
            BATBOX_LOG_WARN("plugin discovery failed (graceful): {}",
                            plugins_result.error());
        } else {
            const auto& plugins = *plugins_result;
            BATBOX_LOG_INFO("plugins loaded: {} plugin(s) discovered",
                            plugins.size());
            for (const auto& p : plugins) {
                if (p.disabled) {
                    BATBOX_LOG_DEBUG("  plugin '{}' v{} — disabled",
                                     p.name, p.version);
                } else {
                    BATBOX_LOG_DEBUG("  plugin '{}' v{} — active ({} skills, {} commands)",
                                     p.name, p.version,
                                     p.skills.size(), p.commands.size());
                }
            }
            // Reload into the registry so active_plugins() is available.
            auto reload_result = plugin_loader.reload(plugin_registry);
            if (!reload_result) {
                BATBOX_LOG_WARN("plugin registry reload failed (graceful): {}",
                                reload_result.error());
            }
        }
    }

    // ------------------------------------------------------------------
    // Step 6 — Skill loading.
    //
    //    Priority (lowest → highest):
    //      a. Bundled skills (embedded at build time via embed.cmake).
    //      b. User-directory skills (~/.batbox/skills, ./.batbox/skills, etc.).
    //
    //    Skills from active plugins are injected after plugin discovery.
    //    Load failures are graceful: malformed .md files are warned + skipped.
    // ------------------------------------------------------------------
    batbox::plugins::SkillLoader skill_loader;

    {
        // 6a. Seed bundled (embedded) skills as the lowest-priority base.
        auto bundled = batbox::skills::BundledSkillsRegistry::all();
        BATBOX_LOG_INFO("skills: {} bundled skill(s) loaded", bundled.size());
        skill_loader.set_bundled_skills(std::move(bundled));

        // 6b. Overlay user-directory skills (4 standard roots).
        skill_loader.load_user_dirs();
        BATBOX_LOG_INFO("skills: {} total skill(s) after user-dir scan",
                        skill_loader.size());
    }

    // ------------------------------------------------------------------
    // Dependency objects for the component graph.
    //
    // These objects are owned here for the lifetime of App::run().
    // Declared in dependency order: later objects may hold references to
    // earlier ones.
    // ------------------------------------------------------------------

    // Config mutex — guards Config mutations from ConfigTool (multi-thread safe).
    std::mutex cfg_mutex;

    // MCP server registry — parse mcp.json; own transport lifecycle.
    batbox::mcp::McpServerRegistry mcp_registry;

    // Agent supervisor — semaphore-bounded sub-agent thread pool (max 4).
    batbox::agents::AgentSupervisor supervisor;

    // Team registry — singleton; access via TeamRegistry::instance().
    batbox::agents::TeamRegistry& team_registry = batbox::agents::TeamRegistry::instance();

    // Plan mode — tracks Planning/Approved state for Enter/Exit/VerifyPlanExecution.
    batbox::conversation::PlanMode plan_mode;

    // Sidecar manager for this interactive session (separate from prewarm).
    batbox::sidecar::SidecarManager sidecar_mgr(config.sidecar);

    // Session store — owns all session files + JSONL index.
    batbox::session::SessionStore session_store;

    // ------------------------------------------------------------------
    // Step 7 — Wire all 39 tools into ToolRegistry (CPP 5.30).
    //
    //     wire_tools() registers tools in dependency order and asserts
    //     exactly 39 are present.  skill_loader must be populated before
    //     this call so the Skill tool has a live loader reference.
    //
    //     Fail-fast: assert(tool_registry.size() == 39) in wire_tools().
    // ------------------------------------------------------------------
    batbox::tools::ToolRegistry tool_registry;

    // TUI-PLAN-T2: Wire ExitPlanMode with a deferred ConfirmFn.
    //
    // tui_theme and screen_mgr are not yet available at step 7 (they are
    // constructed at step 12 below).  We use a shared_ptr<ConfirmFn> as a
    // stable indirection: wire_tools() captures the pointer by value, and
    // the real ConfirmFn is installed into *plan_confirm_fn_storage after
    // tui_theme / screen_mgr / plan_approval_card are constructed (step 12b).
    // The storage is populated before screen_mgr.run() enters the event loop,
    // so no ExitPlanMode call can reach the lambda before it is filled.
    using PlanConfirmStorage = std::shared_ptr<batbox::tools::ExitPlanModeTool::ConfirmFn>;
    auto plan_confirm_fn_storage = std::make_shared<batbox::tools::ExitPlanModeTool::ConfirmFn>();

    batbox::tools::ExitPlanModeTool::ConfirmFn deferred_confirm_fn =
        [plan_confirm_fn_storage](const std::string& plan_text) -> bool {
            auto& fn = *plan_confirm_fn_storage;
            if (fn) {
                return fn(plan_text);
            }
            // Storage not yet populated (programming error) — reject to fail safe.
            return false;
        };

    // TUI-ASKQ-T5: Wire AskUserQuestion with a deferred PromptFn.
    //
    // Same deferred-storage pattern as ExitPlanMode above.  tui_theme and
    // screen_mgr are not yet available at step 7; question_card is constructed
    // at step 12 below.  The real PromptFn is installed into
    // *askq_prompt_fn_storage after question_card and screen_mgr are live.
    // The storage is populated before screen_mgr.run() enters the event loop,
    // so no AskUserQuestion call can reach the lambda before it is filled.
    using AskqPromptStorage = std::shared_ptr<batbox::tools::AskUserQuestionTool::PromptFn>;
    auto askq_prompt_fn_storage = std::make_shared<batbox::tools::AskUserQuestionTool::PromptFn>();

    batbox::tools::AskUserQuestionTool::PromptFn deferred_prompt_fn =
        [askq_prompt_fn_storage](const batbox::tools::QuestionSpec& spec)
            -> std::vector<std::string> {
            auto& fn = *askq_prompt_fn_storage;
            if (fn) {
                return fn(spec);
            }
            // Storage not yet populated (programming error) — return empty (no answer).
            return {};
        };

    batbox::app::wire_tools(
        tool_registry,
        config,
        cfg_mutex,
        sidecar_mgr,
        mcp_registry,
        supervisor,
        team_registry,
        skill_loader,
        plan_mode,
        std::move(deferred_confirm_fn),
        std::move(deferred_prompt_fn));

    BATBOX_LOG_INFO("tool registry ready: {} tools registered",
                    tool_registry.size());

    // ------------------------------------------------------------------
    // Step 8 — Wire all 38 slash commands into SlashCommandRegistry.
    //
    //     wire_commands() registers commands in curated-surface.md order
    //     and asserts exactly 38 are present.
    //
    //     PluginLoader and PluginRegistry are now live — pass real pointers.
    //     DemonPanel is constructed in wire_tui (CPP 1.15); nullptr is safe
    //     — DemonCmd degrades gracefully until CPP 1.14 wires it in.
    // ------------------------------------------------------------------
    batbox::commands::SlashCommandRegistry command_registry;
    batbox::app::wire_commands(
        command_registry,
        &supervisor,          // AgentSupervisor* for /agents, /demon
        &plan_mode,           // PlanMode* for /plan
        &skill_loader,        // SkillLoader* for /skills
        &plugin_loader,       // PluginLoader* for /plugin
        &plugin_registry,     // PluginRegistry* for /plugin
        nullptr);             // DemonPanel* — wired after TUI init (CPP 1.14)

    BATBOX_LOG_INFO("command registry ready: {} commands registered",
                    command_registry.size());

    // ------------------------------------------------------------------
    // Step 9 — AgentSupervisor / TeamRegistry initialization log.
    //
    //    AgentSupervisor is already constructed above.  This step records
    //    the parallelism limit so startup logs are informative.
    // ------------------------------------------------------------------
    BATBOX_LOG_INFO("agent supervisor ready (parallelism limit: {})",
                    config.tools.task_parallel_limit);
    BATBOX_LOG_DEBUG("team registry ready (singleton)");

    // ------------------------------------------------------------------
    // Step 10 — Sidecar prewarm (interactive mode only).
    //
    //    Kick off a background spawn so the first WebFetch/WebSearch call
    //    finds the sidecar already Running.  Controlled by
    //    BATBOX_SIDECAR_PREWARM=1 (config.sidecar.prewarm).
    // ------------------------------------------------------------------
    auto [prewarm_cancel, prewarm_tok_outer] = batbox::CancelToken::make_root();
    std::unique_ptr<batbox::sidecar::SidecarManager> prewarm_mgr;
    if (config.sidecar.prewarm) {
        BATBOX_LOG_INFO("sidecar prewarm enabled — launching background spawn");
        prewarm_mgr = std::make_unique<batbox::sidecar::SidecarManager>(config.sidecar);
        prewarm_mgr->prewarm_async(std::move(prewarm_tok_outer),
            [](std::string_view status) {
                BATBOX_LOG_DEBUG("sidecar prewarm status: {}", status);
            });
    } else {
        BATBOX_LOG_DEBUG("sidecar prewarm disabled");
    }

    // ------------------------------------------------------------------
    // Step 11 — Connect MCP clients (graceful degradation).
    //
    //    start_all() reads ~/.batbox/mcp.json (and ~/.claude/mcp.json)
    //    and starts each configured transport in parallel.
    //
    //    Failures are logged as warnings; a missing mcp.json is normal
    //    (user has no MCP servers configured) and is not an error.
    // ------------------------------------------------------------------
    {
        auto [mcp_cancel_src, mcp_ct] = batbox::CancelToken::make_root();
        auto mcp_errors = mcp_registry.start_all(std::move(mcp_ct));
        if (mcp_errors.empty()) {
            BATBOX_LOG_INFO("MCP registry ready: all configured servers started");
        } else {
            for (const auto& [name, err] : mcp_errors) {
                BATBOX_LOG_WARN("MCP server '{}' failed to start (graceful): {}",
                                name, err);
            }
            BATBOX_LOG_INFO("MCP registry: {} server(s) failed to start — continuing",
                            mcp_errors.size());
        }
    }

    // ------------------------------------------------------------------
    // Install SIGINT handler (CPP 7.6) and SIGTERM handler (CPP A.4).
    //
    // SIGINT double-tap logic:
    //   - First Ctrl+C  → prewarm_cancel.request_stop()
    //                     Cancels in-flight prewarm; sidecar process keeps running.
    //   - Second Ctrl+C within 2 s → prewarm_mgr->abort_startup()
    //                     SIGTERMs the sidecar process group.
    //   - Timer resets after 2 s.
    //
    // SIGTERM:
    //   - Sets g_sigterm_received flag.
    //   - Main thread calls screen_mgr.stop() then App::shutdown().
    // ------------------------------------------------------------------
    {
        struct sigaction sa_int{};
        sa_int.sa_handler = sigint_handler;
        sigemptyset(&sa_int.sa_mask);
        sa_int.sa_flags = SA_RESTART;
        ::sigaction(SIGINT, &sa_int, nullptr);

        struct sigaction sa_term{};
        sa_term.sa_handler = sigterm_handler;
        sigemptyset(&sa_term.sa_mask);
        sa_term.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &sa_term, nullptr);
    }

    int sigint_handled = 0;
    sig_atomic_t sigint_prev_time = 0;

    auto poll_sigint = [&]() {
        const int count = static_cast<int>(g_sigint_count);
        if (count <= sigint_handled) {
            return;
        }
        while (sigint_handled < count) {
            ++sigint_handled;
            const sig_atomic_t now_sec = g_sigint_time_sec;
            const bool double_tap =
                (sigint_handled > 1) &&
                (now_sec - sigint_prev_time) <= 2;

            if (!double_tap) {
                BATBOX_LOG_INFO("Ctrl+C: cancelling in-flight operation");
                prewarm_cancel.request_stop();
            } else {
                BATBOX_LOG_WARN("Ctrl+C x2: aborting sidecar startup");
                if (prewarm_mgr) {
                    prewarm_mgr->abort_startup();
                }
                sigint_handled = 0;
            }
            sigint_prev_time = now_sec;
        }
    };

    // Single poll call for the scaffold (no event loop yet).
    poll_sigint();

    // ------------------------------------------------------------------
    // Step 12 — TUI layout wiring (CPP 1.15).
    //
    // Construct ScreenManager, wire all four TUI components, then run the
    // FTXUI event loop.  wire_tui() handles the Splash → main layout
    // transition and mounts the root Component on the ScreenManager.
    //
    // History and Keybindings are constructed here with defaults; CPP 2.x
    // tasks will load persistent history and override keybindings from
    // config files.
    // ------------------------------------------------------------------
    batbox::tui::ScreenManager screen_mgr;

    batbox::repl::History      history;      // in-memory default; CPP 2.1 wires persistence
    batbox::repl::Keybindings  keybindings;  // default bindings; CPP 2.x wires overrides

    // Obtain the AgentEventQueue from the supervisor's Pimpl.
    // Until CPP 6.5 lands, we construct a local placeholder queue.
    // CPP 6.5 will expose AgentSupervisor::event_queue() and replace this.
    batbox::agents::AgentEventQueue agent_queue;

    // Resolve the active theme from config.ui.theme (enum → theme name → Theme struct).
    const batbox::theme::Theme tui_theme = batbox::theme::theme_from_name(
        batbox::config::Config::to_string(config.ui.theme));

    // ------------------------------------------------------------------
    // Step 12a — Conversation for TUI interactive mode (CPP A.3 fix #29).
    //
    // Client and Conversation are owned here on the run() stack frame,
    // which outlives screen_mgr.run() (below).  Detached worker threads
    // that call run_turn() capture shared ownership of the Conversation via
    // the shared_ptr, preventing use-after-free if multiple turns overlap
    // (not expected in normal use, but safe by design).
    //
    // on_delta_cb posts each streaming token to ScreenManager so the TUI
    // ChatView receives them via post_token → make_token_event → OnEvent.
    //
    // PermissionGate: Default mode (same rules as AcceptEdits for TUI; the
    // PermissionBanner lets the user cycle modes at runtime).
    // ------------------------------------------------------------------
    auto tui_client = std::make_shared<batbox::inference::Client>(config);

    auto perm_store_tui = std::make_shared<batbox::permissions::PermissionStore>(
        batbox::permissions::PermissionStore::default_path());
    const batbox::permissions::PermissionMode tui_perm_mode =
        args.nuclear
            ? batbox::permissions::PermissionMode::Nuclear
            : batbox::permissions::PermissionMode::Default;
    // UI-D2 fix (TUI-T4): construct PermissionCard for the prompt callback.
    // The card is passed to wire_tui() which mounts it as a modal overlay.
    // When the worker thread reaches a tool call that requires confirmation,
    // the prompt_fn below blocks on perm_card->await_user_decision() until the
    // user presses a key in the TUI.  Nuclear mode short-circuits before calling
    // the prompt_fn so the card is never shown when tui_perm_mode == Nuclear.
    auto perm_card = std::make_shared<batbox::tui::PermissionCard>(tui_theme);

    auto tui_gate = std::make_shared<batbox::permissions::PermissionGate>(
        perm_store_tui,
        tui_perm_mode,
        [perm_card, &screen_mgr](std::string_view tool_name,
                                   const batbox::Json& args)
            -> batbox::permissions::Decision {
            // Post ModalShow to wake the FTXUI render loop so the overlay
            // appears on the very next frame (before the cv_ wait blocks).
            screen_mgr.post_event(batbox::tui::Events::ModalShow);
            // Block this worker thread until the user resolves the prompt.
            auto decision = perm_card->await_user_decision(tool_name, args);
            // Post ModalHide to trigger a re-render with the overlay gone.
            screen_mgr.post_event(batbox::tui::Events::ModalHide);
            return decision;
        });

    // TUI-PLAN-T2 (step 12b): Create PlanApprovalCard and populate the deferred
    // ConfirmFn storage wired in step 7.  tui_theme and screen_mgr are now live.
    //
    // ConfirmFn contract (ExitPlanModeTool::ConfirmFn = std::function<bool(const std::string&)>):
    //   Returns true  → Approved; ExitPlanMode transitions PlanMode to Approved.
    //   Returns false → Rejected; ExitPlanMode transitions PlanMode to Inactive.
    //                   Edit outcome maps to false (follow-on: upgrade to ApprovalResult).
    auto plan_approval_card = std::make_shared<batbox::tui::PlanApprovalCard>(tui_theme);

    *plan_confirm_fn_storage =
        [plan_approval_card, &screen_mgr](const std::string& plan_text) -> bool {
            screen_mgr.post_event(batbox::tui::Events::PlanApprovalShow);
            auto result = plan_approval_card->await_user_decision(plan_text);
            screen_mgr.post_event(batbox::tui::Events::ModalHide);
            return result.kind == batbox::tui::PlanApprovalResult::Kind::Approved;
        };

    // TUI-ASKQ-T4: construct QuestionCard for the AskUserQuestion modal.
    // The card is passed to wire_tui() which mounts it as a modal overlay below
    // PlanApprovalCard.  Worker threads call question_card->set_spec() and then
    // await_user_answer() to block until the user resolves the question.
    // The card is shown when ChatView::OnEvent receives a QuestionShow event
    // (posted by the AskUserQuestion tool before blocking).
    auto question_card = std::make_shared<batbox::tui::QuestionCard>(tui_theme);

    // TUI-ASKQ-T5 (step 12c): Populate the deferred AskUserQuestion PromptFn.
    //
    // Now that question_card and screen_mgr are both live, we can build the real
    // PromptFn and install it into the storage captured by the deferred wrapper
    // registered in wire_tools() above.
    //
    // PromptFn contract (AskUserQuestionTool::PromptFn):
    //   Input:  QuestionSpec — parsed question with header, question text, labels,
    //           descriptions, multi_select flag.
    //   Output: vector<string> — chosen label(s); empty = cancelled / no answer.
    //
    // Implementation:
    //   1. Convert QuestionSpec → QuestionShowPayload (field-by-field mapping).
    //   2. Load the payload into question_card via set_spec() (any thread safe).
    //   3. Post Events::QuestionShow to wake the FTXUI render loop so ChatView
    //      sets show_question_card_ = true and the overlay appears immediately.
    //   4. Block this worker thread on question_card->await_user_answer() until
    //      the user resolves (Enter) or cancels (Esc) — the card's internal
    //      condition_variable handles the cross-thread handoff.
    //   5. Post make_question_resolved_event(resolved) so ChatView clears
    //      show_question_card_ and hides the overlay on the next render.
    //   6. Map QuestionResolvedPayload back to vector<string>:
    //        cancelled or no choices → empty vector (tool formats as "(no answer provided)")
    //        multi_select            → chosen_labels as-is
    //        single-select           → first chosen label (or empty if none)
    //
    // Lifetime: question_card (shared_ptr) and screen_mgr (stack ref) both
    // outlive screen_mgr.run(), so the lambda is safe for the event-loop duration.
    *askq_prompt_fn_storage =
        [question_card_weak = std::weak_ptr<batbox::tui::QuestionCard>(question_card),
         &screen_mgr](const batbox::tools::QuestionSpec& spec)
            -> std::vector<std::string> {
            auto qcard = question_card_weak.lock();
            if (!qcard) {
                // Question card has been destroyed — return empty (no answer).
                return {};
            }

            // Build QuestionShowPayload from QuestionSpec.
            batbox::tui::QuestionShowPayload payload;
            payload.header        = spec.header;
            payload.question      = spec.question;
            payload.multi_select  = spec.multi_select;
            payload.labels        = spec.labels;
            payload.descriptions  = spec.descriptions;
            payload.allow_freeform     = false;
            payload.allow_escape_hatch = false;
            // callback is unused in the await_user_answer() cross-thread path.
            payload.callback = nullptr;

            // Pre-load the spec into the card directly so it is ready for the
            // first render frame even before the event loop processes the event.
            qcard->set_spec(payload);

            // Post make_question_show_event (carries payload) — this event has
            // the form "batbox.question-show:TOKEN" which differs from the plain
            // Events::QuestionShow sentinel ("batbox.question-show"), so it is
            // NOT consumed by the WireTui CatchEvent's == check.  It falls
            // through to ChatView::OnEvent via effective_root->OnEvent, where
            // extract_question_show() extracts the payload, calls set_spec()
            // again (idempotent), and sets show_question_card_ = true.
            screen_mgr.post_event(
                batbox::tui::make_question_show_event(payload));

            // Block this worker thread until the user resolves or cancels.
            // QuestionCard::await_user_answer() uses an internal condition_variable
            // that OnEvent() notifies on Enter/Esc — no polling required.
            batbox::tui::QuestionResolvedPayload resolved = qcard->await_user_answer();

            // Post QuestionResolved so ChatView clears show_question_card_ and
            // the overlay disappears on the next render frame.
            screen_mgr.post_event(
                batbox::tui::make_question_resolved_event(resolved));

            // Map resolved payload → vector<string> for AskUserQuestionTool::run().
            // Cancelled or empty selection → empty vector (tool formats as
            // "(no answer provided)").
            if (resolved.cancelled) {
                return {};
            }
            return resolved.chosen_labels;
        };

    // on_delta_cb: forward each streaming token to the ScreenManager so the
    // ChatView's streaming tail is updated in real time.
    auto tui_conversation = std::make_shared<batbox::conversation::Conversation>(
        *tui_client,
        session_store,
        config,
        std::filesystem::current_path(),
        /*on_delta_cb=*/[&screen_mgr](std::string_view chunk) {
            screen_mgr.post_token(chunk);
        },
        &tool_registry,
        tui_gate.get());

    // Wire the message-appended callback so tool-call and tool-result messages
    // are forwarded to ChatView via a make_message_appended_event.
    // This fixes UI-D3: previously tool round-trips were invisible in ChatView.
    tui_conversation->set_on_message_appended_cb(
        [&screen_mgr](std::string_view role,
                      std::string_view tool_name,
                      std::string_view content,
                      bool             is_error) {
            screen_mgr.post_event(
                batbox::tui::make_message_appended_event(
                    std::string(role),
                    std::string(tool_name),
                    std::string(content),
                    is_error));
        });

    // Wire the tool-running callback so InputBar shows "running: <tool>" in the
    // status row while tool dispatch is in progress (UI-D10 / TUI-T9).
    // Also carries args_summary and tool_count for the TUI-FLOW-T2 card render.
    tui_conversation->set_on_tool_running_cb(
        [&screen_mgr](std::string_view tool_name,
                      std::string_view args_summary,
                      int              tool_count) {
            screen_mgr.post_event(
                batbox::tui::make_tool_running_event(
                    std::string(tool_name),
                    std::string(args_summary),
                    tool_count));
        });

    // Wire the tool-done callback so InputBar clears the running indicator
    // once tool dispatch completes (UI-D10 / TUI-T9).
    tui_conversation->set_on_tool_done_cb(
        [&screen_mgr]() {
            screen_mgr.post_event(batbox::tui::make_tool_done_event());
        });

    // Wire the reasoning-started callback so InputBar shows "· thinking..." in
    // the status row when Magistral (or any reasoning model) begins its
    // chain-of-thought phase (TUI-T15).
    tui_conversation->set_on_reasoning_started_cb(
        [&screen_mgr]() {
            screen_mgr.post_event(batbox::tui::make_thinking_started_event());
        });

    // Wire the reasoning-stopped callback so InputBar clears the thinking
    // indicator when the reasoning phase ends (TUI-T15).
    tui_conversation->set_on_reasoning_stopped_cb(
        [&screen_mgr]() {
            screen_mgr.post_event(batbox::tui::make_thinking_stopped_event());
        });

    BATBOX_LOG_INFO("TUI conversation ready (model={})", config.api.default_model);

    // ------------------------------------------------------------------
    // UI-D11 (TUI-T10) — Log session UUID to stderr at startup so users
    // can reference it for --resume without grepping the sessions index.
    //
    // start_session() eagerly creates the session file in the store and
    // sets the UUID now rather than waiting for the first user_message()
    // call.  This line goes to stderr via spdlog (the log sink writes to
    // stderr when no file sink is configured) and matches the existing
    // BATBOX_LOG_INFO convention used throughout App::run().
    //
    // The "session UUID: " prefix is lowercase and kept exactly as
    // specified in UI-D11 so grep -E "^session UUID:" on the log works.
    //
    // Guard: this block is unreachable in --print / --subagent mode
    // because App::run() returns via run_headless() at line 633 before
    // reaching this point — stdout is never polluted.
    // ------------------------------------------------------------------
    {
        auto sess_res = tui_conversation->start_session();
        if (sess_res) {
            BATBOX_LOG_INFO("session UUID: {}", tui_conversation->session_id());
        } else {
            BATBOX_LOG_WARN("session UUID: unavailable ({})", sess_res.error());
        }
    }

    // ------------------------------------------------------------------
    // TuiConvAdapter — TUI-T3 (UI-D6)
    //
    // ConversationHandle bridge used when dispatching slash commands from the
    // TUI submit path.  The adapter wraps the shared_ptr<Conversation> so
    // command implementations can call reset_messages() (for /clear),
    // inject_user_message(), last_assistant_message(), and the CPP S.5
    // session accessors without knowing about the TUI infrastructure.
    //
    // Lifetime: the adapter is a local struct on the run() stack frame.
    // It captures tui_conversation (shared_ptr) so the Conversation stays
    // alive even if run() somehow exits while a command is executing.
    //
    // This adapter is a simplified version of Repl::ConvAdapter from
    // src/repl/Repl.cpp, specialised for the TUI submit path.  It does NOT
    // need inject_user_message() / multi-line / feed_line semantics — the
    // TUI on_submit path handles those at the lambda level.  The most
    // important override is reset_messages() which calls the real
    // Conversation::clear_messages() so /clear actually discards history.
    // ------------------------------------------------------------------
    struct TuiConvAdapter : public batbox::commands::ConversationHandle {
        explicit TuiConvAdapter(
            std::shared_ptr<batbox::conversation::Conversation> conv,
            batbox::tui::ScreenManager& screen)
            : conv_(std::move(conv)), screen_(screen) {}

        // /clear: discard in-memory history and post a user-visible
        // "Conversation cleared." message token + stream_done so ChatView
        // clears its scrollback.
        void reset_messages() override {
            conv_->clear_messages();
            // Post a synthetic assistant token carrying the confirmation
            // message, then stream_done to commit it.  This is the minimal
            // mechanism that informs the user without introducing a new event
            // type (make_message_appended_event does not exist yet — TUI-T5).
            screen_.post_token("Conversation cleared.");
            screen_.post_event(batbox::tui::make_stream_done_event(/*had_error=*/false));
        }

        // inject_user_message: not meaningful in TUI on_submit context — the
        // submit path is not re-entrant.  No-op (commands that call this in
        // the TUI will silently discard the injected text; this is acceptable
        // at MVP; CPP 2.6 will wire this properly).
        void inject_user_message(std::string_view /*text*/) override {}

        // last_assistant_message: delegate to Conversation message history.
        std::string last_assistant_message(std::size_t n) const override {
            const auto& msgs = conv_->messages();
            std::size_t found = 0;
            for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
                if (it->role == batbox::conversation::Role::Assistant) {
                    ++found;
                    if (found == n) { return it->content; }
                }
            }
            return {};
        }

        [[nodiscard]] std::string get_session_id() const override {
            return conv_->session_id();
        }

        // get_messages_json: serialise the live message history to a JSON array.
        // Each element has "role" and "content" keys, matching the wire format
        // used by batbox::conversation::Message.  Called by /compact to obtain
        // the message list for Compactor::compact().
        [[nodiscard]] batbox::Json get_messages_json() const override {
            batbox::Json arr = batbox::Json::array();
            for (const auto& m : conv_->messages()) {
                batbox::Json obj;
                obj["role"]    = std::string(batbox::conversation::to_wire_role(m.role));
                obj["content"] = m.content;
                arr.push_back(obj);
            }
            return arr;
        }

        // set_messages_json: replace the live message list and replay all
        // restored messages into ChatView via MessageAppended events.
        //
        // Called by /resume after loading a session file.  The messages
        // argument is a JSON array of message objects (role + content).
        //
        // Implementation:
        //   1. Build a minimal SessionFile from the JSON array.  The session id
        //      is preserved from the current conv_ so subsequent user_message()
        //      calls continue appending to the same TUI session file rather than
        //      creating a new one.
        //   2. Call conv_->restore(sf) — clears messages_ and repopulates from sf.
        //   3. Post a make_stream_done_event to flush any in-progress streaming
        //      tail from the previous session.
        //   4. Post one make_message_appended_event per restored message so
        //      ChatView gains the prior session content as visible history.
        //
        // ChatView does not have a "clear history" event, so restored messages
        // appear appended after any content already displayed.  This is the
        // expected MVP behaviour: the user sees a separator-like stream_done
        // then the prior session content.
        void set_messages_json(const batbox::Json& messages) override {
            if (!messages.is_array()) return;

            // Build a minimal SessionFile preserving the current session id.
            batbox::session::SessionFile sf;
            {
                const std::string current_sid = conv_->session_id();
                if (!current_sid.empty()) {
                    auto uuid_res = batbox::Uuid::parse(current_sid);
                    if (uuid_res) {
                        sf.id = *uuid_res;
                    } else {
                        sf.id = batbox::Uuid::v4();
                    }
                } else {
                    sf.id = batbox::Uuid::v4();
                }
                sf.messages.reserve(messages.size());
                for (const auto& j : messages) {
                    sf.messages.push_back(j);
                }
            }

            // Restore conversation state (clears messages_ and repopulates).
            const auto restore_res = conv_->restore(sf);
            if (!restore_res) {
                BATBOX_LOG_WARN("TuiConvAdapter::set_messages_json: restore failed: {}",
                                restore_res.error());
                return;
            }

            // Flush any in-progress streaming tail from the previous session.
            screen_.post_event(
                batbox::tui::make_stream_done_event(/*had_error=*/false));

            // Replay all restored messages into ChatView via MessageAppended events.
            for (const auto& m : conv_->messages()) {
                const std::string role_str =
                    std::string(batbox::conversation::to_wire_role(m.role));
                const std::string tool_name =
                    m.tool_name.has_value() ? *m.tool_name : std::string{};
                const bool is_error =
                    m.is_error.has_value() && *m.is_error;

                screen_.post_event(
                    batbox::tui::make_message_appended_event(
                        role_str, tool_name, m.content, is_error));
            }

            BATBOX_LOG_DEBUG("TuiConvAdapter::set_messages_json: restored {} messages",
                             conv_->messages().size());
        }

        batbox::conversation::Conversation& conv_ref() { return *conv_; }

        std::shared_ptr<batbox::conversation::Conversation> conv_;
        batbox::tui::ScreenManager&                         screen_;
    };

    TuiConvAdapter tui_conv_adapter(tui_conversation, screen_mgr);

    // ------------------------------------------------------------------
    // on_submit lambda: invoked on the FTXUI UI thread when the user presses
    // Enter.  Dispatches user_message() + run_turn() to a detached worker
    // thread so the event loop never blocks on inference.
    //
    // UI-D6 fix (TUI-T3):
    //   If the submitted text starts with '/', look it up in command_registry.
    //   On match: split on the first space to extract name + args, build a
    //   CommandContext backed by TuiConvAdapter, and execute() synchronously
    //   on the UI thread (all registered Phase-1 commands are fast/safe on
    //   the UI thread — they manipulate in-memory state and post events).
    //   On unknown slash command: post a token event with an error message
    //   then stream_done so the ChatView shows the error inline.
    //   Either way, return early — do NOT dispatch to the LLM.
    //
    //   For non-slash text: behaviour unchanged — dispatch to Conversation.
    //
    // Argument splitting for commands with args (e.g. /resume <uuid>,
    // /model <name>): split on the first ASCII space after the command name.
    // Everything before the space (exclusive of leading '/') is the command
    // name; everything after is args.
    //
    // The lambda captures tui_conversation (shared_ptr), tui_client
    // (shared_ptr), tui_gate (shared_ptr), and raw refs to screen_mgr and
    // command_registry to keep all dependencies alive for the duration of
    // the worker thread — even if App::run() returns before the thread
    // finishes (graceful: the turn will complete naturally).
    // ------------------------------------------------------------------
    // TUI-FIX-T3: shared cancel source for the current TUI turn.
    auto tui_cancel_mtx = std::make_shared<std::mutex>();
    auto tui_cancel_src = std::make_shared<std::shared_ptr<batbox::CancelSource>>(nullptr);
    // TUI-FIX-T5: turn-in-flight guard prevents concurrent user_message() /
    // run_turn() on the non-thread-safe Conversation when the user submits
    // while a tool-call loop is already in progress.
    auto tui_turn_in_flight = std::make_shared<std::atomic<bool>>(false);

    auto tui_on_submit =
        [tui_conversation, tui_client, tui_gate,
         &screen_mgr, &command_registry, &tui_conv_adapter,
         tui_cancel_mtx, tui_cancel_src,
         tui_turn_in_flight](std::string text) mutable {
            if (text.empty()) return;

            // UI-D6: branch on leading '/' before posting user message event
            // so slash commands appear as "You: /clear" in the chat view just
            // like regular messages (post the event unconditionally, then
            // dispatch or fall through).
            screen_mgr.post_event(batbox::tui::make_user_message_event(text));

            if (!text.empty() && text.front() == '/') {
                // Strip the leading slash, then split on the first space.
                std::string_view sv(text);
                sv.remove_prefix(1); // skip '/'

                std::string_view cmd_name;
                std::string_view cmd_args;
                const auto space_pos = sv.find(' ');
                if (space_pos == std::string_view::npos) {
                    cmd_name = sv;
                    cmd_args = {};
                } else {
                    cmd_name = sv.substr(0, space_pos);
                    cmd_args = sv.substr(space_pos + 1);
                }

                batbox::commands::ISlashCommand* cmd =
                    cmd_name.empty() ? nullptr : command_registry.lookup(cmd_name);

                if (!cmd) {
                    // Unknown slash command: surface an inline error message
                    // to the user via the existing token/stream_done pipeline.
                    // No new event type required (make_message_appended_event
                    // does not exist yet — TUI-T5 will add it).
                    const std::string err_msg =
                        "Unknown command: /" + std::string(cmd_name) +
                        "  (type /help for available commands)";
                    screen_mgr.post_token(err_msg);
                    screen_mgr.post_event(
                        batbox::tui::make_stream_done_event(/*had_error=*/true));
                    return;
                }

                // Build a minimal CommandContext backed by TuiConvAdapter.
                // output / input are connected to null streams: the TUI
                // commands that write to ctx.output use the token/event
                // pipeline (via reset_messages above) or are no-ops for
                // fields not yet wired.  Using a null-sink stream here
                // avoids polluting stderr.
                std::ostringstream cmd_out;
                std::istringstream cmd_in;
                batbox::commands::CommandContext ctx{
                    .output       = cmd_out,
                    .input        = cmd_in,
                    .conversation = tui_conv_adapter,
                    .registry     = command_registry,
                    .cwd          = std::filesystem::current_path(),
                };

                const auto exec_result = cmd->execute(cmd_args, ctx);
                if (!exec_result.has_value()) {
                    const std::string err_msg = "Error: " + exec_result.error();
                    screen_mgr.post_token(err_msg);
                    screen_mgr.post_event(
                        batbox::tui::make_stream_done_event(/*had_error=*/true));
                }

                // If the command wrote output text (e.g. /help, /session) via
                // ctx.output, post it as an assistant token so it appears in
                // the ChatView.
                const std::string out_str = cmd_out.str();
                if (!out_str.empty()) {
                    screen_mgr.post_token(out_str);
                    screen_mgr.post_event(
                        batbox::tui::make_stream_done_event(/*had_error=*/false));
                }

                // /exit: request TUI shutdown (screen_mgr.stop() triggers
                // the FTXUI event-loop to return from run()).
                if (ctx.exit_requested) {
                    screen_mgr.stop();
                }
                return;
            }

            // Non-slash text: dispatch to Conversation + LLM on a worker thread.
            // TUI-FIX-T5: reject concurrent submits — Conversation is not thread-safe.
            // If a tool-call loop (or any run_turn) is still in progress, discard this
            // submit with a brief informational token so the user sees feedback.
            bool expected_idle = false;
            if (!tui_turn_in_flight->compare_exchange_strong(
                    expected_idle, true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // A turn is already in flight: show a one-line indicator.
                screen_mgr.post_token(
                    "\n*(busy — previous turn still running, message discarded)*");
                screen_mgr.post_event(
                    batbox::tui::make_stream_done_event(/*had_error=*/false));
                BATBOX_LOG_WARN("TUI on_submit: turn already in flight — discarding submit");
                return;
            }
            // TUI-FIX-T3: create the cancel source before spawning so the interrupt
            // callback can call request_stop() immediately after detach().
            auto [cs_turn, ct_turn] = batbox::CancelToken::make_root();
            {
                std::lock_guard<std::mutex> lk(*tui_cancel_mtx);
                *tui_cancel_src = std::make_shared<batbox::CancelSource>(std::move(cs_turn));
            }
            std::thread([tui_conversation, tui_client, tui_gate, &screen_mgr,
                         tui_cancel_mtx, tui_cancel_src, tui_turn_in_flight,
                         cancel_tok = std::move(ct_turn),
                         text = std::move(text)]() mutable {
                tui_conversation->user_message(text);
                bool had_error = false;
                try {
                    auto result = tui_conversation->run_turn(std::move(cancel_tok));
                    if (!result) {
                        BATBOX_LOG_WARN("TUI run_turn error: {}", result.error());
                        had_error = true;
                    }
                } catch (...) {
                    had_error = true;
                }
                // TUI-FIX-T5: release the turn-in-flight guard before posting
                // stream_done so the UI can immediately accept a new submit.
                tui_turn_in_flight->store(false, std::memory_order_release);
                // Clear cancel source so a stale Esc after stream_done is a no-op.
                {
                    std::lock_guard<std::mutex> lk(*tui_cancel_mtx);
                    *tui_cancel_src = nullptr;
                }
                // Signal ChatView to commit the streamed content to history
                // and clear the streaming tail.
                screen_mgr.post_event(batbox::tui::make_stream_done_event(had_error));
            }).detach();
        };

    // TUI-FIX-T3: build interrupt callback that fires request_stop() via the
    // mutex-guarded shared cancel source.
    batbox::tui::InputBar::InterruptCallback tui_interrupt_cb =
        [tui_cancel_mtx, tui_cancel_src, &screen_mgr]() {
            std::shared_ptr<batbox::CancelSource> src;
            {
                std::lock_guard<std::mutex> lk(*tui_cancel_mtx);
                src = *tui_cancel_src;
            }
            if (src) {
                src->request_stop();
                BATBOX_LOG_INFO("TUI Esc: stream interrupted by user");
                screen_mgr.post_token("\n*Interrupted.*");
            }
        };

    batbox::app::wire_tui(
        screen_mgr,
        &supervisor,             // AgentSupervisor* â valid (constructed above at step 9)
        agent_queue,
        tui_theme,
        history,
        keybindings,
        config.api.default_model,   // model_name â InputBar status bar
        std::move(tui_on_submit),   // on_submit â Conversation dispatch / slash dispatch
        &command_registry,          // slash_registry â UI-D5 fix (TUI-T3)
        perm_card.get(),            // permission_card â UI-D2 fix (TUI-T4)
        plan_approval_card.get(),   // plan_approval_card â TUI-PLAN-T2
        question_card.get(),        // question_card â TUI-ASKQ-T4
        &mcp_registry,              // mcp_registry â TUI-FLOW-T11 McpStatusPoller
        tui_gate.get(),             // permission_gate â TUI-PERM-T1 Shift+Tab cycle
        std::move(tui_interrupt_cb));  // on_interrupt_cb â TUI-FIX-T3 Esc cancels stream

    BATBOX_LOG_INFO("TUI wired — entering event loop");

    // ------------------------------------------------------------------
    // CPP A.4 — Build ShutdownContext before entering the event loop.
    //
    // All pointers are stack-local objects that outlive the run() frame.
    // The ShutdownContext does NOT own any of these objects.
    // ------------------------------------------------------------------
    ShutdownContext shutdown_ctx;
    shutdown_ctx.screen_mgr     = &screen_mgr;
    shutdown_ctx.mcp_registry   = &mcp_registry;
    shutdown_ctx.sidecar_mgr    = &sidecar_mgr;
    shutdown_ctx.supervisor     = &supervisor;
    shutdown_ctx.session_store  = &session_store;
    shutdown_ctx.plugin_registry = &plugin_registry;
    shutdown_ctx.prewarm_cancel  = &prewarm_cancel;

    // ------------------------------------------------------------------
    // Run the FTXUI event loop.
    //
    // This blocks until:
    //   a. The user types /exit or presses Ctrl+D → screen_mgr.stop()
    //   b. SIGTERM is received → g_sigterm_received=1; we call stop() below
    //   c. An internal event posts a Quit event
    // ------------------------------------------------------------------
    screen_mgr.run();

    BATBOX_LOG_INFO("TUI event loop exited");

    // ------------------------------------------------------------------
    // Post-run: check for pending SIGTERM that arrived during the event loop.
    // If the flag is set, it means the OS asked us to terminate — log it.
    // (The loop has already exited at this point regardless of signal source.)
    // ------------------------------------------------------------------
    if (g_sigterm_received) {
        BATBOX_LOG_INFO("SIGTERM received — running clean shutdown");
    }

    // Restore default signal dispositions before teardown so a second
    // SIGTERM does not re-enter the handler during cleanup.
    ::signal(SIGTERM, SIG_DFL);
    ::signal(SIGINT,  SIG_DFL);

    // ------------------------------------------------------------------
    // CPP A.4 — Execute clean teardown sequence.
    // ------------------------------------------------------------------
    App::shutdown(shutdown_ctx);

    return 0;
}

} // namespace batbox
