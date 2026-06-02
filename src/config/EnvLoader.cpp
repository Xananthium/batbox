// src/config/EnvLoader.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::load_env_file and merge helpers.
// ---------------------------------------------------------------------------

#include <batbox/config/EnvLoader.hpp>
#include <batbox/core/Paths.hpp>

#include <cstdlib>   // getenv, environ
#include <cstring>   // strerror
#include <cerrno>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

// On POSIX, 'environ' gives us the full environment block.
extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace batbox::config {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

/// Strip leading and trailing ASCII whitespace (space, tab, CR) from s.
static std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view kWS{" \t\r"};
    const auto start = s.find_first_not_of(kWS);
    if (start == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(kWS);
    return s.substr(start, end - start + 1);
}

/// Perform ${VAR} substitution in 'value' using process env.
/// Unknown variables are replaced with an empty string.
static std::string substitute_vars(std::string_view value) {
    std::string result;
    result.reserve(value.size());

    std::size_t i = 0;
    while (i < value.size()) {
        if (value[i] == '$' && i + 1 < value.size() && value[i + 1] == '{') {
            const auto close = value.find('}', i + 2);
            if (close != std::string_view::npos) {
                const auto var_name = value.substr(i + 2, close - (i + 2));
                const char* env_val = std::getenv(std::string(var_name).c_str());
                if (env_val) result += env_val;
                i = close + 1;
            } else {
                // Unmatched '{' — emit literally
                result += value[i];
                ++i;
            }
        } else {
            result += value[i];
            ++i;
        }
    }
    return result;
}

/// Unescape a double-quoted value (content between the outer double-quotes).
/// Supported escape sequences: \\  \"  \n  \t  \r
/// On an unrecognised \X sequence, returns nullopt to signal parse error.
static std::optional<std::string> unescape_double_quoted(std::string_view content) {
    std::string out;
    out.reserve(content.size());
    std::size_t i = 0;
    while (i < content.size()) {
        if (content[i] == '\\') {
            if (i + 1 >= content.size()) {
                // Trailing backslash — treat as literal backslash
                out += '\\';
                ++i;
            } else {
                ++i;
                switch (content[i]) {
                    case '\\': out += '\\'; break;
                    case '"':  out += '"';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case '$':  out += '$';  break;  // escape to prevent var subst
                    default:
                        // Unrecognised escape — signal error
                        return std::nullopt;
                }
                ++i;
            }
        } else {
            out += content[i];
            ++i;
        }
    }
    return out;
}

/// Parse a single .env line into (key, value).
/// Returns false (and logs to stderr with line_number) on malformed input.
/// Returns true with key=="" to indicate the line should be silently skipped
/// (empty, comment, blank).
static bool parse_line(std::string_view raw_line,
                        int line_number,
                        std::string_view file_path,
                        std::string& out_key,
                        std::string& out_value) {
    out_key.clear();
    out_value.clear();

    std::string_view line = trim(raw_line);

    // Skip empty lines and comment lines.
    if (line.empty() || line[0] == '#') return true;  // key=="" → skip

    // Strip optional 'export ' prefix (shell compatibility).
    if (line.size() >= 7 && line.substr(0, 7) == "export ") {
        line = trim(line.substr(7));
    }

    // Find the '=' separator.
    const auto eq_pos = line.find('=');
    if (eq_pos == std::string_view::npos) {
        std::cerr << file_path << ":" << line_number
                  << ": warning: missing '=' in env line, skipping: \""
                  << line << "\"\n";
        return false;
    }

    // Extract key (trimmed).
    out_key = std::string(trim(line.substr(0, eq_pos)));
    if (out_key.empty()) {
        std::cerr << file_path << ":" << line_number
                  << ": warning: empty key, skipping\n";
        return false;
    }

    // Extract raw value part (after '=').
    std::string_view raw_value = line.substr(eq_pos + 1);

    // Determine quoting style.
    if (raw_value.size() >= 2 && raw_value.front() == '"' && raw_value.back() == '"') {
        // Double-quoted value: unescape, then substitute ${VAR}.
        const std::string_view inner = raw_value.substr(1, raw_value.size() - 2);
        auto unescaped = unescape_double_quoted(inner);
        if (!unescaped) {
            std::cerr << file_path << ":" << line_number
                      << ": warning: invalid escape sequence in double-quoted value for key '"
                      << out_key << "', skipping\n";
            out_key.clear();
            return false;
        }
        out_value = substitute_vars(*unescaped);
    } else if (raw_value.size() >= 2 && raw_value.front() == '\'' && raw_value.back() == '\'') {
        // Single-quoted value: literal, no escaping, no substitution.
        out_value = std::string(raw_value.substr(1, raw_value.size() - 2));
    } else {
        // Unquoted value: strip trailing whitespace, then tilde expand + var substitute.
        std::string_view trimmed_val = trim(raw_value);

        // Strip inline comment (# not preceded by whitespace is part of the value,
        // but a space + # sequence starts a comment).
        {
            std::size_t cpos = 0;
            bool in_found = false;
            while (cpos < trimmed_val.size()) {
                if (trimmed_val[cpos] == '#' && cpos > 0 &&
                    (trimmed_val[cpos - 1] == ' ' || trimmed_val[cpos - 1] == '\t')) {
                    // Trim from this position back, removing the trailing space too.
                    trimmed_val = trim(trimmed_val.substr(0, cpos));
                    in_found = true;
                    break;
                }
                ++cpos;
            }
            (void)in_found;
        }

        std::string working = std::string(trimmed_val);

        // Tilde expansion for unquoted values.
        if (!working.empty() && working[0] == '~') {
            const auto expanded = batbox::paths::expand_tilde(working);
            working = expanded.string();
        }

        // Variable substitution.
        out_value = substitute_vars(working);
    }

    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// load_env_file()
// ---------------------------------------------------------------------------
[[nodiscard]]
batbox::Result<EnvMap, std::string>
load_env_file(std::filesystem::path env_file) {
    std::ifstream file(env_file);
    if (!file.is_open()) {
        std::string msg = "EnvLoader: cannot open '";
        msg += env_file.string();
        msg += "': ";
        msg += std::strerror(errno);
        return batbox::Err(std::move(msg));
    }

    EnvMap result;
    std::string raw_line;
    int line_number = 0;
    bool first_line = true;

    while (std::getline(file, raw_line)) {
        ++line_number;

        // Strip UTF-8 BOM from the very first line (EF BB BF).
        if (first_line && raw_line.size() >= 3 &&
            static_cast<unsigned char>(raw_line[0]) == 0xEF &&
            static_cast<unsigned char>(raw_line[1]) == 0xBB &&
            static_cast<unsigned char>(raw_line[2]) == 0xBF) {
            raw_line.erase(0, 3);
        }
        first_line = false;

        std::string key, value;
        const bool parsed = parse_line(raw_line, line_number,
                                       env_file.string(), key, value);

        // parsed==true && key empty → blank/comment line: skip silently.
        // parsed==false → malformed: already logged, skip.
        if (parsed && !key.empty()) {
            result[std::move(key)] = std::move(value);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// merge_with_process_env()
// ---------------------------------------------------------------------------
void merge_with_process_env(EnvMap& map, bool process_env_wins) {
    if (environ == nullptr) return;

    for (char** ep = environ; *ep != nullptr; ++ep) {
        const std::string_view entry(*ep);
        const auto eq = entry.find('=');
        if (eq == std::string_view::npos) continue;

        std::string proc_key = std::string(entry.substr(0, eq));
        std::string proc_val = std::string(entry.substr(eq + 1));

        if (process_env_wins) {
            // Process env wins: overwrite whatever the file had (or insert).
            map[std::move(proc_key)] = std::move(proc_val);
        } else {
            // File wins: only insert keys not already present in the map.
            map.emplace(std::move(proc_key), std::move(proc_val));
        }
    }
}

// ---------------------------------------------------------------------------
// get()
// ---------------------------------------------------------------------------
[[nodiscard]]
std::string get(const EnvMap& map,
                std::string_view key,
                std::string_view default_value) {
    const auto it = map.find(std::string(key));
    if (it == map.end()) return std::string(default_value);
    return it->second;
}

} // namespace batbox::config
