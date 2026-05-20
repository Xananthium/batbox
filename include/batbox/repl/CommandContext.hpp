// include/batbox/repl/CommandContext.hpp
//
// batbox::commands::CommandContext — lightweight pass-by-reference bag of
// live application state passed to every ISlashCommand::execute() call.
//
// Ownership
// ---------
// CommandContext is created by the REPL main loop (CPP A.3) once per
// command dispatch and lives for the duration of that call only.  Commands
// must not capture or store the context beyond execute().
//
// Conversation dependency
// -----------------------
// /clear calls ctx.conversation.reset_messages().  The full Conversation
// class is defined in CPP 3.x (not yet implemented).  To avoid a circular
// dependency we forward-declare the minimal interface needed by the S.1
// commands via the ConversationHandle struct below.  CPP 3.x will replace
// ConversationHandle with the real Conversation type; only the call-site
// in ClearCmd.cpp (one line) needs updating when that task lands.
//
// CPP 11.6 addition
// -----------------
// inject_user_message(text) was added to support user-defined commands that
// render a template body and submit it as the next user turn.  Callers that
// implemented ConversationHandle before CPP 11.6 must add an implementation;
// the default no-op base is intentionally not provided so that incomplete
// implementations fail at compile time rather than silently discarding input.
//
// CPP 2.6 note
// ------------
// This is the authoritative definition of CommandContext.  CPP A.3/CPP 2.6
// will augment it with pointers to TUI layer, permission gate, etc.
// Do NOT rename existing fields — they are part of the blueprint contract.
//
// CPP S.12 addition
// -----------------
// vim_mode and keybindings optional pointers were added so that /vim and
// /keybindings can reach the live REPL state.  Both are nullable: the REPL
// main loop sets them when InputBar has been constructed; CLI-only callers
// (tests, headless mode) leave them nullptr, and the commands degrade
// gracefully (vim_mode writes config only; keybindings uses a null-safe path).
//
// CPP S.5 addition
// ----------------
// Session-aware virtual methods added to ConversationHandle:
//   get_session_id()        — returns the active session UUID string
//   get_session_file_path() — absolute path to the session file on disk
//   get_turn_count()        — number of completed conversational turns
//   get_model_name()        — model string used at session start
//   get_messages_json()     — current messages as a JSON array (nlohmann::json)
//   set_messages_json()     — replace the live message list (used by /resume
//                             to restore a loaded session and by /compact to
//                             install the compacted message vector)
// All six methods have non-pure default implementations that return empty
// values, so implementations written before CPP S.5 compile unchanged; only
// the live App::Conversation implementation needs to override them.
//
// CPP S.10 addition
// -----------------
// advisor_mode added to support /advisor toggling the built-in coaching agent.
// When true, AdvisorCmd has been activated and the REPL loop should inject
// advisor suggestions after each assistant turn.  Defaults to false.
//
// UX-A addition
// -------------
// pick_from_list_fn is a nullable pointer to a std::function that shows a
// scrollable picker modal and returns the user-selected index, or nullopt on
// cancel.  Set by the TUI App on the CommandContext at slash-dispatch time so
// /model can display ModalPicker instead of the text-list + getline path.
// Null in CLI/headless mode; /model falls through to the existing text path.

#pragma once

#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>
#include <functional>
#include <optional>
#include <span>

// Forward-declare the REPL types so this header stays dependency-free.
namespace batbox::repl {
    class VimMode;
    class Keybindings;
}

// Forward-declare subsystem types for CPP S.4 status/stats commands.
// These are nullable optional pointers on CommandContext — null in headless
// and test mode; set by the REPL main loop (CPP A.3) when the subsystems
// are available.
namespace batbox::sidecar { class SidecarManager; }
namespace batbox::mcp     { class McpServerRegistry; }
namespace batbox::inference {
    class UsageTracker;
    struct UsageDelta;
}
namespace batbox::agents { class AgentSupervisor; }

namespace batbox::config { struct Config; }

namespace batbox::commands {

// ---------------------------------------------------------------------------
// ConversationHandle — minimal interface for command implementations.
//
// Replaced by batbox::conversation::Conversation in CPP 3.x.
// ---------------------------------------------------------------------------

struct ConversationHandle {
    virtual ~ConversationHandle() = default;

    /// Discard all message turns, resetting the conversation to empty.
    /// Called by /clear.
    virtual void reset_messages() = 0;

    /// Append `text` as the next user-role message and trigger a new
    /// inference turn.  Called by UserSlashCommand::execute() (CPP 11.6)
    /// to submit the rendered prompt body as if the user typed it.
    ///
    /// The REPL main loop (CPP A.3) implements this by writing `text` into
    /// its pending-input queue and returning from the current execute() call;
    /// the next REPL iteration picks up the queued message and runs a turn.
    virtual void inject_user_message(std::string_view text) = 0;

    /// Return the text of the Nth-from-last assistant-role message (1 = most recent).
    ///
    /// Returns an empty string when the conversation contains no assistant messages
    /// or when N exceeds the number of assistant messages present.
    ///
    /// Called by CopyCmd::execute() (CPP S.13) to retrieve the target message.
    virtual std::string last_assistant_message(std::size_t n = 1) const = 0;

    // -------------------------------------------------------------------------
    // CPP S.5 — session-aware accessors
    //
    // Default implementations return empty values so that MockConversation
    // implementations written before CPP S.5 continue to compile unchanged.
    // The live App::Conversation overrides all six with real data.
    // -------------------------------------------------------------------------

    /// UUID string of the active session.  Empty when no session is in progress.
    /// Called by /session to display the current session id.
    [[nodiscard]] virtual std::string get_session_id() const { return {}; }

    /// Absolute path to the .json (or .json.gz) session file on disk.
    /// Returns an empty path when no session is active.
    /// Called by /session to display the file location.
    [[nodiscard]] virtual std::filesystem::path get_session_file_path() const {
        return {};
    }

    /// Number of completed conversational turns (user+assistant pairs).
    /// Returns 0 when no session is active.
    /// Called by /session to display turn count.
    [[nodiscard]] virtual std::size_t get_turn_count() const { return 0; }

    /// Model name string at session start (e.g. "gpt-4o").
    /// Returns an empty string when no session is active.
    /// Called by /session to display the model name.
    [[nodiscard]] virtual std::string get_model_name() const { return {}; }

    /// Return the full message history as a JSON array.
    ///
    /// Each element is an object with at least "role" and "content" keys,
    /// matching the wire format used by batbox::conversation::Message.
    ///
    /// Returns an empty JSON array when no messages have been recorded yet.
    /// Called by /compact to pass the message list to Compactor::compact().
    [[nodiscard]] virtual Json get_messages_json() const { return Json::array(); }

    /// Replace the live message list with `messages`.
    ///
    /// `messages` must be a JSON array of message objects.  The conversation
    /// implementation validates the array on receipt; malformed input may be
    /// silently ignored or cause a best-effort restore.
    ///
    /// Called by /resume (after loading a session file) and by /compact
    /// (after installing the compacted message vector).
    virtual void set_messages_json(const Json& messages) { (void)messages; }
};

// ---------------------------------------------------------------------------
// CommandContext
// ---------------------------------------------------------------------------

struct CommandContext {
    // --- I/O -----------------------------------------------------------------

    /// Sink for command output (stdout in CLI mode; TUI message pane otherwise).
    std::ostream& output;

    /// Source for prompts requiring user confirmation (stdin in CLI mode).
    std::istream& input;

    // --- Lifecycle -----------------------------------------------------------

    /// Set to true by /exit to signal cooperative shutdown to the REPL loop.
    bool exit_requested = false;

    // --- Conversation --------------------------------------------------------

    /// The live conversation.  /clear calls conversation.reset_messages().
    /// User-defined commands call conversation.inject_user_message(text).
    /// Type: ConversationHandle (replaced by Conversation in CPP 3.x).
    ConversationHandle& conversation;

    // --- Registry ------------------------------------------------------------

    /// Read-only view of all registered slash commands.  /help enumerates this.
    const SlashCommandRegistry& registry;

    // --- Filesystem ----------------------------------------------------------

    /// Current working directory at dispatch time.  /init uses this as root.
    std::filesystem::path cwd;

    // --- Editor / REPL state (CPP S.12) --------------------------------------

    /// Live VimMode instance owned by InputBar.  Null in headless / test mode.
    /// /vim calls vim_mode->toggle() when non-null so the change takes effect
    /// immediately in the running REPL without requiring a restart.
    batbox::repl::VimMode* vim_mode = nullptr;

    /// Live Keybindings instance owned by the REPL main loop.  Null in
    /// headless / test mode.  /keybindings reads the current descriptor map
    /// and may call apply_override() after writing a new keybindings.json.
    batbox::repl::Keybindings* keybindings = nullptr;

    /// Path to the user's config directory (~/.batbox by default).
    /// Used by /keybindings to locate keybindings.json and by /terminal-setup
    /// to locate the terminal-snippets/ subdirectory.
    std::filesystem::path config_dir = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".batbox";

    // --- Advisor mode (CPP S.10) ---------------------------------------------

    /// When true, the built-in advisor agent is active: the REPL loop injects
    /// coaching suggestions after each assistant turn.  /advisor toggles this.
    /// Defaults to false (advisor off).
    bool advisor_mode = false;

    // --- Status / Stats subsystems (CPP S.4) ---------------------------------
    // All four pointers are nullable: null in headless/test mode; the REPL
    // main loop sets them when the subsystems are available.  Commands must
    // degrade gracefully (print "(n/a)") when any pointer is null.

    /// Live SidecarManager owned by the App.  /status reads current_state().
    batbox::sidecar::SidecarManager* sidecar_manager = nullptr;

    /// Live McpServerRegistry owned by the App.  /status enumerates servers.
    batbox::mcp::McpServerRegistry* mcp_registry = nullptr;

    /// Thread-safe token/cost accumulator shared across inference calls.
    /// /usage and /cost read session_total(); /stats reads session_total().
    batbox::inference::UsageTracker* usage_tracker = nullptr;

    /// Agent supervisor for sub-agent spawn count.  /stats reads snapshot().
    batbox::agents::AgentSupervisor* agent_supervisor = nullptr;


    // --- Live Config (PEXT3 1.1) ----------------------------------------------
    // Both pointers are nullable: null in headless/test mode; set by the REPL
    // main loop when the App-owned Config has been constructed.  Commands must
    // use these instead of std::getenv to access runtime configuration.

    /// App-owned Config instance.  Null in headless / test mode.
    /// ModelCmd (PEXT3 1.2) reads api.models and api.default_model from here.
    const batbox::config::Config* cfg       = nullptr;

    /// Mutex guarding cfg mutations (hot-reload).  Nullable like cfg.
    std::mutex*                   cfg_mutex = nullptr;

    /// Human-readable permission mode string (e.g. "default", "nuclear").
    /// Owned by the REPL loop.  /status displays this; null → "(n/a)".
    const char* permission_mode_str = nullptr;

    // --- Interactive picker (UX-A) --------------------------------------------
    // Nullable: null in CLI/headless mode; set by the TUI App at slash-dispatch
    // time.  Commands must never store this pointer beyond execute().

    /// Show a scrollable modal picker and block until the user selects or cancels.
    ///
    /// @param title        Header text shown in the modal title bar.
    /// @param items        The list of strings to display (viewed, not copied).
    /// @param current_idx  Index of the currently-active item (highlighted on open).
    /// @returns            The 0-based index of the chosen item, or std::nullopt
    ///                     if the user cancelled (Escape).
    ///
    /// Thread contract: called from a worker thread (same thread as execute()).
    /// The implementation blocks on a condition_variable until the UI thread
    /// resolves the picker.  Calling from the UI thread would deadlock.
    ///
    /// When nullptr: command must fall through to the text-list + getline path
    /// (CLI compatibility).
    std::function<std::optional<std::size_t>(
        std::string_view           title,
        std::span<const std::string> items,
        std::size_t                current_idx)>* pick_from_list_fn = nullptr;
};

} // namespace batbox::commands
