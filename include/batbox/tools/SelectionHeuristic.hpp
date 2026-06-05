// include/batbox/tools/SelectionHeuristic.hpp
//
// batbox::tools::ISelectionHeuristic + ShapeSelectionHeuristic — the
// pre-dispatch investigation-vs-lookup classifier (DIS-1007, the selection
// heuristic / step-8 of the DIS-926 build order).
//
// THE DECISION (decision A — "predict-ahead, confirm-after"):
//   The S7 envelope routes every engulfed tool result through the single
//   IResultDistiller slot.  StandingSelector (SelectionHeuristic's consumer)
//   wraps the closed one-shot SubagentDistiller and adds the closed-vs-standing
//   branch.  This classifier is the PREDICT-AHEAD half: BEFORE paying for any
//   inference it predicts, from tool semantics + result shape, whether the work
//   is a LOOKUP (a single resolved fact → CLOSED, discard the window) or an
//   INVESTIGATION (a broad dig → STANDING, keep a warm window for follow-ups).
//   The CONFIRM-AFTER half (report_gold.follow_up_ok / confidence) lives in
//   StandingSelector and runs AFTER the standing first turn.
//
// WHY a separate, named classifier (not inlined into StandingSelector):
//   The investigation-vs-lookup judgment IS the ownable novelty (DIS-926 step 8).
//   Making it a small, pure, side-effect-free component with an enum output makes
//   the judgment independently testable and the signal set auditable in one place
//   — exactly the "small, named, testable component" the issue asks for.
//
// ANTI-PATTERN #3 (DIS-926 "Do NOT steal" list): the harness INFERS the mode;
//   the caller NEVER flags it.  There is deliberately NO `background`/`standing`
//   boolean on any tool-call surface.  classify() takes only what the dispatch
//   seam already has — tool_name, args, result — and never a caller-supplied
//   mode hint.  (goose's `"background"` boolean on the delegate tool is the
//   anti-pattern this avoids: the choice is the harness's, not the caller's.)
//
// ===========================================================================
// The investigation signal set (the judgment), with rationale per signal
// ===========================================================================
// Two independent inputs, OR-combined into "investigation":
//
//   (1) TOOL SEMANTICS — tool identity IS a legitimate input here.  This is a
//       DIFFERENT decision from S1's engulf trigger (ThresholdEngulfDecider),
//       where the rule was deliberately "size, not identity": S1 asks "is this
//       too big to inline?" (a cost question — identity-blind), whereas this
//       asks "is this output likely the START of a dig the parent will want to
//       follow up on?" (a semantics question — identity is the cheapest, most
//       reliable predictor).  Multi-hit / broad-search tools and large-surface
//       readers read as INVESTIGATION; a single resolved fact reads as LOOKUP.
//
//       The tool-name list is intentionally SMALL and each entry is justified:
//         - "grep"        : a pattern search across a corpus → many hits, the
//                           classic "what else mentions X?" follow-up shape.
//         - "glob"        : a filesystem pattern match → a set of paths to dig
//                           through, not one resolved answer.
//         - "web_search"  : a query returning ranked results → inherently a
//                           broad survey the parent narrows afterwards.
//         - "web_fetch"   : pulls a whole page/document → a large surface the
//                           parent asks follow-ups against ("what did it say
//                           about pricing?").
//       Tools NOT in the list (e.g. "read_file" of a small file, "config_get",
//       a single "task_get") read as LOOKUP by tool semantics and only become
//       investigation via the result-shape signal (2) below — so a tool's
//       identity can PROMOTE it to investigation but never DEMOTE a genuinely
//       broad result.
//
//   (2) RESULT SHAPE — identity-independent corroboration: a result whose body
//       is large OR carries many distinct sections (many newline-delimited
//       hits / records) reads as INVESTIGATION; a single short resolved value
//       reads as LOOKUP.  This catches broad results from tools NOT on the
//       name list (a custom MCP search tool, a big aggregated read) without
//       hard-coding every possible tool.  `ToolResult` itself has no tool
//       field — tool identity arrives via the tool_name parameter, matching the
//       envelope's IResultDistiller::distill signature.
//
// The classifier is PURE and CHEAP (one string compare set + two size checks);
// it must be safe to run on the dispatch thread for every engulfed result.

#pragma once

#include <batbox/core/Json.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <cstddef>
#include <string_view>

namespace batbox::tools {

// =============================================================================
// DispatchMode — the classifier's verdict
// =============================================================================

/// The predicted dispatch path for an engulfed tool result.
enum class DispatchMode {
    /// LOOKUP — a single resolved fact.  Pay the cheap one-shot closed distiller
    /// and discard the window (the LEAST_FORCE default).
    Closed,

    /// INVESTIGATION — a broad dig.  Pay for a warm standing subagent window so
    /// the parent can follow up without re-engulfing the source.
    Standing,
};

/// Human-readable label for a DispatchMode (logs / tests).
[[nodiscard]] constexpr std::string_view dispatch_mode_label(DispatchMode m) noexcept {
    return m == DispatchMode::Standing ? "standing" : "closed";
}

// =============================================================================
// ISelectionHeuristic — the predict-ahead classifier interface
// =============================================================================

/// "Predict, before paying for inference, whether this engulfed tool result is
/// a LOOKUP (closed) or an INVESTIGATION (standing)."  Pure-virtual so the
/// signal set can evolve (or be swapped in tests) without touching
/// StandingSelector.  Implementations MUST be cheap and side-effect free — they
/// run on the dispatch thread for every engulfed result.
class ISelectionHeuristic {
public:
    virtual ~ISelectionHeuristic() = default;

    /// @param tool_name  Canonical name of the tool that produced the result.
    /// @param args       The parsed JSON arguments the tool was dispatched with.
    /// @param result     The ToolResult about to be distilled.
    /// @returns DispatchMode::Standing on a positive investigation signal,
    ///          DispatchMode::Closed otherwise.  NEVER reads a caller-supplied
    ///          mode flag (anti-pattern #3) — only tool semantics + result shape.
    [[nodiscard]] virtual DispatchMode classify(std::string_view  tool_name,
                                                const Json&       args,
                                                const ToolResult& result) const = 0;

protected:
    ISelectionHeuristic() = default;
};

// =============================================================================
// ShapeSelectionHeuristic — the concrete tool-semantics + result-shape classifier
// =============================================================================

/// Default ISelectionHeuristic: investigation iff (tool is a known broad-search
/// tool) OR (the result body is large OR carries many distinct sections).
/// Errors are never investigations (an error must surface verbatim; there is
/// nothing to keep warm).
class ShapeSelectionHeuristic final : public ISelectionHeuristic {
public:
    /// Tunables (defaults chosen to be conservative — promote to standing only
    /// on a genuinely broad result so closed stays the LEAST_FORCE default).
    struct Params {
        /// A non-error body whose size strictly exceeds this reads as
        /// investigation by shape alone (default 8 KiB — well above a single
        /// resolved fact, below a typical large-read distill trigger).
        std::size_t large_body_bytes = 8 * 1024;

        /// A non-error body with at least this many distinct non-empty sections
        /// (newline-delimited hits/records) reads as investigation by shape
        /// (default 16 — a handful of lines is a lookup; many is a dig).
        std::size_t many_sections = 16;
    };

    ShapeSelectionHeuristic() = default;
    explicit ShapeSelectionHeuristic(Params params) noexcept : params_(params) {}

    [[nodiscard]] DispatchMode classify(std::string_view  tool_name,
                                        const Json&       args,
                                        const ToolResult& result) const override;

    /// True iff @p tool_name is a known broad-search / large-surface tool.
    /// Exposed for tests and for the design-note audit of the name list.
    [[nodiscard]] static bool is_investigation_tool(std::string_view tool_name) noexcept;

    [[nodiscard]] const Params& params() const noexcept { return params_; }

private:
    Params params_{};
};

} // namespace batbox::tools
