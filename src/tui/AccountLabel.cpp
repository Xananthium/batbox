// src/tui/AccountLabel.cpp
// ---------------------------------------------------------------------------
// batbox::util::resolve_account_label — TUI-FIX-T8
// See include/batbox/util/AccountLabel.hpp for the contract.
// ---------------------------------------------------------------------------

#include "batbox/util/AccountLabel.hpp"

#include <cstring>
#include <cstdlib>    // std::getenv
#include <optional>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>   // gethostname on Windows
#  pragma comment(lib, "Ws2_32.lib")
#else
#  include <unistd.h>     // gethostname, getuid
#  include <pwd.h>        // getpwuid_r
#  include <sys/types.h>
#endif

namespace batbox::util {

std::string resolve_account_label(const std::optional<std::string>& configured_account)
{
    // ── 1. Prefer explicitly configured account ───────────────────────────
    if (configured_account.has_value() && !configured_account->empty()) {
        return *configured_account;
    }

    // ── 2. Derive username ────────────────────────────────────────────────
    std::string username;

#ifdef _WIN32
    // On Windows use USERNAME env var; no getpwuid equivalent needed.
    const char* win_user = std::getenv("USERNAME");
    if (win_user && win_user[0] != '\0') {
        username = win_user;
    } else {
        username = "user";
    }
#else
    const char* env_user = std::getenv("USER");
    if (env_user && env_user[0] != '\0') {
        username = env_user;
    } else {
        // $USER is unset — fall back to passwd database.
        // Use getpwuid_r (re-entrant) rather than getpwuid to be thread-safe.
        char buf[1024] = {};
        struct passwd pw_storage {};
        struct passwd* pw_result = nullptr;
        if (::getpwuid_r(::getuid(), &pw_storage, buf, sizeof(buf), &pw_result) == 0
                && pw_result != nullptr
                && pw_result->pw_name != nullptr
                && pw_result->pw_name[0] != '\0') {
            username = pw_result->pw_name;
        } else {
            username = "user";
        }
    }
#endif

    // ── 3. Derive hostname ────────────────────────────────────────────────
    char host[256] = {};
    if (::gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') {
        std::strcpy(host, "localhost");
    }
    // Strip any trailing domain suffix for brevity (keep only the first label).
    // e.g. "my-mac.local" → "my-mac"
    char* dot = std::strchr(host, '.');
    if (dot != nullptr) {
        *dot = '\0';
    }

    return username + "@" + host;
}

} // namespace batbox::util
