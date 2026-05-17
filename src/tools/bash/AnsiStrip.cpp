// src/tools/bash/AnsiStrip.cpp
//
// batbox::tools::bash::ansi_strip — remove ANSI/VT escape sequences from text.
//
// Blueprint contract: CPP 5.8 — AnsiStrip (file symbol, src/tools/bash/AnsiStrip.cpp)
//
// Sequences removed (state-machine, no regex):
//   ESC [ <param bytes> <final byte>   — CSI sequences (colors, cursor, erase, …)
//   ESC ] <text> BEL/ST                — OSC sequences (window title, hyperlinks)
//   ESC ( <byte>                        — character-set designators
//   ESC <single char> (other two-byte) — e.g. ESC M, ESC D, ESC E
//   BEL (0x07)                          — terminal bell
//   BS  (0x08)                          — backspace (simplistic; just removed)
//   SO / SI (0x0E / 0x0F)              — shift-out / shift-in
//
// The implementation is a tight state machine that operates on raw bytes,
// so it handles multi-byte UTF-8 sequences correctly (they are passed through
// unmodified — only bytes in 0x00–0x1F and ESC are inspected for control).

#include "AnsiStripInternal.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace batbox::tools::bash {

std::string ansi_strip(std::string_view input) {
    std::string out;
    out.reserve(input.size()); // worst case: no escapes

    enum class State : uint8_t {
        Normal,     // regular character stream
        Esc,        // just saw ESC (0x1B)
        CsiParam,   // inside ESC [ ... (CSI)
        OscBody,    // inside ESC ] ... (OSC) — terminated by BEL or ESC '\'
        OscEsc,     // saw ESC inside OSC body (possible ST = ESC '\')
        TwoChar,    // next byte completes a two-byte ESC sequence
    };

    State state = State::Normal;

    for (unsigned char ch : input) {
        switch (state) {
        case State::Normal:
            if (ch == 0x1B) {           // ESC
                state = State::Esc;
            } else if (ch == 0x07      // BEL — strip
                    || ch == 0x08      // BS  — strip
                    || ch == 0x0E      // SO  — shift-out, strip
                    || ch == 0x0F) {   // SI  — shift-in, strip
                // discard
            } else {
                out += static_cast<char>(ch);
            }
            break;

        case State::Esc:
            if (ch == '[') {
                state = State::CsiParam;
            } else if (ch == ']') {
                state = State::OscBody;
            } else if (ch == '(' || ch == ')' || ch == '*' || ch == '+') {
                // ESC ( B etc. — charset designator; consume next byte
                state = State::TwoChar;
            } else {
                // Other ESC-x two-byte sequences (ESC M, ESC 7, ESC 8, …)
                // Consume this byte and return to Normal.
                state = State::Normal;
            }
            break;

        case State::CsiParam:
            // CSI parameter and intermediate bytes: 0x20–0x3F
            // CSI final bytes: 0x40–0x7E → end of sequence
            if (ch >= 0x40 && ch <= 0x7E) {
                state = State::Normal;
            }
            // else: parameter/intermediate byte — keep consuming, discard
            break;

        case State::OscBody:
            if (ch == 0x07) {           // BEL terminates OSC
                state = State::Normal;
            } else if (ch == 0x1B) {    // ESC inside OSC — may be ST (ESC \)
                state = State::OscEsc;
            }
            // else: OSC body byte — discard
            break;

        case State::OscEsc:
            // ESC '\' (0x5C) = ST — string terminator; ends OSC
            // Any other char: not a valid ST, back to OSC body
            state = (ch == 0x5C) ? State::Normal : State::OscBody;
            break;

        case State::TwoChar:
            // The consuming byte of a two-byte ESC sequence (e.g. charset).
            state = State::Normal;
            break;
        }
    }

    return out;
}

} // namespace batbox::tools::bash
