// src/session/SessionFile.cpp
// =============================================================================
// Implementation of batbox::session::SessionFile I/O.
//
// On-disk format invariant (required for append_message on plain .json files):
//   The file ALWAYS ends with exactly "]\n}" — the closing of the messages
//   array followed by a newline then the closing brace of the top-level object.
//
//   Example skeleton (write_initial with empty messages):
//     {"id":"...","created_at":"...","updated_at":"...","model_at_start":"...",
//      "working_dir":"...","tool_calls_summary":{},"usage_total":{...},
//      "permission_rules_used":[...],"messages":[
//     ]\n}
//
//   After one append:
//     ...,"messages":[
//     {"role":"user","content":"hi"}
//     ]\n}
//
//   The "]\n}" terminator is the crash-safe anchor: append writes rebuild the
//   file atomically, replacing the ']' onwards with the new message.
//
//   For .json.gz files: append_message decompresses to the full session,
//   adds the message in-memory, and recompresses atomically.
//
// Dependencies:
//   - nlohmann/json   (Json.hpp type alias)
//   - zlib            (gzip compress / decompress)
//   - POSIX           (fsync, ftruncate via <unistd.h>)
//   - C++ std         (<filesystem>, <chrono>, <fstream>, …)
// =============================================================================

#include <batbox/session/SessionFile.hpp>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// POSIX headers for fsync / ftruncate / open / close
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace batbox::session {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

// ---------------------------------------------------------------------------
// ISO 8601 UTC time_point ↔ string
// ---------------------------------------------------------------------------

static std::string tp_to_iso8601(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

static std::chrono::system_clock::time_point iso8601_to_tp(const std::string& s) {
    struct tm tm_buf{};
    if (strptime(s.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == nullptr) {
        return std::chrono::system_clock::from_time_t(0);
    }
    return std::chrono::system_clock::from_time_t(timegm(&tm_buf));
}

// ---------------------------------------------------------------------------
// Write bytes to file atomically (temp + rename)
// ---------------------------------------------------------------------------
static Result<void> write_atomic(const std::filesystem::path& dest,
                                  const std::string& data) {
    auto tmp = dest;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            return Err("cannot open temp file for write: " + tmp.string());
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            return Err("write failure: " + tmp.string());
        }
        f.flush();
    }

    // fsync before rename so data is durable on crash.
    {
        int fd = ::open(tmp.c_str(), O_WRONLY);
        if (fd != -1) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return Err("rename failed: " + dest.string());
    }
    return {};
}

// ---------------------------------------------------------------------------
// gzip compress / decompress using zlib
// ---------------------------------------------------------------------------

static Result<std::vector<unsigned char>>
gzip_compress(const std::string& input) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
                     15 + 16,  // gzip header
                     8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return Err(std::string("deflateInit2 failed"));
    }

    zs.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::vector<unsigned char> out;
    out.reserve(input.size() / 2 + 256);

    std::array<unsigned char, 32768> chunk{};
    int ret = Z_OK;
    do {
        zs.next_out  = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        ret = deflate(&zs, Z_FINISH);
        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            return Err(std::string("deflate stream error"));
        }
        std::size_t produced = chunk.size() - zs.avail_out;
        out.insert(out.end(), chunk.data(), chunk.data() + produced);
    } while (ret != Z_STREAM_END);

    deflateEnd(&zs);
    return out;
}

static Result<std::string> gzip_decompress(const std::vector<unsigned char>& input) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {
        return Err(std::string("inflateInit2 failed"));
    }

    zs.next_in  = const_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    out.reserve(input.size() * 3);

    std::array<char, 32768> chunk{};
    int ret = Z_OK;
    do {
        zs.next_out  = reinterpret_cast<Bytef*>(chunk.data());
        zs.avail_out = static_cast<uInt>(chunk.size());
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&zs);
            return Err(std::string("inflate error: ") + zError(ret));
        }
        std::size_t produced = chunk.size() - zs.avail_out;
        out.append(chunk.data(), produced);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return out;
}

// ---------------------------------------------------------------------------
// Canonical on-disk rendering
//
// Writes the session in a specific format that ALWAYS ends with "]\n}":
//
//   <header_without_messages_end>,"messages":[
//   <msg0>,<msg1>,...
//   ]\n}
//
// This guarantees the "]\n}" crash-safe anchor at the end.
// ---------------------------------------------------------------------------
static std::string render_canonical(const SessionFile& sf) {
    // Build the header object without the messages array.
    Json header;
    header["id"]                   = sf.id.to_string();
    header["created_at"]           = tp_to_iso8601(sf.created_at);
    header["updated_at"]           = tp_to_iso8601(sf.updated_at);
    header["model_at_start"]       = sf.model_at_start;
    header["working_dir"]          = sf.working_dir.string();
    // DIS-1020 — only emitted when set, so main-session files are unchanged.
    if (!sf.agent_id.empty())      header["agent_id"] = sf.agent_id;
    if (sf.endpoint.is_object())   header["endpoint"] = sf.endpoint;
    header["tool_calls_summary"]   = sf.tool_calls_summary.is_null()
                                         ? Json::object()
                                         : sf.tool_calls_summary;
    header["usage_total"]          = sf.usage_total.to_json();
    header["permission_rules_used"] = sf.permission_rules_used;

    // Render: strip trailing '}' and append ,"messages":[
    std::string hdr = header.dump();
    hdr.resize(hdr.size() - 1);
    hdr += ",\"messages\":[";

    // Append each message compact on one combined line.
    if (!sf.messages.empty()) {
        hdr += '\n';
        for (std::size_t i = 0; i < sf.messages.size(); ++i) {
            if (i > 0) hdr += ',';
            hdr += sf.messages[i].dump();
        }
    }
    // Close with "\n]\n}"
    hdr += "\n]\n}";
    return hdr;
}

// ---------------------------------------------------------------------------
// Read entire file into a std::string
// ---------------------------------------------------------------------------
static Result<std::string> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return Err("cannot open file: " + p.string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        return Err("read error: " + p.string());
    }
    return ss.str();
}

// ---------------------------------------------------------------------------
// Decompress a .json.gz file to a plain JSON string.
// Returns the raw JSON string.
// ---------------------------------------------------------------------------
static Result<std::string> decompress_gz_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) {
        return Err("cannot open .json.gz: " + p.string());
    }
    std::vector<unsigned char> compressed(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
    if (f.bad()) {
        return Err("read error: " + p.string());
    }
    return gzip_decompress(compressed);
}

// ---------------------------------------------------------------------------
// Append a message to the in-memory canonical JSON string.
// Returns the new complete file content.
// ---------------------------------------------------------------------------
static Result<std::string>
append_to_content(const std::string& content, const Json& message) {
    const std::string marker = "]\n}";
    auto marker_pos = content.rfind(marker);
    if (marker_pos == std::string::npos) {
        return Err(std::string("cannot find closing marker ']\n}' in session content"));
    }

    std::size_t bracket_pos = marker_pos;

    // Detect empty messages array: char before ']' is '\n' and char before
    // that is '[' — i.e., the messages array contains only whitespace.
    bool is_first_message = false;
    if (bracket_pos >= 2 &&
        content[bracket_pos - 1] == '\n' &&
        content[bracket_pos - 2] == '[') {
        is_first_message = true;
    }

    std::string msg_str = message.dump();

    std::string new_file;
    new_file.reserve(bracket_pos + msg_str.size() + 8);
    new_file.append(content, 0, bracket_pos);  // everything before ']'
    if (!is_first_message) new_file += ',';
    new_file += msg_str;
    new_file += "\n]\n}";
    return new_file;
}

} // anonymous namespace

// =============================================================================
// UsageTotal
// =============================================================================

Json UsageTotal::to_json() const {
    return Json{
        {"prompt_tokens",     prompt_tokens},
        {"completion_tokens", completion_tokens}
    };
}

UsageTotal UsageTotal::from_json(const Json& j) {
    UsageTotal u;
    if (j.is_object()) {
        if (auto it = j.find("prompt_tokens"); it != j.end() && it->is_number())
            u.prompt_tokens = it->get<long long>();
        if (auto it = j.find("completion_tokens"); it != j.end() && it->is_number())
            u.completion_tokens = it->get<long long>();
    }
    return u;
}

// =============================================================================
// SessionFile
// =============================================================================

Json SessionFile::to_json() const {
    Json j;
    j["id"]                   = id.to_string();
    j["created_at"]           = tp_to_iso8601(created_at);
    j["updated_at"]           = tp_to_iso8601(updated_at);
    j["model_at_start"]       = model_at_start;
    j["working_dir"]          = working_dir.string();
    // DIS-1020 — only emitted when set (main session leaves them absent).
    if (!agent_id.empty())    j["agent_id"] = agent_id;
    if (endpoint.is_object()) j["endpoint"] = endpoint;
    j["messages"]             = messages;
    j["tool_calls_summary"]   = tool_calls_summary.is_null()
                                    ? Json::object()
                                    : tool_calls_summary;
    j["usage_total"]          = usage_total.to_json();
    j["permission_rules_used"] = permission_rules_used;
    return j;
}

Result<SessionFile> SessionFile::from_json(const Json& j) {
    if (!j.is_object()) {
        return Err(std::string("session JSON is not an object"));
    }

    SessionFile sf;

    // id — required
    auto id_it = j.find("id");
    if (id_it == j.end() || !id_it->is_string()) {
        return Err(std::string("session JSON missing 'id' field"));
    }
    auto parsed_id = Uuid::parse(id_it->get<std::string>());
    if (!parsed_id.has_value()) {
        return Err(std::string("session JSON 'id' is not a valid UUID"));
    }
    sf.id = *parsed_id;

    if (auto it = j.find("created_at"); it != j.end() && it->is_string())
        sf.created_at = iso8601_to_tp(it->get<std::string>());
    if (auto it = j.find("updated_at"); it != j.end() && it->is_string())
        sf.updated_at = iso8601_to_tp(it->get<std::string>());

    if (auto it = j.find("model_at_start"); it != j.end() && it->is_string())
        sf.model_at_start = it->get<std::string>();

    if (auto it = j.find("working_dir"); it != j.end() && it->is_string())
        sf.working_dir = it->get<std::string>();

    // DIS-1020 — soft reads (absent in legacy files → defaults stay empty/null).
    if (auto it = j.find("agent_id"); it != j.end() && it->is_string())
        sf.agent_id = it->get<std::string>();

    if (auto it = j.find("endpoint"); it != j.end() && it->is_object())
        sf.endpoint = *it;

    if (auto it = j.find("messages"); it != j.end() && it->is_array())
        sf.messages = it->get<std::vector<Json>>();

    if (auto it = j.find("tool_calls_summary"); it != j.end() && it->is_object())
        sf.tool_calls_summary = *it;

    if (auto it = j.find("usage_total"); it != j.end() && it->is_object())
        sf.usage_total = UsageTotal::from_json(*it);

    if (auto it = j.find("permission_rules_used"); it != j.end() && it->is_array())
        sf.permission_rules_used = it->get<std::vector<Json>>();

    return sf;
}

// =============================================================================
// write_initial
// =============================================================================

Result<void> write_initial(const std::filesystem::path& path,
                            const SessionFile& sf) {
    std::error_code ec;
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return Err("cannot create session directory: " + parent.string());
        }
    }

    std::string canonical = render_canonical(sf);
    return write_atomic(path, canonical);
}

// =============================================================================
// append_message
// =============================================================================

Result<void> append_message(const std::filesystem::path& path,
                             const Json& message,
                             std::filesystem::path* path_out) {
    if (path_out) *path_out = path;

    if (!std::filesystem::exists(path)) {
        return Err("session file does not exist: " + path.string());
    }

    const auto ext = path.extension().string();
    const bool is_gz = (ext == ".gz");

    if (is_gz) {
        // -----------------------------------------------------------------------
        // Compressed path: decompress → append in-memory → recompress atomically
        // -----------------------------------------------------------------------
        auto decomp_res = decompress_gz_file(path);
        if (!decomp_res) {
            return Err("decompress failed for append: " + decomp_res.error());
        }

        auto new_content_res = append_to_content(*decomp_res, message);
        if (!new_content_res) {
            return Err(new_content_res.error());
        }

        // Recompress and write atomically back to the same .json.gz file.
        auto compressed = gzip_compress(*new_content_res);
        if (!compressed) {
            return Err("gzip compress failed: " + compressed.error());
        }
        std::string gz_bytes(reinterpret_cast<const char*>(compressed->data()),
                             compressed->size());
        auto write_res = write_atomic(path, gz_bytes);
        if (!write_res) {
            return write_res;
        }
        // path_out stays as path (still .json.gz)
        return {};
    }

    // -------------------------------------------------------------------------
    // Plain .json path: fast in-memory string splice + atomic rewrite
    // -------------------------------------------------------------------------
    auto read_res = read_file_bytes(path);
    if (!read_res) {
        return Err(read_res.error());
    }

    auto new_content_res = append_to_content(*read_res, message);
    if (!new_content_res) {
        return Err(new_content_res.error());
    }

    // Write atomically.
    auto write_res = write_atomic(path, *new_content_res);
    if (!write_res) {
        return write_res;
    }

    // After append, check if we need to gzip.
    std::error_code ec;
    auto file_size = std::filesystem::file_size(path, ec);
    if (!ec && file_size >= GZIP_THRESHOLD_BYTES) {
        // Parse the new content to get a SessionFile, then compress.
        auto parse_res = batbox::parse(*new_content_res);
        if (parse_res) {
            auto sf_res = SessionFile::from_json(*parse_res);
            if (sf_res) {
                auto gz_res = save_compressed(path, *sf_res);
                if (gz_res && path_out) {
                    *path_out = *gz_res;
                }
            }
        }
    }

    return {};
}

// =============================================================================
// read_session_file
// =============================================================================

Result<SessionFile> read_session_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Err("session file not found: " + path.string());
    }

    std::string raw;
    const auto ext = path.extension().string();
    const bool is_gz = (ext == ".gz");

    if (is_gz) {
        auto decomp = decompress_gz_file(path);
        if (!decomp) {
            return Err("gzip decompress failed: " + decomp.error());
        }
        raw = std::move(*decomp);
    } else {
        auto read_res = read_file_bytes(path);
        if (!read_res) {
            return Err(read_res.error());
        }
        raw = std::move(*read_res);
    }

    // Crash recovery: truncate to the last valid "]\n}" if garbage follows.
    {
        const std::string marker = "]\n}";
        std::size_t last_ok = raw.rfind(marker);
        if (last_ok != std::string::npos) {
            std::size_t proper_end = last_ok + marker.size();
            if (proper_end < raw.size()) {
                raw.resize(proper_end);

                // Patch the on-disk file for plain .json files.
                if (!is_gz) {
                    int fd = ::open(path.c_str(), O_WRONLY);
                    if (fd != -1) {
                        ::ftruncate(fd, static_cast<off_t>(proper_end));
                        ::fsync(fd);
                        ::close(fd);
                    }
                }
            }
        }
    }

    auto parse_res = batbox::parse(raw);
    if (!parse_res) {
        return Err("JSON parse error in session file: " + parse_res.error());
    }

    return SessionFile::from_json(*parse_res);
}

// =============================================================================
// save_compressed
// =============================================================================

Result<std::filesystem::path>
save_compressed(const std::filesystem::path& path, const SessionFile& sf) {
    std::filesystem::path gz_path;
    const auto ext = path.extension().string();
    if (ext == ".gz") {
        gz_path = path;  // rewrite in place
    } else {
        gz_path = path;
        gz_path += ".gz";  // uuid.json → uuid.json.gz
    }

    std::string canonical = render_canonical(sf);
    auto compressed = gzip_compress(canonical);
    if (!compressed) {
        return Err("gzip compress failed: " + compressed.error());
    }

    std::string gz_as_str(reinterpret_cast<const char*>(compressed->data()),
                          compressed->size());
    auto write_res = write_atomic(gz_path, gz_as_str);
    if (!write_res) {
        return Err("write compressed failed: " + write_res.error());
    }

    if (path != gz_path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    return gz_path;
}

} // namespace batbox::session
