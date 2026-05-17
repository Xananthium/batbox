// include/batbox/core/Paths.hpp
// ---------------------------------------------------------------------------
// Filesystem path helpers for BatBox.
//
// All functions are pure (no I/O caching). Callers that need create-if-absent
// behaviour should call std::filesystem::create_directories() on the returned
// path themselves — this header intentionally keeps I/O out of these helpers.
//
// Side effects:
//   config_dir()  — none (does NOT create the directory)
//   cache_dir()   — none
//   sessions_dir()— none
//   (creation is the caller's responsibility)
//
// Thread safety: safe to call from multiple threads concurrently. Each call
// reads environment variables and/or calls POSIX getpwuid_r; no shared
// mutable state is used inside this module.
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <string_view>

namespace batbox::paths {

// ---------------------------------------------------------------------------
// home_dir()
//
// Returns the current user's home directory.
//
// Resolution order:
//   1. $HOME environment variable (if set and non-empty)
//   2. POSIX getpwuid_r(getuid()) pw_dir field (fallback when $HOME is unset)
//
// Throws: std::runtime_error if neither $HOME nor getpwuid_r yields a result.
// ---------------------------------------------------------------------------
std::filesystem::path home_dir();

// ---------------------------------------------------------------------------
// config_dir()
//
// Returns the BatBox configuration directory.
//
// Resolution order:
//   1. $BATBOX_CONFIG_DIR environment variable (if set and non-empty)
//   2. home_dir() / ".batbox"
//
// NOTE: This function does NOT create the directory. If you need it to exist
// call std::filesystem::create_directories(config_dir()) at startup.
// ---------------------------------------------------------------------------
std::filesystem::path config_dir();

// ---------------------------------------------------------------------------
// expand_tilde(path)
//
// Expands a leading "~/" prefix in 'path' to home_dir().
//
// Examples:
//   expand_tilde("~/foo/bar") -> "/Users/alice/foo/bar"
//   expand_tilde("/absolute")  -> "/absolute"  (unchanged)
//   expand_tilde("relative")   -> "relative"   (unchanged)
//   expand_tilde("~")          -> home_dir()
//
// The function does NOT resolve symlinks or normalise the path beyond the
// tilde substitution.
// ---------------------------------------------------------------------------
std::filesystem::path expand_tilde(std::string_view path);

// ---------------------------------------------------------------------------
// project_root()
//
// Walks up the directory tree from the current working directory looking for
// a project root marker. Returns the first directory that contains either:
//   - ".git"      (a git repository root)
//   - "BATBOX.md" (an explicit BatBox project marker)
//
// If no marker is found before reaching the filesystem root ("/"), returns
// std::filesystem::current_path() (the cwd where the search started).
//
// This function performs filesystem stat calls (exists()) during the walk;
// it does NOT cache the result.
// ---------------------------------------------------------------------------
std::filesystem::path project_root();

} // namespace batbox::paths
