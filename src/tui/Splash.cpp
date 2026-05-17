// src/tui/Splash.cpp
// ---------------------------------------------------------------------------
// batbox::tui::Splash      — legacy full-screen boot splash (implementation).
// batbox::tui::SplashBanner — Claude-Code-style non-modal banner (TUI-FLOW-T4).
//
// TUI-FLOW-T10 additions:
//   SplashBanner::set_changelog() — accept parsed changelog from disk.
//   render_full() falls back to the hardcoded kChangelog array when no
//   dynamic entries have been loaded.
//
// See include/batbox/tui/Splash.hpp for design notes.
// ---------------------------------------------------------------------------

#include "batbox/tui/Splash.hpp"
#include "batbox/tui/Changelog.hpp"
#include "batbox/tui/ThemeApply.hpp"
#include "splash_taglines.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace batbox::tui {

// =============================================================================
// ASCII art logo (legacy Splash only)
// =============================================================================

static constexpr std::string_view kBatAscii =
    "                 .      .\n"
    "                /|      |\\\n"
    "               / |      | \\\n"
    "         ______\\  \\    /  /______\n"
    "        /  _____\\  \\  /  /_____  \\\n"
    "       / /      `\\  \\/  /`      \\ \\\n"
    "      (_/    .    `\\  /`    .    \\_)\n"
    "             |   .-`\\/`-.   |\n"
    "             |  /  (oo)  \\  |\n"
    "              \\/    `--`    \\/\n"
    "             batbox";

static constexpr int kAutoAdvanceMs = 1500;

static int day_of_year() {
    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
#ifdef _WIN32
    ::localtime_s(&local_tm, &now);
#else
    ::localtime_r(&now, &local_tm);
#endif
    return local_tm.tm_yday;
}

static std::string_view pick_tagline() {
    int day = day_of_year();
    std::size_t idx = static_cast<std::size_t>(day) % kTaglines.size();
    return kTaglines[idx];
}

// =============================================================================
// Splash::TimerImpl
// =============================================================================

struct Splash::TimerImpl {
    std::atomic<bool> cancelled{false};
    std::thread th;

    TimerImpl(std::function<void()> fire_fn) {
        th = std::thread([this, fire = std::move(fire_fn)]() {
            int remaining = kAutoAdvanceMs;
            while (remaining > 0) {
                if (cancelled.load(std::memory_order_acquire)) return;
                int slice = std::min(remaining, 10);
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                remaining -= slice;
            }
            if (!cancelled.load(std::memory_order_acquire)) fire();
        });
    }

    void cancel() {
        cancelled.store(true, std::memory_order_release);
        if (th.joinable()) th.join();
    }

    ~TimerImpl() { cancel(); }
};

// =============================================================================
// Splash::should_skip
// =============================================================================

bool Splash::should_skip() {
    const char* env = std::getenv("BATBOX_NO_SPLASH");
    return env != nullptr && std::string_view(env) == "true";
}

// =============================================================================
// Splash: constructor / destructor / Make
// =============================================================================

Splash::Splash(std::function<void()> on_done,
               const batbox::theme::Theme& theme,
               std::string version)
    : on_done_(std::move(on_done))
    , theme_(theme)
    , version_(std::move(version))
{
}

Splash::~Splash() {
    if (timer_) timer_->cancel();
}

ftxui::Component Splash::Make(
    std::function<void()> on_done,
    const batbox::theme::Theme& theme,
    std::string version)
{
    auto ptr = std::make_shared<Splash>(
        std::move(on_done), theme, std::move(version));

    Splash* raw = ptr.get();
    ptr->timer_ = std::make_unique<TimerImpl>([raw]() {
        auto* active = ftxui::ScreenInteractive::Active();
        if (active) active->PostEvent(ftxui::Event::Special(kTimerEventName));
    });

    return ptr;
}

// =============================================================================
// Splash::OnRender
// =============================================================================

ftxui::Element Splash::OnRender() {
    using namespace ftxui;

    ftxui::Color accent = color_for(theme_, ThemeRole::AccentMagenta);
    ftxui::Color muted  = color_for(theme_, ThemeRole::Muted);
    ftxui::Color cyan   = color_for(theme_, ThemeRole::AccentCyan);

    std::vector<ftxui::Element> logo_lines;
    {
        std::string_view art = kBatAscii;
        std::size_t pos = 0;
        while (pos < art.size()) {
            std::size_t nl = art.find('\n', pos);
            std::string_view line;
            if (nl == std::string_view::npos) {
                line = art.substr(pos);
                pos  = art.size();
            } else {
                line = art.substr(pos, nl - pos);
                pos  = nl + 1;
            }
            logo_lines.push_back(
                hbox({filler(), text(std::string(line)) | ftxui::color(accent), filler()})
            );
        }
    }

    ftxui::Element version_el = hbox({
        filler(),
        text("batbox " + version_) | ftxui::color(muted),
        filler(),
    });

    ftxui::Element tagline_el = hbox({
        filler(),
        text(std::string(pick_tagline())) | ftxui::color(cyan),
        filler(),
    });

    std::vector<ftxui::Element> elements;
    elements.push_back(filler());
    for (auto& line : logo_lines) elements.push_back(std::move(line));
    elements.push_back(text(""));
    elements.push_back(version_el);
    elements.push_back(tagline_el);
    elements.push_back(filler());

    return vbox(std::move(elements));
}

// =============================================================================
// Splash::OnEvent
// =============================================================================

bool Splash::OnEvent(ftxui::Event event) {
    if (event == ftxui::Event::Special(kTimerEventName)) {
        dismiss();
        return true;
    }
    if (!event.is_mouse()) {
        dismiss();
        return true;
    }
    return false;
}

// =============================================================================
// Splash::dismiss
// =============================================================================

void Splash::dismiss() {
    if (done_) return;
    done_ = true;
    if (timer_) timer_->cancel();
    if (on_done_) on_done_();
}

// =============================================================================
// SplashBanner — TUI-FLOW-T4 / TUI-FLOW-T10
//
// Renders a Claude-Code-style titled bordered box with:
//   Left  column: "BatBox v<version>" title row, welcome/model/email/cwd
//   Right column: Tips + What's new (last 3 changelog entries)
//
// Changelog entries are loaded from disk via load_changelog() (TUI-FLOW-T10).
// When changelog_entries_ is empty, falls back to the hardcoded kChangelog array.
//
// After collapse() is called (first user submit), renders a single muted line.
// =============================================================================

SplashBanner::SplashBanner(const batbox::theme::Theme& theme, std::string version)
    : theme_(theme)
    , version_(std::move(version))
{
}

/*static*/
ftxui::Component SplashBanner::Make(
    const batbox::theme::Theme& theme,
    std::string version)
{
    return std::make_shared<SplashBanner>(theme, std::move(version));
}

void SplashBanner::set_model(std::string model) {
    model_ = std::move(model);
}

void SplashBanner::set_email(std::string email) {
    email_ = std::move(email);
}

void SplashBanner::set_cwd(std::string cwd) {
    // Replace $HOME prefix with '~'
    const char* home = std::getenv("HOME");
    if (home && !cwd.empty()) {
        std::string home_str(home);
        if (!home_str.empty() &&
            cwd.size() >= home_str.size() &&
            cwd.compare(0, home_str.size(), home_str) == 0) {
            cwd = "~" + cwd.substr(home_str.size());
        }
    }
    cwd_ = std::move(cwd);
}

void SplashBanner::set_changelog(std::vector<ChangelogEntry> entries) {
    changelog_entries_ = std::move(entries);
}

void SplashBanner::collapse() {
    collapsed_ = true;
}

// ---------------------------------------------------------------------------
// SplashBanner::OnRender
// ---------------------------------------------------------------------------

ftxui::Element SplashBanner::OnRender() {
    if (collapsed_) return render_collapsed();
    return render_full();
}

ftxui::Element SplashBanner::render_collapsed() const {
    using namespace ftxui;
    auto muted = color_for(theme_, ThemeRole::Muted);
    return hbox({ text("  welcome back!") | ftxui::color(muted) });
}

ftxui::Element SplashBanner::render_full() const {
    using namespace ftxui;

    auto accent = color_for(theme_, ThemeRole::AccentMagenta);
    auto fg     = color_for(theme_, ThemeRole::Fg);
    auto muted  = color_for(theme_, ThemeRole::Muted);
    auto cyan   = color_for(theme_, ThemeRole::AccentCyan);

    // -----------------------------------------------------------------------
    // Title row (spans full width inside the border)
    // -----------------------------------------------------------------------
    std::string title_text = " BatBox " + version_ + " ";
    Element title_row = hbox({
        text(title_text) | ftxui::color(accent) | bold,
        filler(),
    });

    // -----------------------------------------------------------------------
    // Left column: welcome + model + email + cwd
    // -----------------------------------------------------------------------
    std::string model_str = model_.empty() ? "(no model)"   : model_;
    std::string email_str = email_.empty() ? "(no account)" : email_;
    std::string cwd_str   = cwd_.empty()   ? "~"            : cwd_;

    // Truncate cwd to fit the column
    constexpr std::size_t kCwdMax = 34;
    if (cwd_str.size() > kCwdMax) {
        cwd_str = "..." + cwd_str.substr(cwd_str.size() - (kCwdMax - 3));
    }

    Elements left_lines;
    left_lines.push_back(
        hbox({ text("  welcome back!") | ftxui::color(accent) | bold })
    );
    left_lines.push_back(text(""));  // spacer
    left_lines.push_back(
        hbox({ text("  model:   ") | ftxui::color(muted),
               text(model_str)    | ftxui::color(fg) })
    );
    left_lines.push_back(
        hbox({ text("  account: ") | ftxui::color(muted),
               text(email_str)    | ftxui::color(fg) })
    );
    left_lines.push_back(
        hbox({ text("  cwd:     ") | ftxui::color(muted),
               text(cwd_str)      | ftxui::color(fg) })
    );

    auto left_col = vbox(std::move(left_lines)) | flex;

    // -----------------------------------------------------------------------
    // Right column: Tips for getting started + What's new
    // -----------------------------------------------------------------------
    Elements right_lines;
    right_lines.push_back(
        text("  Tips for getting started") | ftxui::color(cyan) | bold
    );
    right_lines.push_back(
        hbox({ text("  ") | ftxui::color(muted),
               text("> ") | ftxui::color(accent),
               text("type / for slash commands") | ftxui::color(fg) })
    );
    right_lines.push_back(
        hbox({ text("  ") | ftxui::color(muted),
               text("> ") | ftxui::color(accent),
               text("type ? for shortcuts") | ftxui::color(fg) })
    );
    right_lines.push_back(
        hbox({ text("  ") | ftxui::color(muted),
               text("> ") | ftxui::color(accent),
               text("ask about the codebase") | ftxui::color(fg) })
    );
    right_lines.push_back(
        hbox({ text("  ") | ftxui::color(muted),
               text("> ") | ftxui::color(accent),
               text("press Shift+Tab to cycle permissions") | ftxui::color(fg) })
    );

    right_lines.push_back(text(""));  // spacer
    right_lines.push_back(
        text("  What's new") | ftxui::color(cyan) | bold
    );

    // -----------------------------------------------------------------------
    // Changelog entries — dynamic (from disk) or hardcoded fallback.
    //
    // changelog_entries_ is newest-first (as parsed from the file).
    // We show the first 3.  Each entry renders: version (date) + first bullet.
    //
    // If changelog_entries_ is empty, fall back to kChangelog (string_view
    // array, oldest-first — show the last 3).
    // -----------------------------------------------------------------------
    constexpr std::size_t kChangelogShow = 3;

    if (!changelog_entries_.empty()) {
        // Dynamic entries — newest-first in the vector, show first kChangelogShow.
        std::size_t show = std::min(changelog_entries_.size(), kChangelogShow);
        for (std::size_t i = 0; i < show; ++i) {
            const auto& entry = changelog_entries_[i];
            // Format: "v<version>" or "v<version> - <date>"
            std::string label = "v" + entry.version;
            if (!entry.date.empty()) {
                label += " - " + entry.date;
            }
            // Append first bullet if present
            if (!entry.bullets.empty()) {
                // Truncate bullet to avoid overflow in the right column
                std::string bullet = entry.bullets[0];
                constexpr std::size_t kBulletMax = 38;
                if (bullet.size() > kBulletMax) {
                    bullet = bullet.substr(0, kBulletMax - 1) + "…";
                }
                label += ": " + bullet;
            }
            // Truncate the whole label if needed
            constexpr std::size_t kLabelMax = 50;
            if (label.size() > kLabelMax) {
                label = label.substr(0, kLabelMax - 1) + "…";
            }
            right_lines.push_back(
                hbox({ text("  * ") | ftxui::color(accent),
                       text(label)  | ftxui::color(muted) })
            );
        }
    } else {
        // Hardcoded fallback: kChangelog is oldest-first, show last 3.
        std::size_t cl_start = kChangelog.size() > kChangelogShow
                               ? kChangelog.size() - kChangelogShow
                               : 0;
        for (std::size_t i = cl_start; i < kChangelog.size(); ++i) {
            right_lines.push_back(
                hbox({ text("  * ") | ftxui::color(accent),
                       text(std::string(kChangelog[i])) | ftxui::color(muted) })
            );
        }
    }

    auto right_col = vbox(std::move(right_lines));

    // -----------------------------------------------------------------------
    // Compose: title row + body row (left | separator | right), wrapped in border
    // -----------------------------------------------------------------------
    auto body_row = hbox({
        left_col,
        separator() | ftxui::color(muted),
        right_col,
    });

    return vbox({
        title_row,
        separator() | ftxui::color(muted),
        body_row,
    })
    | border
    | ftxui::color(muted);
}

} // namespace batbox::tui
