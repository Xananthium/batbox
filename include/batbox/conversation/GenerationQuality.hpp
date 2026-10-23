#pragma once

#include <batbox/conversation/Message.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace batbox::conversation {

struct GenerationDegeneration {
    std::size_t tokens{0};
    std::size_t repeated_tokens{0};
    std::size_t phrase_tokens{0};
    std::size_t repetitions{0};
};

// Deliberately narrow, content-based guard for extreme English prose loops.
// A 1–4-token phrase must repeat consecutively at least 64 times, span at
// least 128 tokens, and occupy at least 20% of the complete response. Fenced
// and inline code, indented/quoted data, numbers and syntax are not prose.
// A length finish alone is never grounds for rejection. This is a heuristic,
// not a general correctness check; unmarked intentional prose repetition is
// indistinguishable from a generated loop and should be fenced as data.
[[nodiscard]] std::optional<GenerationDegeneration> detect_degenerate_prose(
    std::string_view content);

// Reuse the same exclusions on a growing visible response. Inspect at most
// once per 256 newly received bytes and ignore the unfinished final word;
// callers retain the complete original text and decide how to stop transport.
class StreamingGenerationGuard {
public:
    [[nodiscard]] std::optional<GenerationDegeneration> check(std::string_view content);
private:
    std::size_t checked_bytes_{0};
};

// Only assistant finals are changed, in the caller's active copy. User/tool
// content and assistant tool intents are untouched. A recovery user marker
// keeps a rejected pending final from looking completed after restart.
// The caller remains responsible for retaining the unmodified raw transcript.
[[nodiscard]] std::size_t sanitize_degenerate_assistant_context(
    std::vector<Message>& messages);
void replace_with_generation_recovery(Message& message);

// Call only for prompts assembled by the native autonomous engine. Records
// the generated recall JSON range while leaving embedded user/source text
// outside that range untouched. Metadata survives durable context checkpoints.
[[nodiscard]] batbox::Json autonomous_prompt_provenance(std::string_view content);
// Remove request-query echoes and rejected generated observations only inside
// native recall payloads. Legacy recognition requires an autonomous session
// plus the exact native cycle envelope; generic user messages are never scanned.
[[nodiscard]] std::size_t sanitize_autonomous_prompt_context(
    std::vector<Message>& messages, bool legacy_autonomous_session = false);

// Generated observations only: explicitly tagged source/user/tool memories
// are never rejected. These helpers do not read, write or delete memory data.
[[nodiscard]] bool reject_generated_observation(const batbox::Json& observation);
// Recheck the complete generated content even if the caller supplied a receipt.
// On success, attach native versioned metadata so recalled chunks are not
// mistaken for prose after the memory chunker removes their original fences.
// Returns false for degeneration; other kinds pass without an added receipt.
[[nodiscard]] bool stamp_generation_quality_receipt(batbox::Json& observation);
// Remove rejected generated records from a caller-owned recall copy. Walks
// arrays/objects to cover nested retrieval results, preserving other records
// and chunks whose full original content passed this guard at insertion.
[[nodiscard]] std::size_t filter_degenerate_generated_memories(batbox::Json& recall);

} // namespace batbox::conversation
