// include/batbox/repl/Keybindings.hpp
// ---------------------------------------------------------------------------
// batbox::repl::Keybindings — REPL action-to-key map + ftxui::Event dispatch.
//
// Design
// ------
//  The Keybindings class bridges the string-based keybinding overlay produced
//  by batbox::config::load_keybindings() (CPP 10.6) and the strongly-typed
//  ReplAction enum consumed by the REPL input loop (CPP 2.1).
//
//  Responsibilities:
//    1. Hold the canonical action→key-descriptor map (defaults + user overrides).
//    2. Parse key-descriptor strings into a parallel action→ftxui::Event map
//       once at construction / after apply_override().
//    3. Expose event_to_action(ftxui::Event) for O(1) event dispatch.
//    4. Log a warning when two actions share the same key descriptor (conflict).
//
// Key descriptor format (identical to KeybindingsConfig):
//    Zero or more modifiers joined with '+', followed by the base key:
//      "Ctrl+Enter"   "Shift+Tab"   "Escape"   "Up"   "Ctrl+L"
//    Recognised modifiers (case-insensitive): Ctrl, Shift, Alt, Meta
//    Base keys: single ASCII char  OR  named key (see parse_descriptor()).
//
// FTXUI event mapping
// -------------------
//  parse_descriptor() converts descriptor strings to ftxui::Event values by
//  pattern-matching modifier+base combinations.  The mapping is tested in
//  test_keybindings.cpp.
//
//  Special terminals (kitty keyboard protocol):
//    Some terminals emit CSI-u sequences (e.g. ESC[13;5u for Ctrl+Enter).
//    The default "Ctrl+Enter" binding recognises *both* the kitty CSI-u form
//    (\x1b[13;5u) and the traditional \r\n form so the REPL works on both.
//    Similarly "Shift+Enter" matches \x1b[13;2u and \x0d (bare Return in
//    terminals that cannot distinguish).
//
// Thread safety
// -------------
//  All methods must be called from one thread (the REPL event thread).
//
// Usage
// -----
//   batbox::repl::Keybindings kb;
//   // Apply user overrides loaded by CPP 10.6:
//   auto map = batbox::config::load_keybindings(path).value_or(
//                  batbox::config::default_keybindings());
//   kb.apply_override(map);
//
//   // In the FTXUI OnEvent handler:
//   auto action = kb.event_to_action(event);
//   if (action == batbox::repl::ReplAction::Send) { ... }
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/config/KeybindingsConfig.hpp>

#include <optional>
#include <string>
#include <unordered_map>

#include <ftxui/component/event.hpp>

namespace batbox::repl {

// ---------------------------------------------------------------------------
// ReplAction — strongly-typed REPL action enum.
//
// Every public action the REPL input loop can respond to has an entry here.
// The special value None is returned by event_to_action() when the event
// does not match any bound key.
// ---------------------------------------------------------------------------
enum class ReplAction {
    None,           ///< Event not matched — caller handles normally
    Send,           ///< Submit the current input buffer (Ctrl+Enter by default)
    Cancel,         ///< Cancel in-flight request / close menus (Escape)
    Newline,        ///< Insert a newline into the input (Shift+Enter)
    HistoryUp,      ///< Navigate to previous history entry (Up arrow)
    HistoryDown,    ///< Navigate to next history entry (Down arrow)
    Clear,          ///< Clear the current input buffer (Ctrl+L)
    CycleMode,      ///< Cycle permission mode (Shift+Tab)
    VimToggle,      ///< Toggle vim-mode keybindings on/off
    HistorySearch,  ///< Open Ctrl+R history search overlay
};

// ---------------------------------------------------------------------------
// Keybindings — action→key map with event-dispatch.
// ---------------------------------------------------------------------------
class Keybindings {
public:
    // ---- Lifecycle ---------------------------------------------------------

    /// Construct with built-in default bindings (no I/O, never fails).
    Keybindings();
    ~Keybindings() = default;

    Keybindings(const Keybindings&)            = delete;
    Keybindings& operator=(const Keybindings&) = delete;
    Keybindings(Keybindings&&)                 = default;
    Keybindings& operator=(Keybindings&&)      = default;

    // ---- Defaults ----------------------------------------------------------

    /// Return the built-in default action→key-descriptor map.
    ///
    /// The returned map uses ReplAction values as keys and key-descriptor
    /// strings (e.g. "Ctrl+Enter", "Shift+Tab") as values.  It is a snapshot;
    /// modifying it has no effect on this Keybindings instance.
    ///
    /// Never fails.  No I/O.
    [[nodiscard]]
    static std::unordered_map<ReplAction, std::string> default_keybindings();

    // ---- Override loader ---------------------------------------------------

    /// Overlay user-supplied bindings on top of the current map.
    ///
    /// @param map  A string→string map as returned by
    ///             batbox::config::load_keybindings() or
    ///             batbox::config::default_keybindings().
    ///             Keys are action names ("send", "cancel", …).
    ///             Values are key-descriptor strings ("Ctrl+Enter", …).
    ///
    /// Unknown action names in @p map are silently ignored (they were already
    /// warned about by the config loader).  Descriptor parsing failures
    /// produce a BATBOX_LOG_WARN and leave the binding unchanged.
    ///
    /// Conflict detection: if, after applying all overrides, two different
    /// actions share the same resolved ftxui::Event, a BATBOX_LOG_WARN is
    /// emitted for each conflicting pair.
    void apply_override(const batbox::config::KeybindingMap& map);

    // ---- Event dispatch ----------------------------------------------------

    /// Map an ftxui::Event to a ReplAction.
    ///
    /// Returns ReplAction::None when the event does not match any bound key.
    /// Mouse events and internal FTXUI events always return None.
    [[nodiscard]]
    ReplAction event_to_action(const ftxui::Event& ev) const;

    // ---- Introspection (for /keybindings slash command) --------------------

    /// Return the current key-descriptor string for @p action.
    /// Returns an empty optional if the action has no binding.
    [[nodiscard]]
    std::optional<std::string> key_for(ReplAction action) const;

    /// Return the current descriptor map (action → descriptor string).
    /// Snapshot — modifications have no effect.
    [[nodiscard]]
    std::unordered_map<ReplAction, std::string> descriptor_map() const;

private:
    // ---- Internal types ----------------------------------------------------

    /// One binding: a descriptor string and its resolved ftxui::Event.
    struct Binding {
        std::string    descriptor;
        ftxui::Event   event;
    };

    // ---- Internal helpers --------------------------------------------------

    /// Parse a key-descriptor string ("Ctrl+Enter", "Shift+Tab", …) into an
    /// ftxui::Event.  Returns std::nullopt if the descriptor is unrecognised.
    [[nodiscard]]
    static std::optional<ftxui::Event> parse_descriptor(std::string_view desc);

    /// Rebuild event_map_ from binding_map_ after any mutation.
    /// Also performs conflict detection.
    void rebuild_event_map();

    /// Map from action-name string to ReplAction (for apply_override lookup).
    [[nodiscard]]
    static ReplAction action_from_name(std::string_view name) noexcept;

    // ---- State -------------------------------------------------------------

    /// action → Binding (descriptor + resolved event)
    std::unordered_map<ReplAction, Binding> binding_map_;

    /// event-input-string → action  (for O(1) dispatch in event_to_action)
    std::unordered_map<std::string, ReplAction> event_map_;

    /// Extra multi-event aliases: one physical event may match multiple
    /// descriptors (e.g. Ctrl+Enter has two terminal encodings).
    /// These are keyed by the extra event input strings and point to actions.
    std::unordered_map<std::string, ReplAction> alias_map_;
};

} // namespace batbox::repl
