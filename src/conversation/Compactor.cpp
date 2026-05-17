// src/conversation/Compactor.cpp
// =============================================================================
// Implementation of batbox::conversation::Compactor (CPP 3.5).
//
// Design notes:
//   - The summarisation prompt instructs the model to produce a dense paragraph
//     preserving: file paths, error symptoms, tool calls made, decisions taken,
//     and open to-dos.  This mirrors what claude-code's /compact does.
//   - The result is injected as a Role::System message so that downstream
//     inference requests treat it as background context rather than a user
//     or assistant turn.
//   - stream=false is forced on the summary request (same as Client::chat).
//   - max_tokens is capped at 1024 to keep the summary message small.
//   - The model used for summarisation is whatever Config::api.default_model
//     the Client was constructed with.  Callers can customise this by
//     adjusting their Config before constructing the Client.
// =============================================================================

#include <batbox/conversation/Compactor.hpp>

#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace batbox::conversation {

// =============================================================================
// Summarisation system prompt
// =============================================================================

static constexpr std::string_view kSummarisationSystemPrompt =
    "You are a precise conversation summariser. The user will provide a "
    "sequence of conversation turns. Produce a single dense paragraph (200-400 "
    "words) that preserves:\n"
    "  - Every file path mentioned or modified\n"
    "  - Error messages and their symptoms\n"
    "  - Tool calls made and their outcomes\n"
    "  - Decisions reached and their rationale\n"
    "  - Open to-dos or unresolved questions\n"
    "Do NOT include pleasantries, greetings, or filler phrases. "
    "Write in third person past tense. "
    "Begin the summary with 'Summary of earlier context:'.";

// =============================================================================
// Constructor
// =============================================================================

Compactor::Compactor(int keep_last_n, StatusCallback on_status)
    : keep_last_n_(keep_last_n >= 0 ? keep_last_n : 0)
    , on_status_(std::move(on_status))
{}

// =============================================================================
// compact()
// =============================================================================

Result<std::vector<Message>>
Compactor::compact(const std::vector<Message>& msgs,
                   batbox::inference::Client&  client,
                   batbox::CancelToken         ct)
{
    const int total = static_cast<int>(msgs.size());

    // Determine the split point.
    // tail_start == index of first message kept verbatim.
    const int tail_start = std::max(0, total - keep_last_n_);

    // If there is nothing to summarise (head is empty), return unchanged.
    if (tail_start == 0) {
        return msgs;
    }

    // Check for cancellation before the network call.
    ct.throw_if_cancelled();

    // Build head slice.
    std::vector<Message> head(msgs.begin(), msgs.begin() + tail_start);
    std::vector<Message> tail(msgs.begin() + tail_start, msgs.end());

    // Build the summarisation request.
    batbox::inference::ChatRequest req;
    req.model    = "gpt-4o";      // default; caller controls via Config
    req.stream   = false;
    req.max_tokens = 1024;
    req.messages = build_summary_request_messages(head);

    // Call the inference client.
    auto resp_result = client.chat(req);
    if (!resp_result.has_value()) {
        return batbox::Err(std::move(resp_result).error());
    }

    const auto& resp = resp_result.value();
    std::string summary_text =
        resp.content.value_or("(no summary returned by model)");

    // Construct the summary Message (Role::System).
    Message summary_msg;
    summary_msg.role    = Role::System;
    summary_msg.content = std::move(summary_text);

    // Assemble the new conversation: [summary] + verbatim tail.
    std::vector<Message> compacted;
    compacted.reserve(1 + tail.size());
    compacted.push_back(std::move(summary_msg));
    for (auto& m : tail) {
        compacted.push_back(std::move(m));
    }

    // Fire the status callback.
    if (on_status_) {
        on_status_(format_status_note(total,
                                       /*summary_count=*/1,
                                       static_cast<int>(tail.size())));
    }

    return compacted;
}

// =============================================================================
// build_summary_request_messages()
//
// Wire layout:
//   [0] system  — kSummarisationSystemPrompt
//   [1] user    — "Here is the conversation history to summarise:" +
//                  each head turn as "ROLE: content\n"
// =============================================================================

/*static*/
std::vector<batbox::inference::WireMessage>
Compactor::build_summary_request_messages(const std::vector<Message>& head)
{
    // Concatenate the head turns into one user message.
    std::string history;
    history.reserve(head.size() * 256);

    history += "Here is the conversation history to summarise:\n\n";

    for (const auto& m : head) {
        std::string_view role_str;
        switch (m.role) {
            case Role::System:    role_str = "SYSTEM";    break;
            case Role::User:      role_str = "USER";      break;
            case Role::Assistant: role_str = "ASSISTANT"; break;
            case Role::Tool:      role_str = "TOOL";      break;
        }
        history += role_str;
        history += ": ";
        history += m.content;
        history += "\n\n";
    }

    batbox::inference::WireMessage sys_msg;
    sys_msg.role    = "system";
    sys_msg.content = std::string(kSummarisationSystemPrompt);

    batbox::inference::WireMessage user_msg;
    user_msg.role    = "user";
    user_msg.content = std::move(history);

    return {std::move(sys_msg), std::move(user_msg)};
}

// =============================================================================
// format_status_note()
// =============================================================================

/*static*/
std::string Compactor::format_status_note(int total_turns,
                                           int summary_count,
                                           int verbatim_count)
{
    return "context compacted: "
         + std::to_string(total_turns)
         + " turns \xE2\x86\x92 "   // UTF-8 → (U+2192)
         + std::to_string(summary_count)
         + " summary + "
         + std::to_string(verbatim_count)
         + " recent";
}

} // namespace batbox::conversation
