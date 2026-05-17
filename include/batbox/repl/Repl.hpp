// include/batbox/repl/Repl.hpp
// =============================================================================
// batbox::repl::Repl — main REPL input loop for the batbox CLI.
//
// Responsibilities
// ----------------
//  • Drives the interactive input loop: reads one line from InputBar on submit,
//    classifies the prefix, dispatches to the appropriate handler, and repeats.
//  • Prefix dispatch table:
//      '/'  → SlashCommandRegistry::lookup + execute (slash commands)
//      '!'  → BashTool direct execution without model involvement
//      '#'  → Append text to project BATBOX.md file
//      '@'  → Expand the mention inline then route to Conversation::user_message
//      else → Conversation::user_message + run_turn (AI conversation)
//  • Handles Ctrl+C (cancel in-flight inference), Ctrl+D (clean exit), up/down
//    (history navigation), and multi-line input.
//  • Multi-line input:
//      Line ending with '\' (backslash-continue): collects all segments.
//      Blank line when accumulating: terminates and submits the block.
//
// Ownership model
// ---------------
//  Repl does NOT own InputBar — InputBar is an FTXUI component managed by the
//  ScreenManager.  Instead the App layer wires InputBar::SubmitCallback to
//  Repl::on_submit_callback() which routes the submitted line to handle_input().
//
// Thread model
// ------------
//  handle_input() is intended to be called from a single thread (the REPL /
//  FTXUI submit thread).  cancel() is safe to call from any thread (e.g. a
//  signal handler for Ctrl+C or the FTXUI event thread).
//
// Headless / testing
// ------------------
//  When constructed without an InputBar and driven via feed_line(), the loop
//  can be exercised in unit tests without a terminal.
//
// Blueprint contract (CPP 2.6)
// ============================
//   class  batbox::repl::Repl
//   method batbox::repl::Repl::handle_input
// =============================================================================

#pragma once

#include <batbox/repl/CommandContext.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/Autocomplete.hpp>
#include <batbox/repl/History.hpp>
#include <batbox/repl/HistorySearch.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Forward declarations — avoid pulling in heavy headers.
namespace batbox::conversation { class Conversation; }
namespace batbox::tools        { class BashTool;     }
namespace batbox::tools        { struct ToolContext;  }

namespace batbox::repl {

// =============================================================================
// Repl
// =============================================================================

class Repl {
public:
    // -------------------------------------------------------------------------
    // Dependencies injected at construction time (all non-owning).
    //
    // conversation  — the live Conversation; Repl calls user_message + run_turn.
    // registry      — slash command registry for '/' prefix dispatch.
    // bash_tool     — BashTool instance for '!' prefix direct execution.
    // history       — shared History; Repl pushes submitted lines.
    // autocomplete  — Autocomplete engine (used to resolve '@' expansions).
    // cwd           — initial working directory.
    // ctx_extras    — optional extra fields that the App sets on CommandContext
    //                 (sidecar_manager, mcp_registry, usage_tracker, etc.).
    //                 May be nullptr; accessed safely.
    // -------------------------------------------------------------------------
    explicit Repl(batbox::conversation::Conversation&        conversation,
                  batbox::commands::SlashCommandRegistry&    registry,
                  batbox::tools::BashTool&                   bash_tool,
                  batbox::repl::History&                     history,
                  batbox::repl::Autocomplete&                autocomplete,
                  std::filesystem::path                      cwd,
                  batbox::commands::CommandContext*          ctx_extras = nullptr);

    // Non-copyable, non-movable (holds references).
    Repl(const Repl&)            = delete;
    Repl& operator=(const Repl&) = delete;
    Repl(Repl&&)                 = delete;
    Repl& operator=(Repl&&)      = delete;

    ~Repl();

    // -------------------------------------------------------------------------
    // handle_input(line)
    //
    // Blueprint contract entry point.  Called by the FTXUI submit callback
    // (on_submit_callback()) or by feed_line() in headless/test mode.
    //
    // Classifies the line by its first character and dispatches:
    //   '/'  → dispatch_slash(line.substr(1))
    //   '!'  → dispatch_bash(line.substr(1))
    //   '#'  → dispatch_note(line.substr(1))
    //   '@'  → dispatch_mention(line)
    //   else → dispatch_chat(line)
    //
    // Multi-line continuation:
    //   If line ends with '\' the trailing backslash is stripped and the line
    //   is appended to the pending multi-line buffer.  The function returns
    //   without dispatching until a line without '\' is received, at which
    //   point the full buffer is submitted as a single message.
    //
    // Empty line while accumulating multi-line input terminates the block.
    //
    // After dispatch, the original line is pushed to history (unless blank).
    //
    // @param line  The raw input line (not null-terminated).
    // -------------------------------------------------------------------------
    void handle_input(std::string_view line);

    // -------------------------------------------------------------------------
    // feed_line(line)  — headless / test API.
    //
    // Equivalent to calling handle_input(line) directly.  Provided as a
    // named method for clarity in test code.
    // -------------------------------------------------------------------------
    void feed_line(std::string_view line);

    // -------------------------------------------------------------------------
    // cancel()
    //
    // Fires the active CancelSource, aborting any in-flight inference or bash
    // execution.  Safe to call from any thread.
    // -------------------------------------------------------------------------
    void cancel();

    // -------------------------------------------------------------------------
    // request_exit()
    //
    // Sets the exit flag.  exit_requested() returns true after this call.
    // Also set when /exit fires CommandContext::exit_requested = true.
    // -------------------------------------------------------------------------
    void request_exit();

    // -------------------------------------------------------------------------
    // exit_requested() const
    //
    // Returns true after request_exit() has been called or /exit executed.
    // -------------------------------------------------------------------------
    [[nodiscard]] bool exit_requested() const noexcept;

    // -------------------------------------------------------------------------
    // on_submit_callback()
    //
    // Returns an InputBar::SubmitCallback (std::function<void(std::string)>)
    // that wraps handle_input().  Pass this to InputBar construction so that
    // Ctrl+Enter / Enter routes submitted lines to Repl::handle_input.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::function<void(std::string)> on_submit_callback();

    // -------------------------------------------------------------------------
    // set_output_stream(os)
    //
    // Override the output stream used for command responses and error messages.
    // Defaults to std::cout.  Call before run() to redirect output in tests.
    // -------------------------------------------------------------------------
    void set_output_stream(std::ostream& os);

    // -------------------------------------------------------------------------
    // set_input_stream(is)
    //
    // Override the input stream used for interactive confirmations.
    // Defaults to std::cin.
    // -------------------------------------------------------------------------
    void set_input_stream(std::istream& is);

private:
    // ---- Prefix dispatch handlers -------------------------------------------

    /// '/' prefix: look up and execute a slash command.
    void dispatch_slash(std::string_view args_including_name);

    /// '!' prefix: execute a bash command directly (no model).
    void dispatch_bash(std::string_view command);

    /// '#' prefix: append note text to the project BATBOX.md.
    void dispatch_note(std::string_view note_text);

    /// '@' prefix: expand mention then route to conversation as user message.
    void dispatch_mention(std::string_view line);

    /// default: append user message and run one inference turn.
    void dispatch_chat(std::string_view text);

    // ---- Helpers ------------------------------------------------------------

    /// Build a CommandContext wired to the live state.
    [[nodiscard]] batbox::commands::CommandContext make_command_context();

    /// Locate the nearest BATBOX.md walking up from cwd_.
    /// Returns empty path if none found.
    [[nodiscard]] std::filesystem::path find_batbox_md() const;

    /// Append `text` as a new line to `file`, creating it if necessary.
    /// Returns false on I/O error (prints error to output_).
    bool append_to_file(const std::filesystem::path& file, std::string_view text);

    /// Reset the cancel source + token pair for the next operation.
    void reset_cancel();


    // ---- State --------------------------------------------------------------

    // Injected dependencies (non-owning references).
    batbox::conversation::Conversation&     conversation_;
    batbox::commands::SlashCommandRegistry& registry_;
    batbox::tools::BashTool&                bash_tool_;
    batbox::repl::History&                  history_;
    batbox::repl::Autocomplete&             autocomplete_;

    // Optional extra CommandContext fields (nullable).
    batbox::commands::CommandContext*       ctx_extras_;

    // Working directory (mutated by /init; read by dispatch_note, etc.)
    std::filesystem::path cwd_;

    // Active cancellation.  Shared pointer so cancel() can fire from any thread.
    std::shared_ptr<CancelSource>   cancel_src_;

    // Exit flag.
    std::atomic<bool> exit_flag_{false};

    // Multi-line accumulation buffer.
    std::string multiline_buf_;
    bool        multiline_active_{false};

    // I/O streams (defaults to std::cout / std::cin; overridable for testing).
    std::ostream* output_{nullptr};
    std::istream* input_{nullptr};

    // ConversationHandle adapter — bridges CommandContext::conversation to
    // the real batbox::conversation::Conversation.
    struct ConvAdapter;
    std::unique_ptr<ConvAdapter> conv_adapter_;
};

} // namespace batbox::repl
