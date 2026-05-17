// src/theme/themes.cpp
// ---------------------------------------------------------------------------
// Five named colour palettes for batbox.
//
// Each Theme struct is assembled here with explicit RGB values documented in
// pmdraft.md.  All five are constexpr-initialised at program startup.
//
// Palettes:
//   miss-kittin     (default) — Miss Kittin-era electroclash: magenta/cyan/black
//   stock-exchange            — Finance terminal: cyan/yellow on black
//   frank-sinatra             — Smoky 50s: sepia/cream/amber on near-black
//   monochrome                — Strict white-on-black; accents become white/grey
//   classic                   — Original claude-code colour set (nostalgic)
// ---------------------------------------------------------------------------
#include <batbox/theme/Theme.hpp>
#include <batbox/config/SettingsLoader.hpp>

#include <cstdlib>      // std::getenv

namespace batbox::theme {

// ============================================================================
// Palette definitions
// ============================================================================

namespace {

// Shorthand so palette definitions stay readable.
inline ftxui::Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ftxui::Color::RGB(r, g, b);
}

// ---------------------------------------------------------------------------
// miss-kittin — electroclash default
// ---------------------------------------------------------------------------
// Aesthetic: Miss Kittin circa 1999-2003.  Hot magenta (#ff2a8c) slashes
// through near-black (#0a0a0a).  Cyan (#28ddff) for cost/todo accents.
// Acid green (#39ff70) for success, vivid red (#ff3b3b) for errors.
// ---------------------------------------------------------------------------
const Theme kMissKittin = {
    .bg             = rgb(10,  10,  10 ),  // #0a0a0a  near-black
    .fg             = rgb(232, 232, 232),  // #e8e8e8  off-white
    .accent_magenta = rgb(255, 42,  140),  // #ff2a8c  hot magenta
    .accent_cyan    = rgb(40,  221, 255),  // #28ddff  electric cyan
    .muted          = rgb(102, 102, 102),  // #666666  mid-grey
    .success        = rgb(57,  255, 112),  // #39ff70  acid green
    .error          = rgb(255, 59,  59 ),  // #ff3b3b  vivid red
    .diff_add_fg    = rgb(57,  255, 112),  // #39ff70  acid green
    .diff_add_bg    = rgb(14,  30,  14 ),  // #0e1e0e  dark green tint
    .diff_remove_fg = rgb(255, 85,  85 ),  // #ff5555  soft red
    .diff_remove_bg = rgb(30,  14,  14 ),  // #1e0e0e  dark red tint
    .prompt_prefix  = rgb(255, 42,  140),  // #ff2a8c  hot magenta (matches accent)
    .code_bg        = rgb(20,  20,  20 ),  // #141414  slightly lighter than bg
    .name           = "miss-kittin",
};

// ---------------------------------------------------------------------------
// stock-exchange — finance terminal
// ---------------------------------------------------------------------------
// Cyan (#00cccc) as the hot accent; yellow-green (#cccc00) as cool accent.
// Muted olive gives the Bloomberg-green-screen feel.
// ---------------------------------------------------------------------------
const Theme kStockExchange = {
    .bg             = rgb(10,  10,  10 ),  // #0a0a0a
    .fg             = rgb(212, 212, 200),  // #d4d4c8  warm off-white
    .accent_magenta = rgb(0,   204, 204),  // #00cccc  cyan (primary accent)
    .accent_cyan    = rgb(204, 204, 0  ),  // #cccc00  yellow (secondary accent)
    .muted          = rgb(85,  102, 85 ),  // #556655  olive-grey
    .success        = rgb(0,   204, 102),  // #00cc66  trading-green
    .error          = rgb(204, 51,  0  ),  // #cc3300  warning-red
    .diff_add_fg    = rgb(0,   204, 102),  // #00cc66
    .diff_add_bg    = rgb(10,  26,  10 ),  // #0a1a0a
    .diff_remove_fg = rgb(204, 68,  0  ),  // #cc4400
    .diff_remove_bg = rgb(26,  10,  0  ),  // #1a0a00
    .prompt_prefix  = rgb(0,   204, 204),  // #00cccc
    .code_bg        = rgb(17,  17,  17 ),  // #111111
    .name           = "stock-exchange",
};

// ---------------------------------------------------------------------------
// frank-sinatra — smoky 1950s supper-club
// ---------------------------------------------------------------------------
// Warm sepia tones.  Near-black with brown/amber cast.  Amber (#c87850) as
// the hot accent.  Warm grey (#a09080) as the cool accent.
// ---------------------------------------------------------------------------
const Theme kFrankSinatra = {
    .bg             = rgb(13,  11,  8  ),  // #0d0b08  warm near-black
    .fg             = rgb(232, 220, 200),  // #e8dcc8  cream
    .accent_magenta = rgb(200, 120, 80 ),  // #c87850  warm amber
    .accent_cyan    = rgb(160, 144, 128),  // #a09080  warm grey
    .muted          = rgb(112, 96,  80 ),  // #706050  mid-brown
    .success        = rgb(136, 170, 68 ),  // #88aa44  muted olive-green
    .error          = rgb(204, 68,  34 ),  // #cc4422  burnt orange-red
    .diff_add_fg    = rgb(136, 170, 68 ),  // #88aa44
    .diff_add_bg    = rgb(14,  18,  8  ),  // #0e1208
    .diff_remove_fg = rgb(204, 68,  34 ),  // #cc4422
    .diff_remove_bg = rgb(24,  10,  6  ),  // #180a06
    .prompt_prefix  = rgb(200, 120, 80 ),  // #c87850
    .code_bg        = rgb(16,  13,  8  ),  // #100d08
    .name           = "frank-sinatra",
};

// ---------------------------------------------------------------------------
// monochrome — strict white-on-black
// ---------------------------------------------------------------------------
// No chromatic accents; accent roles map to white/light-grey so no UI
// element disappears entirely.  Maximum readability on any terminal.
// ---------------------------------------------------------------------------
const Theme kMonochrome = {
    .bg             = rgb(0,   0,   0  ),  // #000000
    .fg             = rgb(255, 255, 255),  // #ffffff
    .accent_magenta = rgb(255, 255, 255),  // #ffffff  (accent = white)
    .accent_cyan    = rgb(204, 204, 204),  // #cccccc  (secondary = light grey)
    .muted          = rgb(136, 136, 136),  // #888888  mid-grey
    .success        = rgb(255, 255, 255),  // #ffffff
    .error          = rgb(170, 170, 170),  // #aaaaaa  dark grey (distinguishable)
    .diff_add_fg    = rgb(255, 255, 255),  // #ffffff
    .diff_add_bg    = rgb(17,  17,  17 ),  // #111111
    .diff_remove_fg = rgb(204, 204, 204),  // #cccccc
    .diff_remove_bg = rgb(10,  10,  10 ),  // #0a0a0a
    .prompt_prefix  = rgb(255, 255, 255),  // #ffffff
    .code_bg        = rgb(10,  10,  10 ),  // #0a0a0a
    .name           = "monochrome",
};

// ---------------------------------------------------------------------------
// classic — original claude-code colour set
// ---------------------------------------------------------------------------
// Warm slate background (#1a1a1a), neutral grey text (#d4d4d4).  Amber-orange
// (#da8548) primary accent, steel blue (#51afef) secondary — the look users
// know from the TypeScript original.
// ---------------------------------------------------------------------------
const Theme kClassic = {
    .bg             = rgb(26,  26,  26 ),  // #1a1a1a  dark slate
    .fg             = rgb(212, 212, 212),  // #d4d4d4  neutral grey
    .accent_magenta = rgb(218, 133, 72 ),  // #da8548  warm orange
    .accent_cyan    = rgb(81,  175, 239),  // #51afef  steel blue
    .muted          = rgb(91,  98,  104),  // #5b6268  blue-grey
    .success        = rgb(152, 190, 101),  // #98be65  muted green
    .error          = rgb(255, 108, 107),  // #ff6c6b  salmon-red
    .diff_add_fg    = rgb(152, 190, 101),  // #98be65
    .diff_add_bg    = rgb(26,  42,  26 ),  // #1a2a1a
    .diff_remove_fg = rgb(255, 108, 107),  // #ff6c6b
    .diff_remove_bg = rgb(42,  26,  26 ),  // #2a1a1a
    .prompt_prefix  = rgb(218, 133, 72 ),  // #da8548
    .code_bg        = rgb(34,  34,  34 ),  // #222222
    .name           = "classic",
};

} // anonymous namespace

// ============================================================================
// theme_from_name()
// ============================================================================

Theme theme_from_name(std::string_view name) {
    if (name == "miss-kittin")    return kMissKittin;
    if (name == "stock-exchange") return kStockExchange;
    if (name == "frank-sinatra")  return kFrankSinatra;
    if (name == "monochrome")     return kMonochrome;
    if (name == "classic")        return kClassic;
    // Unknown or empty name → default to miss-kittin
    return kMissKittin;
}

// ============================================================================
// load_theme()
// ============================================================================

Theme load_theme(const batbox::config::Settings& settings) {
    // 1. Check BATBOX_THEME environment variable (highest precedence)
    if (const char* env = std::getenv("BATBOX_THEME"); env != nullptr && env[0] != '\0') {
        return theme_from_name(env);
    }
    // 2. Fall back to settings.theme field
    if (!settings.theme.empty()) {
        return theme_from_name(settings.theme);
    }
    // 3. Miss-kittin default
    return kMissKittin;
}

} // namespace batbox::theme
