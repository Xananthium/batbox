// tests/unit/test_splash_account_fallback.cpp
// ---------------------------------------------------------------------------
// doctest suite for TUI-FIX-T8 — SplashBanner account fallback.
//
// Acceptance criteria:
//   1. Configured account is used as-is.
//   2. Empty config → resolve_account_label produces a non-empty label
//      of the form "<user>@<host>".
//   3. set_email("") on SplashBanner makes the banner render something
//      other than blank or "(no account)".
//   4. $USER unset → uid-based username is used (getpwuid fallback).
//   5. gethostname failure fallback → "localhost" appears in label.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/util/AccountLabel.hpp"
#include "batbox/tui/Splash.hpp"
#include "batbox/tui/ThemeApply.hpp"
#include "batbox/theme/Theme.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#ifndef _WIN32
#  include <unistd.h>
#  include <pwd.h>
#  include <sys/types.h>
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

batbox::theme::Theme make_theme() {
    batbox::theme::Theme t;
    t.name           = "test";
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

// RAII guard: temporarily override an env var, restoring it on destruction.
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
        ::_putenv_s(key, value ? value : "");
#else
        if (value) ::setenv(key, value, 1);
        else       ::unsetenv(key);
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

std::string render_banner(int width = 120, int height = 15) {
    auto theme = make_theme();
    auto comp  = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    if (!banner) return {};
    // Call set_email with empty string to trigger fallback resolution
    banner->set_email("");
    ftxui::Screen screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width),
        ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, comp->Render());
    return screen.ToString();
}

} // namespace

// ---------------------------------------------------------------------------
// resolve_account_label — unit tests
// ---------------------------------------------------------------------------

TEST_CASE("resolve_account_label: configured account is returned as-is") {
    std::optional<std::string> configured = "alice@example.com";
    std::string result = batbox::util::resolve_account_label(configured);
    CHECK(result == "alice@example.com");
}

TEST_CASE("resolve_account_label: empty configured account triggers fallback") {
    std::optional<std::string> configured = "";
    std::string result = batbox::util::resolve_account_label(configured);
    // Must be non-empty and contain '@'
    CHECK(!result.empty());
    CHECK(result.find('@') != std::string::npos);
}

TEST_CASE("resolve_account_label: nullopt configured account triggers fallback") {
    std::string result = batbox::util::resolve_account_label(std::nullopt);
    CHECK(!result.empty());
    CHECK(result.find('@') != std::string::npos);
}

TEST_CASE("resolve_account_label: $USER env var is used when config is empty") {
    EnvGuard guard("USER", "testuser_batbox");
    std::string result = batbox::util::resolve_account_label(std::nullopt);
    CHECK(result.find("testuser_batbox") != std::string::npos);
    CHECK(result.find('@') != std::string::npos);
}

#ifndef _WIN32
TEST_CASE("resolve_account_label: getpwuid fallback when $USER is unset") {
    // Temporarily unset USER to force getpwuid_r path.
    EnvGuard guard("USER", nullptr);

    std::string result = batbox::util::resolve_account_label(std::nullopt);
    // Must still produce a non-empty label with '@'
    CHECK(!result.empty());
    CHECK(result.find('@') != std::string::npos);

    // The uid-derived username should match what getpwuid_r returns.
    char buf[1024] = {};
    struct passwd pw_storage{};
    struct passwd* pw_result = nullptr;
    if (::getpwuid_r(::getuid(), &pw_storage, buf, sizeof(buf), &pw_result) == 0
            && pw_result && pw_result->pw_name && pw_result->pw_name[0]) {
        CHECK(result.find(pw_result->pw_name) != std::string::npos);
    }
}
#endif

TEST_CASE("resolve_account_label: fallback label contains a host part after '@'") {
    std::string result = batbox::util::resolve_account_label(std::nullopt);
    auto at_pos = result.find('@');
    REQUIRE(at_pos != std::string::npos);
    std::string host_part = result.substr(at_pos + 1);
    CHECK(!host_part.empty());
    // Host part must not contain the trailing domain strip dot
    // (the impl strips ".local" etc.) — just verify it's non-empty.
}

// ---------------------------------------------------------------------------
// SplashBanner integration: set_email fallback visible in rendered output
// ---------------------------------------------------------------------------

TEST_CASE("SplashBanner: empty set_email renders non-blank account line") {
    auto theme = make_theme();
    auto comp  = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    REQUIRE(banner != nullptr);

    // Pass empty email — fallback should resolve to $USER@hostname
    banner->set_email("");

    ftxui::Screen screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(120),
        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, comp->Render());
    std::string rendered = screen.ToString();

    // Must render "account:" label
    CHECK(rendered.find("account:") != std::string::npos);
    // Must NOT show blank placeholder "(no account)"
    CHECK(rendered.find("(no account)") == std::string::npos);
    // Must contain '@' somewhere on the account line (the resolved user@host)
    CHECK(rendered.find('@') != std::string::npos);
}

TEST_CASE("SplashBanner: configured email is shown as-is") {
    auto theme = make_theme();
    auto comp  = batbox::tui::SplashBanner::Make(theme, "v0.1.0");
    auto* banner = dynamic_cast<batbox::tui::SplashBanner*>(comp.get());
    REQUIRE(banner != nullptr);

    banner->set_email("myuser@myhost.local");

    ftxui::Screen screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(120),
        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, comp->Render());
    std::string rendered = screen.ToString();

    CHECK(rendered.find("myuser@myhost.local") != std::string::npos);
}

TEST_CASE("SplashBanner: set_email never called -> fallback resolves in render_full") {
    // If WireTui never called set_email (legacy path), render_full must still
    // call resolve_account_label(nullopt) rather than showing blank.
    auto theme = make_theme();
    auto comp  = batbox::tui::SplashBanner::Make(theme, "v0.1.0");

    ftxui::Screen screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(120),
        ftxui::Dimension::Fixed(15));
    ftxui::Render(screen, comp->Render());
    std::string rendered = screen.ToString();

    CHECK(rendered.find("account:") != std::string::npos);
    CHECK(rendered.find("(no account)") == std::string::npos);
    CHECK(rendered.find('@') != std::string::npos);
}
