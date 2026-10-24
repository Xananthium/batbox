#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/GenerationQuality.hpp>

#include <string>

using batbox::conversation::StreamingGenerationGuard;

TEST_CASE("streaming repetition stops after a bounded received prefix") {
    StreamingGenerationGuard guard;
    std::string content;
    bool rejected = false;
    for (int i = 0; i < 4096; ++i) {
        content += " our";
        if (const auto found = guard.check(content)) {
            CHECK(found->repeated_tokens >= 128);
            CHECK(found->phrase_tokens == 1);
            CHECK(content.size() <= 1024);
            rejected = true;
            break;
        }
    }
    CHECK(rejected);
}

TEST_CASE("streaming detection ignores an unfinished word and repeated source data") {
    StreamingGenerationGuard guard;
    std::string content;
    for (int i = 0; i < 127; ++i) content += "our ";
    CHECK_FALSE(guard.check(content + "our"));

    for (const std::string text : {
            "```text\n", "~~~~\n", "`", "    ", "> "}) {
        StreamingGenerationGuard data_guard;
        content = text;
        for (int i = 0; i < 512; ++i) {
            // Character-sized chunks exercise incomplete Markdown fences,
            // inline spans and words in the growing received response.
            for (char ch : std::string("our ")) {
                content += ch;
                CHECK_FALSE(data_guard.check(content));
            }
        }
    }
}

TEST_CASE("streaming guard detects prose after a completed source block") {
    StreamingGenerationGuard guard;
    std::string content = "```text\n";
    for (int i = 0; i < 160; ++i) content += "our ";
    content += "\n```\n";
    CHECK_FALSE(guard.check(content));
    bool rejected = false;
    for (int i = 0; i < 512; ++i) {
        content += "moving forward ";
        if (guard.check(content)) { rejected = true; break; }
    }
    CHECK(rejected);
}

TEST_CASE("private reasoning rejection receipts cannot become message content") {
    using namespace batbox::conversation;
    Message message;
    message.role = Role::Assistant;
    auto raw = to_json(message);
    raw["generation_rejected"] = "degenerate_repetition";
    raw["generation_quality"] = {{"channel", "reasoning"},
        {"received_reasoning_bytes", 8}, {"reasoning", " our our"}};
    const auto restored = from_json(raw);
    CHECK(restored.content.empty());
    CHECK_FALSE(restored.tool_calls.has_value());
    CHECK_FALSE(to_json(restored).contains("generation_quality"));
    CHECK(raw["generation_quality"]["reasoning"] == " our our");
}
