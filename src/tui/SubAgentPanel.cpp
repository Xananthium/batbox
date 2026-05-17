// src/tui/SubAgentPanel.cpp
// ---------------------------------------------------------------------------
// batbox::tui::SubAgentPanel + TuiAgentTickerThread — implementation.
//
// See include/batbox/tui/SubAgentPanel.hpp for design notes.
//
// Key design decisions:
//   - TuiAgentTickerThread sleeps exactly 100 ms per cycle (10Hz cap).
//     It posts at most one FTXUI event per wake-up if dirty_seq advanced.
//   - SubAgentPanel::OnEvent refreshes snapshot_ via AgentSupervisor::snapshot()
//     (forward-decl only here; full include will be added when CPP 6.5 lands).
//   - Render() reads snapshot_ directly — same UI thread, no lock needed for
//     the read path (snapshot_mtx_ guards against future multi-thread access).
//   - The unordered_map<string, steady_clock::time_point> started_at_ tracks
//     agent start times so we can compute elapsed display strings.
// ---------------------------------------------------------------------------

#include <batbox/tui/SubAgentPanel.hpp>
#include <batbox/tui/Events.hpp>
#include <batbox/tui/ThemeApply.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// AgentSupervisor is now fully defined via SubAgentPanel.hpp → AgentSupervisor.hpp.

namespace batbox::tui {

// =============================================================================
// Internal constants
// =============================================================================

static constexpr int kTickIntervalMs  = 100; // 10Hz max
static constexpr int kTickSliceMs     = 10;  // granularity for stop_requested checks

// Braille spinner frames cycling while an agent is "running".
static constexpr std::string_view kSpinnerFrames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
static constexpr std::size_t kSpinnerCount =
    sizeof(kSpinnerFrames) / sizeof(kSpinnerFrames[0]);

// =============================================================================
// TuiAgentTickerThread
// =============================================================================

TuiAgentTickerThread::TuiAgentTickerThread(
    const batbox::agents::AgentEventQueue& queue,
    std::function<void()>                  post_fn)
    : queue_(queue)
    , post_fn_(std::move(post_fn))
    , thread_([this](std::stop_token st) { run(std::move(st)); })
{
}

void TuiAgentTickerThread::stop() {
    thread_.request_stop();
    // std::jthread destructor will join; stop() signals early for deterministic
    // ordering in callers who care about shutdown sequence.
}

void TuiAgentTickerThread::run(std::stop_token st) {
    uint64_t last_seen_seq = queue_.dirty_seq();

    while (!st.stop_requested()) {
        // Sleep kTickIntervalMs in kTickSliceMs increments so stop_requested()
        // is checked frequently and shutdown latency stays under kTickSliceMs.
        for (int i = 0; i < (kTickIntervalMs / kTickSliceMs) && !st.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickSliceMs));
        }
        if (st.stop_requested()) {
            break;
        }

        const uint64_t current_seq = queue_.dirty_seq();
        if (current_seq != last_seen_seq) {
            last_seen_seq = current_seq;
            post_fn_();
            // post_fn_ is called at most once per 100 ms window — 10Hz cap.
        }
        // If unchanged: no post, no CPU wake to the UI thread.
    }
}

// =============================================================================
// SubAgentPanel — construction / destruction
// =============================================================================

SubAgentPanel::SubAgentPanel(
    batbox::agents::AgentSupervisor*        supervisor,
    const batbox::agents::AgentEventQueue&  queue,
    const batbox::theme::Theme&             theme)
    : supervisor_(supervisor)
    , queue_(queue)
    , theme_(theme)
{
    // Start the ticker thread.
    // post_fn_ posts an agents-dirty event to the currently-active FTXUI
    // ScreenInteractive.  ScreenInteractive::Active() returns nullptr when no
    // screen loop is running; PostEvent in that case is a no-op.
    ticker_ = std::make_unique<TuiAgentTickerThread>(
        queue_,
        []() {
            auto* screen = ftxui::ScreenInteractive::Active();
            if (screen) {
                screen->PostEvent(batbox::tui::make_agents_dirty_event());
            }
        });

    // Initial snapshot pull (safe even if supervisor_ is nullptr).
    refresh_snapshot();
}

SubAgentPanel::~SubAgentPanel() {
    if (ticker_) {
        ticker_->stop();
    }
}

// static
ftxui::Component SubAgentPanel::Make(
    batbox::agents::AgentSupervisor*        supervisor,
    const batbox::agents::AgentEventQueue&  queue,
    const batbox::theme::Theme&             theme)
{
    return std::shared_ptr<SubAgentPanel>(new SubAgentPanel(supervisor, queue, theme));
}

// =============================================================================
// SubAgentPanel::refresh_snapshot()
//
// Called on the UI thread from OnEvent(AgentsDirty).
// =============================================================================

void SubAgentPanel::refresh_snapshot() {
    if (!supervisor_) {
        std::lock_guard<std::mutex> lk(snapshot_mtx_);
        snapshot_.clear();
        return;
    }

    // Call AgentSupervisor::snapshot() (blueprint CPP 6.5 signature):
    //   std::vector<AgentSnapshot> snapshot()
    auto fresh = supervisor_->snapshot();

    // Track first-seen times for newly-appeared agents (for elapsed display).
    const auto now = std::chrono::steady_clock::now();
    for (const auto& s : fresh) {
        started_at_.emplace(s.id, now);  // emplace is a no-op if key already exists
    }

    // Remove start-time entries for agents no longer in the snapshot.
    for (auto it = started_at_.begin(); it != started_at_.end(); ) {
        bool still_present = false;
        for (const auto& s : fresh) {
            if (s.id == it->first) { still_present = true; break; }
        }
        it = still_present ? std::next(it) : started_at_.erase(it);
    }

    std::lock_guard<std::mutex> lk(snapshot_mtx_);
    snapshot_ = std::move(fresh);
}

// =============================================================================
// SubAgentPanel::OnEvent()
// =============================================================================

bool SubAgentPanel::OnEvent(ftxui::Event event) {
    // Catch AgentsDirty events: both the bare sentinel constant
    // (Events::AgentsDirty — used for identity tests in OnEvent handlers)
    // and payload events whose input() starts with the "batbox.agents-dirty:"
    // prefix (produced by make_agents_dirty_event()).
    const std::string& input  = event.input();
    const std::string& prefix = Events::AgentsDirty.input();

    const bool is_agents_dirty =
        (input == prefix) ||
        (input.size() > prefix.size() &&
         input.compare(0, prefix.size(), prefix) == 0 &&
         input[prefix.size()] == ':');

    if (is_agents_dirty) {
        // Consume the payload so the event registry doesn't leak entries.
        (void)extract_agents_dirty(event);

        // Refresh our agent snapshot from AgentSupervisor on the UI thread.
        refresh_snapshot();

        return true;   // event consumed; FTXUI will call OnRender() on next loop
    }

    return false;
}

// =============================================================================
// SubAgentPanel::OnRender()
// =============================================================================

ftxui::Element SubAgentPanel::OnRender() {
    using namespace ftxui;

    // Snapshot copy under lock (UI thread in practice; mutex is defensive).
    std::vector<batbox::agents::AgentSnapshot> snap;
    {
        std::lock_guard<std::mutex> lk(snapshot_mtx_);
        snap = snapshot_;
    }

    // Spinner frame index: advances at 10Hz based on wall time.
    const auto ms_since_epoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::size_t spinner_frame =
        static_cast<std::size_t>(ms_since_epoch / kTickIntervalMs) % kSpinnerCount;

    // Panel header: "sub-agents" (accent) + right-aligned count (muted).
    Element header = hbox({
        text("sub-agents") | bold | color(color_for(theme_, ThemeRole::AccentMagenta)),
        filler(),
        text(std::to_string(snap.size())) | color(color_for(theme_, ThemeRole::Muted)),
    });

    Elements rows;
    rows.push_back(std::move(header));
    rows.push_back(separator() | color(color_for(theme_, ThemeRole::Muted)));

    if (snap.empty()) {
        rows.push_back(
            text("no active agents") |
            color(color_for(theme_, ThemeRole::Muted)) |
            italic
        );
    } else {
        for (const auto& agent : snap) {
            rows.push_back(render_agent_row(agent, panel_width_, spinner_frame));

            // Last-message preview: last non-empty line of last_5_lines (muted, truncated).
            std::string preview;
            for (auto it = agent.last_5_lines.rbegin();
                 it != agent.last_5_lines.rend(); ++it) {
                if (!it->empty()) {
                    preview = *it;
                    break;
                }
            }
            if (!preview.empty()) {
                const int max_len = std::max(4, panel_width_ - 4);
                if (static_cast<int>(preview.size()) > max_len) {
                    preview = preview.substr(0, static_cast<std::size_t>(max_len - 1)) + "\xe2\x80\xa6"; // "…"
                }
                rows.push_back(
                    hbox({
                        text("  "),
                        text(preview) | color(color_for(theme_, ThemeRole::Muted)),
                    })
                );
            }
        }
    }

    return vbox(std::move(rows)) | size(WIDTH, EQUAL, panel_width_);
}

// =============================================================================
// SubAgentPanel::render_agent_row()
// =============================================================================

ftxui::Element SubAgentPanel::render_agent_row(
    const batbox::agents::AgentSnapshot& snap,
    int   /*panel_width*/,
    std::size_t spinner_frame) const
{
    using namespace ftxui;

    // --- Status glyph ---
    std::string glyph;
    if (snap.status == "running") {
        glyph = std::string(kSpinnerFrames[spinner_frame % kSpinnerCount]);
    } else {
        glyph = status_glyph(snap.status);
    }

    const ftxui::Color glyph_color = [&]() -> ftxui::Color {
        if (snap.status == "running")              return color_for(theme_, ThemeRole::AccentCyan);
        if (snap.status == "done")                 return color_for(theme_, ThemeRole::Success);
        if (snap.status == "failed"  ||
            snap.status == "error")                return color_for(theme_, ThemeRole::Error);
        return color_for(theme_, ThemeRole::Muted); // queued, cancelled, unknown
    }();

    // --- Name as [[name]] link text ---
    const std::string link_name = "[[" + snap.name + "]]";

    // --- Status label (truncated to 9 chars) ---
    std::string status_label = snap.status;
    if (status_label.size() > 9) {
        status_label.resize(9);
    }

    // --- Elapsed time ---
    const std::string elapsed = elapsed_str(snap.id);

    // --- Token count ---
    std::string token_str;
    if (snap.token_count > 0) {
        token_str = std::to_string(snap.token_count) + "t";
    }

    // Assemble: glyph · [[name]] · status · elapsed · tokens
    Elements parts;
    parts.push_back(text(glyph + " ") | color(glyph_color));
    parts.push_back(text(link_name)   | color(color_for(theme_, ThemeRole::AccentCyan)));
    parts.push_back(text("  "));
    parts.push_back(text(status_label) | color(color_for(theme_, ThemeRole::Muted)));

    if (!elapsed.empty()) {
        parts.push_back(text("  "));
        parts.push_back(text(elapsed) | color(color_for(theme_, ThemeRole::Muted)));
    }
    if (!token_str.empty()) {
        parts.push_back(text(" "));
        parts.push_back(text(token_str) | color(color_for(theme_, ThemeRole::Muted)));
    }

    return hbox(std::move(parts));
}

// =============================================================================
// SubAgentPanel::status_glyph()  (non-running states)
// =============================================================================

// static
std::string SubAgentPanel::status_glyph(std::string_view status) {
    if (status == "done")                     return "\xe2\x9c\x93"; // ✓
    if (status == "failed" || status == "error") return "\xe2\x9c\x97"; // ✗
    if (status == "cancelled")                return "\xe2\x96\xa0"; // ■
    return "\xc2\xb7"; // · (queued / unknown)
}

// =============================================================================
// SubAgentPanel::elapsed_str()
// =============================================================================

std::string SubAgentPanel::elapsed_str(const std::string& agent_id) const {
    const auto it = started_at_.find(agent_id);
    if (it == started_at_.end()) {
        return {};
    }
    const double secs =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - it->second).count();

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << secs << "s";
    return oss.str();
}

} // namespace batbox::tui
