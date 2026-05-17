// src/commands/CopyCmd.cpp
//
// batbox::commands::CopyCmd — implements the /copy slash command.
//
// Copies the last (or Nth-from-last) assistant message from the live
// conversation to the system clipboard.
//
// Clipboard dispatch order:
//   1. BATBOX_CLIPBOARD_CMD env var  (test/user override)
//   2. darwin  → pbcopy
//   3. linux   → xclip -selection clipboard  →  wl-copy (fallback)
//   4. other   → Err("no clipboard tool available")
//
// Arguments:
//   (none)  → copy the most-recent assistant message (n=1)
//   N       → copy the Nth-from-last assistant message (N ≥ 1)
//
// No aliases.
//
// Registration entry point:
//   void register_copy_cmd(SlashCommandRegistry&);

#include <batbox/commands/ISlashCommand.hpp>
#include <batbox/commands/SlashCommandRegistry.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/repl/CommandContext.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace batbox::commands {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Detect the clipboard command to use.
///
/// Priority:
///   1. BATBOX_CLIPBOARD_CMD env var  (test hook — any value accepted verbatim)
///   2. macOS  → "pbcopy"
///   3. Linux  → "xclip -selection clipboard"  if xclip is on PATH
///              "wl-copy"                       if xclip is absent
///   4. other  → empty string (caller emits the error)
///
/// Returns empty string when no clipboard tool is found.
[[nodiscard]] std::string detect_clipboard_cmd() {
    // 1. Test/user override.  When the variable is set (even to empty),
    //    it takes full effect: empty means "no clipboard tool" (test suppression);
    //    non-empty means "use this command verbatim".
    if (const char* override = std::getenv("BATBOX_CLIPBOARD_CMD")) {
        // Variable is present in environment.
        if (override[0] != '\0') return std::string(override);
        return {};  // deliberately disabled
    }

    // 2. macOS.
#if defined(__APPLE__)
    return "pbcopy";
#elif defined(__linux__)
    // 3a. Try xclip.
    {
        FILE* probe = ::popen("xclip -version 2>/dev/null", "r");
        if (probe) {
            int rc = ::pclose(probe);
            if (rc == 0) return "xclip -selection clipboard";
        }
    }
    // 3b. Try wl-copy.
    {
        FILE* probe = ::popen("wl-copy --version 2>/dev/null", "r");
        if (probe) {
            int rc = ::pclose(probe);
            if (rc == 0) return "wl-copy";
        }
    }
    return "";  // neither found
#else
    return "";  // unsupported platform
#endif
}

/// Write `text` to the clipboard via `cmd` (opened with popen in write mode).
/// Returns an error string on failure, empty on success.
[[nodiscard]] std::string write_to_clipboard(const std::string& cmd,
                                             std::string_view   text)
{
    FILE* fp = ::popen(cmd.c_str(), "w");
    if (!fp) {
        return std::string("popen(\"") + cmd + "\") failed: " + std::strerror(errno);
    }

    const std::size_t written = std::fwrite(text.data(), 1, text.size(), fp);
    const int rc = ::pclose(fp);

    if (written != text.size()) {
        return std::string("fwrite truncated (") + std::to_string(written)
               + " of " + std::to_string(text.size()) + " bytes written)";
    }
    if (rc != 0) {
        return cmd + " exited with status " + std::to_string(rc);
    }
    return {};
}

/// Parse the numeric argument string `args`.
/// Returns {n, ""} on success or {0, error_message} on failure.
[[nodiscard]] std::pair<std::size_t, std::string>
parse_n(std::string_view args)
{
    // Strip leading/trailing whitespace.
    auto start = args.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {1, {}};  // no arg → n=1
    auto end = args.find_last_not_of(" \t\r\n");
    args = args.substr(start, end - start + 1);

    // Parse unsigned integer.
    std::size_t n = 0;
    auto [ptr, ec] = std::from_chars(args.data(), args.data() + args.size(), n);

    if (ec != std::errc{} || ptr != args.data() + args.size() || n == 0) {
        return {0, std::string("/copy: invalid argument '")
                   + std::string(args) + "'. Usage: /copy [N]"};
    }
    return {n, {}};
}

}  // namespace

// ---------------------------------------------------------------------------
// CopyCmd
// ---------------------------------------------------------------------------

class CopyCmd final : public ISlashCommand {
public:
    // ---- Identity -----------------------------------------------------------

    [[nodiscard]] std::string_view name() const noexcept override {
        return "copy";
    }

    [[nodiscard]] std::string_view description() const noexcept override {
        return "Copy the last assistant message (or Nth-from-last) to the clipboard.";
    }

    [[nodiscard]] std::string_view usage() const noexcept override {
        return "/copy [N]";
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

batbox::Result<void> CopyCmd::execute(
    std::string_view args,
    CommandContext&  ctx)
{
    // --- 1. Parse N -----------------------------------------------------------

    auto [n, parse_err] = parse_n(args);
    if (!parse_err.empty()) {
        return batbox::Err(std::move(parse_err));
    }

    // --- 2. Retrieve the Nth-from-last assistant message ----------------------

    const std::string text = ctx.conversation.last_assistant_message(n);
    if (text.empty()) {
        if (n == 1) {
            return batbox::Err(
                std::string("/copy: no assistant messages in this conversation."));
        }
        return batbox::Err(
            std::string("/copy: no assistant message at position ") +
            std::to_string(n) + " (conversation may be shorter).");
    }

    // --- 3. Detect clipboard tool --------------------------------------------

    const std::string clip_cmd = detect_clipboard_cmd();
    if (clip_cmd.empty()) {
        return batbox::Err(
            std::string("/copy: no clipboard tool found.\n"
                        "Install xclip or wl-copy (Linux), or set "
                        "BATBOX_CLIPBOARD_CMD to a command that reads stdin."));
    }

    // --- 4. Write to clipboard -----------------------------------------------

    const std::string err = write_to_clipboard(clip_cmd, text);
    if (!err.empty()) {
        return batbox::Err("/copy: clipboard write failed — " + err);
    }

    // --- 5. Confirm ----------------------------------------------------------

    ctx.output << "Copied " << text.size() << " byte"
               << (text.size() == 1 ? "" : "s") << " to clipboard";
    if (n > 1) ctx.output << " (message " << n << " from last)";
    ctx.output << ".\n";

    return {};
}

// ---------------------------------------------------------------------------
// Registration function
// ---------------------------------------------------------------------------

void register_copy_cmd(SlashCommandRegistry& registry) {
    auto res = registry.register_command(std::make_shared<CopyCmd>());
    (void)res;
}

}  // namespace batbox::commands
