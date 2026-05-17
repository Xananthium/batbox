// include/batbox/mcp/McpFraming.hpp
// ---------------------------------------------------------------------------
// MCP stdio-transport framing — LSP Content-Length protocol
//
// The MCP spec uses the same wire framing as LSP (Language Server Protocol):
//
//   Content-Length: <N>\r\n
//   \r\n
//   <N bytes of UTF-8 JSON>
//
// An optional Content-Type header may appear between Content-Length and the
// blank separator line; this implementation accepts it and ignores it on
// decode (the spec mandates JSON so the type is always application/json).
//
// Design
// ------
// FrameWriter  — stateless; encodes a single JSON payload into wire bytes.
//               frame_message() is the free-function convenience wrapper that
//               matches the blueprint contract.
//
// FrameReader  — stateful streaming decoder.  Callers feed raw bytes as they
//               arrive (from a pipe, socket, or any stream) via feed().  Each
//               call returns a (possibly empty) vector of complete JSON payloads
//               that were fully received.  Partial frames are buffered
//               internally and completed across subsequent feed() calls.
//               Malformed frames (missing/invalid Content-Length, body shorter
//               than declared) are discarded and queued in errors() so callers
//               can log them without interrupting the message stream.
//
// Build standalone (from repo root):
//   c++ -std=c++20 -I include \
//       tests/unit/test_mcp_framing.cpp src/mcp/McpFraming.cpp \
//       -o /tmp/test_mcp_framing && /tmp/test_mcp_framing
//
// With vcpkg doctest:
//   c++ -std=c++20 -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_mcp_framing.cpp src/mcp/McpFraming.cpp \
//       -o /tmp/test_mcp_framing && /tmp/test_mcp_framing
// ---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::mcp {

// ============================================================================
// FrameWriter — encode JSON payloads into wire frames
// ============================================================================

/// Stateless encoder that wraps a JSON payload in the LSP Content-Length frame.
///
/// Usage:
///   FrameWriter writer;
///   std::string wire = writer.encode(json_string);
///   // write wire to the transport
class FrameWriter {
public:
    FrameWriter() = default;

    /// Encode a JSON payload as a complete wire frame.
    ///
    /// Produces:
    ///   "Content-Length: <N>\r\n\r\n<payload>"
    /// where N is the byte length of json_payload (UTF-8 encoded).
    ///
    /// @param json_payload  The JSON text to frame.  Need not be null-terminated.
    /// @return              Wire-format bytes ready to write to the transport.
    [[nodiscard]] std::string encode(std::string_view json_payload) const;
};

// ============================================================================
// FrameReader — decode a byte stream into complete JSON payloads
// ============================================================================

/// Stateful streaming decoder for the LSP Content-Length framing protocol.
///
/// Thread safety: not thread-safe.  Use one FrameReader per thread or protect
/// externally.
///
/// Usage:
///   FrameReader reader;
///   // In your read loop:
///   auto messages = reader.feed(chunk);
///   for (auto& msg : messages) {
///       process(msg);
///   }
///   if (!reader.errors().empty()) {
///       for (auto& e : reader.errors()) log_warn(e);
///       reader.clear_errors();
///   }
class FrameReader {
public:
    FrameReader() = default;

    // Non-copyable (owns mutable buffer state).
    FrameReader(const FrameReader&)            = delete;
    FrameReader& operator=(const FrameReader&) = delete;
    FrameReader(FrameReader&&)                 = default;
    FrameReader& operator=(FrameReader&&)      = default;

    /// Feed raw bytes from the transport into the decoder.
    ///
    /// Returns all complete JSON payloads that became available after appending
    /// chunk to the internal buffer.  The vector may be empty (partial frame
    /// received) or contain multiple entries (several frames arrived together).
    ///
    /// Malformed frames (missing Content-Length, non-numeric length, body too
    /// short after the declared length) are skipped: the decoder attempts to
    /// resync by scanning forward for the next "Content-Length:" token, and the
    /// error description is pushed into the errors queue.
    ///
    /// @param chunk  Bytes received from the transport in this read call.
    /// @return       Complete JSON payloads, in order of arrival.
    [[nodiscard]] std::vector<std::string> feed(std::string_view chunk);

    /// Pending error descriptions accumulated since the last clear_errors() call.
    ///
    /// Does not throw; returns an empty vector when no errors occurred.
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept;

    /// Discard all accumulated error descriptions.
    void clear_errors() noexcept;

    /// Discard all buffered bytes and error state, returning to the initial
    /// empty state.  Useful when restarting a transport connection.
    void reset() noexcept;

    /// Number of bytes currently sitting in the internal buffer waiting for
    /// more data to complete the current frame.  Primarily useful for tests
    /// and diagnostics.
    [[nodiscard]] std::size_t pending_bytes() const noexcept;

private:
    // ----- internal state ----------------------------------------------------

    // Accumulates bytes across feed() calls.  We hold the raw byte stream here
    // and slice complete frames out as they become available.
    std::string buffer_;

    // Error messages accumulated from malformed frames.
    std::vector<std::string> errors_;

    // ----- helpers -----------------------------------------------------------

    // Try to extract one complete frame from buffer_, consuming its bytes.
    // Returns the JSON body on success, std::nullopt when the buffer does not
    // yet hold a complete frame.  Throws a std::runtime_error (caught by feed())
    // when the header is present but malformed.
    struct ParseResult {
        enum class Tag { kComplete, kIncomplete, kMalformed };
        Tag         tag;
        std::string payload;    // valid when tag == kComplete
        std::string error;      // valid when tag == kMalformed
        std::size_t consumed;   // bytes to erase from buffer_; valid for kComplete and kMalformed
    };

    [[nodiscard]] ParseResult try_parse_one() const;
};

// ============================================================================
// Free-function convenience wrapper (blueprint contract symbol)
// ============================================================================

/// Encode a JSON object as a wire frame.
///
/// This is the blueprint-contract entry point.  Internally delegates to
/// FrameWriter::encode().
///
/// @param msg  nlohmann::json or any type that serialises to a std::string via
///             msg.dump().  To keep the header free of nlohmann, the overload
///             accepting a pre-serialised string is provided below.
///
/// Usage with pre-serialised JSON (most common from transport layer):
///   std::string wire = batbox::mcp::frame_message(json_string);
[[nodiscard]] inline std::string frame_message(std::string_view json_string) {
    return FrameWriter{}.encode(json_string);
}

} // namespace batbox::mcp
