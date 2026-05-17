// tests/unit/test_theme.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::theme::Theme + ThemeApply
//
// Coverage:
//   1.  theme_from_name("miss-kittin")    → all 13 roles correct
//   2.  theme_from_name("stock-exchange") → spot-check key roles
//   3.  theme_from_name("frank-sinatra")  → spot-check key roles
//   4.  theme_from_name("monochrome")     → spot-check key roles
//   5.  theme_from_name("classic")        → spot-check key roles
//   6.  theme_from_name("")              → falls back to miss-kittin
//   7.  theme_from_name("unknown-name")  → falls back to miss-kittin
//   8.  color_for() returns correct ftxui::Color for every ThemeRole on miss-kittin
//   9.  color_for() covers all 13 ThemeRole values without throwing
//   10. load_theme(): settings.theme respected
//   11. load_theme(): BATBOX_THEME env override takes precedence over settings
//   12. load_theme(): empty settings + no env → miss-kittin default
//   13. Theme::name field matches lookup string
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/config/SettingsLoader.hpp>

#include <cstdlib>

using namespace batbox::theme;
using namespace batbox::tui;

// ---------------------------------------------------------------------------
// Helper: construct an RGB Color for expected-value comparisons.
// ftxui::Color::RGB() is a static factory; operator== is defined on Color.
// ---------------------------------------------------------------------------
namespace {

ftxui::Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ftxui::Color::RGB(r, g, b);
}

// RAII guard that sets an env var for the duration of a test scope and
// restores (or unsets) it on destruction.
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
        ::setenv(key, value, /*overwrite=*/1);
#endif
    }

    ~EnvGuard() {
        if (had_prev_) {
#ifdef _WIN32
            ::_putenv_s(key_.c_str(), prev_val_.c_str());
#else
            ::setenv(key_.c_str(), prev_val_.c_str(), 1);
#endif
        } else {
#ifdef _WIN32
            ::_putenv_s(key_.c_str(), "");
#else
            ::unsetenv(key_.c_str());
#endif
        }
    }
};

} // anonymous namespace

// ============================================================================
// TEST SUITE 1 — theme_from_name()
// ============================================================================

TEST_SUITE("theme_from_name") {

// ---------------------------------------------------------------------------
// 1. miss-kittin: verify all 13 documented colour values
// ---------------------------------------------------------------------------
TEST_CASE("miss-kittin returns correct colors for all 13 roles") {
    Theme t = theme_from_name("miss-kittin");
    CHECK(t.name == "miss-kittin");
    CHECK(t.bg             == rgb(10,  10,  10 ));
    CHECK(t.fg             == rgb(232, 232, 232));
    CHECK(t.accent_magenta == rgb(255, 42,  140));
    CHECK(t.accent_cyan    == rgb(40,  221, 255));
    CHECK(t.muted          == rgb(102, 102, 102));
    CHECK(t.success        == rgb(57,  255, 112));
    CHECK(t.error          == rgb(255, 59,  59 ));
    CHECK(t.diff_add_fg    == rgb(57,  255, 112));
    CHECK(t.diff_add_bg    == rgb(14,  30,  14 ));
    CHECK(t.diff_remove_fg == rgb(255, 85,  85 ));
    CHECK(t.diff_remove_bg == rgb(30,  14,  14 ));
    CHECK(t.prompt_prefix  == rgb(255, 42,  140));
    CHECK(t.code_bg        == rgb(20,  20,  20 ));
}

// ---------------------------------------------------------------------------
// 2. stock-exchange: verify key roles
// ---------------------------------------------------------------------------
TEST_CASE("stock-exchange has correct key colors") {
    Theme t = theme_from_name("stock-exchange");
    CHECK(t.name == "stock-exchange");
    CHECK(t.bg             == rgb(10,  10,  10 ));
    CHECK(t.fg             == rgb(212, 212, 200));
    CHECK(t.accent_magenta == rgb(0,   204, 204));
    CHECK(t.accent_cyan    == rgb(204, 204, 0  ));
    CHECK(t.muted          == rgb(85,  102, 85 ));
    CHECK(t.success        == rgb(0,   204, 102));
    CHECK(t.error          == rgb(204, 51,  0  ));
    CHECK(t.diff_add_fg    == rgb(0,   204, 102));
    CHECK(t.diff_add_bg    == rgb(10,  26,  10 ));
    CHECK(t.diff_remove_fg == rgb(204, 68,  0  ));
    CHECK(t.diff_remove_bg == rgb(26,  10,  0  ));
    CHECK(t.prompt_prefix  == rgb(0,   204, 204));
    CHECK(t.code_bg        == rgb(17,  17,  17 ));
}

// ---------------------------------------------------------------------------
// 3. frank-sinatra: verify key roles
// ---------------------------------------------------------------------------
TEST_CASE("frank-sinatra has correct key colors") {
    Theme t = theme_from_name("frank-sinatra");
    CHECK(t.name == "frank-sinatra");
    CHECK(t.bg             == rgb(13,  11,  8  ));
    CHECK(t.fg             == rgb(232, 220, 200));
    CHECK(t.accent_magenta == rgb(200, 120, 80 ));
    CHECK(t.accent_cyan    == rgb(160, 144, 128));
    CHECK(t.muted          == rgb(112, 96,  80 ));
    CHECK(t.success        == rgb(136, 170, 68 ));
    CHECK(t.error          == rgb(204, 68,  34 ));
    CHECK(t.diff_add_fg    == rgb(136, 170, 68 ));
    CHECK(t.diff_add_bg    == rgb(14,  18,  8  ));
    CHECK(t.diff_remove_fg == rgb(204, 68,  34 ));
    CHECK(t.diff_remove_bg == rgb(24,  10,  6  ));
    CHECK(t.prompt_prefix  == rgb(200, 120, 80 ));
    CHECK(t.code_bg        == rgb(16,  13,  8  ));
}

// ---------------------------------------------------------------------------
// 4. monochrome: verify key roles
// ---------------------------------------------------------------------------
TEST_CASE("monochrome has correct key colors") {
    Theme t = theme_from_name("monochrome");
    CHECK(t.name == "monochrome");
    CHECK(t.bg             == rgb(0,   0,   0  ));
    CHECK(t.fg             == rgb(255, 255, 255));
    CHECK(t.accent_magenta == rgb(255, 255, 255));
    CHECK(t.accent_cyan    == rgb(204, 204, 204));
    CHECK(t.muted          == rgb(136, 136, 136));
    CHECK(t.success        == rgb(255, 255, 255));
    CHECK(t.error          == rgb(170, 170, 170));
    CHECK(t.diff_add_fg    == rgb(255, 255, 255));
    CHECK(t.diff_add_bg    == rgb(17,  17,  17 ));
    CHECK(t.diff_remove_fg == rgb(204, 204, 204));
    CHECK(t.diff_remove_bg == rgb(10,  10,  10 ));
    CHECK(t.prompt_prefix  == rgb(255, 255, 255));
    CHECK(t.code_bg        == rgb(10,  10,  10 ));
}

// ---------------------------------------------------------------------------
// 5. classic: verify key roles
// ---------------------------------------------------------------------------
TEST_CASE("classic has correct key colors") {
    Theme t = theme_from_name("classic");
    CHECK(t.name == "classic");
    CHECK(t.bg             == rgb(26,  26,  26 ));
    CHECK(t.fg             == rgb(212, 212, 212));
    CHECK(t.accent_magenta == rgb(218, 133, 72 ));
    CHECK(t.accent_cyan    == rgb(81,  175, 239));
    CHECK(t.muted          == rgb(91,  98,  104));
    CHECK(t.success        == rgb(152, 190, 101));
    CHECK(t.error          == rgb(255, 108, 107));
    CHECK(t.diff_add_fg    == rgb(152, 190, 101));
    CHECK(t.diff_add_bg    == rgb(26,  42,  26 ));
    CHECK(t.diff_remove_fg == rgb(255, 108, 107));
    CHECK(t.diff_remove_bg == rgb(42,  26,  26 ));
    CHECK(t.prompt_prefix  == rgb(218, 133, 72 ));
    CHECK(t.code_bg        == rgb(34,  34,  34 ));
}

// ---------------------------------------------------------------------------
// 6. Empty name → miss-kittin fallback
// ---------------------------------------------------------------------------
TEST_CASE("empty string falls back to miss-kittin") {
    Theme t = theme_from_name("");
    CHECK(t.name == "miss-kittin");
    CHECK(t.accent_magenta == rgb(255, 42, 140));
}

// ---------------------------------------------------------------------------
// 7. Unknown name → miss-kittin fallback
// ---------------------------------------------------------------------------
TEST_CASE("unknown name falls back to miss-kittin") {
    Theme t = theme_from_name("requiem-for-a-dead-star");
    CHECK(t.name == "miss-kittin");
    CHECK(t.bg == rgb(10, 10, 10));
}

} // TEST_SUITE("theme_from_name")

// ============================================================================
// TEST SUITE 2 — color_for()
// ============================================================================

TEST_SUITE("color_for") {

// ---------------------------------------------------------------------------
// 8. color_for() returns correct ftxui::Color for every ThemeRole on miss-kittin
// ---------------------------------------------------------------------------
TEST_CASE("color_for returns correct values for all 13 roles on miss-kittin") {
    Theme t = theme_from_name("miss-kittin");

    CHECK(color_for(t, ThemeRole::Bg)           == rgb(10,  10,  10 ));
    CHECK(color_for(t, ThemeRole::Fg)           == rgb(232, 232, 232));
    CHECK(color_for(t, ThemeRole::AccentMagenta)== rgb(255, 42,  140));
    CHECK(color_for(t, ThemeRole::AccentCyan)   == rgb(40,  221, 255));
    CHECK(color_for(t, ThemeRole::Muted)        == rgb(102, 102, 102));
    CHECK(color_for(t, ThemeRole::Success)      == rgb(57,  255, 112));
    CHECK(color_for(t, ThemeRole::Error)        == rgb(255, 59,  59 ));
    CHECK(color_for(t, ThemeRole::DiffAddFg)    == rgb(57,  255, 112));
    CHECK(color_for(t, ThemeRole::DiffAddBg)    == rgb(14,  30,  14 ));
    CHECK(color_for(t, ThemeRole::DiffRemoveFg) == rgb(255, 85,  85 ));
    CHECK(color_for(t, ThemeRole::DiffRemoveBg) == rgb(30,  14,  14 ));
    CHECK(color_for(t, ThemeRole::PromptPrefix) == rgb(255, 42,  140));
    CHECK(color_for(t, ThemeRole::CodeBg)       == rgb(20,  20,  20 ));
}

// ---------------------------------------------------------------------------
// 9. color_for() covers all 13 ThemeRole values without throwing
// ---------------------------------------------------------------------------
TEST_CASE("color_for does not throw for any ThemeRole") {
    Theme t = theme_from_name("miss-kittin");
    const ThemeRole all_roles[] = {
        ThemeRole::Bg, ThemeRole::Fg,
        ThemeRole::AccentMagenta, ThemeRole::AccentCyan,
        ThemeRole::Muted, ThemeRole::Success, ThemeRole::Error,
        ThemeRole::DiffAddFg, ThemeRole::DiffAddBg,
        ThemeRole::DiffRemoveFg, ThemeRole::DiffRemoveBg,
        ThemeRole::PromptPrefix, ThemeRole::CodeBg,
    };
    for (ThemeRole role : all_roles) {
        { [[maybe_unused]] auto c = color_for(t, role); }
    }
}

} // TEST_SUITE("color_for")

// ============================================================================
// TEST SUITE 3 — load_theme()
// ============================================================================

TEST_SUITE("load_theme") {

// ---------------------------------------------------------------------------
// 10. settings.theme field is used when BATBOX_THEME is absent
// ---------------------------------------------------------------------------
TEST_CASE("settings.theme selects the named theme") {
    EnvGuard guard("BATBOX_THEME", "");

    batbox::config::Settings settings;
    settings.theme = "classic";
    Theme t = load_theme(settings);
    CHECK(t.name == "classic");
    CHECK(t.accent_magenta == rgb(218, 133, 72));
}

// ---------------------------------------------------------------------------
// 11. BATBOX_THEME env var overrides settings.theme
// ---------------------------------------------------------------------------
TEST_CASE("BATBOX_THEME env overrides settings.theme") {
    EnvGuard guard("BATBOX_THEME", "monochrome");

    batbox::config::Settings settings;
    settings.theme = "classic";  // would give classic; env should win
    Theme t = load_theme(settings);
    CHECK(t.name == "monochrome");
    CHECK(t.bg == rgb(0, 0, 0));
}

// ---------------------------------------------------------------------------
// 12. Empty settings + no env → miss-kittin default
// ---------------------------------------------------------------------------
TEST_CASE("empty settings with no env falls back to miss-kittin") {
    EnvGuard guard("BATBOX_THEME", "");

    batbox::config::Settings settings;
    // settings.theme is empty by default
    Theme t = load_theme(settings);
    CHECK(t.name == "miss-kittin");
}

// ---------------------------------------------------------------------------
// 13. name field matches the lookup string for all five themes
// ---------------------------------------------------------------------------
TEST_CASE("Theme::name matches the lookup string for all 5 themes") {
    const char* names[] = {
        "miss-kittin", "stock-exchange", "frank-sinatra", "monochrome", "classic"
    };
    for (const char* name : names) {
        Theme t = theme_from_name(name);
        CHECK(t.name == name);
    }
}

} // TEST_SUITE("load_theme")
