// include/batbox/tui/SubAgentPanel.hpp
// ---------------------------------------------------------------------------
// batbox::tui::SubAgentPanel — right-side panel showing live sub-agents.
//
// Design
// ------
// SubAgentPanel is an FTXUI ComponentBase that renders the live sub-agent
// status sidebar.  It reads from a forward-declared AgentSupervisor via the
// snapshot() API (returning std::vector<AgentSnapshot>).
//
// Because AgentSupervisor (CPP 6.5) is not yet implemented, this file uses
// forward declarations for all types it needs from <batbox/agents/AgentSupervisor.hpp>.
// When AgentSupervisor lands, the .cpp file simply includes the full header;
// the .hpp here remains forward-declaration-only to avoid a circular include.
//
// Rendering
// ---------
// For each AgentSnapshot the panel renders one row:
//
//   ▸ [[agent-name]]  running  ●●● 2.3s  412t
//   ▸ [[verify]]      done     ──── 0.8s  67t
//   ▸ [[debug]]       queued   ···
//
// Columns:
//   1. Spinner/status glyph (●●● running, ✓ done, ✗ error, · queued, ■ cancelled)
//   2. Name rendered as [[name]] link text visible in terminal
//   3. Status label  (queued / running / done / failed / cancelled)
//   4. Elapsed time  (seconds, 1 decimal)
//   5. Token count
//   6. Last-message preview (truncated to panel width, muted colour)
//
// When no agents are active: renders a single muted "no active agents" line.
//
// Refresh strategy — change-driven 10Hz
// --------------------------------------
// TuiAgentTickerThread is a std::jthread that sleeps 100 ms between ticks.
// On each wake it reads AgentEventQueue::dirty_seq() (an atomic uint64).
// If the seq has advanced since the last post, it calls
//   ftxui::ScreenInteractive::Active()->PostEvent(make_agents_dirty_event())
// and updates its last-seen seq.  If unchanged: no post, zero CPU.
//
// SubAgentPanel::OnEvent handles Events::AgentsDirty (prefix check, not
// equality, so payload events are caught too):
//   - extract_agents_dirty() drains the payload
//   - refresh snapshot from AgentSupervisor::snapshot()
//   - return true (event consumed, FTXUI re-renders via Render())
//
// Threading
// ---------
// TuiAgentTickerThread only touches the atomic dirty_seq_ and posts one
// FTXUI event per tick cycle — all rendering happens on the UI thread.
// snapshot() may acquire a lock inside AgentSupervisor; that lock must not
// be held longer than the snapshot copy takes.
//
// Blueprint contract (Non-technical Deb / CPP 1.13 locked names):
//   class   batbox::tui::SubAgentPanel      : public ftxui::ComponentBase
//   class   batbox::tui::TuiAgentTickerThread
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/agents/AgentEvent.hpp>
#include <batbox/theme/Theme.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// AgentSupervisor.hpp provides AgentSnapshot + AgentSupervisor (CPP 6.5 landed).
#include <batbox/agents/AgentSupervisor.hpp>

namespace batbox::tui {

// =============================================================================
// TuiAgentTickerThread
//
// A dedicated std::jthread that polls AgentEventQueue::dirty_seq() at 10Hz
// (100 ms sleep) and posts an agents-dirty FTXUI event whenever the seq has
// advanced since the last post.  Zero CPU when agents are idle.
//
// Blueprint contract (CPP 1.13):
//   class batbox::tui::TuiAgentTickerThread
// =============================================================================
class TuiAgentTickerThread {
public:
    // -------------------------------------------------------------------------
    // Construction
    //
    // @param queue   Reference to the AgentEventQueue whose dirty_seq() we poll.
    //                Must outlive this object (owned by AgentSupervisor).
    // @param post_fn Callable invoked (on the ticker thread) when dirty_seq
    //                advances.  Typically calls
    //                  ScreenInteractive::Active()->PostEvent(make_agents_dirty_event())
    //                May be called from a background thread — must be thread-safe.
    // -------------------------------------------------------------------------
    explicit TuiAgentTickerThread(
        const batbox::agents::AgentEventQueue& queue,
        std::function<void()>                  post_fn);

    // Non-copyable, non-movable (owns a jthread).
    TuiAgentTickerThread(const TuiAgentTickerThread&)            = delete;
    TuiAgentTickerThread& operator=(const TuiAgentTickerThread&) = delete;
    TuiAgentTickerThread(TuiAgentTickerThread&&)                 = delete;
    TuiAgentTickerThread& operator=(TuiAgentTickerThread&&)      = delete;

    // Destructor requests stop and joins the thread (jthread does this
    // automatically via request_stop + join in its destructor).
    ~TuiAgentTickerThread() = default;

    // -------------------------------------------------------------------------
    // stop()
    // Signal the ticker thread to exit.  Returns immediately; the thread
    // finishes within one 100 ms sleep interval.
    // -------------------------------------------------------------------------
    void stop();

private:
    void run(std::stop_token st);

    const batbox::agents::AgentEventQueue& queue_;
    std::function<void()>                  post_fn_;
    std::jthread                           thread_;
};

// =============================================================================
// SubAgentPanel
//
// FTXUI ComponentBase rendering the live sub-agent sidebar.
//
// Blueprint contract (CPP 1.13):
//   class batbox::tui::SubAgentPanel : public ftxui::ComponentBase
// =============================================================================
class SubAgentPanel : public ftxui::ComponentBase {
public:
    // -------------------------------------------------------------------------
    // Make() — factory (FTXUI convention: return shared_ptr<ComponentBase>)
    //
    // @param supervisor  Reference to AgentSupervisor.  Must outlive the panel.
    //                    Passed as a pointer so it can be nullptr before CPP 6.5
    //                    lands; panel renders "no active agents" in that case.
    // @param queue       Reference to AgentEventQueue for dirty_seq polling.
    // @param theme       Active theme (ThemeRef = const Theme&).
    // -------------------------------------------------------------------------
    [[nodiscard]]
    static ftxui::Component Make(
        batbox::agents::AgentSupervisor*        supervisor,
        const batbox::agents::AgentEventQueue&  queue,
        const batbox::theme::Theme&             theme);

    // Destructor stops the ticker thread.
    ~SubAgentPanel() override;

    // Non-copyable, non-movable (owns TuiAgentTickerThread + snapshot state).
    SubAgentPanel(const SubAgentPanel&)            = delete;
    SubAgentPanel& operator=(const SubAgentPanel&) = delete;
    SubAgentPanel(SubAgentPanel&&)                 = delete;
    SubAgentPanel& operator=(SubAgentPanel&&)      = delete;

    // -------------------------------------------------------------------------
    // FTXUI ComponentBase interface
    // -------------------------------------------------------------------------

    /// Render the agent list.  Called on the UI thread by the FTXUI loop.
    ftxui::Element OnRender() override;

    /// Handle events.  Catches Events::AgentsDirty to refresh snapshot.
    /// Returns true when the event is consumed.
    bool OnEvent(ftxui::Event event) override;

private:
    // Private constructor — use Make().
    SubAgentPanel(batbox::agents::AgentSupervisor*        supervisor,
                  const batbox::agents::AgentEventQueue&  queue,
                  const batbox::theme::Theme&             theme);

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    /// Refresh snapshot_ from AgentSupervisor::snapshot() (UI thread).
    void refresh_snapshot();

    /// Render one agent row.
    [[nodiscard]] ftxui::Element render_agent_row(
        const batbox::agents::AgentSnapshot& snap,
        int         panel_width,
        std::size_t spinner_frame) const;

    /// Status glyph for an agent status string.
    [[nodiscard]] static std::string status_glyph(std::string_view status);

    /// Elapsed seconds string ("1.2s") from started_at_ map entry or "-.--s".
    [[nodiscard]] std::string elapsed_str(const std::string& agent_id) const;

    // -------------------------------------------------------------------------
    // State (all accessed on the UI thread, except ticker which only reads
    // the AtomicEventQueue::dirty_seq_)
    // -------------------------------------------------------------------------

    batbox::agents::AgentSupervisor*        supervisor_;        ///< May be nullptr
    const batbox::agents::AgentEventQueue&  queue_;             ///< For ticker
    const batbox::theme::Theme&             theme_;

    /// Snapshot of agent state refreshed on each AgentsDirty event.
    /// Protected by snapshot_mtx_ (written by refresh_snapshot on UI thread,
    /// read by Render on UI thread — same thread, mutex is defensive).
    std::vector<batbox::agents::AgentSnapshot> snapshot_;
    std::mutex                                  snapshot_mtx_;

    /// Track when each agent was first seen (for elapsed time display).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> started_at_;

    /// The ticker thread: polls dirty_seq_, posts agents-dirty events.
    std::unique_ptr<TuiAgentTickerThread> ticker_;

    /// Panel display width in columns (set from terminal width on Render).
    int panel_width_{30};
};

} // namespace batbox::tui
