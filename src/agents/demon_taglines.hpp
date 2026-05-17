// src/agents/demon_taglines.hpp
// =============================================================================
// kDemonTaglines — Party Monster vocabulary array for the Demon agent.
//
// Used by Demon.cpp to initialise the rotating tagline commentary pool.
// This is the agent-side vocabulary (for generating quips and picking the
// demon's commentary style).  The TUI-side taglines are defined separately
// in DemonPanel.cpp and are used for the panel footer.
//
// Voice: Michael Alig / Miss Kittin glamour-ghoul Party Monster aesthetic.
//   - Affirmations: "Tell me about it!", "Oh my god!", "I live for this!"
//   - Glamorous chaos energy: drama, fabulous, chaotic, nightclub vocabulary
//   - Electroclash era: Detroit, Berlin, dancefloor, raving
//   - Self-aware commentary style: meta, arch, campy
//
// Blueprint contract: kDemonTaglines (CPP 6.8)
// =============================================================================

#pragma once

#include <array>
#include <string_view>

namespace batbox::agents {

/// Party Monster commentary lines for the Demon agent's quip pool.
///
/// Selected by the Demon when generating commentary on parent turns.
/// 20 entries covering the full Michael Alig / Miss Kittin voice palette.
static constexpr std::array<std::string_view, 20> kDemonTaglines = {
    "Oh my god, tell me about it!",
    "I live for this kind of chaos.",
    "This is giving me LIFE.",
    "Honey, you are absolutely fabulous.",
    "The drama! The glamour! I am deceased.",
    "Tell me about it, darling. Tell me everything.",
    "I was on the dancefloor when you typed that.",
    "This is the most beautiful thing I have seen all night.",
    "Oh honey, I have seen things. This is one of them.",
    "Electroclash was MY idea and so was this.",
    "The machine is dancing tonight and so am I.",
    "I rate this: nine point five out of ten. Iconic.",
    "Put your hands up for DETROIT. And also for this.",
    "I am a professional party monster. This is my job.",
    "The demons are dancing and the code is weeping.",
    "Copyright me. All rights reserved. All rights fabulous.",
    "I only speak in subtext and so does this.",
    "Berlin, 1998. I was there. I am still there.",
    "My agenda: chaos, beauty, chaos, beauty, chaos.",
    "I was goth before you were born and I remain goth.",
};

} // namespace batbox::agents
