// include/batbox/util/AccountLabel.hpp
// ---------------------------------------------------------------------------
// batbox::util::resolve_account_label() — TUI-FIX-T8
//
// Returns a display string for the "account:" row in SplashBanner.
//
//   1. If configured_account is non-empty, return it as-is.
//   2. Otherwise build "$USER@<hostname>" from the environment:
//        - $USER — if unset or empty, fall back to getpwuid(getuid())->pw_name.
//        - hostname via gethostname(); if that fails, use "localhost".
//
// Thread-safe: no mutable global state.  The getpwuid_r / gethostname calls
// are both async-signal-safe enough for a UI thread.
// ---------------------------------------------------------------------------
#pragma once

#include <optional>
#include <string>

namespace batbox::util {

/// Return a non-empty account label to show in SplashBanner.
///
/// @param configured_account  Value from BatBox config / env (may be empty or absent).
/// @returns                   configured_account if non-empty; otherwise
///                            "<user>@<hostname>" derived from the OS.
[[nodiscard]]
std::string resolve_account_label(const std::optional<std::string>& configured_account);

} // namespace batbox::util
