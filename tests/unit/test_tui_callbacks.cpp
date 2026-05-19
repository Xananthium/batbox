// tests/unit/test_tui_callbacks.cpp
// =============================================================================
// Unit test for TuiCallbacks POD (PEXT3 1.4).
//
// Verifies:
//   - Aggregate initialisation with all 6 fields set
//   - Each field is callable after construction
//   - Default-constructed fields are empty (empty std::function)
//   - Struct is passable by const-reference without copying
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tui/TuiCallbacks.hpp>
#include <batbox/tools/AskUserQuestionTool.hpp>  // for batbox::tools::QuestionSpec

TEST_CASE("TuiCallbacks: aggregate init and all 6 fields callable") {
    bool plan_confirm_called  = false;
    bool askq_called          = false;
    bool delta_called         = false;
    bool msg_appended_called  = false;
    bool tool_running_called  = false;
    bool stream_term_called   = false;

    batbox::tui::TuiCallbacks cbs{
        .plan_confirm_fn = [&](const std::string&) -> bool {
            plan_confirm_called = true;
            return true;
        },
        .askq_prompt_fn = [&](const batbox::tools::QuestionSpec&) -> std::vector<std::string> {
            askq_called = true;
            return {"yes"};
        },
        .on_delta_cb = [&](std::string_view) {
            delta_called = true;
        },
        .on_message_appended_cb = [&](std::string_view, std::string_view,
                                      std::string_view, bool) {
            msg_appended_called = true;
        },
        .on_tool_running_cb = [&](std::string_view, std::string_view, int) {
            tool_running_called = true;
        },
        .on_stream_terminal_cb = [&]() {
            stream_term_called = true;
        },
    };

    // All fields must be non-empty after aggregate init.
    REQUIRE(static_cast<bool>(cbs.plan_confirm_fn));
    REQUIRE(static_cast<bool>(cbs.askq_prompt_fn));
    REQUIRE(static_cast<bool>(cbs.on_delta_cb));
    REQUIRE(static_cast<bool>(cbs.on_message_appended_cb));
    REQUIRE(static_cast<bool>(cbs.on_tool_running_cb));
    REQUIRE(static_cast<bool>(cbs.on_stream_terminal_cb));

    // plan_confirm_fn: returns true and sets flag.
    CHECK(cbs.plan_confirm_fn("some plan text") == true);
    CHECK(plan_confirm_called);

    // askq_prompt_fn: returns chosen label and sets flag.
    batbox::tools::QuestionSpec qs;
    auto ans = cbs.askq_prompt_fn(qs);
    CHECK(askq_called);
    REQUIRE(ans.size() == 1);
    CHECK(ans[0] == "yes");

    // on_delta_cb: fires and sets flag.
    cbs.on_delta_cb("token");
    CHECK(delta_called);

    // on_message_appended_cb: fires and sets flag.
    cbs.on_message_appended_cb("assistant", "BashTool", "result text", false);
    CHECK(msg_appended_called);

    // on_tool_running_cb: fires and sets flag.
    cbs.on_tool_running_cb("BashTool", "ls .", 1);
    CHECK(tool_running_called);

    // on_stream_terminal_cb: fires and sets flag.
    cbs.on_stream_terminal_cb();
    CHECK(stream_term_called);
}

TEST_CASE("TuiCallbacks: default-constructed fields are empty") {
    batbox::tui::TuiCallbacks empty{};

    CHECK_FALSE(static_cast<bool>(empty.plan_confirm_fn));
    CHECK_FALSE(static_cast<bool>(empty.askq_prompt_fn));
    CHECK_FALSE(static_cast<bool>(empty.on_delta_cb));
    CHECK_FALSE(static_cast<bool>(empty.on_message_appended_cb));
    CHECK_FALSE(static_cast<bool>(empty.on_tool_running_cb));
    CHECK_FALSE(static_cast<bool>(empty.on_stream_terminal_cb));
}

TEST_CASE("TuiCallbacks: passable by const-reference without copy") {
    bool called = false;

    batbox::tui::TuiCallbacks cbs{
        .on_delta_cb = [&](std::string_view) { called = true; },
    };

    // Lambda that accepts only const TuiCallbacks& — the struct must not be copied.
    auto consume = [](const batbox::tui::TuiCallbacks& cb) {
        if (cb.on_delta_cb) {
            cb.on_delta_cb("hello");
        }
    };

    consume(cbs);
    CHECK(called);
}

TEST_CASE("TuiCallbacks: plan_confirm_fn returns false") {
    batbox::tui::TuiCallbacks cbs{
        .plan_confirm_fn = [](const std::string&) -> bool { return false; },
    };

    CHECK(static_cast<bool>(cbs.plan_confirm_fn));
    CHECK(cbs.plan_confirm_fn("rejected plan") == false);
}

TEST_CASE("TuiCallbacks: askq_prompt_fn returns empty on cancel") {
    batbox::tui::TuiCallbacks cbs{
        .askq_prompt_fn = [](const batbox::tools::QuestionSpec&)
            -> std::vector<std::string> { return {}; },
    };

    CHECK(static_cast<bool>(cbs.askq_prompt_fn));
    batbox::tools::QuestionSpec qs;
    CHECK(cbs.askq_prompt_fn(qs).empty());
}
