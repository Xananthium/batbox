// include/batbox/tools/ReportGoldTool.hpp
//
// batbox::tools::ReportGoldTool — the structured final-output contract a
// distillation subagent emits its golden line through (S4, DIS-980).
//
// Ported from goose's final_output_tool.rs::FinalOutputTool.  The decisive
// property: the distillation subagent DECLARES its result through a dedicated
// tool call — report_gold(answer, [confidence], [follow_up_ok]) — rather than
// the parent heuristically extracting "the golden line" from free text.  That
// removes the extract_response_text fragility the DIS-926 report flags as an
// anti-pattern: the handoff is a contract, not a parse.
//
// How it is used (closed mode, S4):
//   The SubagentDistiller injects this tool's schema_json() as the ONLY tool in
//   a one-shot ChatRequest and pins tool_choice to it, so the local model is
//   FORCED to answer through report_gold.  The distiller then parses the
//   returned tool_call arguments via ReportGoldTool::parse() — the same code
//   that backs run() — so there is a single source of truth for the shape.
//
// report_gold is the distiller's internal contract; it is NOT registered into
// the main agent's tool surface (the main model never calls it), so wiring it
// here does not change the curated 39-tool registry.
//
// Blueprint: implements batbox::tools::ITool.

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/tools/ITool.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// ReportGold — the parsed report_gold payload
// =============================================================================

/// The structured result a distillation subagent declares.
struct ReportGold {
    /// The distilled golden line — the answer the big output was meant to
    /// produce.  Required and non-empty for a valid report_gold call.
    std::string answer;

    /// Optional subagent self-rated confidence in [0,1].
    std::optional<double> confidence;

    /// Optional hint that this output is worth keeping warm for follow-up
    /// questions.  This is a SIGNAL INTO THE FUTURE closed-vs-standing
    /// selection decision (S2/S3) — it is captured but deliberately NOT acted
    /// upon in S1+S4 (closed mode always discards the window).
    std::optional<bool> follow_up_ok;
};

// =============================================================================
// ReportGoldTool
// =============================================================================

class ReportGoldTool final : public ITool {
public:
    // -- ITool surface -------------------------------------------------------

    /// "report_gold"
    [[nodiscard]] std::string_view name() const override;

    [[nodiscard]] std::string_view description() const override;

    /// The OpenAI tools[*].function object: report_gold(answer, confidence?,
    /// follow_up_ok?) with "answer" required.
    [[nodiscard]] Json schema_json() const override;

    /// Parse args and surface the structured result.  On a valid call returns
    /// ToolResult::ok(answer, {answer, confidence?, follow_up_ok?}); on an
    /// invalid call (missing/empty/non-string answer) returns ToolResult::error.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    /// report_gold only declares a result — it reads/mutates nothing.
    [[nodiscard]] bool is_read_only() const override { return true; }

    /// Never prompt — it is the subagent's structured handoff, not a side effect.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

    // -- Shared parser (single source of truth for the shape) ----------------

    /// Parse a report_gold arguments object into a ReportGold.
    ///
    /// Returns std::nullopt when @p args is not an object, or "answer" is
    /// absent / not a string / empty.  The distiller treats nullopt as
    /// "report_gold was not validly called" and falls back to the raw output.
    [[nodiscard]] static std::optional<ReportGold> parse(const Json& args);
};

} // namespace batbox::tools
