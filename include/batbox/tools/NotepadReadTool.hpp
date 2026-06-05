// include/batbox/tools/NotepadReadTool.hpp
//
// batbox::tools::NotepadReadTool — the notepad READ/GREP tool (DIS-981, S6).
//
//   Tool name : "notepad_read"
//   Arguments : args["query"]     — string, optional. Case-insensitive
//                                    substring; returns only matching entries
//                                    ("what did I note about auth?"). Omit to
//                                    return the whole pad.
//               args["max_chars"] — integer, optional. Output budget
//                                    (default 8192). Keeps a large pad bounded.
//
// The pad is itself a tool-subagent target: the agent queries it without
// re-reading the whole thing.  Because every tool routes through the S7
// dispatch envelope, a large pad read is automatically eligible for S1+S4
// distillation — that comes for free; this tool does not special-case it.
//
//   is_read_only()          == true   (never mutates the pad)
//   requires_confirmation() == false  (a read needs no prompt)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/NotepadStore.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <string_view>

namespace batbox::tools {

class NotepadReadTool final : public ITool {
public:
    /// Construct against the default pad root, or an explicit `root` (tests).
    explicit NotepadReadTool(std::filesystem::path root = {}) : store_(std::move(root)) {}

    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json schema_json() const override;
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    [[nodiscard]] bool is_read_only() const override { return true; }
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    NotepadStore store_;
};

} // namespace batbox::tools
