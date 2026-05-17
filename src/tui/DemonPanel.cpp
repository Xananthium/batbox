// src/tui/DemonPanel.cpp
// ---------------------------------------------------------------------------
// batbox::tui::DemonPanel — Party Monster easter-egg panel implementation.
//
// See include/batbox/tui/DemonPanel.hpp for the full design contract,
// threading model, and layout specification.
// ---------------------------------------------------------------------------

#include "batbox/tui/DemonPanel.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace batbox::tui {

// =============================================================================
// Party Monster taglines
//
// Miss Kittin / electroclash / party-monster aesthetic.
// Rotates every 5 seconds based on wall-clock time.
// =============================================================================

static constexpr std::array<std::string_view, 12> kDemonTaglines = {
    "you are invited to my rave",
    "this conversation is now a club",
    "electroclash was my idea",
    "i rate this prompt: 9.5",
    "professional party monster",
    "the demons are dancing tonight",
    "i only speak in subtext",
    "copyright me, all rights reserved",
    "put your hands up for detroit",
    "my agenda: chaos, beauty, chaos",
    "i was goth before you were born",
    "the machine is on the dancefloor",
};

// =============================================================================
// Panel geometry and colour constants
//
// Hot-pink palette hardcoded regardless of theme — that's the joke.
// =============================================================================

// Panel dimensions
static constexpr int kPanelWidth  = 24;
static constexpr int kPanelHeight = 7;

// Hot-pink border and accent (#ff007f)
static const ftxui::Color kDemonPink    = ftxui::Color::RGB(255, 0, 127);
// Near-black background with a hint of deep red (#140008)
static const ftxui::Color kDemonBg      = ftxui::Color::RGB(20, 0, 8);
// Off-white foreground
static const ftxui::Color kDemonFg      = ftxui::Color::RGB(255, 230, 240);
// Muted pink for secondary text
static const ftxui::Color kDemonMuted   = ftxui::Color::RGB(180, 80, 120);

// Tagline rotation period
static constexpr int kTaglineRotationSecs = 5;

// Ticker interval
static constexpr int kTickerIntervalMs = 200;  // 5Hz max

// =============================================================================
// compute_tagline_idx()
// =============================================================================

// static
std::size_t DemonPanel::compute_tagline_idx() {
    using namespace std::chrono;
    const auto now_sec = duration_cast<seconds>(
        steady_clock::now().time_since_epoch()).count();
    return static_cast<std::size_t>(now_sec / kTaglineRotationSecs) %
           kDemonTaglines.size();
}

// =============================================================================
// TuiDemonTickerThread
// =============================================================================

DemonPanel::TuiDemonTickerThread::TuiDemonTickerThread(
        ftxui::ScreenInteractive& screen,
        DemonPanel*               panel)
    : screen_(screen)
    , panel_(panel)
    , thread_([this](std::stop_token st) { run(st); })
{
}

DemonPanel::TuiDemonTickerThread::~TuiDemonTickerThread() {
    // jthread destructor calls request_stop() then join() automatically.
    // No explicit action required here.
}

void DemonPanel::TuiDemonTickerThread::run(std::stop_token stop_tok) {
    // Keep track of what we last posted so we only fire when something changed.
    std::size_t last_tagline_idx  = compute_tagline_idx();
    std::string last_comment_ver;

    // Drain one initial event to ensure the panel renders on first show.
    screen_.PostEvent(make_demon_dirty_event(""));

    while (!stop_tok.stop_requested()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kTickerIntervalMs));

        if (stop_tok.stop_requested()) break;

        const std::size_t new_tagline_idx = compute_tagline_idx();

        // Sample comment dirty version (version string changes when comment changes).
        std::string current_comment_ver;
        {
            std::lock_guard<std::mutex> lk(panel_->comment_mtx_);
            current_comment_ver = panel_->comment_dirty_ver_;
        }

        const bool tagline_changed = (new_tagline_idx != last_tagline_idx);
        const bool comment_changed = (current_comment_ver != last_comment_ver);

        if (tagline_changed || comment_changed) {
            last_tagline_idx  = new_tagline_idx;
            last_comment_ver  = current_comment_ver;
            // Post event only if the panel is visible — avoids waking the
            // FTXUI loop needlessly when the demon is dismissed.
            if (panel_->visible_.load(std::memory_order_relaxed)) {
                screen_.PostEvent(make_demon_dirty_event(""));
            }
        }
    }
}

// =============================================================================
// Construction / destruction
// =============================================================================

DemonPanel::DemonPanel(const batbox::theme::Theme& theme,
                       ftxui::ScreenInteractive&   screen)
    : theme_(theme)
    , screen_(screen)
    , cached_tagline_idx_(compute_tagline_idx())
{
    ticker_ = std::make_unique<TuiDemonTickerThread>(screen_, this);
}

DemonPanel::~DemonPanel() {
    // ticker_ destructor stops the jthread via stop_token.
    ticker_.reset();
}

// static
ftxui::Component DemonPanel::Make(
        const batbox::theme::Theme& theme,
        ftxui::ScreenInteractive&   screen) {
    return std::make_shared<DemonPanel>(theme, screen);
}

// =============================================================================
// Visibility controls
// =============================================================================

void DemonPanel::show() {
    visible_.store(true, std::memory_order_release);
    // Post a dirty event so the panel renders immediately on next loop tick.
    screen_.PostEvent(make_demon_dirty_event(""));
}

void DemonPanel::hide() {
    visible_.store(false, std::memory_order_release);
}

void DemonPanel::toggle() {
    if (visible_.load(std::memory_order_acquire)) {
        hide();
    } else {
        show();
    }
}

bool DemonPanel::visible() const {
    return visible_.load(std::memory_order_acquire);
}

// =============================================================================
// Demon comment (thread-safe)
// =============================================================================

void DemonPanel::set_demon_comment(std::string text) {
    std::lock_guard<std::mutex> lk(comment_mtx_);
    latest_comment_    = std::move(text);
    // Bump version string to a unique token the ticker can detect.
    comment_dirty_ver_ = latest_comment_;  // content itself as version key
}

std::string DemonPanel::get_demon_comment() const {
    std::lock_guard<std::mutex> lk(comment_mtx_);
    return latest_comment_;
}

// =============================================================================
// Tagline index (for tests)
// =============================================================================

std::size_t DemonPanel::current_tagline_idx() const {
    return cached_tagline_idx_;
}

// =============================================================================
// OnEvent (UI thread)
// =============================================================================

bool DemonPanel::OnEvent(ftxui::Event event) {
    if (event == Events::DemonDirty) {
        // Consume payload (erases registry entry).
        (void)extract_demon_dirty(event);

        // Update cached state on the UI thread so OnRender() can read without locks.
        cached_tagline_idx_ = compute_tagline_idx();
        {
            std::lock_guard<std::mutex> lk(comment_mtx_);
            cached_comment_ = latest_comment_;
        }

        // Returning true signals FTXUI to re-render.
        return true;
    }
    return false;
}

// =============================================================================
// OnRender (UI thread)
// =============================================================================

ftxui::Element DemonPanel::OnRender() {
    using namespace ftxui;

    if (!visible_.load(std::memory_order_acquire)) {
        return ftxui::emptyElement();
    }

    // --- Title bar -----------------------------------------------------------
    Element title_elem = ftxui::hbox({
        ftxui::text(" \U0001F479 PARTY MONSTER")
            | ftxui::color(kDemonPink)
            | ftxui::bold,
        ftxui::filler(),
    }) | ftxui::bgcolor(kDemonBg);

    // --- Separator -----------------------------------------------------------
    Element sep1 = ftxui::separatorLight() | ftxui::color(kDemonPink);

    // --- Comment area (2 lines, word-wrapped to panel width) ----------------
    // We clip the comment to two display lines of kPanelWidth-2 chars each.
    const std::string& comment = cached_comment_;
    const int          max_line_len = kPanelWidth - 4;  // 2 border + 2 padding

    auto clip_line = [&](const std::string& src, std::size_t offset) -> std::string {
        if (offset >= src.size()) return std::string(max_line_len, ' ');
        std::string seg = src.substr(offset, static_cast<std::size_t>(max_line_len));
        // Pad to fixed width
        if (static_cast<int>(seg.size()) < max_line_len) {
            seg.resize(static_cast<std::size_t>(max_line_len), ' ');
        }
        return seg;
    };

    Element comment_line1;
    Element comment_line2;

    if (comment.empty()) {
        // Show muted placeholder when no comment yet.
        comment_line1 = ftxui::text(" awaiting transmission  ")
            | ftxui::color(kDemonMuted)
            | ftxui::bgcolor(kDemonBg);
        comment_line2 = ftxui::text(std::string(kPanelWidth - 2, ' '))
            | ftxui::bgcolor(kDemonBg);
    } else {
        comment_line1 = ftxui::text(" " + clip_line(comment, 0))
            | ftxui::color(kDemonFg)
            | ftxui::bgcolor(kDemonBg);
        const std::size_t line2_offset = static_cast<std::size_t>(max_line_len);
        const std::string line2_text   = clip_line(comment, line2_offset);
        comment_line2 = ftxui::text(" " + line2_text)
            | ftxui::color(kDemonFg)
            | ftxui::bgcolor(kDemonBg);
    }

    // --- Separator -----------------------------------------------------------
    Element sep2 = ftxui::separatorLight() | ftxui::color(kDemonPink);

    // --- Tagline bar ---------------------------------------------------------
    const std::string_view tagline_sv =
        kDemonTaglines[cached_tagline_idx_ % kDemonTaglines.size()];

    // Clip tagline to max_line_len - 2 (leaving room for the ◆ spinner char).
    std::string tagline_str(tagline_sv);
    if (static_cast<int>(tagline_str.size()) > max_line_len - 2) {
        tagline_str.resize(static_cast<std::size_t>(max_line_len - 2));
    }
    // Pad so ◆ is always right-aligned.
    while (static_cast<int>(tagline_str.size()) < max_line_len - 2) {
        tagline_str += ' ';
    }

    Element tagline_elem = ftxui::hbox({
        ftxui::text(" " + tagline_str) | ftxui::color(kDemonMuted),
        ftxui::text(" ◆")         | ftxui::color(kDemonPink) | ftxui::bold,
        ftxui::filler(),
    }) | ftxui::bgcolor(kDemonBg);

    // --- Compose panel -------------------------------------------------------
    Element panel_inner = ftxui::vbox({
        title_elem,
        sep1,
        comment_line1,
        comment_line2,
        sep2,
        tagline_elem,
    })
        | ftxui::size(WIDTH,  EQUAL, kPanelWidth)
        | ftxui::size(HEIGHT, EQUAL, kPanelHeight)
        | ftxui::border
        | ftxui::color(kDemonPink)    // border inherits this color
        | ftxui::bgcolor(kDemonBg)
        | ftxui::clear_under;

    // --- Position: bottom-right via hbox filler ------------------------------
    // The outer vbox is composed by the caller (ScreenManager layout), so we
    // just push the panel to the right edge here; vertical positioning is
    // handled by the parent layout's filler().
    return ftxui::hbox({
        ftxui::filler(),
        panel_inner,
    });
}

} // namespace batbox::tui
