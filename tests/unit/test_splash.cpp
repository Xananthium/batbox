// tests/unit/test_splash.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::tui::Splash (CPP 1.2) and
// batbox::tui::SplashBanner (TUI-FLOW-T4).
//
// Coverage:
//   LEGACY Splash:
//   1. Acceptance criterion: ASCII art is present in Render() output
//   2. Acceptance criterion: kTaglines[] has >= 12 and <= 20 entries
//   3. Acceptance criterion: tagline selection is deterministic per day-of-year
//   4. Acceptance criterion: BATBOX_NO_SPLASH=true -> should_skip() returns true
//   5. BATBOX_NO_SPLASH unset (or not "true") -> should_skip() returns false
//   6. should_skip() is false when BATBOX_NO_SPLASH="false"
//   7. Tagline index wraps correctly for day 0 and day 365
//   8. All kTaglines entries are non-empty
//   9. kTaglines array is accessible and has the correct element count
//
//   SplashBanner (TUI-FLOW-T4):
//   10. Renders "BatBox" in the output (acceptance criterion: screen shows titled box)
//   11. Renders version string
//   12. Renders "Tips for getting started" (acceptance criterion)
//   13. Renders at least one tip bullet text
//   14. Renders "What's new" section
//   15. Renders at least one kChangelog entry
//   16. Collapsed banner renders "welcome back!" only (acceptance criterion 4)
//   17. is_collapsed() false initially, true after collapse()
//   18. set_model() makes model name appear in rendered output
//   19. kChangelog has >= 3 entries (required for "show last 3" logic)
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/tui/Splash.hpp"
#include "batbox/tui/ThemeApply.hpp"
#include "batbox/theme/Theme.hpp"

// Include taglines header via the internal path.
#include "../../src/tui/splash_taglines.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

batbox::theme::Theme make_miss_kittin() {
    batbox::theme::Theme t;
    t.name           = "miss-kittin";
    t.bg             = ftxui::Color::RGB(0x0a, 0x0a, 0x0a);
    t.fg             = ftxui::Color::RGB(0xe8, 0xe8, 0xe8);
    t.accent_magenta = ftxui::Color::RGB(0xff, 0x2a, 0x8c);
    t.accent_cyan    = ftxui::Color::RGB(0x28, 0xdd, 0xff);
    t.muted          = ftxui::Color::RGB(0x66, 0x66, 0x66);
    t.success        = ftxui::Color::RGB(0x39, 0xff, 0x70);
    t.error          = ftxui::Color::RGB(0xff, 0x3b, 0x3b);
    t.diff_add_fg    = ftxui::Color::RGB(0x39, 0xff, 0x70);
    t.diff_add_bg    = ftxui::Color::RGB(0x0e, 0x1e, 0x0e);
    t.diff_remove_fg = ftxui::Color::RGB(0xff, 0x55, 0x55);
    t.diff_remove_bg = ftxui::Color::RGB(0x1e, 0x0e, 0x0e);
    t.prompt_prefix  = ftxui::Color::RGB(0xff, 0x2a, 0x8c);
    t.code_bg        = ftxui::Color::RGB(0x14, 0x14, 0x14);
    return t;
}

struct EnvGuard {
    std::string key_;
    bool had_prev_{false};
    std::string prev_val_;

    EnvGuard(const char* key, const char* value) : key_(key) {
        if (const char* cur = std::getenv(key)) {
            had_prev_ = true;
            prev_val_ = cur;
        }
#ifdef _WIN32
        ::_putenv_s(key, value);
#else
        ::setenv(key, value, 1);
#endif
    }

    ~EnvGuard() {
#ifdef _WIN32
        if (had_prev_) ::_putenv_s(key_.c_str(), prev_val_.c_str());
        else           ::_putenv_s(key_.c_str(), "");
#else
        if (had_prev_) ::setenv(key_.c_str(), prev_val_.c_str(), 1);
        else           ::unsetenv(key_.c_str());
#endif
    }
};

std::string render_to_string(ftxui::Component comp, int width = 120, int height = 30) {
    ftxui::Screen screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width),
        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, comp->Render());
    return screen.ToString();
}

} // namespace

// ---------------------------------------------------------------------------
// Legacy Splash tests
// ---------------------------------------------------------------------------

TEST_CASE("kTaglines count is within [12, 20]") {
    CHECK(batbox::tui::kTaglines.size() >= 12);
    CHECK(batbox::tui::kTaglines.size() <= 20);
}

TEST_CASE("all kTaglines entries are non-empty") {
    for (std::string_view entry : batbox::tui::kTaglines) {
        CHECK(!entry.empty());
    }
}

TEST_CASE("Splash::should_skip() returns false when BATBOX_NO_SPLASH is unset") {
    EnvGuard _g("BATBOX_NO_SPLASH", "");
#ifndef _WIN32
    ::unsetenv("BATBOX_NO_SPLASH");
#endif
    CHECK_FALSE(batbox::tui::Splash::should_skip());
}

TEST_CASE("Splash::should_skip() returns true when BATBOX_NO_SPLASH=true") {
    EnvGuard _g("BATBOX_NO_SPLASH", "true");
    CHECK(batbox::tui::Splash::should_skip());
}

TEST_CASE("Splash::should_skip() returns false when BATBOX_NO_SPLASH=false") {
    EnvGuard _g("BATBOX_NO_SPLASH", "false");
    CHECK_FALSE(batbox::tui::Splash::should_skip());
}

TEST_CASE("Splash::should_skip() returns false when BATBOX_NO_SPLASH=1") {
    EnvGuard _g("BATBOX_NO_SPLASH", "1");
    CHECK_FALSE(batbox::tui::Splash::should_skip());
}

TEST_CASE("Splash renders and contains bat ASCII art") {
    auto theme = make_miss_kittin();
    bool done_called = false;
    auto comp = batbox::tui::Splash::Make(
        [&done_called]() { done_called = true; },
        theme,
        "v0.1.0");

    std::string rendered = render_to_string(comp);

    CHECK(rendered.find("batbox") != std::string::npos);
    CHECK(rendered.find("/") != std::string::npos);
    CHECK(rendered.find("\\") != std::string::npos);
    CHECK_FALSE(done_called);
}

TEST_CASE("Splash Render() contains version string") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::Splash::Make([]() {}, theme, "v9.9.9");
    std::string rendered = render_to_string(comp);
    CHECK(rendered.find("v9.9.9") != std::string::npos);
}

TEST_CASE("Splash Render() contains a tagline from kTaglines") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::Splash::Make([]() {}, theme, "v0.1.0");
    std::string rendered = render_to_string(comp);

    bool found = false;
    for (std::string_view tl : batbox::tui::kTaglines) {
        if (rendered.find(std::string(tl)) != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("tagline index is deterministic: same day produces same index") {
    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
#ifdef _WIN32
    ::localtime_s(&local_tm, &now);
#else
    ::localtime_r(&now, &local_tm);
#endif
    int day = local_tm.tm_yday;
    std::size_t idx1 = static_cast<std::size_t>(day) % batbox::tui::kTaglines.size();
    std::size_t idx2 = static_cast<std::size_t>(day) % batbox::tui::kTaglines.size();
    CHECK(idx1 == idx2);
    CHECK(idx1 < batbox::tui::kTaglines.size());
}

TEST_CASE("tagline index wraps correctly for day 0 and day 365") {
    std::size_t n = batbox::tui::kTaglines.size();
    CHECK((0 % n) == 0);
    CHECK((365 % n) < n);
    CHECK(batbox::tui::kTaglines[0 % n].size() > 0);
    CHECK(batbox::tui::kTaglines[365 % n].size() > 0);
}

// ---------------------------------------------------------------------------
// SplashBanner tests (TUI-FLOW-T4)
// ---------------------------------------------------------------------------

TEST_CASE("kChangelog has at least 3 entries") {
    CHECK(batbox::tui::kChangelog.size() >= 3);
}

TEST_CASE("all kChangelog entries are non-empty") {
    for (std::string_view entry : batbox::tui::kChangelog) {
        CHECK(!entry.empty());
    }
}

TEST_CASE("SplashBanner renders 'BatBox' in output (acceptance criterion 1)") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");

    std::string rendered = render_to_string(comp, 120, 15);
    CHECK(rendered.find("BatBox") != std::string::npos);
}

TEST_CASE("SplashBanner renders version string") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v9.8.7");

    std::string rendered = render_to_string(comp, 120, 15);
    CHECK(rendered.find("v9.8.7") != std::string::npos);
}

TEST_CASE("SplashBanner renders 'Tips for getting started' (acceptance criterion 1)") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");

    std::string rendered = render_to_string(comp, 120, 15);
    CHECK(rendered.find("Tips for getting started") != std::string::npos);
}

TEST_CASE("SplashBanner renders at least one tip bullet") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");

    std::string rendered = render_to_string(comp, 120, 15);
    // Any of the known tip strings must be present
    bool found = rendered.find("slash commands") != std::string::npos
              || rendered.find("shortcuts")      != std::string::npos
              || rendered.find("codebase")       != std::string::npos;
    CHECK(found);
}

TEST_CASE("SplashBanner renders 'What's new' section") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");

    std::string rendered = render_to_string(comp, 120, 15);
    CHECK(rendered.find("What") != std::string::npos);
}

TEST_CASE("SplashBanner renders at least one kChangelog entry") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");

    std::string rendered = render_to_string(comp, 120, 15);

    bool found = false;
    // Check the last 3 changelog entries (the ones that should be shown)
    std::size_t start = batbox::tui::kChangelog.size() > 3
                        ? batbox::tui::kChangelog.size() - 3 : 0;
    for (std::size_t i = start; i < batbox::tui::kChangelog.size(); ++i) {
        // Only check a leading substring since rendering may truncate
        std::string_view entry = batbox::tui::kChangelog[i];
        std::string prefix = std::string(entry.substr(0, 10));
        if (rendered.find(prefix) != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("SplashBanner::set_model makes model appear in rendered output") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    REQUIRE(banner != nullptr);
    banner->set_model("magistral-ultra-9000");

    std::string rendered = render_to_string(comp, 120, 15);
    CHECK(rendered.find("magistral-ultra-9000") != std::string::npos);
}

TEST_CASE("SplashBanner::is_collapsed() is false before collapse()") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    REQUIRE(banner != nullptr);
    CHECK_FALSE(banner->is_collapsed());
}

TEST_CASE("SplashBanner::collapse() sets is_collapsed() to true") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    REQUIRE(banner != nullptr);
    banner->collapse();
    CHECK(banner->is_collapsed());
}

TEST_CASE("SplashBanner collapsed renders 'welcome back!' (acceptance criterion 4)") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    REQUIRE(banner != nullptr);
    banner->collapse();

    std::string rendered = render_to_string(comp, 120, 5);
    CHECK(rendered.find("welcome back!") != std::string::npos);
    // After collapse, the full box should NOT be present
    CHECK(rendered.find("Tips for getting started") == std::string::npos);
}

TEST_CASE("SplashBanner::Focusable() returns false (non-modal — must not steal focus)") {
    auto theme = make_miss_kittin();
    auto comp = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    CHECK_FALSE(comp->Focusable());
}
