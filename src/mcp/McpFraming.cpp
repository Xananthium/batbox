// src/mcp/McpFraming.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::mcp::FrameWriter and batbox::mcp::FrameReader.
//
// Wire format (identical to LSP stdio transport):
//
//   Content-Length: <decimal-N>\r\n
//   [Content-Type: application/vnd.schemastore.json; charset=utf-8\r\n]  ← optional
//   \r\n
//   <N bytes of JSON body>
//
// Framing rules implemented here:
//   1.  The header block ends at the first "\r\n\r\n" sequence.
//   2.  "Content-Length:" (case-insensitive search for resilience) must be
//       present exactly once before the separator.
//   3.  The value after "Content-Length:" is a non-negative decimal integer.
//       Leading/trailing whitespace is tolerated.
//   4.  Any other headers (e.g., Content-Type) are accepted and ignored.
//   5.  After the separator exactly N bytes form the JSON body.
//   6.  Malformed frames: the decoder scans forward from the error position
//       to find the next "Content-Length:" token so subsequent valid frames
//       are still decoded.
// ---------------------------------------------------------------------------

#include <batbox/mcp/McpFraming.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::mcp {

// ============================================================================
// Internal helpers (anonymous namespace — not part of the public API)
// ============================================================================

namespace {

constexpr std::string_view kHeaderSep      = "\r\n\r\n";
constexpr std::string_view kContentLength  = "content-length:";  // lower-cased for search

// Case-insensitive ASCII tolower for single char.
[[nodiscard]] constexpr char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// Case-insensitive search for needle inside haystack.
// Returns position of first match or std::string::npos.
[[nodiscard]] std::size_t ci_find(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return 0;
    if (haystack.size() < needle.size()) return std::string::npos;
    const std::size_t limit = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (to_lower(haystack[i + j]) != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return std::string::npos;
}

// Trim leading and trailing ASCII whitespace from a string_view.
[[nodiscard]] std::string_view trim(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                           sv.front() == '\r' || sv.front() == '\n'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                           sv.back() == '\r' || sv.back() == '\n'))
        sv.remove_suffix(1);
    return sv;
}

// Parse a non-negative decimal integer from sv.
// Returns std::nullopt if sv is empty, non-numeric, or would overflow std::size_t.
[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view sv) noexcept {
    sv = trim(sv);
    if (sv.empty()) return std::nullopt;
    std::size_t value = 0;
    auto result = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (result.ec != std::errc{} || result.ptr != sv.data() + sv.size())
        return std::nullopt;
    return value;
}

// Build the "Content-Length: N\r\n\r\n" header prefix.
[[nodiscard]] std::string make_header(std::size_t body_size) {
    std::string h;
    h.reserve(32);
    h += "Content-Length: ";
    h += std::to_string(body_size);
    h += "\r\n\r\n";
    return h;
}

} // anonymous namespace

// ============================================================================
// FrameWriter
// ============================================================================

std::string FrameWriter::encode(std::string_view json_payload) const {
    std::string frame;
    frame.reserve(32 + json_payload.size());
    frame += make_header(json_payload.size());
    frame.append(json_payload.data(), json_payload.size());
    return frame;
}

// ============================================================================
// FrameReader
// ============================================================================

std::vector<std::string> FrameReader::feed(std::string_view chunk) {
    buffer_.append(chunk.data(), chunk.size());

    std::vector<std::string> messages;

    while (true) {
        ParseResult pr = try_parse_one();

        if (pr.tag == ParseResult::Tag::kIncomplete) {
            // Not enough data yet — wait for more bytes.
            break;
        }

        if (pr.tag == ParseResult::Tag::kMalformed) {
            errors_.push_back(std::move(pr.error));
            // Consume the bytes the parser identified as bad, then try again
            // so subsequent valid frames are not lost.
            buffer_.erase(0, pr.consumed);
            continue;
        }

        // kComplete
        messages.push_back(std::move(pr.payload));
        buffer_.erase(0, pr.consumed);
    }

    return messages;
}

const std::vector<std::string>& FrameReader::errors() const noexcept {
    return errors_;
}

void FrameReader::clear_errors() noexcept {
    errors_.clear();
}

void FrameReader::reset() noexcept {
    buffer_.clear();
    errors_.clear();
}

std::size_t FrameReader::pending_bytes() const noexcept {
    return buffer_.size();
}

// ---------------------------------------------------------------------------
// try_parse_one — core state-machine step
//
// Operates on buffer_ (read-only — does NOT modify it).  Returns a ParseResult
// describing:
//   kIncomplete  — need more bytes; consumed == 0
//   kMalformed   — bad header; consumed == bytes to skip (at least 1)
//   kComplete    — full frame; payload holds body; consumed == total frame bytes
// ---------------------------------------------------------------------------
FrameReader::ParseResult FrameReader::try_parse_one() const {
    std::string_view buf(buffer_);

    if (buf.empty()) {
        return {ParseResult::Tag::kIncomplete, {}, {}, 0};
    }

    // -----------------------------------------------------------------------
    // Step 1: Locate the header/body separator "\r\n\r\n"
    // -----------------------------------------------------------------------
    const std::size_t sep_pos = buf.find(kHeaderSep);

    if (sep_pos == std::string_view::npos) {
        // Separator not yet arrived — could still be a partial header, OR we
        // could have garbage at the front.  If the buffer is growing large
        // without a separator, we need to resync.
        //
        // Heuristic: if we already have more than 8 KB without seeing a
        // separator we assume this prefix is garbage and scan forward for
        // the next "Content-Length:" token to resync.
        constexpr std::size_t kMaxHeaderSearch = 8192;
        if (buf.size() >= kMaxHeaderSearch) {
            // Skip the first byte and look for "Content-Length:" to resync.
            std::size_t next_cl = ci_find(buf.substr(1), kContentLength);
            std::size_t skip = (next_cl == std::string::npos)
                                   ? buf.size()          // discard all
                                   : (next_cl + 1);      // jump to next header
            return {
                ParseResult::Tag::kMalformed,
                {},
                "MCP framing: no header separator found after " +
                    std::to_string(kMaxHeaderSearch) + " bytes; discarding " +
                    std::to_string(skip) + " bytes",
                skip
            };
        }
        // Otherwise just wait for more data.
        return {ParseResult::Tag::kIncomplete, {}, {}, 0};
    }

    // -----------------------------------------------------------------------
    // Step 2: Extract and parse the header block
    // -----------------------------------------------------------------------
    std::string_view header_block = buf.substr(0, sep_pos);
    const std::size_t body_offset = sep_pos + kHeaderSep.size();

    // Find "Content-Length:" (case-insensitive) within the header block.
    const std::size_t cl_pos = ci_find(header_block, kContentLength);

    if (cl_pos == std::string_view::npos) {
        // No Content-Length header — skip past the separator and try to resync.
        return {
            ParseResult::Tag::kMalformed,
            {},
            "MCP framing: missing Content-Length header in frame starting at buffer offset 0",
            body_offset   // skip the entire malformed header+separator
        };
    }

    // Value starts after "Content-Length:" and runs to end of that line (\r\n).
    std::string_view after_cl = header_block.substr(cl_pos + kContentLength.size());
    // Trim to the end of this logical line (before the next \r\n if present).
    const std::size_t eol = after_cl.find("\r\n");
    std::string_view cl_value_sv = (eol == std::string_view::npos)
                                        ? after_cl
                                        : after_cl.substr(0, eol);

    auto body_size_opt = parse_size(cl_value_sv);

    if (!body_size_opt) {
        return {
            ParseResult::Tag::kMalformed,
            {},
            std::string("MCP framing: invalid Content-Length value: '") +
                std::string(trim(cl_value_sv)) + "'",
            body_offset   // skip header+separator
        };
    }

    const std::size_t body_size = *body_size_opt;

    // -----------------------------------------------------------------------
    // Step 3: Check whether the full body has arrived
    // -----------------------------------------------------------------------
    if (buf.size() < body_offset + body_size) {
        // Body is still arriving — wait for more bytes.
        return {ParseResult::Tag::kIncomplete, {}, {}, 0};
    }

    // -----------------------------------------------------------------------
    // Step 4: Extract the body
    // -----------------------------------------------------------------------
    std::string_view body = buf.substr(body_offset, body_size);
    const std::size_t total_consumed = body_offset + body_size;

    return {
        ParseResult::Tag::kComplete,
        std::string(body),
        {},
        total_consumed
    };
}

} // namespace batbox::mcp
