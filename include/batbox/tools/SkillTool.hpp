// include/batbox/tools/SkillTool.hpp
// =============================================================================
// batbox::tools::SkillTool — ITool that invokes a named skill loaded by
// batbox::plugins::SkillLoader.
//
// Contract (CPP 5.19 blueprint):
//   name()            = "Skill"
//   is_read_only()    = false  (skill body may cause side-effecting tool calls)
//   requires_confirmation() = false  (skill bodies are user-authored)
//
//   JSON args:
//     name   (string, required)   — canonical skill name to invoke
//     input  (string, optional)   — user-supplied argument; replaces every
//                                   occurrence of $ARGS in the skill body
//
//   Behaviour:
//     1. Validate args.name is present and non-empty.
//     2. Look up the skill in the injected SkillLoader.
//        If not found: return ToolResult::error("Unknown skill: <name>").
//     3. Substitute $ARGS with args.input (or "" when input is absent)
//        in the skill's prompt_body.
//     4. Build structured_payload:
//          {
//            "skill_name":    <name>,
//            "allowed_tools": [...] | null,   // null when list is empty
//            "model":         <str> | null     // null when not set
//          }
//     5. Return ToolResult::ok(rendered_body, payload).
//        The caller is responsible for injecting the body as a user message.
//
// SkillLoader ownership:
//   SkillTool holds a non-owning reference to a SkillLoader.  The loader must
//   outlive the tool.  This mirrors the pattern used by ToolRegistry (tools
//   reference external state via references rather than shared_ptr to keep
//   allocation and lifetime management simple).
//
// Blueprint contract: batbox::tools::SkillTool (CPP 5.19)
// =============================================================================

#pragma once

#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/tools/ITool.hpp>

namespace batbox::tools {

// =============================================================================
// SkillTool
// =============================================================================

class SkillTool final : public ITool {
public:
    /// Construct a SkillTool backed by the given SkillLoader.
    ///
    /// @param loader  Reference to the application's SkillLoader.
    ///                The loader must outlive this SkillTool instance.
    explicit SkillTool(batbox::plugins::SkillLoader& loader);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------
    [[nodiscard]] std::string_view name()        const override;
    [[nodiscard]] std::string_view description() const override;
    [[nodiscard]] Json             schema_json() const override;

    // -------------------------------------------------------------------------
    // ITool execution
    // -------------------------------------------------------------------------
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// Skills are user-authored but may invoke side-effecting tools; not
    /// read-only.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// Skills are user-created content; no additional confirmation prompt is
    /// needed beyond whatever the invoked tools themselves require.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    batbox::plugins::SkillLoader& loader_;
};

} // namespace batbox::tools
