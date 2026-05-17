// src/commands/CompactCmd.cpp
//
// batbox::commands::CompactCmd — implements the /compact slash command.
//
// /compact manually triggers context compaction by:
//   1. Retrieving the current message list via
//      ctx.conversation.get_messages_json().
//   2. Deserialising each JSON message object into
//      batbox::conversation::Message via batbox::conversation::from_json().
//   3. Building an inference::Client from Config::load_default() so the
//      summarisation call uses the user's configured API key and endpoint.
//   4. Constructing a Compactor with keep_last_n_turns_verbatim from config.
//   5. Calling Compactor::compact(messages, client, ct).
//   6. Serialising the compacted messages back to JSON via to_json() and
//      installing them via ctx.conversation.set_messages_json().
//   7. Writing the status note from the Compactor callback to ctx.output.
//
// If the conversation is too short to compact (head would be empty) the
// Compactor returns the original messages unchanged; /compact reports
// "Nothing to compact" in that case.
//
// No aliases.
//
// Registration entry point:
//   void register_compact_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/config/Config.hpp>
#include <batbox/conversation/Compactor.hpp>
#include <batbox/conversation/Message.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// CompactCmd
// ---------------------------------------------------------------------------

class CompactCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "compact";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Manually compact the conversation context, summarising older turns.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/compact";
    }

    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {};
    }

    [[nodiscard]] bool requires_args() const noexcept override { return false; }

    // ---- Execute ------------------------------------------------------------

    [[nodiscard]] batbox::Result<void> execute(
        std::string_view args,
        CommandContext&  ctx) override;
};

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

batbox::Result<void> CompactCmd::execute(
    std::string_view /*args*/,
    CommandContext&  ctx)
{
    // --- 1. Get the current message list as JSON. ----------------------------

    const batbox::Json messages_json = ctx.conversation.get_messages_json();

    if (!messages_json.is_array() || messages_json.empty()) {
        ctx.output << "Nothing to compact: conversation is empty.\n";
        return {};
    }

    // --- 2. Deserialise JSON array → vector<Message>. -----------------------
    //
    // batbox::conversation::from_json() throws std::invalid_argument on
    // malformed input; wrap in try/catch and convert to Err.

    std::vector<batbox::conversation::Message> messages;
    messages.reserve(messages_json.size());

    try {
        for (const auto& item : messages_json) {
            messages.push_back(batbox::conversation::from_json(item));
        }
    } catch (const std::invalid_argument& ex) {
        return batbox::Err(
            std::string("/compact: malformed message in conversation: ") +
            ex.what());
    } catch (const std::exception& ex) {
        return batbox::Err(
            std::string("/compact: unexpected error deserialising messages: ") +
            ex.what());
    }

    // --- 3. Load config and build the inference client. ---------------------

    batbox::config::Config cfg = batbox::config::Config::load_default();

    if (cfg.api.api_key.empty()) {
        return batbox::Err(
            std::string("/compact: BATBOX_API_KEY is not set. "
                        "Cannot call the inference API for summarisation."));
    }

    batbox::inference::Client client{cfg};

    // --- 4. Build Compactor using keep_last_n_turns_verbatim from config. ---

    std::string status_note;
    batbox::conversation::Compactor compactor{
        cfg.compact.keep_last_n_turns_verbatim,
        [&status_note](const std::string& note) {
            status_note = note;
        }
    };

    // --- 5. Compact. ---------------------------------------------------------

    const std::size_t original_count = messages.size();
    batbox::CancelToken ct;  // non-cancelled token; user can interrupt via Ctrl-C

    auto compact_res = compactor.compact(messages, client, std::move(ct));
    if (!compact_res.has_value()) {
        return batbox::Err("/compact: compaction failed: " + compact_res.error());
    }

    const auto& compacted = compact_res.value();

    // Compactor no-ops when the head is empty (conversation too short to split).
    if (compacted.size() == original_count) {
        ctx.output << "Nothing to compact: conversation is too short "
                   << "(fewer than " << (cfg.compact.keep_last_n_turns_verbatim + 1)
                   << " turns).\n";
        return {};
    }

    // --- 6. Serialise compacted messages → JSON and install. ----------------

    batbox::Json compacted_json = batbox::Json::array();
    for (const auto& msg : compacted) {
        compacted_json.push_back(batbox::conversation::to_json(msg));
    }

    ctx.conversation.set_messages_json(compacted_json);

    // --- 7. Report status to output. ----------------------------------------

    if (!status_note.empty()) {
        ctx.output << status_note << "\n";
    } else {
        ctx.output << "Context compacted: " << original_count
                   << " messages \xe2\x86\x92 "   // UTF-8 →
                   << compacted.size() << " messages.\n";
    }

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_compact_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<CompactCmd>());
    (void)res;
}

} // namespace batbox::commands
