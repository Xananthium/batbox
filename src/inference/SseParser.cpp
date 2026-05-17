// src/inference/SseParser.cpp
// =============================================================================
// Implementation of batbox::inference::SseParser.
//
// WHATWG Server-Sent Events processing model, §9.2.6:
//   https://html.spec.whatwg.org/multipage/server-sent-events.html
//
// Key invariants maintained by this implementation:
//   1. Buffer never exceeds kMaxBufferBytes — checked before every append.
//   2. An event with no data: field is silently discarded (spec: "dispatch-empty").
//   3. Multiple data: lines are joined with a single '\n' between them; the
//      trailing '\n' that the spec says to strip is never appended.
//   4. \r before \n (CRLF from LM Studio) is stripped in next_line().
//   5. Event boundaries are recognised as either \n\n (LF-only) or \n\r\n
//      (the blank line in a CRLF stream is \r\n, so two consecutive line
//      endings produce \r\n\r\n = \n\r\n after the first \r is absorbed).
//   6. data: [DONE] produces an SseEvent with is_done=true and data="[DONE]".
//   7. Malformed field names (unrecognised) are silently ignored per spec.
//   8. retry: lines are parsed and silently discarded (no retry logic here).
// =============================================================================

#include <batbox/inference/SseParser.hpp>
#include <batbox/core/Logging.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::inference {

namespace {

// Convenience: is sv the exact literal "[DONE]"?
inline bool is_done_sentinel(std::string_view sv) noexcept {
    return sv == "[DONE]";
}

// ---------------------------------------------------------------------------
// find_event_boundary()
//
// Searches `s` starting at `from` for the next SSE event boundary.
// An event boundary is a blank line, which in raw bytes can appear as:
//
//   \n\n      — pure LF (Unix, OpenAI)
//   \n\r\n    — CRLF blank line (\r\n\r\n as a stream; the first \n of this
//               sequence is the line terminator for the preceding content line,
//               the \r\n is the blank line itself)
//
// Returns {first_newline_pos, separator_length} where first_newline_pos is the
// index of the \n that ends the last content line before the boundary, and
// separator_length is the number of bytes that constitute the blank-line
// separator (2 for \n\n, 3 for \n\r\n).
//
// Returns {npos, 0} if no boundary is found.
// ---------------------------------------------------------------------------
struct BoundaryResult {
    std::size_t pos;    // index of first \n in the separator
    std::size_t sep;    // total length of the separator (2 or 3)
};

BoundaryResult find_event_boundary(const std::string& s, std::size_t from) {
    std::size_t n = s.size();
    for (std::size_t i = from; i + 1 < n; ++i) {
        if (s[i] == '\n') {
            if (s[i + 1] == '\n') {
                // \n\n — LF blank line
                return {i, 2};
            }
            if (i + 2 < n && s[i + 1] == '\r' && s[i + 2] == '\n') {
                // \n\r\n — CRLF blank line
                return {i, 3};
            }
        }
    }
    return {std::string::npos, 0};
}

} // anonymous namespace

// =============================================================================
// SseParser::feed
// =============================================================================
batbox::Result<std::vector<SseEvent>> SseParser::feed(std::string_view bytes) {
    // Guard against unbounded buffer growth.
    if (buffer_.size() + bytes.size() > kMaxBufferBytes) {
        auto lg = batbox::log::get("sse_parser");
        lg->error(
            "SseParser buffer overflow: current={} incoming={} limit={}",
            buffer_.size(), bytes.size(), kMaxBufferBytes);
        return batbox::Err(std::string("SseParser buffer overflow: buffer exceeded 16 MB limit"));
    }

    buffer_.append(bytes.data(), bytes.size());

    std::vector<SseEvent> events;

    // Walk through the buffer looking for event boundaries (\n\n or \n\r\n).
    // We track `search_start` to avoid rescanning bytes already examined.
    std::size_t search_start = 0;

    while (true) {
        auto [boundary, sep_len] = find_event_boundary(buffer_, search_start);
        if (boundary == std::string::npos) {
            // No complete event boundary in the buffered data yet.
            break;
        }

        // Extract the event block: everything from search_start up to and
        // including the \n that closes the last field line in this event.
        // boundary is the index of that closing \n.
        std::string_view block(buffer_.data() + search_start,
                               boundary - search_start + 1); // +1 includes the closing \n

        // Advance past the full boundary separator for the next iteration.
        search_start = boundary + sep_len;

        // Parse the block into an SseEvent.
        auto maybe_event = parse_event(block);
        if (maybe_event.has_value()) {
            events.push_back(std::move(*maybe_event));
        }
        // If parse_event returned nullopt the block was dispatch-empty (no
        // data: field, or only a retry: / comment) — silently skip per spec.
    }

    // Remove consumed bytes from the front of the buffer.
    if (search_start > 0) {
        buffer_.erase(0, search_start);
    }

    return events;
}

// =============================================================================
// SseParser::reset
// =============================================================================
void SseParser::reset() {
    buffer_.clear();
}

// =============================================================================
// SseParser::buffered_bytes
// =============================================================================
std::size_t SseParser::buffered_bytes() const noexcept {
    return buffer_.size();
}

// =============================================================================
// SseParser::next_line  (static helper)
//
// Scans `view` for the next '\n', returns the line content *without* the
// trailing '\n' (and also without a '\r' immediately before the '\n').
// Advances `view` past the '\n'.
// Returns false and leaves `view` and `line` unchanged if no '\n' is found.
// =============================================================================
bool SseParser::next_line(std::string_view& view, std::string_view& line) {
    auto pos = view.find('\n');
    if (pos == std::string_view::npos) {
        return false;
    }
    // Line content is everything before the '\n'.
    std::string_view raw_line = view.substr(0, pos);
    // Strip a trailing '\r' (CRLF line ending, e.g. LM Studio).
    if (!raw_line.empty() && raw_line.back() == '\r') {
        raw_line.remove_suffix(1);
    }
    line = raw_line;
    view.remove_prefix(pos + 1); // advance past the '\n'
    return true;
}

// =============================================================================
// SseParser::parse_event  (static helper)
//
// Processes one raw event block (the bytes between two consecutive blank
// lines) and returns either an SseEvent or nullopt for dispatch-empty events.
//
// Parsing rules (WHATWG spec §9.2.6 "event stream interpretation"):
//   - Line starting with ':' → comment; ignore.
//   - "field: value" (space after colon is optional and consumed once).
//   - Known field names: data, event, id, retry.
//   - Multiple data: lines: append '\n' + new value to the data buffer.
//   - retry: value must be ASCII digits; if not, ignore the line.
//   - Unrecognised field names: silently ignored.
//   - If data buffer is empty after processing the block: dispatch-empty.
//   - Otherwise: emit the event (data trimmed of trailing '\n' per spec).
// =============================================================================
std::optional<SseEvent> SseParser::parse_event(std::string_view block) {
    std::string data_buf;
    bool        data_set  = false;
    std::string event_buf;
    std::string id_buf;

    std::string_view remaining = block;
    std::string_view line;

    while (next_line(remaining, line)) {
        // Empty line — should not appear inside a block (the caller split on
        // blank lines already), but guard defensively.
        if (line.empty()) {
            continue;
        }

        // Comment line.
        if (line[0] == ':') {
            continue;
        }

        // Split on first ':' to get field name and value.
        auto colon_pos = line.find(':');
        std::string_view field_name;
        std::string_view field_value;

        if (colon_pos == std::string_view::npos) {
            // No colon: the whole line is the field name, value is empty string.
            field_name  = line;
            field_value = std::string_view{};
        } else {
            field_name  = line.substr(0, colon_pos);
            field_value = line.substr(colon_pos + 1);
            // Consume exactly one leading space in the value (spec §9.2.6 step 6).
            if (!field_value.empty() && field_value[0] == ' ') {
                field_value.remove_prefix(1);
            }
        }

        // Dispatch on field name.
        if (field_name == "data") {
            if (data_set) {
                // Append '\n' then the new value (spec: concatenate with LF).
                data_buf += '\n';
            }
            data_buf.append(field_value.data(), field_value.size());
            data_set = true;
        } else if (field_name == "event") {
            event_buf.assign(field_value.data(), field_value.size());
        } else if (field_name == "id") {
            // Per spec, do not set last-event-id if the value contains U+0000.
            if (field_value.find('\0') == std::string_view::npos) {
                id_buf.assign(field_value.data(), field_value.size());
            }
        } else if (field_name == "retry") {
            // Must be all ASCII digits to be valid; silently ignore otherwise.
            bool all_digits = !field_value.empty();
            for (char c : field_value) {
                if (c < '0' || c > '9') { all_digits = false; break; }
            }
            // Retry interval is noted but not acted upon inside the pure parser.
            (void)all_digits;
        } else {
            // Unknown field name — silently ignored per spec.
            auto lg = batbox::log::get("sse_parser");
            lg->debug("SseParser: unknown SSE field '{}' — ignored", field_name);
        }
    }

    // Dispatch-empty: no data: field was seen.
    if (!data_set) {
        return std::nullopt;
    }

    SseEvent ev;
    ev.data  = std::move(data_buf);
    ev.event = std::move(event_buf);
    ev.id    = std::move(id_buf);

    // Recognise the OpenAI/standard [DONE] terminator.
    if (is_done_sentinel(ev.data)) {
        ev.is_done = true;
    }

    return ev;
}

} // namespace batbox::inference
