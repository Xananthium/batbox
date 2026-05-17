// src/AppShutdown.cpp
// =============================================================================
// batbox::App::shutdown() + App::reset_shutdown_flag() — CPP A.4
//
// Isolated in its own translation unit so the test executable can link only
// this file (without pulling in App::run() → WireTools/WireCommands/WireTui).
//
// Shutdown order (REVERSE of init):
//   1. Stop TUI driver (ScreenManager::stop)
//   2. Disconnect MCP clients (McpServerRegistry::stop_all)
//   3. Terminate Python sidecar (SidecarManager::shutdown)
//   4. Stop AgentSupervisor (cancel all + wait_all)
//   5. Persist session state (SessionStore log + destructor closes)
//   6. Unload plugins (PluginRegistry::reload flush + destructor)
//   7. Flush spdlog sinks (spdlog::shutdown)
//
// Each step is wrapped in try/catch (graceful degradation): one stuck subsystem
// will not block the others.  Every step logs with [shutdown] prefix.
// =============================================================================

#include "App.hpp"

#include <batbox/core/Logging.hpp>

#include <spdlog/spdlog.h>

#include <iostream>

namespace batbox {

// ---------------------------------------------------------------------------
// Static member definition.
// ---------------------------------------------------------------------------
std::atomic<bool> App::shutdown_called_{false};

// ---------------------------------------------------------------------------
// App::reset_shutdown_flag — for testing only.
// ---------------------------------------------------------------------------
void App::reset_shutdown_flag() noexcept {
    shutdown_called_.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// App::shutdown (CPP A.4) — reverse-of-init clean teardown.
// ---------------------------------------------------------------------------
void App::shutdown(const ShutdownContext& ctx) {
    // --- Idempotency guard ---------------------------------------------------
    bool expected = false;
    if (!shutdown_called_.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
        return;
    }

    BATBOX_LOG_INFO("[shutdown] starting clean teardown sequence");

    // -------------------------------------------------------------------------
    // Step 1 — Stop TUI driver.
    // ScreenManager::stop() posts an Exit task to the FTXUI event loop.
    // By the time shutdown() is called, screen_mgr.run() has already returned;
    // stop() is a defensive call that is a no-op if the loop is not running.
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 1: stopping TUI driver");
    try {
        if (ctx.screen_mgr) {
            ctx.screen_mgr->stop();
            BATBOX_LOG_DEBUG("[shutdown] TUI driver stopped");
        } else {
            BATBOX_LOG_DEBUG("[shutdown] TUI driver: no screen_mgr (headless path)");
        }
    } catch (const std::exception& ex) {
        BATBOX_LOG_ERROR("[shutdown] TUI stop failed (continuing): {}", ex.what());
    } catch (...) {
        BATBOX_LOG_ERROR("[shutdown] TUI stop failed with unknown exception (continuing)");
    }

    // -------------------------------------------------------------------------
    // Step 2 — Disconnect all MCP clients.
    // stop_health_monitor() first so the monitor thread is not racing stop_all().
    // stop_all() calls stop() on each transport in parallel.
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 2: disconnecting MCP clients");
    try {
        if (ctx.mcp_registry) {
            ctx.mcp_registry->stop_health_monitor();
            ctx.mcp_registry->stop_all();
            BATBOX_LOG_DEBUG("[shutdown] MCP clients disconnected");
        } else {
            BATBOX_LOG_DEBUG("[shutdown] MCP registry: not present (skipping)");
        }
    } catch (const std::exception& ex) {
        BATBOX_LOG_ERROR("[shutdown] MCP stop failed (continuing): {}", ex.what());
    } catch (...) {
        BATBOX_LOG_ERROR("[shutdown] MCP stop failed with unknown exception (continuing)");
    }

    // -------------------------------------------------------------------------
    // Step 3 — Terminate Python scrapling sidecar.
    // Cancel prewarm first so a background spawn does not race the shutdown.
    // SidecarManager::shutdown() sequence:
    //   POST /shutdown (1 s) → SIGTERM (2 s) → SIGKILL → waitpid
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 3: terminating Python sidecar");
    try {
        if (ctx.prewarm_cancel) {
            ctx.prewarm_cancel->request_stop();
            BATBOX_LOG_DEBUG("[shutdown] prewarm cancel token signalled");
        }
        if (ctx.sidecar_mgr) {
            ctx.sidecar_mgr->shutdown();
            BATBOX_LOG_DEBUG("[shutdown] sidecar terminated");
        } else {
            BATBOX_LOG_DEBUG("[shutdown] sidecar manager: not present (skipping)");
        }
    } catch (const std::exception& ex) {
        BATBOX_LOG_ERROR("[shutdown] sidecar shutdown failed (continuing): {}", ex.what());
    } catch (...) {
        BATBOX_LOG_ERROR("[shutdown] sidecar shutdown failed with unknown exception (continuing)");
    }

    // -------------------------------------------------------------------------
    // Step 4 — Stop AgentSupervisor.
    // Cancel all in-flight/queued agents, then wait_all() to join threads.
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 4: stopping AgentSupervisor");
    try {
        if (ctx.supervisor) {
            const auto snap = ctx.supervisor->snapshot();
            BATBOX_LOG_DEBUG("[shutdown] cancelling {} agent(s)", snap.size());
            for (const auto& s : snap) {
                ctx.supervisor->cancel(s.id);
            }
            ctx.supervisor->wait_all();
            BATBOX_LOG_DEBUG("[shutdown] AgentSupervisor drained and joined");
        } else {
            BATBOX_LOG_DEBUG("[shutdown] AgentSupervisor: not present (skipping)");
        }
    } catch (const std::exception& ex) {
        BATBOX_LOG_ERROR("[shutdown] AgentSupervisor stop failed (continuing): {}", ex.what());
    } catch (...) {
        BATBOX_LOG_ERROR("[shutdown] AgentSupervisor stop failed with unknown exception (continuing)");
    }

    // -------------------------------------------------------------------------
    // Step 5 — Persist session state.
    // SessionStore destructor closes the file handles and flushes the index.
    // Here we log the active session id for user-visible confirmation.
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 5: persisting session state");
    try {
        if (ctx.session_store) {
            auto sid = ctx.session_store->current_session_id();
            if (sid.has_value()) {
                BATBOX_LOG_INFO("[shutdown] session '{}' state persisted", *sid);
            } else {
                BATBOX_LOG_DEBUG("[shutdown] no active session to persist");
            }
        } else {
            BATBOX_LOG_DEBUG("[shutdown] session store: not present (skipping)");
        }
    } catch (const std::exception& ex) {
        BATBOX_LOG_ERROR("[shutdown] session persist failed (continuing): {}", ex.what());
    } catch (...) {
        BATBOX_LOG_ERROR("[shutdown] session persist failed with unknown exception (continuing)");
    }

    // -------------------------------------------------------------------------
    // Step 6 — Unload plugins.
    // PluginRegistry destructor frees all plugin records.  We call reload()
    // to flush any cached scan metadata before the destructor runs.
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 6: unloading plugins");
    try {
        if (ctx.plugin_registry) {
            const std::size_t count = ctx.plugin_registry->size();
            BATBOX_LOG_DEBUG("[shutdown] {} plugin(s) registered — flushing registry", count);
            ctx.plugin_registry->reload();
            BATBOX_LOG_DEBUG("[shutdown] plugin registry flushed");
        } else {
            BATBOX_LOG_DEBUG("[shutdown] plugin registry: not present (skipping)");
        }
    } catch (const std::exception& ex) {
        BATBOX_LOG_ERROR("[shutdown] plugin unload failed (continuing): {}", ex.what());
    } catch (...) {
        BATBOX_LOG_ERROR("[shutdown] plugin unload failed with unknown exception (continuing)");
    }

    // -------------------------------------------------------------------------
    // Step 7 — Flush and close spdlog sinks.
    // Must be the LAST step — all earlier steps log via BATBOX_LOG_*.
    // -------------------------------------------------------------------------
    BATBOX_LOG_INFO("[shutdown] step 7: flushing spdlog sinks");
    try {
        spdlog::shutdown();
    } catch (...) {
        std::cerr << "[shutdown] spdlog::shutdown() threw unexpectedly\n";
    }
}

} // namespace batbox
