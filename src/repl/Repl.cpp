// src/repl/Repl.cpp
// =============================================================================
// batbox::repl::Repl — main REPL input loop with prefix dispatch.
//
// Prefix table
// ------------
//   '/'  → SlashCommandRegistry lookup + ISlashCommand::execute
//   '!'  → BashTool::run() directly (no model)
//   '#'  → Append text to project BATBOX.md
//   '@'  → Expand mention, then Conversation::user_message + run_turn
//   else → Conversation::user_message + run_turn
//
// Multi-line input
// ----------------
//   A line ending with '\' (backslash continuation) has the trailing backslash
//   stripped and the segment is appended to multiline_buf_ with '\n'.  The
//   accumulated buffer is dispatched when the user submits a line that does NOT
//   end with '\', or an empty line (when accumulating).
//
// Cancellation
// ------------
//   cancel() fires the active CancelSource.  A fresh source is created via
//   reset_cancel() before each dispatch_chat or dispatch_bash call.
//   The CancelToken is vended from the source via CancelToken::make_root().
//
// ConvAdapter
// -----------
//   CommandContext::conversation is a ConversationHandle& (pure virtual).  We
//   provide ConvAdapter as a local shim that forwards reset_messages(),
//   inject_user_message(), last_assistant_message(), and the CPP S.5 session
//   accessors to the real batbox::conversation::Conversation.
// =============================================================================

#include "batbox/repl/Repl.hpp"

#include <batbox/repl/CommandContext.hpp>
#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/conversation/Conversation.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/repl/Autocomplete.hpp>
#include <batbox/tools/BashTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace batbox::repl {

// =============================================================================
// ConvAdapter — ConversationHandle bridge to the real Conversation
// =============================================================================

struct Repl::ConvAdapter : public batbox::commands::ConversationHandle {
    explicit ConvAdapter(batbox::conversation::Conversation& conv,
                         Repl&                              owner)
        : conv_{conv}, owner_{owner} {}

    // -------------------------------------------------------------------------
    void reset_messages() override {
        // For /clear: drop in-memory history by swapping in an empty JSON array.
        // The full restore path (Conversation::restore) requires a SessionFile;
        // setting an empty array here signals the Conversation to clear itself.
        // Concrete App implementations override this with the real Conversation
        // clear mechanism.
        (void)this; // Intentionally a no-op at Repl level — App overrides.
    }

    // -------------------------------------------------------------------------
    void inject_user_message(std::string_view text) override {
        // User-defined slash commands call this to queue a rendered prompt body
        // as the next user turn.  We feed it directly to handle_input so the
        // REPL picks it up on the next iteration.
        owner_.feed_line(text);
    }

    // -------------------------------------------------------------------------
    std::string last_assistant_message(std::size_t n) const override {
        const auto& msgs = conv_.messages();
        std::size_t found = 0;
        // Walk newest-first to find the Nth assistant message.
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (it->role == batbox::conversation::Role::Assistant) {
                ++found;
                if (found == n) {
                    return it->content;
                }
            }
        }
        return {};
    }

    // ---- CPP S.5 session accessors (default degraded implementations) -------

    [[nodiscard]] std::string get_session_id() const override {
        return conv_.session_id();
    }

    [[nodiscard]] fs::path get_session_file_path() const override {
        return {}; // Not directly accessible via Conversation interface.
    }

    [[nodiscard]] std::size_t get_turn_count() const override {
        const auto& msgs = conv_.messages();
        std::size_t turns = 0;
        for (const auto& m : msgs) {
            if (m.role == batbox::conversation::Role::User) {
                ++turns;
            }
        }
        return turns;
    }

    [[nodiscard]] std::string get_model_name() const override {
        return {}; // Not directly accessible without Config.
    }

    [[nodiscard]] batbox::Json get_messages_json() const override {
        batbox::Json arr = batbox::Json::array();
        for (const auto& m : conv_.messages()) {
            batbox::Json obj;
            obj["role"]    = std::string(batbox::conversation::to_wire_role(m.role));
            obj["content"] = m.content;
            arr.push_back(obj);
        }
        return arr;
    }

    void set_messages_json(const batbox::Json& /*messages*/) override {
        // The App layer overrides this for /resume and /compact.  At the Repl
        // level we do not have direct access to Conversation internals.
    }

    batbox::conversation::Conversation& conv_;
    Repl&                               owner_;
};

// =============================================================================
// Constructor
// =============================================================================

Repl::Repl(batbox::conversation::Conversation&     conversation,
           batbox::commands::SlashCommandRegistry&  registry,
           batbox::tools::BashTool&                 bash_tool,
           batbox::repl::History&                   history,
           batbox::repl::Autocomplete&              autocomplete,
           fs::path                                 cwd,
           batbox::commands::CommandContext*         ctx_extras)
    : conversation_{conversation}
    , registry_{registry}
    , bash_tool_{bash_tool}
    , history_{history}
    , autocomplete_{autocomplete}
    , ctx_extras_{ctx_extras}
    , cwd_{std::move(cwd)}
    , cancel_src_{std::make_shared<CancelSource>()}
    , output_{&std::cout}
    , input_{&std::cin}
    , conv_adapter_{std::make_unique<ConvAdapter>(conversation, *this)}
{}

// Destructor defined here (not inline) so that unique_ptr<ConvAdapter> can be
// destroyed with ConvAdapter as a complete type.  If ~Repl were defaulted
// inline in the header, the compiler would attempt to instantiate
// ~unique_ptr<ConvAdapter> at the include site where ConvAdapter is only
// forward-declared, producing an incomplete-type error.
Repl::~Repl() = default;

// =============================================================================
// Public API
// =============================================================================

void Repl::set_output_stream(std::ostream& os) {
    output_ = &os;
}

void Repl::set_input_stream(std::istream& is) {
    input_ = &is;
}

void Repl::cancel() {
    if (cancel_src_) {
        cancel_src_->request_stop();
    }
}

void Repl::request_exit() {
    exit_flag_.store(true, std::memory_order_relaxed);
}

bool Repl::exit_requested() const noexcept {
    return exit_flag_.load(std::memory_order_relaxed);
}

std::function<void(std::string)> Repl::on_submit_callback() {
    return [this](std::string line) {
        handle_input(line);
    };
}

// =============================================================================
// feed_line — headless / test entry point
// =============================================================================

void Repl::feed_line(std::string_view line) {
    handle_input(line);
}

// =============================================================================
// handle_input — blueprint contract method
// =============================================================================

void Repl::handle_input(std::string_view line) {
    // ------------------------------------------------------------------
    // Multi-line continuation: line ends with backslash '\'.
    // ------------------------------------------------------------------
    if (!line.empty() && line.back() == '\\') {
        // Strip the trailing backslash and append to the accumulation buffer.
        multiline_buf_ += std::string(line.substr(0, line.size() - 1));
        multiline_buf_ += '\n';
        multiline_active_ = true;
        return; // Await more input.
    }

    // If accumulating and the user submits an empty line, terminate the block.
    if (multiline_active_) {
        if (!line.empty()) {
            multiline_buf_ += std::string(line);
        }
        std::string full = std::move(multiline_buf_);
        multiline_buf_.clear();
        multiline_active_ = false;
        // Process the assembled block (no trailing backslash).
        handle_input(full);
        return;
    }

    // ------------------------------------------------------------------
    // Push to history before dispatch (skip blank/whitespace-only lines).
    // ------------------------------------------------------------------
    {
        const bool blank = line.find_first_not_of(" \t\n\r") == std::string_view::npos;
        if (!blank) {
            history_.push(line);
        }
    }

    // ------------------------------------------------------------------
    // Prefix dispatch.
    // ------------------------------------------------------------------
    if (line.empty()) {
        return; // Nothing to dispatch.
    }

    const char first = line.front();

    if (first == '/') {
        dispatch_slash(line.substr(1));
    } else if (first == '!') {
        dispatch_bash(line.substr(1));
    } else if (first == '#') {
        dispatch_note(line.substr(1));
    } else if (first == '@') {
        dispatch_mention(line);
    } else {
        dispatch_chat(line);
    }
}

// =============================================================================
// make_command_context
// =============================================================================

batbox::commands::CommandContext Repl::make_command_context() {
    batbox::commands::CommandContext ctx{
        .output       = *output_,
        .input        = *input_,
        .conversation = *conv_adapter_,
        .registry     = registry_,
        .cwd          = cwd_,
    };

    // Copy optional subsystem pointers from ctx_extras_ if available.
    if (ctx_extras_) {
        ctx.vim_mode            = ctx_extras_->vim_mode;
        ctx.keybindings         = ctx_extras_->keybindings;
        ctx.config_dir          = ctx_extras_->config_dir;
        ctx.advisor_mode        = ctx_extras_->advisor_mode;
        ctx.sidecar_manager     = ctx_extras_->sidecar_manager;
        ctx.mcp_registry        = ctx_extras_->mcp_registry;
        ctx.usage_tracker       = ctx_extras_->usage_tracker;
        ctx.agent_supervisor    = ctx_extras_->agent_supervisor;
        ctx.permission_mode_str = ctx_extras_->permission_mode_str;
    }

    return ctx;
}

// =============================================================================
// dispatch_slash — '/' prefix
// =============================================================================

void Repl::dispatch_slash(std::string_view args_including_name) {
    // Split "name [args]" into command name and trailing arguments.
    std::string_view name;
    std::string_view args;

    const auto space_pos = args_including_name.find(' ');
    if (space_pos == std::string_view::npos) {
        name = args_including_name;
        args = {};
    } else {
        name = args_including_name.substr(0, space_pos);
        args = args_including_name.substr(space_pos + 1);
    }

    if (name.empty()) {
        // Bare '/' — list all commands as a quick hint.
        *output_ << "Available commands (type /help for details):\n";
        for (auto* cmd : registry_.all()) {
            *output_ << "  /" << cmd->name();
            if (!cmd->description().empty()) {
                *output_ << " \xe2\x80\x94 " << cmd->description();
            }
            *output_ << "\n";
        }
        return;
    }

    batbox::commands::ISlashCommand* cmd = registry_.lookup(name);
    if (!cmd) {
        *output_ << "Unknown command: /" << name
                 << "  (type /help for available commands)\n";
        return;
    }

    batbox::commands::CommandContext ctx = make_command_context();
    const auto result = cmd->execute(args, ctx);

    if (!result.has_value()) {
        *output_ << "Error: " << result.error() << "\n";
    }

    // Propagate exit request from the command (e.g. /exit).
    if (ctx.exit_requested) {
        request_exit();
    }

    // Propagate working directory changes (e.g. /init may set cwd).
    cwd_ = ctx.cwd;

    // Propagate advisor mode changes.
    if (ctx_extras_) {
        ctx_extras_->advisor_mode = ctx.advisor_mode;
    }
}

// =============================================================================
// dispatch_bash — '!' prefix
// =============================================================================

void Repl::dispatch_bash(std::string_view command) {
    // Trim leading whitespace from the command string.
    while (!command.empty() && (command.front() == ' ' || command.front() == '\t')) {
        command.remove_prefix(1);
    }

    if (command.empty()) {
        *output_ << "Usage: !<shell-command>   (e.g.  !ls -la)\n";
        return;
    }

    // Build a fresh cancel token for this execution.
    reset_cancel();
    CancelToken token = cancel_src_->token();

    // Construct JSON args matching BashTool's schema.
    batbox::Json args;
    args["command"] = std::string(command);

    // Build a minimal ToolContext (default permissions, no plan-mode).
    batbox::tools::ToolContext tool_ctx;
    tool_ctx.cwd          = cwd_;
    tool_ctx.cancel_token = std::move(token);
    tool_ctx.mode         = batbox::permissions::PermissionMode::Default;

    const batbox::tools::ToolResult result = bash_tool_.run(args, tool_ctx);

    // Print output — ensure it ends with a newline.
    *output_ << result.body;
    if (!result.body.empty() && result.body.back() != '\n') {
        *output_ << '\n';
    }

    // Reset cancel for the next operation.
    reset_cancel();
}

// =============================================================================
// dispatch_note — '#' prefix
// =============================================================================

void Repl::dispatch_note(std::string_view note_text) {
    // Strip leading whitespace from the note text.
    while (!note_text.empty() && (note_text.front() == ' ' || note_text.front() == '\t')) {
        note_text.remove_prefix(1);
    }

    if (note_text.empty()) {
        *output_ << "Usage: #<note text>   (appends to project BATBOX.md)\n";
        return;
    }

    // Find or create the nearest BATBOX.md.
    fs::path batbox_md = find_batbox_md();
    if (batbox_md.empty()) {
        // No BATBOX.md found walking up — create one in the current working dir.
        batbox_md = cwd_ / "BATBOX.md";
    }

    if (append_to_file(batbox_md, note_text)) {
        *output_ << "Note appended to " << batbox_md.string() << "\n";
    }
}

// =============================================================================
// dispatch_mention — '@' prefix
// =============================================================================

void Repl::dispatch_mention(std::string_view line) {
    // '@skill-name' or '@agent-name' at the start of the line is passed
    // through to the conversation as a user message.  The system prompt
    // already embeds skill descriptions; the model interprets '@mention' as
    // a cue to invoke the named skill or agent.
    //
    // Future expansion: resolve the mention via Autocomplete and prepend the
    // skill's content verbatim to the prompt before routing to run_turn.
    dispatch_chat(line);
}

// =============================================================================
// dispatch_chat — default (no special prefix)
// =============================================================================

void Repl::dispatch_chat(std::string_view text) {
    // Build a fresh cancel token for this inference turn.
    reset_cancel();
    CancelToken token = cancel_src_->token();

    // Append the user message to the conversation.
    conversation_.user_message(text);

    // Run one inference turn (streaming; blocks until the turn is done or cancelled).
    auto result = conversation_.run_turn(std::move(token));

    if (!result.has_value()) {
        const std::string& err = result.error();
        if (err == "cancelled") {
            *output_ << "\n[Cancelled]\n";
        } else {
            *output_ << "[Error]: " << err << "\n";
        }
    }

    // Reset cancel for the next turn.
    reset_cancel();
}

// =============================================================================
// Private helpers
// =============================================================================

fs::path Repl::find_batbox_md() const {
    // Walk up from cwd_ looking for a BATBOX.md file.
    fs::path dir = cwd_;
    for (;;) {
        const fs::path candidate = dir / "BATBOX.md";
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) {
            break; // Reached the filesystem root.
        }
        dir = parent;
    }
    return {}; // Not found.
}

bool Repl::append_to_file(const fs::path& file, std::string_view text) {
    std::error_code ec;
    // Ensure the parent directory exists.
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        *output_ << "[Error]: Cannot create directory "
                 << file.parent_path().string() << ": " << ec.message() << "\n";
        return false;
    }

    std::ofstream ofs(file, std::ios::app | std::ios::out);
    if (!ofs) {
        *output_ << "[Error]: Cannot open " << file.string() << " for writing\n";
        return false;
    }

    ofs << text << '\n';
    if (!ofs) {
        *output_ << "[Error]: Write failed to " << file.string() << "\n";
        return false;
    }

    return true;
}

void Repl::reset_cancel() {
    // Replace the active CancelSource with a fresh one.
    cancel_src_ = std::make_shared<CancelSource>();
}

} // namespace batbox::repl
