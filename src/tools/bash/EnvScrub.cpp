// src/tools/bash/EnvScrub.cpp
//
// batbox::tools::bash::EnvScrub — environment variable scrubbing.
//
// Blueprint contract: CPP 5.8 — EnvScrub (file symbol, src/tools/bash/EnvScrub.cpp)
// Pseudocode: allowlist env scrub: keep PATH,HOME,USER,LANG,TERM,SHELL; clear rest
//
// Design:
//   scrub_env(allowlist) reads the calling process's `environ` array and returns
//   a new environment containing only entries whose names appear in the allowlist.
//   If the caller passes an empty allowlist, the default set is used:
//     {PATH, HOME, USER, LANG, TERM, SHELL}
//
//   Secret keys (BATBOX_API_KEY, ANTHROPIC_API_KEY, OPENAI_API_KEY, etc.) are
//   never carried into the child even if somehow listed in the allowlist — they
//   are in a hard-coded deny-list that overrides the allowlist.
//
// Return value:
//   A vector of "KEY=VALUE" strings suitable for passing to execvpe().
//   An auxiliary null-terminated pointer array is built by PtyBackend right
//   before execvp.

#include "EnvScrubInternal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern char** environ; // POSIX

namespace batbox::tools::bash {

// ---------------------------------------------------------------------------
// Hard-coded secrets deny list — these are NEVER carried into the child,
// regardless of what the caller puts in env_allowlist.
// ---------------------------------------------------------------------------
static const std::unordered_set<std::string>& secrets_denylist() {
    static const std::unordered_set<std::string> kDeny{
        "BATBOX_API_KEY",
        "ANTHROPIC_API_KEY",
        "OPENAI_API_KEY",
        "CLAUDE_API_KEY",
        "GEMINI_API_KEY",
        "COHERE_API_KEY",
        "MISTRAL_API_KEY",
        "HUGGINGFACE_TOKEN",
        "AWS_SECRET_ACCESS_KEY",
        "AWS_SESSION_TOKEN",
        "GITHUB_TOKEN",
        "GH_TOKEN",
        "NPM_TOKEN",
        "PYPI_TOKEN",
        "DATABASE_URL",           // may contain embedded credentials
        "REDIS_URL",
        "MONGODB_URI",
    };
    return kDeny;
}

// ---------------------------------------------------------------------------
// Default allowlist used when the caller passes an empty vector.
// ---------------------------------------------------------------------------
static const std::vector<std::string>& default_allowlist() {
    static const std::vector<std::string> kAllow{
        "PATH",
        "HOME",
        "USER",
        "LOGNAME",
        "LANG",
        "LC_ALL",
        "LC_CTYPE",
        "TERM",
        "COLORTERM",
        "SHELL",
        "TMPDIR",
        "TMP",
        "TEMP",
    };
    return kAllow;
}

// ---------------------------------------------------------------------------
// scrub_env()
// ---------------------------------------------------------------------------
std::vector<std::string> scrub_env(const std::vector<std::string>& allowlist) {
    // Build the effective allowlist — use default if caller passed empty.
    const std::vector<std::string>& effective =
        allowlist.empty() ? default_allowlist() : allowlist;

    std::unordered_set<std::string> allow_set(effective.begin(), effective.end());

    const auto& deny = secrets_denylist();

    std::vector<std::string> result;
    result.reserve(allow_set.size());

    for (char** ep = environ; ep && *ep; ++ep) {
        std::string_view entry(*ep);
        auto eq = entry.find('=');
        if (eq == std::string_view::npos) continue;

        std::string key(entry.substr(0, eq));

        // Never carry secrets even if caller explicitly listed them.
        if (deny.count(key)) continue;

        if (allow_set.count(key)) {
            result.emplace_back(entry);
        }
    }

    // Ensure TERM is set to something sane for pty children even if the
    // caller's allowlist didn't include it or the parent TERM is unset.
    bool has_term = false;
    for (const auto& kv : result) {
        if (kv.size() >= 5 && kv.substr(0, 5) == "TERM=") {
            has_term = true;
            break;
        }
    }
    if (!has_term) {
        result.emplace_back("TERM=xterm-256color");
    }

    return result;
}

} // namespace batbox::tools::bash
