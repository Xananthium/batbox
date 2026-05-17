// include/batbox/tui/PermissionCard.hpp
// ---------------------------------------------------------------------------
// batbox::tui::PermissionCard — tool-confirmation modal with [a/A/n/N/e] keys.
//
// Design
// ------
// PermissionCard is an FTXUI ComponentBase subclass that renders a floating
// permission dialog when a tool call needs user confirmation.  It displays:
//
//   1. A title bar: "Permission Request" in AccentCyan.
//   2. The tool name in AccentMagenta (bold).
//   3. A pretty-printed JSON preview of the tool arguments (indented, in code_bg).
//   4. When a rule pattern is being drafted (AlwaysAllow / AlwaysDeny):
//      a one-line editable pattern input so the user can refine the glob.
//   5. A five-line key-hint footer:
//        [a] allow once     [A] always allow
//        [n] deny           [N] always deny
//        [e] edit args      [Esc] cancel
//
// Keys (OnEvent):
//   a / A — Allow once / Always allow
//   n / N — Deny once / Always deny
//   e     — Edit args: opens $EDITOR / nano / pico / vi on the args JSON,
//            reads back the edited JSON, returns Decision with edit_text set.
//   Esc   — Cancel (treated as one-shot Deny)
//
// Editor integration (TUI-FIX-T7)
// ---------------------------------
// When the user presses e, OnEvent:
//   1. Calls batbox::util::edit_string_in_editor(args_preview_, screen_) which:
//      a. Writes args JSON to a temp file.
//      b. Suspends FTXUI via screen_->WithRestoredIO() (if screen_ is set).
//      c. Execs the resolved editor (resolve_editor()).
//      d. Reads back the edited file.
//   2. Sets Decision::edit_text to the edited content and resolves.
//
// Call set_screen(&screen_mgr.screen_interactive()) before the first
// await_user_decision() to enable proper terminal handling.
//
// Threading
// ---------
// The component lives on the UI thread.  The dispatch layer worker thread calls
// await_user_decision(tool_name, args) which:
//   1. Populates display state under a mutex.
//   2. Blocks on std::condition_variable until the UI thread resolves.
//   3. Returns a batbox::permissions::Decision.
//
// The UI thread calls resolve() from within OnEvent when a key is pressed.
//
// Usage pattern (with ftxui::Modal):
// ------------------------------------
//   bool show_card = false;
//   auto card = std::make_shared<PermissionCard>(theme);
//   card->set_screen(&screen_mgr.screen_interactive());
//   auto root  = ftxui::Modal(base, card, &show_card);
//
//   // Worker thread:
//   show_card = true;
//   screen.PostEvent(Events::ModalShow);
//   auto decision = card->await_user_decision("Bash", args_json);
//   show_card = false;
//
// Blueprint contract: batbox::tui::PermissionCard (blueprints table, task CPP 1.10)
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/theme/Theme.hpp>
#include <batbox/tui/ThemeApply.hpp>
#include <batbox/permissions/PermissionGate.hpp>
#include <batbox/core/Json.hpp>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>

namespace batbox::tui {

// =============================================================================
// PermissionCard — FTXUI modal Component for tool-permission confirmation
// =============================================================================

/// Modal overlay for tool permission prompts.
///
/// Worker thread calls await_user_decision() — it blocks until the user
/// presses one of the five action keys.  The UI thread dispatches keys via
/// OnEvent, calls resolve(), and wakes the worker thread.
///
/// Blueprint contract: class batbox::tui::PermissionCard (CPP 1.10)
class PermissionCard : public ftxui::ComponentBase {
public:
    // =========================================================================
    // Construction
    // =========================================================================

    /// Construct a PermissionCard bound to the given theme.
    ///
    /// @param theme  Live theme reference; must outlive this component.
    explicit PermissionCard(const batbox::theme::Theme& theme);

    ~PermissionCard() override = default;

    // Non-copyable, non-movable (ComponentBase semantics).
    PermissionCard(const PermissionCard&)            = delete;
    PermissionCard& operator=(const PermissionCard&) = delete;
    PermissionCard(PermissionCard&&)                 = delete;
    PermissionCard& operator=(PermissionCard&&)      = delete;

    // =========================================================================
    // Screen injection (TUI-FIX-T7)
    // =========================================================================

    /// Inject the live ScreenInteractive pointer so that e can call
    /// WithRestoredIO() before launching the external editor.
    ///
    /// Must be called before the first await_user_decision() on the UI thread.
    /// Passing nullptr disables the WithRestoredIO suspension path (editor is
    /// launched via std::system without terminal handoff — only for tests).
    ///
    /// @param screen  Pointer to the live ftxui::ScreenInteractive; may be null.
    void set_screen(ftxui::ScreenInteractive* screen) noexcept { screen_ = screen; }

    // =========================================================================
    // Blocking entry-point (called from a WORKER thread)
    // =========================================================================

    /// Block the calling thread until the user makes a permission decision.
    ///
    /// This method:
    ///   1. Populates tool_name_, args_json_, and args_preview_ under the mutex.
    ///   2. Blocks on condition_variable until the UI thread resolves.
    ///   3. Returns the batbox::permissions::Decision carrying the user choice.
    ///
    /// @param tool_name  Name of the tool requesting permission.
    /// @param args       Tool arguments as a JSON object.
    ///
    /// Thread: MUST be called from a non-UI thread.  Calling from the UI thread
    ///         would deadlock (condition_variable starves the FTXUI event loop).
    ///
    /// Blueprint contract name: await_user_decision
    [[nodiscard]]
    batbox::permissions::Decision await_user_decision(std::string_view tool_name,
                                                       const batbox::Json& args);

    // =========================================================================
    // ComponentBase overrides (UI thread)
    // =========================================================================

    /// Render the permission card as a centered modal box.
    ///
    /// Blueprint contract name: OnRender
    ftxui::Element OnRender() override;

    /// Handle keyboard events.
    ///   a → Allow once       A → Always allow (with rule)
    ///   n → Deny once        N → Always deny (with rule)
    ///   e → Edit args in $EDITOR / nano / pico / vi
    ///   Esc → Cancel (one-shot deny)
    ///
    /// Returns true when the event is consumed (always for the five action keys).
    ///
    /// Blueprint contract name: OnEvent
    bool OnEvent(ftxui::Event event) override;

    // =========================================================================
    // Accessors (exposed for tests)
    // =========================================================================

    /// The tool name currently displayed.  Empty before first await_user_decision().
    [[nodiscard]] const std::string& tool_name() const noexcept { return tool_name_; }

    /// The pretty-printed args preview currently displayed.
    [[nodiscard]] const std::string& args_preview() const noexcept { return args_preview_; }

    /// True while await_user_decision() is blocked waiting for a response.
    [[nodiscard]] bool pending() const;

private:
    // =========================================================================
    // Internal helpers
    // =========================================================================

    /// Resolve the pending await() with decision `d` and wake the worker thread.
    /// Called from OnEvent on the UI thread.
    void resolve(batbox::permissions::Decision d);

    /// Build a pretty-printed JSON preview string from args.
    /// Falls back to a compact dump on JSON errors.
    static std::string build_preview(const batbox::Json& args);

    /// Build the default rule pattern for always-allow / always-deny.
    /// Format: "ToolName(arg_glob)" where arg_glob extracts the main arg field.
    static std::string build_default_rule(std::string_view tool_name,
                                           const batbox::Json& args);

    // =========================================================================
    // Data members
    // =========================================================================

    const batbox::theme::Theme& theme_;  ///< Active colour palette (ref, not owned)

    /// Live ScreenInteractive pointer — injected via set_screen() (TUI-FIX-T7).
    /// Used by the e handler to suspend FTXUI while the external editor runs.
    /// Null in headless/test contexts.
    ftxui::ScreenInteractive* screen_{nullptr};

    // Display state — set by await_user_decision() before waiting, read by
    // OnRender() on the UI thread.  Protected by mtx_.
    std::string    tool_name_;     ///< Tool requesting permission
    std::string    args_preview_;  ///< Pretty-printed JSON (indented)
    std::string    default_rule_;  ///< Pre-filled rule pattern for A/N choices

    // Synchronisation between worker thread (await_user_decision) and UI thread.
    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    bool                    pending_{false};   ///< true while await is blocked
    bool                    resolved_{false};  ///< set by resolve() to wake await
    batbox::permissions::Decision result_;     ///< Written by resolve()
};

} // namespace batbox::tui
