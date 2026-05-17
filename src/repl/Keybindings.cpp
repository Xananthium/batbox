// src/repl/Keybindings.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::repl::Keybindings.
//
// FTXUI event input-string constants (verified empirically):
//   Escape          \x1b
//   Tab             \x09
//   Shift+Tab       \x1b[Z   (ftxui::Event::TabReverse)
//   Return          \x0a     (ftxui::Event::Return / CtrlJ)
//   Ctrl+M          \x0d     (bare Enter on most terminals)
//   Ctrl+L          \x0c
//   Ctrl+R          \x12
//   Ctrl+C          \x03
//   ArrowUp         \x1b[A
//   ArrowDown       \x1b[B
//   Ctrl+Enter kitty \x1b[13;5u
//   Shift+Enter kitty \x1b[13;2u
//   Ctrl+Enter trad  \x0d\x0a
// ---------------------------------------------------------------------------

#include <batbox/repl/Keybindings.hpp>
#include <batbox/core/Logging.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include <ftxui/component/event.hpp>

namespace batbox::repl {

// ============================================================================
// Constant FTXUI event input strings
// ============================================================================
namespace {

// These are the raw input() strings that FTXUI assigns to each event.
// Verified by compiling against the installed FTXUI library.
constexpr std::string_view kInputEscape         = "\x1b";
constexpr std::string_view kInputTab            = "\x09";
constexpr std::string_view kInputShiftTab       = "\x1b[Z";
constexpr std::string_view kInputReturn         = "\x0a";    // CtrlJ / Return
constexpr std::string_view kInputCtrlM          = "\x0d";    // bare Enter (most terminals)
constexpr std::string_view kInputCtrlL          = "\x0c";
constexpr std::string_view kInputCtrlR          = "\x12";
constexpr std::string_view kInputCtrlC          = "\x03";
constexpr std::string_view kInputCtrlD          = "\x04";
constexpr std::string_view kInputArrowUp        = "\x1b[A";
constexpr std::string_view kInputArrowDown      = "\x1b[B";
// Kitty keyboard protocol encodings for Shift+Enter and Ctrl+Enter:
constexpr std::string_view kInputShiftEnterKitty = "\x1b[13;2u";
constexpr std::string_view kInputCtrlEnterKitty  = "\x1b[13;5u";
// Traditional Ctrl+Enter on terminals that distinguish: \r\n
constexpr std::string_view kInputCtrlEnterTrad   = "\x0d\x0a";

/// Build a map from lowercase modifier name → modifier bitmask.
/// Bitmasks: Ctrl=1, Shift=2, Alt=4, Meta=8.
constexpr int kModCtrl  = 1;
constexpr int kModShift = 2;
constexpr int kModAlt   = 4;

/// Parse a single token into a modifier bit.  Returns -1 if not a modifier.
static int token_to_modifier_bit(std::string_view token) noexcept {
    // Case-insensitive compare via tolower on each char.
    auto eq_ci = [](std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                return false;
        return true;
    };
    if (eq_ci(token, "ctrl"))  return kModCtrl;
    if (eq_ci(token, "shift")) return kModShift;
    if (eq_ci(token, "alt"))   return kModAlt;
    if (eq_ci(token, "meta"))  return kModAlt; // treat meta as alt
    return -1;
}

/// Case-insensitive string equality helper.
static bool str_eq_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

} // anonymous namespace

// ============================================================================
// parse_descriptor()
// ============================================================================

// static
std::optional<ftxui::Event> Keybindings::parse_descriptor(std::string_view desc) {
    if (desc.empty()) return std::nullopt;

    // Tokenise by '+'.  The last token is the base key; all preceding tokens
    // must be modifier names.
    std::vector<std::string_view> tokens;
    {
        std::string_view rem = desc;
        while (!rem.empty()) {
            auto plus = rem.find('+');
            if (plus == std::string_view::npos) {
                tokens.push_back(rem);
                break;
            }
            tokens.push_back(rem.substr(0, plus));
            rem = rem.substr(plus + 1);
        }
    }
    if (tokens.empty()) return std::nullopt;

    // Last token = base key.  All other tokens = modifiers.
    std::string_view base = tokens.back();
    int mods = 0;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        int bit = token_to_modifier_bit(tokens[i]);
        if (bit < 0) {
            // Unrecognised modifier token — treat as single char if only one
            // such token and no real modifier bits set yet.  Otherwise fail.
            return std::nullopt;
        }
        mods |= bit;
    }

    // ---------------------------------------------------------------------------
    // Map (mods, base) → ftxui::Event
    //
    // We construct FTXUI events by building their exact input() strings, because
    // ftxui::Event::Special / Character compare by the input_ field.
    // ---------------------------------------------------------------------------

    // No modifier — named key or single char.
    if (mods == 0) {
        if (str_eq_ci(base, "escape"))   return ftxui::Event::Escape;
        if (str_eq_ci(base, "tab"))      return ftxui::Event::Tab;
        if (str_eq_ci(base, "enter"))    return ftxui::Event::Return;       // \x0a
        if (str_eq_ci(base, "return"))   return ftxui::Event::Return;
        if (str_eq_ci(base, "up"))       return ftxui::Event::ArrowUp;
        if (str_eq_ci(base, "down"))     return ftxui::Event::ArrowDown;
        if (str_eq_ci(base, "left"))     return ftxui::Event::ArrowLeft;
        if (str_eq_ci(base, "right"))    return ftxui::Event::ArrowRight;
        if (str_eq_ci(base, "backspace"))return ftxui::Event::Backspace;
        if (str_eq_ci(base, "delete"))   return ftxui::Event::Delete;
        if (str_eq_ci(base, "insert"))   return ftxui::Event::Insert;
        if (str_eq_ci(base, "home"))     return ftxui::Event::Home;
        if (str_eq_ci(base, "end"))      return ftxui::Event::End;
        if (str_eq_ci(base, "pageup"))   return ftxui::Event::PageUp;
        if (str_eq_ci(base, "pagedown")) return ftxui::Event::PageDown;
        if (str_eq_ci(base, "space"))    return ftxui::Event::Character(' ');
        if (str_eq_ci(base, "f1"))  return ftxui::Event::F1;
        if (str_eq_ci(base, "f2"))  return ftxui::Event::F2;
        if (str_eq_ci(base, "f3"))  return ftxui::Event::F3;
        if (str_eq_ci(base, "f4"))  return ftxui::Event::F4;
        if (str_eq_ci(base, "f5"))  return ftxui::Event::F5;
        if (str_eq_ci(base, "f6"))  return ftxui::Event::F6;
        if (str_eq_ci(base, "f7"))  return ftxui::Event::F7;
        if (str_eq_ci(base, "f8"))  return ftxui::Event::F8;
        if (str_eq_ci(base, "f9"))  return ftxui::Event::F9;
        if (str_eq_ci(base, "f10")) return ftxui::Event::F10;
        if (str_eq_ci(base, "f11")) return ftxui::Event::F11;
        if (str_eq_ci(base, "f12")) return ftxui::Event::F12;

        // Single printable character.
        if (base.size() == 1) {
            return ftxui::Event::Character(std::string(base));
        }
        return std::nullopt;
    }

    // Shift + Tab → TabReverse (\x1b[Z)
    if (mods == kModShift && str_eq_ci(base, "tab")) {
        return ftxui::Event::TabReverse;
    }

    // Shift + Enter / Return → primary kitty encoding; \x0d alias handled in
    // apply_override via alias_map_.
    if (mods == kModShift && (str_eq_ci(base, "enter") || str_eq_ci(base, "return"))) {
        return ftxui::Event::Special(std::string(kInputShiftEnterKitty));
    }

    // Ctrl + Enter / Return → kitty encoding; alias handles traditional \r\n
    if (mods == kModCtrl && (str_eq_ci(base, "enter") || str_eq_ci(base, "return"))) {
        // Primary: kitty \x1b[13;5u
        return ftxui::Event::Special(std::string(kInputCtrlEnterKitty));
    }

    // Ctrl + single ASCII letter: map to control code (0x01–0x1a for a–z).
    if (mods == kModCtrl && base.size() == 1) {
        char c = (char)std::tolower((unsigned char)base[0]);
        if (c >= 'a' && c <= 'z') {
            // FTXUI represents Ctrl+X as a special string containing the ctrl byte.
            char ctrl_byte = (char)(c - 'a' + 1);
            return ftxui::Event::Special(std::string(1, ctrl_byte));
        }
        // Ctrl+[ = Escape
        if (c == '[') return ftxui::Event::Escape;
    }

    // Alt / Meta + single ASCII letter.
    if ((mods == kModAlt) && base.size() == 1) {
        char c = (char)base[0];
        // FTXUI Alt+X is ESC followed by the character: "\x1b<char>"
        std::string s = "\x1b";
        s += c;
        return ftxui::Event::Special(s);
    }

    // Alt + named key: ESC followed by the named key's escape sequence.
    if (mods == kModAlt) {
        // First resolve the base key with no modifier to get its event.
        auto base_ev = parse_descriptor(base);
        if (base_ev) {
            // Alt+X is represented as ESC followed by the base-key sequence.
            std::string s = "";
            s += base_ev->input();
            return ftxui::Event::Special(s);
        }
    }

    return std::nullopt;
}

// ============================================================================
// action_from_name()
// ============================================================================

// static
ReplAction Keybindings::action_from_name(std::string_view name) noexcept {
    if (name == "send")           return ReplAction::Send;
    if (name == "cancel")         return ReplAction::Cancel;
    if (name == "newline")        return ReplAction::Newline;
    if (name == "history_up")     return ReplAction::HistoryUp;
    if (name == "history_down")   return ReplAction::HistoryDown;
    if (name == "clear")          return ReplAction::Clear;
    if (name == "cycle_mode")     return ReplAction::CycleMode;
    if (name == "vim_toggle")     return ReplAction::VimToggle;
    if (name == "history_search") return ReplAction::HistorySearch;
    return ReplAction::None;
}

// ============================================================================
// default_keybindings() — static
// ============================================================================

// static
std::unordered_map<ReplAction, std::string> Keybindings::default_keybindings() {
    return {
        { ReplAction::Send,          "Ctrl+Enter"  },
        { ReplAction::Cancel,        "Escape"      },
        { ReplAction::CycleMode,     "Shift+Tab"   },
        { ReplAction::Newline,       "Shift+Enter" },
        { ReplAction::HistoryUp,     "Up"          },
        { ReplAction::HistoryDown,   "Down"        },
        { ReplAction::Clear,         "Ctrl+L"      },
        { ReplAction::VimToggle,     "Ctrl+G"      },  // Ctrl+G toggles vim-mode; Escape is Cancel only
        { ReplAction::HistorySearch, "Ctrl+R"      },
    };
}

// ============================================================================
// Constructor
// ============================================================================

Keybindings::Keybindings() {
    // Populate binding_map_ from the static defaults.
    const auto defaults = default_keybindings();
    for (const auto& [action, desc] : defaults) {
        auto ev_opt = parse_descriptor(desc);
        if (!ev_opt) {
            BATBOX_LOG_WARN("Keybindings: default descriptor '{}' could not be parsed — skipped",
                            desc);
            continue;
        }
        binding_map_[action] = Binding{ desc, *ev_opt };
    }
    rebuild_event_map();
}

// ============================================================================
// rebuild_event_map()
// ============================================================================

void Keybindings::rebuild_event_map() {
    event_map_.clear();
    alias_map_.clear();

    // -- Populate event_map_ from binding_map_ --------------------------------
    for (const auto& [action, binding] : binding_map_) {
        event_map_[binding.event.input()] = action;
    }

    // -- Add terminal-encoding aliases ----------------------------------------
    //
    // "Ctrl+Enter" can arrive as either the kitty sequence (\x1b[13;5u) or
    // the traditional \r\n, or just \x0d.  The primary binding resolves to
    // the kitty form; add the traditional forms as aliases.
    {
        auto it = binding_map_.find(ReplAction::Send);
        if (it != binding_map_.end() && it->second.descriptor == "Ctrl+Enter") {
            // Alias: traditional Ctrl+Enter = \r\n
            alias_map_[std::string(kInputCtrlEnterTrad)] = ReplAction::Send;
            // Alias: \x0d (bare CR) — when the terminal can't distinguish Enter from Ctrl+Enter
            // NOTE: We deliberately do NOT alias bare \x0d here as that would capture every
            // plain Enter press.  Only the kitty and \r\n forms are aliased.
        }
    }

    // "Shift+Enter" primary is kitty \x1b[13;2u; bare \x0d is NOT aliased
    // (it conflicts with Enter).  No extra aliases needed here.

    // -- Conflict detection ---------------------------------------------------
    // Build a reverse map: event-input-string → list of (action, descriptor)
    std::unordered_map<std::string, std::vector<ReplAction>> reverse;
    for (const auto& [action, binding] : binding_map_) {
        reverse[binding.event.input()].push_back(action);
    }
    for (const auto& [input_str, actions] : reverse) {
        if (actions.size() < 2) continue;
        // Convert action to name for the warning.
        auto action_name = [](ReplAction a) -> std::string_view {
            switch (a) {
                case ReplAction::Send:          return "send";
                case ReplAction::Cancel:        return "cancel";
                case ReplAction::Newline:       return "newline";
                case ReplAction::HistoryUp:     return "history_up";
                case ReplAction::HistoryDown:   return "history_down";
                case ReplAction::Clear:         return "clear";
                case ReplAction::CycleMode:     return "cycle_mode";
                case ReplAction::VimToggle:     return "vim_toggle";
                case ReplAction::HistorySearch: return "history_search";
                default:                        return "none";
            }
        };
        // Log one warning per conflicting pair (first vs each subsequent).
        for (std::size_t i = 1; i < actions.size(); ++i) {
            BATBOX_LOG_WARN(
                "Keybindings: conflict — actions '{}' and '{}' share the same key binding",
                action_name(actions[0]), action_name(actions[i]));
        }
    }
}

// ============================================================================
// apply_override()
// ============================================================================

void Keybindings::apply_override(const batbox::config::KeybindingMap& map) {
    for (const auto& [name, desc] : map) {
        const ReplAction action = action_from_name(name);
        if (action == ReplAction::None) {
            // Unknown to the REPL layer — skip silently (already warned by config loader).
            continue;
        }

        auto ev_opt = parse_descriptor(desc);
        if (!ev_opt) {
            BATBOX_LOG_WARN(
                "Keybindings: apply_override: unrecognised key descriptor '{}' for action '{}' — keeping previous binding",
                desc, name);
            continue;
        }

        binding_map_[action] = Binding{ desc, *ev_opt };
    }
    rebuild_event_map();
}

// ============================================================================
// event_to_action()
// ============================================================================

ReplAction Keybindings::event_to_action(const ftxui::Event& ev) const {
    // Mouse events and FTXUI internal events never map to REPL actions.
    if (ev.is_mouse()) return ReplAction::None;

    const std::string& input = ev.input();

    {
        auto it = event_map_.find(input);
        if (it != event_map_.end()) return it->second;
    }
    {
        auto it = alias_map_.find(input);
        if (it != alias_map_.end()) return it->second;
    }
    return ReplAction::None;
}

// ============================================================================
// key_for()
// ============================================================================

std::optional<std::string> Keybindings::key_for(ReplAction action) const {
    auto it = binding_map_.find(action);
    if (it == binding_map_.end()) return std::nullopt;
    return it->second.descriptor;
}

// ============================================================================
// descriptor_map()
// ============================================================================

std::unordered_map<ReplAction, std::string> Keybindings::descriptor_map() const {
    std::unordered_map<ReplAction, std::string> result;
    result.reserve(binding_map_.size());
    for (const auto& [action, binding] : binding_map_) {
        result[action] = binding.descriptor;
    }
    return result;
}

} // namespace batbox::repl
