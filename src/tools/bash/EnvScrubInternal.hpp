// src/tools/bash/EnvScrubInternal.hpp
//
// Internal header for EnvScrub — used only within the bash sub-library.
// Not installed to include/batbox/; callers use BashRunner's public API.

#pragma once

#include <string>
#include <vector>

namespace batbox::tools::bash {

/// Returns a filtered environment suitable for execvp() in the child process.
/// Only entries whose keys appear in @p allowlist survive (or the default
/// allowlist if @p allowlist is empty). Hard-coded secrets are always stripped.
/// Guarantees TERM=xterm-256color is present.
[[nodiscard]] std::vector<std::string> scrub_env(
    const std::vector<std::string>& allowlist);

} // namespace batbox::tools::bash
