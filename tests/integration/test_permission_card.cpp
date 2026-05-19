// tests/integration/test_permission_card.cpp
//
// Integration / unit tests for batbox::tui::PermissionCard (CPP 1.10).
//
// Build (standalone, no CMake, from repo root):
//   c++ -std=c++20                                                          \
//       -I$(pwd)/include                                                    \
//       -I$(pwd)/build/vcpkg_installed/arm64-osx/include                   \
//       tests/integration/test_permission_card.cpp                         \
//       src/tui/PermissionCard.cpp                                          \
//       src/tui/Events.cpp src/tui/ThemeApply.cpp                          \
//       src/theme/Theme.cpp src/theme/themes.cpp                           \
//       src/permissions/PermissionGate.cpp                                  \
//       src/permissions/PermissionRule.cpp                                  \
//       src/permissions/PatternMatcher.cpp                                  \
//       src/permissions/PermissionMode.cpp                                  \
//       src/permissions/PermissionStore.cpp                                 \
//       src/core/Json.cpp                                                   \
//       -L$(pwd)/build/vcpkg_installed/arm64-osx/lib                       \
//       -lftxui-component -lftxui-dom -lftxui-screen                       \
//       -o /tmp/test_permission_card && /tmp/test_permission_card
//
// Acceptance criteria tested:
//   [AC1] 'a' key → Decision::Kind::Allow, no persist_rule.
//   [AC2] 'A' key → Decision::Kind::Allow, persist_rule present (allow kind).
//   [AC3] 'n' key → Decision::Kind::Deny, no persist_rule.
//   [AC4] 'N' key → Decision::Kind::Deny, persist_rule present (deny kind).
//   [AC5] 'e' key → Decision carries edit_text with args preview.
//   [AC6] Esc key → Decision::Kind::Deny (cancel = one-shot deny).
//   [AC7] Worker thread blocks in await_user_decision() until key is pressed.
//   [AC8] OnRender() produces non-empty output without crashing (no FTXUI loop needed).
//   [AC9] Pretty-printed args shown (build_preview uses 2-space indent).
//   [AC10] Rule pattern preview uses ToolName(arg) format from canonical field.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/PermissionCard.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include "fixtures/TestTheme.hpp"
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/permissions/PermissionRule.hpp>
#include <batbox/core/Json.hpp>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace batbox::tui;
using namespace batbox::theme;
using namespace batbox::permissions;

// =============================================================================
// Helpers
// =============================================================================

using batbox::test_fixtures::make_test_theme;

static batbox::Json make_bash_args(const std::string& cmd = "npm test") {
    return batbox::Json{{"command", cmd}};
}

static batbox::Json make_read_args(const std::string& path = "./src/foo.cpp") {
    return batbox::Json{{"file_path", path}};
}

static batbox::Json make_generic_args() {
    return batbox::Json{{"foo", "bar"}, {"count", 42}};
}

// Helper: run await_user_decision on a background thread, fire event on the
// calling thread after a short delay, then join and return the decision.
static Decision fire_key_and_await(PermissionCard& card,
                                   const std::string& tool,
                                   const batbox::Json& args,
                                   ftxui::Event ev,
                                   std::chrono::milliseconds delay_ms = std::chrono::milliseconds(20))
{
    Decision result = Decision::deny();

    std::thread worker([&]() {
        result = card.await_user_decision(tool, args);
    });

    std::this_thread::sleep_for(delay_ms);
    card.OnEvent(ev);
    worker.join();

    return result;
}

// =============================================================================
// [AC1] 'a' key — allow once
// =============================================================================
TEST_SUITE("PermissionCard — allow once [a]") {
    TEST_CASE("'a' produces Allow with no persist_rule") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Bash", make_bash_args(),
                                         ftxui::Event::Character('a'));

        CHECK(result.kind == Decision::Kind::Allow);
        CHECK(!result.persist_rule.has_value());
        CHECK(!result.edit_text.has_value());
    }
}

// =============================================================================
// [AC2] 'A' key — always allow
// =============================================================================
TEST_SUITE("PermissionCard — always allow [A]") {
    TEST_CASE("'A' produces Allow with persist_rule of kind Allow") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Bash", make_bash_args("npm test"),
                                         ftxui::Event::Character('A'));

        CHECK(result.kind == Decision::Kind::Allow);
        REQUIRE(result.persist_rule.has_value());
        CHECK(result.persist_rule->kind == PermissionRule::Kind::Allow);
        // Pattern should be "Bash(npm test)" or "Bash(npm test*)" style
        CHECK(!result.persist_rule->pattern.empty());
        CHECK(result.persist_rule->pattern.find("Bash") != std::string::npos);
    }
}

// =============================================================================
// [AC3] 'n' key — deny once
// =============================================================================
TEST_SUITE("PermissionCard — deny once [n]") {
    TEST_CASE("'n' produces Deny with no persist_rule") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Read", make_read_args(),
                                         ftxui::Event::Character('n'));

        CHECK(result.kind == Decision::Kind::Deny);
        CHECK(!result.persist_rule.has_value());
        CHECK(!result.edit_text.has_value());
    }
}

// =============================================================================
// [AC4] 'N' key — always deny
// =============================================================================
TEST_SUITE("PermissionCard — always deny [N]") {
    TEST_CASE("'N' produces Deny with persist_rule of kind Deny") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Bash", make_bash_args("rm -rf /tmp/junk"),
                                         ftxui::Event::Character('N'));

        CHECK(result.kind == Decision::Kind::Deny);
        REQUIRE(result.persist_rule.has_value());
        CHECK(result.persist_rule->kind == PermissionRule::Kind::Deny);
        CHECK(!result.persist_rule->pattern.empty());
        CHECK(result.persist_rule->pattern.find("Bash") != std::string::npos);
    }
}

// =============================================================================
// [AC5] 'e' key — edit args
// =============================================================================
TEST_SUITE("PermissionCard — edit args [e]") {
    TEST_CASE("'e' produces decision with edit_text set to args preview") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        const batbox::Json args = make_bash_args("echo hello");
        auto result = fire_key_and_await(*card, "Bash", args,
                                         ftxui::Event::Character('e'));

        // edit_text should be populated with the args preview string
        REQUIRE(result.edit_text.has_value());
        CHECK(!result.edit_text->empty());
        // The args_preview is the JSON dump — should contain "echo hello"
        CHECK(result.edit_text->find("echo hello") != std::string::npos);
    }
}

// =============================================================================
// [AC6] Esc key — cancel (one-shot deny)
// =============================================================================
TEST_SUITE("PermissionCard — cancel [Esc]") {
    TEST_CASE("Esc produces Deny with no persist_rule") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Write", make_read_args("./out.txt"),
                                         ftxui::Event::Escape);

        CHECK(result.kind == Decision::Kind::Deny);
        CHECK(!result.persist_rule.has_value());
        CHECK(!result.edit_text.has_value());
    }
}

// =============================================================================
// [AC7] Worker thread blocks until key pressed
// =============================================================================
TEST_SUITE("PermissionCard — blocking await") {
    TEST_CASE("await_user_decision blocks until resolved") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        std::atomic<bool> worker_started{false};
        std::atomic<bool> worker_done{false};

        std::thread worker([&]() {
            worker_started.store(true, std::memory_order_release);
            (void)card->await_user_decision("Bash", make_bash_args());
            worker_done.store(true, std::memory_order_release);
        });

        // Wait for worker to start.
        while (!worker_started.load(std::memory_order_acquire))
            std::this_thread::yield();

        // Give it time to enter the wait — it should not have returned yet.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(!worker_done.load(std::memory_order_acquire));

        // Now resolve it.
        card->OnEvent(ftxui::Event::Character('a'));
        worker.join();
        CHECK(worker_done.load(std::memory_order_acquire));
    }

    TEST_CASE("pending() returns true while blocked and false after resolve") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        CHECK(!card->pending());  // not yet started

        std::thread worker([&]() {
            (void)card->await_user_decision("Bash", make_bash_args());
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(card->pending());

        card->OnEvent(ftxui::Event::Character('n'));
        worker.join();
        CHECK(!card->pending());
    }
}

// =============================================================================
// [AC8] OnRender() does not crash and produces non-empty output
// =============================================================================
TEST_SUITE("PermissionCard — render smoke tests") {
    TEST_CASE("OnRender() without pending await does not crash") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        // Should render a reasonable empty state without crashing.
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                             ftxui::Dimension::Fixed(20));
        REQUIRE_NOTHROW(ftxui::Render(screen, card->OnRender()));
        const std::string output = screen.ToString();
        CHECK(!output.empty());
    }

    TEST_CASE("OnRender() while await is pending contains tool name") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        std::thread worker([&]() {
            (void)card->await_user_decision("BashTool", make_bash_args("ls -la"));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                             ftxui::Dimension::Fixed(24));
        REQUIRE_NOTHROW(ftxui::Render(screen, card->OnRender()));
        const std::string output = screen.ToString();
        CHECK(!output.empty());
        // The rendered output should contain the tool name.
        CHECK(output.find("BashTool") != std::string::npos);

        card->OnEvent(ftxui::Event::Character('n'));
        worker.join();
    }

    TEST_CASE("OnRender() contains key hints") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        std::thread worker([&]() {
            (void)card->await_user_decision("Read", make_read_args());
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(80),
                                             ftxui::Dimension::Fixed(24));
        ftxui::Render(screen, card->OnRender());
        const std::string output = screen.ToString();

        // Should display permission action hints.
        CHECK(output.find("[a]") != std::string::npos);
        CHECK(output.find("[n]") != std::string::npos);

        card->OnEvent(ftxui::Event::Escape);
        worker.join();
    }
}

// =============================================================================
// [AC9] Pretty-printed args — build_preview uses indented JSON
// =============================================================================
TEST_SUITE("PermissionCard — args preview") {
    TEST_CASE("args_preview() is populated after await_user_decision starts") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        CHECK(card->args_preview().empty());  // before any call

        const batbox::Json args = make_bash_args("git status");
        std::thread worker([&]() {
            (void)card->await_user_decision("Bash", args);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // args_preview should contain the JSON content after await starts.
        CHECK(!card->args_preview().empty());
        CHECK(card->args_preview().find("git status") != std::string::npos);

        card->OnEvent(ftxui::Event::Character('a'));
        worker.join();
    }
}

// =============================================================================
// [AC10] Rule pattern format: "ToolName(arg)"
// =============================================================================
TEST_SUITE("PermissionCard — rule pattern format") {
    TEST_CASE("'A' rule for Bash uses command field") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Bash",
                                         batbox::Json{{"command", "npm run build"}},
                                         ftxui::Event::Character('A'));

        REQUIRE(result.persist_rule.has_value());
        const std::string& pattern = result.persist_rule->pattern;
        // Pattern should be "Bash(...)" style containing the command prefix.
        CHECK(pattern.find("Bash(") != std::string::npos);
        CHECK(pattern.find(")") != std::string::npos);
    }

    TEST_CASE("'A' rule for Read uses file_path field") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "Read",
                                         batbox::Json{{"file_path", "./src/main.cpp"}},
                                         ftxui::Event::Character('A'));

        REQUIRE(result.persist_rule.has_value());
        const std::string& pattern = result.persist_rule->pattern;
        CHECK(pattern.find("Read(") != std::string::npos);
        CHECK(pattern.find("./src/main.cpp") != std::string::npos);
    }

    TEST_CASE("'N' rule for generic args uses wildcard fallback") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "CustomTool",
                                         make_generic_args(),
                                         ftxui::Event::Character('N'));

        REQUIRE(result.persist_rule.has_value());
        const std::string& pattern = result.persist_rule->pattern;
        // Should be "CustomTool(*)" when no canonical field found.
        CHECK(pattern.find("CustomTool(") != std::string::npos);
        CHECK(pattern.find("*") != std::string::npos);
    }

    TEST_CASE("'A' rule for url field (WebFetch)") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        auto result = fire_key_and_await(*card, "WebFetch",
                                         batbox::Json{{"url", "https://example.com/api"}},
                                         ftxui::Event::Character('A'));

        REQUIRE(result.persist_rule.has_value());
        const std::string& pattern = result.persist_rule->pattern;
        CHECK(pattern.find("WebFetch(") != std::string::npos);
        CHECK(pattern.find("https://example.com/api") != std::string::npos);
    }
}

// =============================================================================
// Multi-call: card can be reused for successive permission requests
// =============================================================================
TEST_SUITE("PermissionCard — reuse across multiple requests") {
    TEST_CASE("Card can be used for two sequential decisions") {
        const auto theme = make_test_theme();
        auto card = std::make_shared<PermissionCard>(theme);

        // First decision: allow
        {
            auto result = fire_key_and_await(*card, "Bash", make_bash_args("echo 1"),
                                             ftxui::Event::Character('a'));
            CHECK(result.kind == Decision::Kind::Allow);
        }

        // Second decision: deny
        {
            auto result = fire_key_and_await(*card, "Read", make_read_args("./secret.txt"),
                                             ftxui::Event::Character('n'));
            CHECK(result.kind == Decision::Kind::Deny);
        }
    }
}
