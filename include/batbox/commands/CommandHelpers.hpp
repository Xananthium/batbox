#pragma once
// include/batbox/commands/CommandHelpers.hpp
//
// Shared free-function helpers for src/commands/*.cpp files.
// All helpers are pure (no I/O, no global state) or well-bounded (run_git_command).
// Included directly by .cpp files; not part of the public API.
//
// Design: free functions over data. No class. No state.

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#ifndef _WIN32
#  include <sys/wait.h>
#endif

#include <batbox/core/Result.hpp>

namespace batbox::commands {

/// Strip leading and trailing ASCII whitespace.
[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Split `s` at the first whitespace boundary.
/// Returns {first_word, remainder_after_whitespace}.
[[nodiscard]] inline std::pair<std::string_view, std::string_view>
split_first(std::string_view s) noexcept {
    const auto space = s.find_first_of(" \t");
    if (space == std::string_view::npos) return {s, {}};
    const auto rest_start = s.find_first_not_of(" \t", space);
    const std::string_view rest =
        (rest_start == std::string_view::npos) ? std::string_view{} : s.substr(rest_start);
    return {s.substr(0, space), rest};
}

/// Run a shell command via popen, capturing combined stdout+stderr.
/// Sets exit_code to the process exit status. Returns Err on popen failure.
/// Caps output at 1 MiB.
[[nodiscard]] inline batbox::Result<std::string> run_git_command(
        const std::string& cmd,
        int& exit_code)
{
    const std::string full_cmd = cmd + " 2>&1";
    FILE* fp = ::popen(full_cmd.c_str(), "r");
    if (!fp) {
        return batbox::Err(std::string("popen failed: ") + std::strerror(errno));
    }
    std::string output;
    output.reserve(4096);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        output += buf.data();
        if (output.size() >= 1024 * 1024) {
            output += "\n[diff output truncated at 1 MiB]\n";
            break;
        }
    }
    const int status = ::pclose(fp);
#ifdef _WIN32
    exit_code = status;
#else
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; // NOLINT
#endif
    return output;
}

/// Detect whether popen output contains a "not a git repository" message.
[[nodiscard]] inline bool is_not_a_git_repo(const std::string& output, int exit_code) noexcept {
    auto ci_find = [&](std::string_view needle) -> bool {
        std::string lo = output, ln(needle);
        for (char& c : lo) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        for (char& c : ln) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return lo.find(ln) != std::string::npos;
    };
    if (ci_find("not a git repository")) return true;
    if (ci_find("fatal:"))              return true;
    if (exit_code == 128 || exit_code == 129) return true;
    return false;
}

} // namespace batbox::commands
