// src/core/Paths.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox/core/Paths.hpp filesystem helpers.
// ---------------------------------------------------------------------------

#include "batbox/core/Paths.hpp"

#include <cstdlib>      // std::getenv
#include <stdexcept>    // std::runtime_error
#include <string>

// POSIX-only headers (macOS + Linux)
#include <pwd.h>        // struct passwd, getpwuid_r
#include <sys/types.h>  // uid_t
#include <unistd.h>     // getuid

namespace batbox::paths {

// ---------------------------------------------------------------------------
// home_dir()
// ---------------------------------------------------------------------------
std::filesystem::path home_dir() {
    // 1. Prefer $HOME environment variable.
    const char* env_home = std::getenv("HOME");
    if (env_home != nullptr && env_home[0] != '\0') {
        return std::filesystem::path{env_home};
    }

    // 2. Fall back to getpwuid_r for the real UID.
    //    We use the re-entrant variant so this is safe to call from any thread.
    const uid_t uid = ::getuid();

    // Start with a reasonable buffer size; grow if getpwuid_r returns ERANGE.
    std::string buf(1024, '\0');
    struct passwd pw{};
    struct passwd* result = nullptr;

    while (true) {
        const int rc = ::getpwuid_r(uid, &pw, buf.data(),
                                    static_cast<std::size_t>(buf.size()),
                                    &result);
        if (rc == 0) {
            break;              // success (result may still be nullptr)
        }
        if (rc == ERANGE) {
            buf.resize(buf.size() * 2);  // buffer too small — double and retry
            continue;
        }
        // Any other error from getpwuid_r is fatal.
        throw std::runtime_error{
            "batbox::paths::home_dir: getpwuid_r failed with errno " +
            std::to_string(rc)};
    }

    if (result == nullptr || result->pw_dir == nullptr || result->pw_dir[0] == '\0') {
        throw std::runtime_error{
            "batbox::paths::home_dir: could not determine home directory "
            "($HOME unset, getpwuid_r returned no entry)"};
    }

    return std::filesystem::path{result->pw_dir};
}

// ---------------------------------------------------------------------------
// config_dir()
// ---------------------------------------------------------------------------
std::filesystem::path config_dir() {
    // 1. Honour $BATBOX_CONFIG_DIR override.
    const char* env_cfg = std::getenv("BATBOX_CONFIG_DIR");
    if (env_cfg != nullptr && env_cfg[0] != '\0') {
        return std::filesystem::path{env_cfg};
    }

    // 2. Default: ~/.batbox
    return home_dir() / ".batbox";
}

// ---------------------------------------------------------------------------
// expand_tilde()
// ---------------------------------------------------------------------------
std::filesystem::path expand_tilde(std::string_view path) {
    if (path.empty()) {
        return std::filesystem::path{path};
    }

    // Only expand when the path starts with "~".
    if (path[0] != '~') {
        return std::filesystem::path{path};
    }

    // "~" alone expands to home_dir().
    if (path.size() == 1) {
        return home_dir();
    }

    // "~/" prefix: replace "~/" with home_dir() + separator.
    if (path[1] == '/') {
        // Append the part after "~/"
        return home_dir() / std::filesystem::path{path.substr(2)};
    }

    // "~word" (e.g. "~alice") — we do NOT expand other-user tildes.
    // Return the path unchanged; the caller can interpret it as they see fit.
    return std::filesystem::path{path};
}

// ---------------------------------------------------------------------------
// project_root()
// ---------------------------------------------------------------------------
std::filesystem::path project_root() {
    std::filesystem::path candidate = std::filesystem::current_path();
    const std::filesystem::path fs_root = candidate.root_path();

    while (true) {
        // Check for either marker.
        if (std::filesystem::exists(candidate / ".git") ||
            std::filesystem::exists(candidate / "BATBOX.md")) {
            return candidate;
        }

        // Stop at filesystem root to avoid an infinite loop.
        if (candidate == fs_root) {
            break;
        }

        candidate = candidate.parent_path();
    }

    // No marker found anywhere — fall back to cwd.
    return std::filesystem::current_path();
}

} // namespace batbox::paths
