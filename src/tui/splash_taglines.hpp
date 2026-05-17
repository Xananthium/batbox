// src/tui/splash_taglines.hpp
// ---------------------------------------------------------------------------
// Miss Kittin era electroclash taglines + SplashBanner changelog entries.
//
// kTaglines — daily rotating tagline for legacy Splash (16 entries).
// kChangelog — what's-new entries shown in the SplashBanner right panel.
//
// Count: kTaglines has 16 entries (12-20 as required by task spec).
//        kChangelog has 5 entries; SplashBanner shows the last 3.
// ---------------------------------------------------------------------------
#pragma once

#include <array>
#include <string_view>

namespace batbox::tui {

inline constexpr std::array<std::string_view, 16> kTaglines = {{
    "frank sinatra",
    "1982",
    "requiem for a dead star",
    "stock exchange",
    "professional distortion",
    "the hacker",
    "i am not a robot",
    "meet my friend",
    "rippin kittin",
    "darling dada",
    "i com from the stars",
    "loving machine",
    "batbox · the fall of dance music",
    "cold synths, hot terminal",
    "electroclash for the working developer",
    "made in europe, runs everywhere",
}};

// Each entry is a one-line summary shown in the "What's new" right panel.
// Keep this ordered oldest-first; SplashBanner shows the last 3.
inline constexpr std::array<std::string_view, 5> kChangelog = {{
    "perf HUD chip in status bar (BATBOX_PERF_HUD=1)",
    "slash-palette slash-command filtering",
    "tool-call cards with expand/collapse (ctrl+o)",
    "thinking indicator during reasoning phase",
    "Claude-Code-style splash with tips + what's new",
}};

} // namespace batbox::tui
