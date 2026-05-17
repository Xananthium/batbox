// src/App.hpp
// =============================================================================
// batbox::App — top-level application object.
//
// Lifetime contract:
//   1. Construct App with parsed AppArgs.
//   2. App::run(args) is the single entry point from main().
//      It initialises logging, loads config, prints the splash banner,
//      and then hands off to the TUI + REPL + ConversationEngine wiring
//      that lands in the CPP A-series tasks.
//
// Blueprint contract (blueprints table, task CPP B.11 + CPP A.1 + CPP A.4):
//   class  batbox::App          src/App.hpp
//   file   src/App.hpp
//   file   src/App.cpp
//
// CPP A.4 — Clean shutdown
// ========================
// App::shutdown() performs reverse-of-init teardown in REVERSE order of init:
//   1. Stop TUI driver (ScreenManager::stop) — drain pending render
//   2. Disconnect all MCP clients (McpServerRegistry::stop_all)
//   3. Terminate Python sidecar (SidecarManager::shutdown — /shutdown + SIGTERM
//      + SIGKILL + waitpid sequence with timeouts)
//   4. Stop AgentSupervisor — cancel all in-flight agents, join worker threads
//   5. Persist open session state via SessionStore (flush + close SQLite handle)
//   6. Unload plugins — clear PluginRegistry
//   7. Flush + close spdlog sinks (spdlog::shutdown())
//
// SIGINT / SIGTERM handling:
//   Handlers are async-signal-safe (only write volatile sig_atomic_t flags).
//   SIGTERM sets g_sigterm_received so the main thread can call screen_mgr.stop().
//   Actual teardown always runs on the main thread, NEVER in a signal handler.
//   App::shutdown() is idempotent via an atomic<bool> shutdown_called_ flag.
// =============================================================================

#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/tools/ToolRegistry.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/tui/Screen.hpp>
#include <batbox/mcp/McpServerRegistry.hpp>
#include <batbox/sidecar/SidecarManager.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/session/SessionStore.hpp>
#include <batbox/plugins/PluginRegistry.hpp>

#include <atomic>
#include <string>
#include <vector>

namespace batbox {

// ---------------------------------------------------------------------------
// PrintFormat — output format for --print headless mode (CPP A.1).
// ---------------------------------------------------------------------------
enum class PrintFormat {
    Plain,     ///< plain text (default): emit assistant message content to stdout
    Json,      ///< JSON: emit {"role":"assistant","content":"...","usage":{...}}
    Markdown,  ///< markdown: content as-is (markdown-aware renderers will honour)
};

// ---------------------------------------------------------------------------
// AppArgs — parsed, typed representation of the CLI flags.
// Populated by main() via CLI11 and passed into App::run().
// ---------------------------------------------------------------------------
struct AppArgs {
    // -- Positional ---------------------------------------------------------
    std::string  prompt;              ///< optional positional prompt text

    // -- Headless / print mode ----------------------------------------------
    bool         print_mode{false};   ///< --print : headless, no TUI
    std::string  prompt_flag;         ///< --prompt "text"  (headless input)
    PrintFormat  print_format{PrintFormat::Plain}; ///< --print-format plain|json|markdown

    // -- Session resumption -------------------------------------------------
    std::string  resume_id;           ///< --resume <session-id>
    bool         resume_latest{false};///< --resume-latest : load the most recent session

    // -- Model override -----------------------------------------------------
    std::string  model;               ///< --model <name>

    // -- Config override ----------------------------------------------------
    std::string  env_file;            ///< --env <path> : alternative .env file

    // -- Verbosity ----------------------------------------------------------
    bool         verbose{false};      ///< --verbose : enable trace/debug logging

    // -- Permission mode ----------------------------------------------------
    bool         nuclear{false};      ///< --nuclear : start in nuclear ☢️ permission mode

    // -- Subagent infrastructure --------------------------------------------
    // --subagent is an internal flag used when batbox spawns itself as a
    // sub-agent worker.  It suppresses the splash banner and forces
    // headless execution so the parent process can parse stdout cleanly.
    bool         subagent{false};     ///< --subagent : internal sub-agent launch flag

    // -- Subcommands (populated by CLI11 subcommand callbacks) --------------
    bool         run_setup_sidecar{false}; ///< setup-sidecar subcommand selected
    bool         run_migrate{false};       ///< migrate subcommand selected
};

// ---------------------------------------------------------------------------
// ShutdownContext — bundles all subsystem pointers needed by App::shutdown().
//
// All pointers are nullable: App::shutdown() skips nullptr subsystems safely.
// Constructed in App::run() after all components are initialised; passed to
// shutdown() so teardown happens on the main thread with correct lifetimes.
// ---------------------------------------------------------------------------
struct ShutdownContext {
    batbox::tui::ScreenManager*          screen_mgr{nullptr};
    batbox::mcp::McpServerRegistry*      mcp_registry{nullptr};
    batbox::sidecar::SidecarManager*     sidecar_mgr{nullptr};
    batbox::agents::AgentSupervisor*     supervisor{nullptr};
    batbox::session::SessionStore*       session_store{nullptr};
    batbox::plugins::PluginRegistry*     plugin_registry{nullptr};
    CancelSource*                        prewarm_cancel{nullptr};
};

// ---------------------------------------------------------------------------
// App — owns the full application lifetime after argument parsing.
//
// Design intent (CPP B.11 + CPP A.1 + CPP A.4):
//   App::run() initialises logging and config, emits the startup banner,
//   and then either:
//     a. Runs the headless --print mode (CPP A.1): a single-turn non-TUI
//        conversation using Conversation + Client, writes output to stdout,
//        and returns with exit code 0/1/2/130.
//     b. Falls through to the TUI + REPL wiring (CPP A.3+).
// ---------------------------------------------------------------------------
class App {
public:
    App() = delete;

    // run() — single entry point called from main().
    //
    // Returns:
    //   0   — clean exit / success
    //   1   — model / inference error (printed to stderr)
    //   2   — tool error or unhandled exception (caught by main catch-all)
    //   130 — interrupted by Ctrl+C (SIGINT)
    static int run(const AppArgs& args);

    // shutdown() — perform reverse-of-init teardown (CPP A.4).
    //
    // Idempotent: the first call executes all teardown steps; subsequent calls
    // return immediately (guarded by an atomic<bool> shutdown_called_ flag).
    //
    // Each step logs with [shutdown] prefix via spdlog.
    //
    // Graceful degradation: if one subsystem teardown throws or fails, the
    // error is logged and teardown continues with the remaining subsystems.
    // A stuck MCP server will NOT block sidecar termination.
    //
    // nullptr entries in ctx are skipped silently.
    static void shutdown(const ShutdownContext& ctx);

    // reset_shutdown_flag() — for testing only: allows calling shutdown() twice
    // in unit tests without the idempotency guard blocking the second call.
    // NOT for use in production code.
    static void reset_shutdown_flag() noexcept;

private:
    // Idempotency guard: set to true atomically on first shutdown() call.
    static std::atomic<bool> shutdown_called_;

    // -- Splash banner (Miss Kittin era electroclash aesthetic) -------------
    // Prints the lowercase bat ASCII art + version line + rotating tagline.
    // Suppressed when args.subagent == true or args.print_mode == true.
    static void print_splash(const AppArgs& args);

    // -- Nuclear mode banner ------------------------------------------------
    // Prints the ☢️ NUCLEAR MODE banner when --nuclear is active.
    static void print_nuclear_banner();

    // -- Headless print mode (CPP A.1) --------------------------------------
    // Single-turn non-TUI conversation: read prompt from args (argv or stdin),
    // run one Conversation::run_turn(), write the assistant message to stdout,
    // and exit.  No FTXUI dependency in this path.
    //
    // tool_registry — the fully-wired ToolRegistry (same as TUI mode uses).
    //                 Non-null; headless mode supports tool calls.
    // gate          — PermissionGate configured for non-interactive use
    //                 (nuclear if --nuclear, otherwise AcceptEdits for scripting).
    //
    // Returns: 0 success, 1 model/restore error, 2 session not found, 130 SIGINT.
    static int run_headless(const AppArgs& args,
                            const batbox::config::Config& config,
                            batbox::tools::ToolRegistry& tool_registry,
                            batbox::permissions::PermissionGate& gate);
};

} // namespace batbox
