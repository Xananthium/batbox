// include/batbox/tools/NotepadAppendTool.hpp
//
// batbox::tools::NotepadAppendTool — the notepad WRITE tool (DIS-981, S6).
//
//   Tool name : "notepad_append"
//   Arguments : args["note"]    — string, required, non-empty. The nugget to jot.
//               args["section"] — string, optional. A light "## <header>" label.
//
// The agent calls this by its own hand to jot a nugget worth keeping into its
// working notepad: a running-plan line, a distilled finding, a decision-and-why.
// LEAST_FORCE: jot the nugget, not the transcript — the tool appends exactly
// what the caller passes, never the surrounding context.
//
// Append semantics (never overwrite).  The pad is out-of-band (NOT a Message),
// so what is jotted here survives compaction by construction and is re-injected
// each turn as a tail reminder.
//
//   is_read_only()          == false  (mutates the pad)
//   requires_confirmation() == false  (jotting a note needs no prompt)
//
// Session keying mirrors TodoWriteTool: session_id when present, else agent_id,
// else "default" — one pad per task/session.

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/tools/NotepadStore.hpp>
#include <batbox/core/Json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace batbox::tools {

class NotepadAppendTool final : public ITool {
public:
    /// Construct against the default pad root, or an explicit `root` (tests).
    explicit NotepadAppendTool(std::filesystem::path root = {}) : store_(std::move(root)) {}

    [[nodiscard]] std::string_view name() const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json schema_json() const override;
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    [[nodiscard]] bool is_read_only() const override { return false; }
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    NotepadStore store_;
};

} // namespace batbox::tools
