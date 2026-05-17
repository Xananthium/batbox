// src/commands/AgentsCmd.cpp
//
// batbox::commands::AgentsCmd — implements the /agents slash command.
//
// /agents lists all active (and recently-completed) sub-agents by calling
// AgentSupervisor::snapshot() and rendering a tabular report to ctx.output.
//
// Each row shows:
//   id (truncated), name, status, current_step, token count
//
// When no AgentSupervisor is wired in (headless / test mode) the command
// reports that no agent runtime is available.
//
// Registration entry point:
//   void register_agents_cmd(SlashCommandRegistry&, batbox::agents::AgentSupervisor*);
//   void register_agents_cmd(SlashCommandRegistry&);   // supervisor = nullptr

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Truncate a string to at most `n` characters, appending "…" (UTF-8) when
/// truncated.  Returns the string unchanged when len <= n.
[[nodiscard]] std::string truncate(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    // Replace the last 3 bytes of the truncated portion with "..."
    if (n <= 3) return s.substr(0, n);
    return s.substr(0, n - 3) + "...";
}

/// Map a lifecycle status string to a short display symbol.
[[nodiscard]] std::string_view status_symbol(const std::string& status) {
    if (status == "running")   return "[>]";
    if (status == "queued")    return "[.]";
    if (status == "completed") return "[ok]";
    if (status == "cancelled") return "[x]";
    if (status == "errored")   return "[!]";
    return "[?]";
}

/// Render a snapshot table.  Writes the formatted table to `out`.
void render_snapshot(std::ostream& out,
                     const std::vector<batbox::agents::AgentSnapshot>& snaps)
{
    if (snaps.empty()) {
        out << "  No agents registered in this session.\n";
        return;
    }

    // Column widths.
    constexpr std::size_t kIdWidth   = 8;   // first 8 chars of UUID
    constexpr std::size_t kNameWidth = 20;
    constexpr std::size_t kStatWidth = 12;  // "[completed]" is 11
    constexpr std::size_t kStepWidth = 30;
    constexpr std::size_t kTokWidth  = 8;

    // Header.
    out << '\n';
    out << "  " << std::left
        << std::setw(kIdWidth)   << "ID"
        << "  " << std::setw(kNameWidth)  << "NAME"
        << "  " << std::setw(kStatWidth)  << "STATUS"
        << "  " << std::setw(kStepWidth)  << "CURRENT STEP"
        << "  " << std::setw(kTokWidth)   << "TOKENS"
        << '\n';
    out << "  " << std::string(kIdWidth,   '-')
        << "  " << std::string(kNameWidth, '-')
        << "  " << std::string(kStatWidth, '-')
        << "  " << std::string(kStepWidth, '-')
        << "  " << std::string(kTokWidth,  '-')
        << '\n';

    for (const auto& snap : snaps) {
        // Truncate id to first 8 chars.
        const std::string id_str = snap.id.size() > kIdWidth
                                   ? snap.id.substr(0, kIdWidth)
                                   : snap.id;

        const std::string stat_str =
            std::string(status_symbol(snap.status)) + " " + snap.status;

        out << "  " << std::left
            << std::setw(kIdWidth)   << id_str
            << "  " << std::setw(kNameWidth)  << truncate(snap.name, kNameWidth)
            << "  " << std::setw(kStatWidth)  << truncate(stat_str, kStatWidth)
            << "  " << std::setw(kStepWidth)  << truncate(snap.current_step, kStepWidth)
            << "  " << std::setw(kTokWidth)   << snap.token_count
            << '\n';

        // Print last output lines if any are available.
        for (const auto& line : snap.last_5_lines) {
            out << "    " << truncate(line, 76) << '\n';
        }
    }

    // Summary counts.
    std::size_t running = 0, queued = 0, done = 0;
    for (const auto& s : snaps) {
        if (s.status == "running")   ++running;
        else if (s.status == "queued") ++queued;
        else                          ++done;
    }
    out << '\n';
    out << "  " << snaps.size() << " agent(s)"
        << "  running: " << running
        << "  queued: "  << queued
        << "  done: "    << done
        << '\n';
    out << '\n';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AgentsCmd
// ---------------------------------------------------------------------------

class AgentsCmd final : public ISlashCommand {
public:
    /// Construct with an optional live AgentSupervisor pointer.
    /// When supervisor is nullptr the command runs in headless mode and
    /// reports that no agent runtime is available.
    explicit AgentsCmd(batbox::agents::AgentSupervisor* supervisor = nullptr)
        : supervisor_(supervisor) {}

    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "agents";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "List active sub-agents: status, current step, and token usage.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/agents";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view   /*args*/,
        CommandContext&    ctx) override;

private:
    batbox::agents::AgentSupervisor* supervisor_;  ///< Non-owning; may be null.
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> AgentsCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    if (supervisor_ == nullptr) {
        ctx.output << "  /agents: no agent supervisor available in this context.\n";
        ctx.output << "  Sub-agent management requires the full REPL runtime (CPP A.3).\n";
        return {};
    }

    const std::vector<batbox::agents::AgentSnapshot> snaps = supervisor_->snapshot();
    render_snapshot(ctx.output, snaps);
    return {};
}

// ---------------------------------------------------------------------------
// Registration functions
// ---------------------------------------------------------------------------

void register_agents_cmd(SlashCommandRegistry&            registry,
                          batbox::agents::AgentSupervisor* supervisor)
{
    auto res = registry.register_command(
        std::make_shared<AgentsCmd>(supervisor));
    (void)res;
}

void register_agents_cmd(SlashCommandRegistry& registry) {
    register_agents_cmd(registry, nullptr);
}

} // namespace batbox::commands
