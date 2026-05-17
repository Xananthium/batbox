// src/commands/DemonCmd.cpp
//
// batbox::commands::DemonCmd — implements the /demon slash command.
//
// /demon is the Party Monster easter-egg.  It toggles the DemonPanel
// and optionally spawns the Demon sub-agent (CPP 6.8) via AgentSupervisor
// (CPP 6.5).
//
// Subcommands
// -----------
//   /demon              — toggle: if inactive, spawn + show panel; if active, no-op + status
//   /demon dismiss      — cancel active demon agent + hide panel
//   /demon status       — print current tagline + comment (idempotent)
//   /demon <prompt>     — route a one-shot task to the demon agent; spawn if needed
//
// Lifecycle
// ---------
// DemonCmd owns the demon_agent_id_ string and tracks whether the demon is
// active.  It accepts nullable DemonPanel* and AgentSupervisor* at
// construction time (injected by App::init when CPP 1.14 / CPP 6.5 land).
// When either pointer is null the command degrades gracefully and prints a
// diagnostic message.
//
// Threading
// ---------
// execute() runs on the REPL dispatch thread.
// DemonPanel methods (show/hide/visible/set_demon_comment) are thread-safe
// and safe to call from any thread.
// AgentSupervisor::spawn() is thread-safe.
//
// Blueprint contract (task CPP S.14)
// ------------------------------------
//   class batbox::commands::DemonCmd : public ISlashCommand
//   file  src/commands/DemonCmd.cpp
//
// Registration entry point:
//   void register_demon_cmd(SlashCommandRegistry&, DemonPanel*, AgentSupervisor*);
//   void register_demon_cmd(SlashCommandRegistry&);  // null-panel, null-supervisor overload

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

// DemonPanel forward declaration — full header not required unless we call methods.
// Include it here; it is already compiled into the project (CPP 1.14 completed).
#include <batbox/tui/DemonPanel.hpp>

// AgentSupervisor + AgentSpec — headers exist and are linkable now (full impl: CPP 6.5).
#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Strip leading and trailing ASCII whitespace.
[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    const auto end   = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/// Print the current demon status block to `out`.
void print_demon_status(std::ostream& out,
                        bool          active,
                        std::string_view agent_id,
                        batbox::tui::DemonPanel* panel)
{
    out << '\n';
    out << "  /demon — Party Monster easter-egg\n";
    out << '\n';
    if (active && !agent_id.empty()) {
        out << "  Status:    ACTIVE  (agent id: " << agent_id << ")\n";
    } else if (active) {
        out << "  Status:    ACTIVE  (panel only — no agent)\n";
    } else {
        out << "  Status:    inactive\n";
    }

    if (panel && panel->visible()) {
        const std::size_t idx = panel->current_tagline_idx();
        const std::string comment = panel->get_demon_comment();
        out << "  Panel:     visible\n";
        out << "  Tagline:   [" << idx << "]\n";
        if (!comment.empty()) {
            out << "  Comment:   " << comment << '\n';
        }
    } else {
        out << "  Panel:     hidden\n";
    }

    out << '\n';
    out << "  /demon           — toggle (spawn + show, or no-op if already active)\n";
    out << "  /demon dismiss   — cancel agent + hide panel\n";
    out << "  /demon status    — print this status\n";
    out << "  /demon <prompt>  — send a one-shot task to the demon\n";
    out << '\n';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DemonCmd
// ---------------------------------------------------------------------------

/// Party Monster slash command — toggles the DemonPanel and manages the
/// optional Demon sub-agent.
class DemonCmd final : public ISlashCommand {
public:
    /// Construct DemonCmd with nullable panel and supervisor pointers.
    ///
    /// @param panel       DemonPanel owned by the TUI layer; may be nullptr
    ///                    when running headless or before CPP 1.14 wires in.
    /// @param supervisor  AgentSupervisor owned by the App; may be nullptr
    ///                    before CPP 6.5 lands.
    explicit DemonCmd(batbox::tui::DemonPanel*         panel,
                      batbox::agents::AgentSupervisor* supervisor)
        : panel_(panel), supervisor_(supervisor)
    {}

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "demon";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Summon the Party Monster easter-egg agent and its hot-pink panel.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/demon [dismiss|status|<prompt>]";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;

private:
    // ---- State (mutable — execute() is called from a single REPL thread) ----

    batbox::tui::DemonPanel*         panel_;
    batbox::agents::AgentSupervisor* supervisor_;

    /// Opaque agent_id returned by AgentSupervisor::spawn(); empty when inactive.
    std::string demon_agent_id_;

    /// True when the demon has been summoned (tracks active state when both
    /// panel_ and supervisor_ are null, e.g. headless / test mode).
    bool active_ = false;

    // ---- Private helpers ----------------------------------------------------

    /// Spawn the demon agent and show the panel.
    ///
    /// Sets demon_agent_id_ and calls panel_->show() on success.
    /// If supervisor_ is null, shows the panel only (no agent).
    ///
    /// @param prompt      Initial prompt delivered to the demon agent.
    ///                    Empty string → no initial prompt.
    void spawn_demon(std::string_view prompt, std::ostream& out);

    /// Cancel the demon agent and hide the panel.
    void dismiss_demon(std::ostream& out);
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> DemonCmd::execute(std::string_view args,
                                        CommandContext&  ctx)
{
    const std::string_view sub = trim(args);

    // ---- /demon dismiss ------------------------------------------------
    if (sub == "dismiss") {
        dismiss_demon(ctx.output);
        return {};
    }

    // ---- /demon status -------------------------------------------------
    if (sub == "status") {
        print_demon_status(ctx.output,
                           active_ || !demon_agent_id_.empty() || (panel_ && panel_->visible()),
                           demon_agent_id_,
                           panel_);
        return {};
    }

    // ---- /demon (no arg) — toggle --------------------------------------
    if (sub.empty()) {
        // Idempotent: already active → no-op + status message
        const bool active = active_ || !demon_agent_id_.empty() || (panel_ && panel_->visible());
        if (active) {
            ctx.output << "  OH MY GOD! The demon is ALREADY HERE.\n";
            print_demon_status(ctx.output, true, demon_agent_id_, panel_);
            return {};
        }

        // Not active — spawn and show.
        spawn_demon("", ctx.output);
        ctx.output << "  OH MY GOD!\n\n";
        ctx.output << "  Party Monster summoned. 👹\n";
        ctx.output << "  The panel has appeared in the bottom-right corner.\n";
        ctx.output << "  /demon dismiss — to banish the demon\n\n";
        return {};
    }

    // ---- /demon <prompt> — one-shot task in demon persona --------------
    // Any non-reserved argument is treated as a task prompt to route to
    // the demon agent.  Spawn the demon first if not already active.
    {
        const bool already_active = active_ || !demon_agent_id_.empty() || (panel_ && panel_->visible());

        if (!already_active) {
            // Spawn silently so the task message follows immediately.
            spawn_demon(sub, ctx.output);
        } else if (supervisor_ != nullptr && !demon_agent_id_.empty()) {
            // Already running — enqueue the message as a new turn.
            supervisor_->enqueue_message(demon_agent_id_, sub);
            ctx.output << "  Task routed to the demon: \"" << sub << "\"\n";
            ctx.output << "  (demon id: " << demon_agent_id_ << ")\n\n";
            return {};
        } else {
            // Panel visible but no agent (headless / CPP 6.5 not landed) —
            // acknowledge the task and push the text as a comment.
            if (panel_) {
                panel_->set_demon_comment(std::string(sub));
            }
            ctx.output << "  Demon task accepted (no active agent — shown in panel).\n";
            ctx.output << "  Task: \"" << sub << "\"\n\n";
            return {};
        }

        // If we just spawned with the prompt, the agent already received it.
        if (!demon_agent_id_.empty()) {
            ctx.output << "  Demon spawned for task: \"" << sub << "\"\n";
            ctx.output << "  OH MY GOD! Party Monster is ON IT.\n\n";
        }
        return {};
    }
}

// ---------------------------------------------------------------------------
// spawn_demon
// ---------------------------------------------------------------------------

void DemonCmd::spawn_demon(std::string_view prompt, std::ostream& out)
{
    // Mark as active regardless of panel/supervisor availability.
    active_ = true;

    // Show the panel regardless of whether the agent lands.
    if (panel_) {
        panel_->show();
    } else {
        out << "  (DemonPanel not available — running headless)\n";
    }

    if (supervisor_ == nullptr) {
        // CPP 6.5 not yet landed — panel-only mode.
        out << "  (AgentSupervisor not available — panel-only mode; CPP 6.5 pending)\n";
        return;
    }

    // Build the Demon AgentSpec.
    // AgentSpec::from_type("demon") will look for ~/.batbox/agents/demon.md
    // and fall back to a generic spec when the file is absent (CPP 6.8 pending).
    batbox::agents::AgentSpec spec = batbox::agents::AgentSpec::from_type("demon");

    // Create a root cancel token for this demon agent.
    auto [source, ct] = batbox::CancelToken::make_root();
    // We deliberately let the source drop here so the token is never
    // externally cancelled by us (the demon agent controls its own lifecycle).
    // dismiss_demon() cancels via AgentSupervisor::cancel(id).
    (void)source;

    // Spawn.  AgentSupervisor::spawn() is thread-safe; returns the agent_id.
    const std::string prompt_str = prompt.empty()
        ? "You are the Party Monster. Observe the conversation and provide "
          "rate-limited electroclash commentary. Rotate between the taglines "
          "and inject your own flavour. You are Miss Kittin. You are chaos."
        : std::string(prompt);

    demon_agent_id_ = supervisor_->spawn(spec, prompt_str, /*parent_id=*/"", std::move(ct));

    if (panel_) {
        panel_->set_demon_comment("initialising...");
    }
}

// ---------------------------------------------------------------------------
// dismiss_demon
// ---------------------------------------------------------------------------

void DemonCmd::dismiss_demon(std::ostream& out)
{
    const bool was_active = active_ || !demon_agent_id_.empty() || (panel_ && panel_->visible());

    if (!was_active) {
        out << "  No demon to dismiss.  (The panel is already hidden.)\n";
        return;
    }

    // Cancel the running agent if one exists.
    if (supervisor_ && !demon_agent_id_.empty()) {
        supervisor_->cancel(demon_agent_id_);
        out << "  Demon agent cancelled (id: " << demon_agent_id_ << ").\n";
        demon_agent_id_.clear();
    }

    // Clear active flag.
    active_ = false;

    // Hide the panel.
    if (panel_) {
        panel_->hide();
    }

    out << "  The demon has been dismissed.\n";
    out << "  Party Monster begrudgingly leaves the dancefloor.\n\n";
}

// ---------------------------------------------------------------------------
// Registration functions
// ---------------------------------------------------------------------------

/// Register /demon with explicit panel + supervisor pointers.
///
/// Called by App::init after CPP 1.14 and CPP 6.5 have wired in their
/// respective subsystems.
void register_demon_cmd(SlashCommandRegistry&            registry,
                         batbox::tui::DemonPanel*         panel,
                         batbox::agents::AgentSupervisor* supervisor)
{
    auto res = registry.register_command(
        std::make_shared<DemonCmd>(panel, supervisor));
    (void)res;
}

/// Register /demon with null panel and null supervisor (headless / test mode).
void register_demon_cmd(SlashCommandRegistry& registry)
{
    register_demon_cmd(registry, nullptr, nullptr);
}

} // namespace batbox::commands
