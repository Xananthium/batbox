// include/batbox/inference/SseParser.hpp
// =============================================================================
// batbox::inference::SseParser — stateful SSE line-splitter and event reassembler.
//
// Spec compliance:
//   - WHATWG Server-Sent Events spec (https://html.spec.whatwg.org/multipage/server-sent-events.html)
//   - OpenAI streaming completions SSE shape: data: <json>\n\n with data: [DONE] terminator
//   - Multi-line data: fields concatenated with \n per spec
//   - event:, id:, retry: fields parsed
//   - Comments (lines starting with :) silently ignored
//   - \r\n and \n line endings both accepted (LM Studio compat: strips \r)
//   - Partial chunks buffered until a complete event boundary is seen
//   - Buffer capped at 16 MB; exceeding the cap returns an error
//
// Usage:
//   SseParser parser;
//   // In the HTTP write callback:
//   auto result = parser.feed(chunk_bytes);
//   if (!result) { /* buffer overflow — abort */ }
//   for (auto& ev : *result) {
//       if (ev.is_done) break;
//       process(ev.data);
//   }
//
// Thread-safety: not thread-safe; external locking required if fed from multiple threads.
// =============================================================================

#pragma once

#include <batbox/core/Result.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::inference {

// =============================================================================
// SseEvent — one complete SSE event emitted by SseParser::feed().
// =============================================================================
struct SseEvent {
    /// The payload. For multi-line data: fields the lines are joined with '\n'
    /// exactly as required by the WHATWG spec.  For a data: [DONE] event this
    /// field is "[DONE]" and is_done is true.
    std::string data;

    /// Value of the event: field (empty string when the field was absent).
    std::string event;

    /// Value of the id: field (empty string when the field was absent).
    std::string id;

    /// True when data == "[DONE]".  Callers should stop consuming after this.
    bool is_done = false;
};

// =============================================================================
// SseParser — stateful chunked-input parser.
// =============================================================================
class SseParser {
public:
    /// Maximum internal buffer size.  feed() returns an error when the buffer
    /// would exceed this limit.
    static constexpr std::size_t kMaxBufferBytes = 16u * 1024u * 1024u; // 16 MB

    SseParser()  = default;
    ~SseParser() = default;

    // Non-copyable, movable.
    SseParser(const SseParser&)            = delete;
    SseParser& operator=(const SseParser&) = delete;
    SseParser(SseParser&&)                 = default;
    SseParser& operator=(SseParser&&)      = default;

    // -------------------------------------------------------------------------
    // feed() — primary API.
    //
    // Appends `bytes` to the internal buffer, splits on \n\n event boundaries,
    // parses each complete event, and returns all events whose boundary was
    // reached within this call.  Any trailing bytes that belong to an incomplete
    // (partial) event are kept in the buffer for the next call.
    //
    // Returns:
    //   Ok   — vector of zero or more complete SseEvent values (may be empty
    //          if no event boundary was reached yet).
    //   Err  — std::string describing a fatal parser error (buffer overflow).
    //          After an error the parser is in an undefined state; callers
    //          should discard it.
    //
    // Blueprint contract:
    //   Result<std::vector<SseEvent>> feed(std::string_view bytes)
    // -------------------------------------------------------------------------
    [[nodiscard]] batbox::Result<std::vector<SseEvent>> feed(std::string_view bytes);

    /// Reset the parser to initial state (clears the internal buffer).
    void reset();

    /// Current number of bytes held in the internal partial-event buffer.
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
    // Internal accumulation buffer.  Holds bytes received so far that have not
    // yet formed a complete event (i.e., no \n\n boundary seen yet).
    std::string buffer_;

    // -------------------------------------------------------------------------
    // parse_event() — parse one raw event block (the text between two \n\n
    // boundaries) into an SseEvent.  Returns nullopt when the block contains
    // no data: field (dispatch-empty per spec: such events are discarded).
    // -------------------------------------------------------------------------
    static std::optional<SseEvent> parse_event(std::string_view block);

    // -------------------------------------------------------------------------
    // next_line() — extract the next line from `view`, advancing `view` past it.
    // Strips a trailing \r before the \n so CRLF sources work transparently.
    // Returns true and sets `line` on success; returns false when `view` is
    // exhausted with no remaining newline.
    // -------------------------------------------------------------------------
    static bool next_line(std::string_view& view, std::string_view& line);
};

} // namespace batbox::inference
