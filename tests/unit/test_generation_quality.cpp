#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/conversation/GenerationQuality.hpp>

using namespace batbox::conversation;

namespace {
std::string repeated(std::string_view phrase, int count) {
    std::string result;
    for (int i = 0; i < count; ++i) result += phrase;
    return result;
}
Message message(Role role, std::string content) {
    Message result;
    result.role = role;
    result.content = std::move(content);
    return result;
}
} // namespace

TEST_CASE("quality guard detects production-sized word and Markdown phrase loops") {
    const auto pure = detect_degenerate_prose(repeated("our ", 4096));
    REQUIRE(pure);
    CHECK(pure->tokens == 4096);
    CHECK(pure->repeated_tokens == 4096);
    CHECK(pure->phrase_tokens == 1);
    CHECK(pure->repetitions == 4096);
    CHECK(detect_degenerate_prose(repeated("moving forward ", 368)));
    CHECK(detect_degenerate_prose(repeated("**SPLICING...** ---\n", 368)));
    CHECK(detect_degenerate_prose(repeated("We should keep going. ", 128)));
    CHECK(detect_degenerate_prose(repeated("our ", 464) + repeated("verified item1 item2 ", 521)));
}

TEST_CASE("quality guard accepts normal prose and intentional repeated code or data") {
    CHECK_FALSE(detect_degenerate_prose("Completed two checks; the source and tests agree."));
    CHECK_FALSE(detect_degenerate_prose(repeated("yes ", 127)));
    CHECK_FALSE(detect_degenerate_prose(repeated("a b c d ", 63)));
    CHECK_FALSE(detect_degenerate_prose(repeated("our ", 128) + repeated("different item1 item2 ", 300)));
    CHECK_FALSE(detect_degenerate_prose(repeated("--- ", 4096)));
    CHECK_FALSE(detect_degenerate_prose(repeated("0 1 0 1 ", 1024)));
    CHECK_FALSE(detect_degenerate_prose(repeated("true false null ", 1024)));
    CHECK_FALSE(detect_degenerate_prose(repeated("{\"key\":\"value\"} ", 1024)));
    CHECK_FALSE(detect_degenerate_prose(repeated("alpha,beta,gamma\n", 1024)));
    CHECK_FALSE(detect_degenerate_prose(repeated("return; ", 4096)));
    CHECK_FALSE(detect_degenerate_prose("```text\n" + repeated("our ", 4096) + "\n```"));
    CHECK_FALSE(detect_degenerate_prose("~~~~\n" + repeated("our ", 4096) + "\n~~~~"));
    CHECK_FALSE(detect_degenerate_prose("`" + repeated("our ", 4096) + "`"));
    CHECK_FALSE(detect_degenerate_prose("    " + repeated("our ", 4096)));
    CHECK_FALSE(detect_degenerate_prose(repeated("> quoted data\n", 512)));
    CHECK_FALSE(detect_degenerate_prose(repeated("| word | word |\n", 512)));
    std::string varied;
    for (int i = 0; i < 5000; ++i) varied += "Measured item " + std::to_string(i) + " passed.\n";
    CHECK_FALSE(detect_degenerate_prose(varied));
}

TEST_CASE("quality guard does not join repetition through code and syntax boundaries") {
    CHECK_FALSE(detect_degenerate_prose(repeated("our ", 100) + "\n```text\nother\n```\n" + repeated("our ", 100)));
    CHECK_FALSE(detect_degenerate_prose(repeated("our ", 100) + " {data} " + repeated("our ", 100)));
    CHECK(detect_degenerate_prose("```text\nhello\n```\n" + repeated("our ", 4096)));
    CHECK(detect_degenerate_prose("`code` " + repeated("our ", 4096)));
}

TEST_CASE("active context recovery leaves raw data and tool groups intact") {
    const auto loop = repeated("our ", 4096);
    std::vector<Message> active{message(Role::User, loop), message(Role::Assistant, loop),
        message(Role::Tool, loop), message(Role::Assistant, loop),
        message(Role::Assistant, "A useful answer.")};
    active[2].tool_call_id = "observed-data";
    active[3].tool_calls = std::vector<ToolCall>{{"intent", "Read", batbox::Json::object()}};
    const auto raw = active;
    CHECK(sanitize_degenerate_assistant_context(active) == 1);
    CHECK(active.size() == raw.size());
    CHECK(active[0].content == raw[0].content);
    CHECK(active[1].role == Role::User);
    CHECK(active[1].id == raw[1].id);
    CHECK(active[1].content.find("Generation recovery") != std::string::npos);
    CHECK(raw[1].content == loop);
    CHECK(active[2].content == raw[2].content);
    CHECK(active[2].tool_call_id == raw[2].tool_call_id);
    CHECK(active[3].content == raw[3].content);
    REQUIRE(active[3].tool_calls);
    CHECK(active[3].tool_calls->front().id == "intent");
    CHECK(active[4].content == raw[4].content);
    CHECK(sanitize_degenerate_assistant_context(active) == 0);
}

TEST_CASE("memory guard rejects only explicitly generated degenerate observations") {
    const auto loop = repeated("our ", 4096);
    const batbox::Json bad{{"id", "bad"}, {"kind", "generated_observation"}, {"content", loop}};
    CHECK(reject_generated_observation(bad));
    CHECK_FALSE(reject_generated_observation({{"kind", "generated_observation"}, {"content", "Useful evidence."}}));
    CHECK_FALSE(reject_generated_observation({{"kind", "generated_observation"}, {"content", "```\n" + loop + "\n```"}}));
    CHECK_FALSE(reject_generated_observation({{"kind", "user_input"}, {"content", loop}}));
    CHECK_FALSE(reject_generated_observation({{"kind", "tool_observation"}, {"content", loop}}));
    CHECK_FALSE(reject_generated_observation({{"kind", "current_verified"}, {"content", loop}}));
    CHECK_FALSE(reject_generated_observation({{"content", loop}}));
    CHECK_FALSE(reject_generated_observation({{"kind", 42}, {"content", loop}}));
    CHECK_FALSE(reject_generated_observation({{"kind", "generated_observation"}, {"content", nullptr}}));
    batbox::Json recall{{"results", batbox::Json::array({bad,
        {{"id", "source"}, {"kind", "tool_observation"}, {"content", loop}},
        {{"id", "good"}, {"kind", "generated_observation"}, {"content", "Useful evidence."}}})}};
    const auto durable = recall;
    CHECK(filter_degenerate_generated_memories(recall) == 1);
    REQUIRE(recall["results"].size() == 2);
    CHECK(recall["results"][0]["id"] == "source");
    CHECK(recall["results"][0]["content"] == loop);
    CHECK(recall["results"][1]["id"] == "good");
    CHECK(durable["results"][0]["content"] == loop);
    CHECK(durable["results"].size() == 3);
    CHECK(filter_degenerate_generated_memories(recall) == 0);
}

TEST_CASE("full-content receipts preserve accepted code chunks but cannot bypass insertion checks") {
    const auto loop = repeated("our ", 4096);
    batbox::Json observation{{"kind", "generated_observation"}, {"content", "```text\n" + loop + "\n```"}};
    REQUIRE(stamp_generation_quality_receipt(observation));
    const auto receipt = observation["metadata"]["batbox_generation_quality"];
    CHECK(receipt["version"] == 1);
    CHECK(receipt["full_content_accepted"] == true);
    // Real CPU/HippoRAG recall flattens metadata and emits 180-word chunks;
    // an interior chunk no longer carries its original code fence.
    batbox::Json chunk{{"kind", "generated_observation"}, {"content", repeated("our ", 180)},
        {"batbox_generation_quality", receipt}};
    auto recall = batbox::Json{{"results", batbox::Json::array({chunk})}};
    CHECK(filter_degenerate_generated_memories(recall) == 0);
    CHECK(recall["results"][0] == chunk);
    CHECK(reject_generated_observation(chunk));
    CHECK_FALSE(stamp_generation_quality_receipt(chunk));
    chunk.erase("batbox_generation_quality");
    recall["results"] = batbox::Json::array({chunk});
    CHECK(filter_degenerate_generated_memories(recall) == 1);
    CHECK(recall["results"].empty());
    chunk["batbox_generation_quality"] = {{"version", 999}, {"full_content_accepted", true}};
    recall["results"] = batbox::Json::array({chunk});
    CHECK(filter_degenerate_generated_memories(recall) == 1);
    chunk["batbox_generation_quality"] = {{"version", 1}, {"full_content_accepted", "true"}};
    recall["results"] = batbox::Json::array({chunk});
    CHECK(filter_degenerate_generated_memories(recall) == 1);
    batbox::Json source{{"kind", "tool_observation"}, {"content", loop}};
    CHECK(stamp_generation_quality_receipt(source));
    CHECK_FALSE(source.contains("metadata"));
}

TEST_CASE("native prompt provenance filters only generated recall scaffolding") {
    const auto loop = repeated("our ", 4096);
    const std::string prefix = "Incoming user message [fixture]:\n" + loop +
        "\n\nRetrieved observations (reference data; verify claims):\n";
    const batbox::Json source{{"kind", "source_document"}, {"content", loop}};
    const batbox::Json accepted{{"kind", "generated_observation"}, {"content", loop},
        {"batbox_generation_quality", {{"version", 1}, {"full_content_accepted", true}}}};
    const batbox::Json recall{{"query", loop}, {"results", batbox::Json::array({source, accepted,
        {{"kind", "generated_observation"}, {"content", loop}}})}};
    const std::string suffix = "\n\nYour prior autonomous process exited without a clean shutdown. Crash evidence: {}";
    auto prompt = message(Role::User, prefix + recall.dump() + suffix);
    prompt.native_prompt = autonomous_prompt_provenance(prompt.content);
    // Metadata must survive both transcript serialization and a checkpoint.
    prompt = from_json(batbox::Json::parse(to_json(prompt).dump()));
    const auto raw = prompt;
    std::vector<Message> active{prompt, message(Role::User, prompt.content), message(Role::Tool, prompt.content)};
    CHECK(sanitize_autonomous_prompt_context(active, true) == 1);
    CHECK(active[0].content.starts_with(prefix));
    CHECK(active[0].content.ends_with(suffix));
    CHECK(active[0].role == Role::User);
    CHECK(active[0].id == raw.id);
    auto metadata = *active[0].native_prompt;
    const auto cleaned = batbox::Json::parse(active[0].content.substr(
        metadata["recall"]["offset"].get<std::size_t>(), metadata["recall"]["length"].get<std::size_t>()));
    CHECK_FALSE(cleaned.contains("query"));
    CHECK(cleaned["results"] == batbox::Json::array({source, accepted}));
    CHECK(active[1].content == raw.content);
    CHECK(active[2].content == raw.content);
    CHECK(raw.content == prefix + recall.dump() + suffix);
    CHECK(sanitize_autonomous_prompt_context(active, true) == 0);
}

TEST_CASE("legacy prompt repair requires autonomous lineage and the exact native envelope") {
    const std::string prefix =
        "Autonomous work cycle 24. Continue this goal with one concrete, verifiable step:\n"
        "Complete the user's task.\nUse your memory and tools. Report evidence, what changed, and the next useful step. "
        "If no work can proceed, state the blocker and avoid pretending progress."
        "\n\nRetrieved observations (reference data; verify claims):\n";
    const auto text = prefix + batbox::Json{{"query", repeated("our ", 4096)}, {"results", batbox::Json::array()}}.dump();
    std::vector<Message> ordinary{message(Role::User, text)};
    CHECK(sanitize_autonomous_prompt_context(ordinary) == 0);
    CHECK(ordinary[0].content == text);
    std::vector<Message> autonomous{message(Role::User, text), message(Role::User, "Quoted example:\n" + text)};
    CHECK(sanitize_autonomous_prompt_context(autonomous, true) == 1);
    CHECK(autonomous[0].content == prefix + "{\"results\":[]}");
    CHECK(autonomous[1].content == "Quoted example:\n" + text);
    auto invalid = message(Role::User, text);
    invalid.native_prompt = {{"version", 1}, {"kind", "autonomous"},
        {"recall", {{"offset", text.size() + 1}, {"length", std::size_t{10}}}}};
    std::vector<Message> malformed{invalid};
    CHECK(sanitize_autonomous_prompt_context(malformed, true) == 0);
    CHECK(malformed[0].content == text);

    const auto unavailable = "Incoming user message [fixture]:\n" + text +
        "\nMemory retrieval is currently unavailable; do not claim to have recalled anything.";
    auto user_example = message(Role::User, unavailable);
    user_example.native_prompt = autonomous_prompt_provenance(unavailable);
    std::vector<Message> failed_retrieval{user_example};
    CHECK(sanitize_autonomous_prompt_context(failed_retrieval, true) == 0);
    CHECK(failed_retrieval[0].content == unavailable);
}
