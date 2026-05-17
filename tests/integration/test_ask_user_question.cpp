// tests/integration/test_ask_user_question.cpp
//
// doctest integration tests for AskUserQuestionTool (task CPP 5.20).
//
// Strategy: all tests inject a PromptFn callback so that no actual stdin
// interaction is required.  Headless-mode detection (isatty) is implicitly
// exercised in the "no callback + no TTY" path, which is the CI environment.
//
// Acceptance criteria covered:
//   [AC1] Single-select: one option returned
//   [AC2] Multi-select: array of options returned
//   [AC3] User cancellation: answer = "(no answer provided)"
//   [AC4] Headless mode (--print): error "AskUserQuestion not available in headless mode"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/tools/AskUserQuestionTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/permissions/PermissionMode.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace batbox;
using namespace batbox::tools;
using namespace batbox::permissions;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ToolContext make_ctx() {
    ToolContext ctx;
    ctx.cwd        = std::filesystem::temp_directory_path();
    ctx.mode       = PermissionMode::Default;
    ctx.session_id = "test-session";
    ctx.agent_id   = "";
    return ctx;
}

/// Build a minimal single-select args object with one question and two options.
static Json make_single_select_args(
    const std::string& question = "Favourite fruit?",
    const std::vector<std::string>& labels = {"Apple", "Banana"})
{
    Json opts = Json::array();
    for (const auto& lbl : labels) {
        opts.push_back({{"label", lbl}});
    }
    return {
        {"questions", Json::array({
            {
                {"question", question},
                {"options",  opts}
            }
        })}
    };
}

/// Build a multi-select args object.
static Json make_multi_select_args(
    const std::string& question = "Pick your tools:",
    const std::vector<std::string>& labels = {"Hammer", "Wrench", "Screwdriver"})
{
    Json opts = Json::array();
    for (const auto& lbl : labels) {
        opts.push_back({{"label", lbl}});
    }
    return {
        {"questions", Json::array({
            {
                {"question",    question},
                {"multiSelect", true},
                {"options",     opts}
            }
        })}
    };
}

// =============================================================================
// TEST SUITE: identity and schema
// =============================================================================
TEST_SUITE("AskUserQuestionTool — identity and schema") {

    TEST_CASE("name() returns 'AskUserQuestion'") {
        AskUserQuestionTool tool;
        CHECK(tool.name() == std::string_view("AskUserQuestion"));
    }

    TEST_CASE("description() is non-empty") {
        AskUserQuestionTool tool;
        CHECK_FALSE(std::string(tool.description()).empty());
    }

    TEST_CASE("schema_json() has correct structure") {
        AskUserQuestionTool tool;
        Json s = tool.schema_json();
        REQUIRE(s.is_object());
        CHECK(s.contains("name"));
        CHECK(s.contains("description"));
        CHECK(s.contains("parameters"));
        CHECK(s["name"].get<std::string>() == "AskUserQuestion");

        const auto& params = s["parameters"];
        REQUIRE(params.contains("properties"));
        CHECK(params["properties"].contains("questions"));
    }

    TEST_CASE("schema name matches name()") {
        AskUserQuestionTool tool;
        std::string schema_name = tool.schema_json()["name"].get<std::string>();
        CHECK(schema_name == std::string(tool.name()));
    }

    TEST_CASE("is_read_only() == false") {
        AskUserQuestionTool tool;
        CHECK_FALSE(tool.is_read_only());
    }

    TEST_CASE("requires_confirmation() == false") {
        AskUserQuestionTool tool;
        CHECK_FALSE(tool.requires_confirmation());
    }
}

// =============================================================================
// TEST SUITE: AC1 — Single-select question: one option returned
// =============================================================================
TEST_SUITE("AskUserQuestionTool — AC1: single-select") {

    TEST_CASE("single-select returns chosen label as a string") {
        // Inject a PromptFn that always picks the first option.
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            REQUIRE_FALSE(spec.labels.empty());
            return {spec.labels.front()};
        });

        auto ctx = make_ctx();
        Json args = make_single_select_args("Favourite fruit?", {"Apple", "Banana"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());

        const Json& payload = *r.structured_payload;
        REQUIRE(payload.contains("answers"));
        const auto& answers = payload["answers"];
        REQUIRE(answers.is_array());
        REQUIRE(answers.size() == 1);

        const auto& entry = answers[0];
        CHECK(entry["question"].get<std::string>() == "Favourite fruit?");
        // Single-select answer must be a string, not an array
        REQUIRE(entry["answer"].is_string());
        CHECK(entry["answer"].get<std::string>() == "Apple");
    }

    TEST_CASE("single-select with second option chosen") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            REQUIRE(spec.labels.size() >= 2);
            return {spec.labels[1]};
        });

        auto ctx = make_ctx();
        Json args = make_single_select_args("Favourite fruit?", {"Apple", "Banana"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        const auto& answers = r.structured_payload.value()["answers"];
        CHECK(answers[0]["answer"].get<std::string>() == "Banana");
    }

    TEST_CASE("single-select with header and description preserved in spec") {
        bool header_seen   = false;
        bool desc_seen     = false;

        AskUserQuestionTool tool([&](const QuestionSpec& spec) -> std::vector<std::string> {
            header_seen = (spec.header == "Category");
            desc_seen   = (!spec.descriptions.empty() && spec.descriptions[0] == "A sweet red fruit");
            return {spec.labels.front()};
        });

        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {
                    {"question", "Pick a fruit:"},
                    {"header",   "Category"},
                    {"options",  Json::array({
                        {{"label", "Apple"}, {"description", "A sweet red fruit"}},
                        {{"label", "Mango"}}
                    })}
                }
            })}
        };
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        CHECK(header_seen);
        CHECK(desc_seen);
    }

    TEST_CASE("multi-question single-select: answers array has one entry per question") {
        int call_count = 0;
        AskUserQuestionTool tool([&](const QuestionSpec& spec) -> std::vector<std::string> {
            ++call_count;
            return {spec.labels.front()};
        });

        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {
                    {"question", "Q1"},
                    {"options",  Json::array({{{"label","A"}},{{"label","B"}}})},
                },
                {
                    {"question", "Q2"},
                    {"options",  Json::array({{{"label","X"}},{{"label","Y"}}})},
                }
            })}
        };
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        const auto& answers = r.structured_payload.value()["answers"];
        CHECK(answers.size() == 2);
        CHECK(call_count == 2);
        CHECK(answers[0]["question"].get<std::string>() == "Q1");
        CHECK(answers[1]["question"].get<std::string>() == "Q2");
    }
}

// =============================================================================
// TEST SUITE: AC2 — Multi-select: array of options returned
// =============================================================================
TEST_SUITE("AskUserQuestionTool — AC2: multi-select") {

    TEST_CASE("multi-select returns array of chosen labels") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            // Select first two options
            REQUIRE(spec.labels.size() >= 2);
            return {spec.labels[0], spec.labels[1]};
        });

        auto ctx = make_ctx();
        Json args = make_multi_select_args("Pick tools:", {"Hammer", "Wrench", "Screwdriver"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        REQUIRE(r.structured_payload.has_value());
        const auto& answers = r.structured_payload.value()["answers"];
        REQUIRE(answers.size() == 1);

        const auto& entry = answers[0];
        CHECK(entry["question"].get<std::string>() == "Pick tools:");
        // Multi-select answer must be a JSON array
        REQUIRE(entry["answer"].is_array());
        REQUIRE(entry["answer"].size() == 2);
        CHECK(entry["answer"][0].get<std::string>() == "Hammer");
        CHECK(entry["answer"][1].get<std::string>() == "Wrench");
    }

    TEST_CASE("multi-select with all options chosen") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            return spec.labels;  // all options
        });

        auto ctx = make_ctx();
        Json args = make_multi_select_args("Pick all:", {"A", "B", "C"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        const auto& answers = r.structured_payload.value()["answers"];
        const auto& answer  = answers[0]["answer"];
        REQUIRE(answer.is_array());
        CHECK(answer.size() == 3);
    }

    TEST_CASE("multi-select with single selection still returns array") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            return {spec.labels[0]};
        });

        auto ctx = make_ctx();
        Json args = make_multi_select_args("Single pick in multi:", {"Option1", "Option2"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        const auto& answer = r.structured_payload.value()["answers"][0]["answer"];
        REQUIRE(answer.is_array());
        CHECK(answer.size() == 1);
        CHECK(answer[0].get<std::string>() == "Option1");
    }
}

// =============================================================================
// TEST SUITE: AC3 — User cancellation: returns "(no answer provided)"
// =============================================================================
TEST_SUITE("AskUserQuestionTool — AC3: user cancellation") {

    TEST_CASE("empty selection (user skipped) → '(no answer provided)'") {
        AskUserQuestionTool tool([](const QuestionSpec&) -> std::vector<std::string> {
            return {};  // simulate no selection / cancellation
        });

        auto ctx = make_ctx();
        Json args = make_single_select_args("Choose:", {"Yes", "No"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);  // tool itself succeeds; the skipped answer is encoded
        const auto& answers = r.structured_payload.value()["answers"];
        REQUIRE(answers.size() == 1);
        REQUIRE(answers[0]["answer"].is_string());
        CHECK(answers[0]["answer"].get<std::string>() == "(no answer provided)");
    }

    TEST_CASE("mixed: first question answered, second skipped") {
        int call_no = 0;
        AskUserQuestionTool tool([&](const QuestionSpec& spec) -> std::vector<std::string> {
            ++call_no;
            if (call_no == 1) return {spec.labels.front()};
            return {};
        });

        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {{"question","Q1"}, {"options", Json::array({{{"label","A"}},{{"label","B"}}})}}  ,
                {{"question","Q2"}, {"options", Json::array({{{"label","X"}},{{"label","Y"}}})}},
            })}
        };
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        const auto& answers = r.structured_payload.value()["answers"];
        REQUIRE(answers.size() == 2);
        CHECK(answers[0]["answer"].get<std::string>() == "A");
        CHECK(answers[1]["answer"].get<std::string>() == "(no answer provided)");
    }

    TEST_CASE("multi-select with empty selection → '(no answer provided)'") {
        AskUserQuestionTool tool([](const QuestionSpec&) -> std::vector<std::string> {
            return {};
        });

        auto ctx = make_ctx();
        Json args = make_multi_select_args("Pick:", {"A", "B"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        const auto& answer = r.structured_payload.value()["answers"][0]["answer"];
        REQUIRE(answer.is_string());
        CHECK(answer.get<std::string>() == "(no answer provided)");
    }

    TEST_CASE("CancelToken fired before run → ToolResult::error('cancelled')") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            return {spec.labels.front()};
        });

        auto [src, tok] = CancelToken::make_root();
        src.request_stop();  // fire cancellation before run()

        auto ctx = make_ctx();
        ctx.cancel_token = std::move(tok);

        Json args = make_single_select_args();
        ToolResult r = tool.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body == "cancelled");
    }
}

// =============================================================================
// TEST SUITE: AC4 — Headless mode: error when no callback and no TTY
// =============================================================================
TEST_SUITE("AskUserQuestionTool — AC4: headless mode") {

    TEST_CASE("no prompt_fn + non-TTY stdin → headless error") {
        // The test runner's stdin is not a TTY in CI (piped from /dev/null or
        // a pipe).  We construct the tool without a PromptFn.
        // If for some reason the test runner IS running with a TTY (interactive
        // developer session), this test is skipped to avoid blocking.
        if (::isatty(STDIN_FILENO)) {
            // Interactive session: we cannot test the headless path without
            // mocking isatty; skip gracefully.
            MESSAGE("Skipping headless test: stdin is a TTY (interactive session).");
            return;
        }

        AskUserQuestionTool tool;  // no PromptFn — stdin path with isatty guard
        auto ctx = make_ctx();
        Json args = make_single_select_args();
        ToolResult r = tool.run(args, ctx);

        CHECK(r.is_error);
        CHECK(r.body == "AskUserQuestion not available in headless mode");
    }

    TEST_CASE("with prompt_fn wired, headless guard is bypassed") {
        // Even if stdin is not a TTY, a wired PromptFn should bypass the guard.
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            return {spec.labels.front()};
        });

        auto ctx = make_ctx();
        Json args = make_single_select_args();
        ToolResult r = tool.run(args, ctx);

        // Should succeed regardless of TTY status
        CHECK_FALSE(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: validation errors
// =============================================================================
TEST_SUITE("AskUserQuestionTool — validation errors") {

    TEST_CASE("missing questions field → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        ToolResult r = tool.run(Json::object(), ctx);
        CHECK(r.is_error);
        CHECK_FALSE(r.body.empty());
    }

    TEST_CASE("empty questions array → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        Json args = {{"questions", Json::array()}};
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("1-4") != std::string::npos);
    }

    TEST_CASE("more than 4 questions → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        Json q_entry = {
            {"question", "Q"},
            {"options",  Json::array({{{"label","A"}},{{"label","B"}}})}
        };
        Json args = {
            {"questions", Json::array({q_entry, q_entry, q_entry, q_entry, q_entry})}
        };
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("1-4") != std::string::npos);
    }

    TEST_CASE("question with only 1 option → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {
                    {"question", "Q1"},
                    {"options",  Json::array({{{"label","OnlyOne"}}})}
                }
            })}
        };
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("2-4") != std::string::npos);
    }

    TEST_CASE("question with 5 options → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {
                    {"question", "Q1"},
                    {"options",  Json::array({
                        {{"label","A"}},{{"label","B"}},{{"label","C"}},
                        {{"label","D"}},{{"label","E"}}
                    })}
                }
            })}
        };
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
        CHECK(r.body.find("2-4") != std::string::npos);
    }

    TEST_CASE("missing question text → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {
                    {"options", Json::array({{{"label","A"}},{{"label","B"}}})}
                    // "question" field deliberately absent
                }
            })}
        };
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
    }

    TEST_CASE("option missing label → error") {
        AskUserQuestionTool tool([](const QuestionSpec& s) -> std::vector<std::string> {
            return {s.labels.front()};
        });
        auto ctx = make_ctx();
        Json args = {
            {"questions", Json::array({
                {
                    {"question", "Q"},
                    {"options",  Json::array({
                        {{"description", "no label here"}},  // missing label
                        {{"label", "OK"}}
                    })}
                }
            })}
        };
        ToolResult r = tool.run(args, ctx);
        CHECK(r.is_error);
    }
}

// =============================================================================
// TEST SUITE: body and structured_payload consistency
// =============================================================================
TEST_SUITE("AskUserQuestionTool — result body and payload") {

    TEST_CASE("body is valid JSON and parses to the same answers as payload") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            return {spec.labels.front()};
        });
        auto ctx = make_ctx();
        Json args = make_single_select_args("Q?", {"Yes", "No"});
        ToolResult r = tool.run(args, ctx);

        REQUIRE_FALSE(r.is_error);
        // Body must be parseable JSON
        auto parsed = Json::parse(r.body);
        REQUIRE(parsed.is_object());
        REQUIRE(parsed.contains("answers"));

        // Body and structured_payload must agree
        REQUIRE(r.structured_payload.has_value());
        CHECK(parsed == *r.structured_payload);
    }

    TEST_CASE("max 4 questions all answered — result has 4 entries") {
        AskUserQuestionTool tool([](const QuestionSpec& spec) -> std::vector<std::string> {
            return {spec.labels.front()};
        });
        auto ctx = make_ctx();

        Json q = {
            {"question", "Q"},
            {"options",  Json::array({{{"label","A"}},{{"label","B"}}})},
        };
        Json args = {{"questions", Json::array({q, q, q, q})}};

        ToolResult r = tool.run(args, ctx);
        REQUIRE_FALSE(r.is_error);
        CHECK(r.structured_payload.value()["answers"].size() == 4);
    }
}
