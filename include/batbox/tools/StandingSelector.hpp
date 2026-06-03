// include/batbox/tools/StandingSelector.hpp
//
// batbox::tools::StandingSelector — the closed-vs-standing selection organ
// (DIS-1007).  THE ownable-novelty organ of the batbox liberation track and
// step 8 of the DIS-926 build order ("the selection heuristic — promote
// closed→standing on investigation-vs-lookup. This is the ownable novelty;
// everything above is the substrate that makes it cheap").
//
// =============================================================================
// THE DECISION (already made — decision A, "predict-ahead, confirm-after")
// =============================================================================
// The S7 envelope routes every engulfed tool result through ONE IResultDistiller
// slot.  Until now that slot held the CLOSED one-shot SubagentDistiller (a single
// synchronous Provider::chat() to the local 3090, forced through report_gold,
// window discarded).  StandingSelector is a NEW IResultDistiller that WRAPS the
// closed distiller (it owns one) and adds the closed-vs-standing branch:
//
//   * DEFAULT = CLOSED, byte-identical.  With no positive investigation signal,
//     distill() delegates verbatim to the owned SubagentDistiller.  This is
//     behaviour-identical to today so the S1/S4 distiller tests stay green (AC1).
//
//   * INVESTIGATION PREDICTED → STANDING.  On a positive pre-dispatch signal
//     (ShapeSelectionHeuristic → DispatchMode::Standing) StandingSelector, INSTEAD
//     of the one-shot, dispatches a real WARM SubAgent via
//     AgentSupervisor::spawn(spec, prompt, "", ct) with
//     spec.endpoint = EndpointOverride{ .use_distill_endpoint = true } — pointing
//     the SubAgent at the SAME local-3090 endpoint as cfg.distill.*.  It distills
//     the first turn to gold via the SHARED report_gold contract
//     (SubagentDistiller::extract_gold), then reads report_gold.follow_up_ok /
//     confidence and CONFIRMS-AFTER:
//        - follow_up_ok == true (and NOT a trivially-high-confidence lookup)
//             → AgentSupervisor::promote(id)  (keep the window WARM)
//        - follow_up_ok == false
//             → do NOT promote; close/evict the window the subagent itself says
//               is done, and return its gold as the (closed-equivalent) result.
//        - low confidence leans KEEP-WARM (an uncertain answer is worth a follow-up).
//
//   * REUSE FACT (don't fight it):  the closed one-shot leaves no warm window to
//     adopt — closed and standing are SEPARATE inference paths.  The PREDICTION
//     decides which path you pay for.  A mispredicted lookup loses only the warm
//     window, never the gold (the gold is already returned and lives in the S6
//     notepad) — recoverable, LRU-bounded, self-correcting.
//
// =============================================================================
// PREDICT-AHEAD signal (the judgment — AC3): the HARNESS infers, the caller
// NEVER flags.  There is deliberately NO background/standing boolean on any
// tool-call surface (anti-pattern #3).  The investigation-vs-lookup judgment is
// the small, named, testable ShapeSelectionHeuristic (see SelectionHeuristic.hpp)
// — tool semantics (broad-search tools) OR result shape (large / many sections).
// Tool identity IS a legitimate input here — a DIFFERENT decision from S1's
// size-not-identity engulf trigger.
// =============================================================================
//
// =============================================================================
// ROBUSTNESS / FAIL-CLOSED (AC5) — non-negotiable
// =============================================================================
// distill() MUST never throw and never hang.  It inherits S1/S4's contract: ANY
// failure on the standing path (3090 unreachable, spawn fails, gold parse fails,
// supervisor refuses) falls back to the CLOSED one-shot distiller; if THAT fails,
// it returns the original result unchanged.  The standing path is wrapped in
// try/catch with a SINGLE convergence to the closed fallback, mirroring
// SubagentDistiller::distill's structure.  A mis-promoted window evicts cleanly
// (the supervisor's LRU / cancel token — lossless because gold is already out).
//
// =============================================================================
// BOUND (AC4)
// =============================================================================
// Standing promotions are bounded by the AgentSupervisor's EXISTING
// max_standing_subagents LRU.  StandingSelector adds NO new bound: a burst of
// investigations cannot grow the pool past the bound — the LRU evicts the
// least-recently-interrogated losslessly.
//
// =============================================================================
// OBSERVABILITY (AC6) — four distinct paths, each structurally provable via logs
// =============================================================================
//   (1) CLOSED                 — no investigation signal → one-shot.
//   (2) PROMOTE                — investigation + follow_up_ok → standing kept warm.
//   (3) FOLLOW_UP_OK-CANCEL    — investigation predicted but follow_up_ok==false
//                                → window closed, gold returned (closed-equivalent).
//   (4) standing-path FALLBACK — any standing-path failure → closed fallback.
// (LRU-EVICT-on-pressure, the fourth conceptual path, is emitted by the
// AgentSupervisor itself when a promote() / set_max_standing_subagents() evicts.)

#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/tools/SelectionHeuristic.hpp>
#include <batbox/tools/SubagentDistiller.hpp>
#include <batbox/tools/ToolSubagentEnvelope.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace batbox::agents { class AgentSupervisor; }

namespace batbox::tools {

class ToolRegistry;  // forward — for install_standing_selection()

// =============================================================================
// StandingSelector
// =============================================================================

class StandingSelector final : public IResultDistiller {
public:
    /// @param cfg         Live runtime config (held by const-ref; the caller MUST
    ///                    keep it alive for this selector's lifetime).  Reads
    ///                    cfg.distill.* at call time so hot-reloads are picked up.
    /// @param supervisor  The standing-subagent pool.  When null, the standing
    ///                    path is structurally unavailable and StandingSelector
    ///                    behaves byte-identically to the closed distiller (a
    ///                    safe, explicit degraded mode — never a half-standing
    ///                    state).  Held by raw pointer (non-owning; the App owns
    ///                    the supervisor and outlives the registry/envelope).
    /// @param heuristic   The predict-ahead classifier.  Defaults to
    ///                    ShapeSelectionHeuristic; injectable for tests.
    StandingSelector(const config::Config&                cfg,
                     agents::AgentSupervisor*             supervisor,
                     std::shared_ptr<ISelectionHeuristic> heuristic =
                         std::make_shared<ShapeSelectionHeuristic>());

    [[nodiscard]] ToolResult distill(std::string_view tool_name,
                                     const Json&      args,
                                     ToolResult       result,
                                     ToolContext&     ctx) const override;

    /// The most recent standing agent id this selector promoted (kept warm), or
    /// the empty string if the last decision did not promote.  Lets a caller /
    /// test reach the warm window for a follow-up interrogation without re-
    /// engulfing the source (AC2).  Updated under an internal mutex; thread-safe.
    [[nodiscard]] std::string last_standing_id() const;

private:
    /// The standing path: spawn a warm SubAgent on the distill endpoint, distill
    /// its first turn to gold (shared report_gold contract), confirm-after via
    /// follow_up_ok / confidence, promote-or-evict.  THROWS on every standing-path
    /// failure so distill() converges them all on the closed fallback (AC5).
    /// On success returns the gold ToolResult (and, when promoted, leaves the
    /// warm window registered + records last_standing_id_).
    [[nodiscard]] ToolResult run_standing(std::string_view  tool_name,
                                          const Json&       args,
                                          const ToolResult& result,
                                          ToolContext&      ctx) const;

    const config::Config&                cfg_;
    agents::AgentSupervisor*             supervisor_;     // non-owning, may be null
    std::shared_ptr<ISelectionHeuristic> heuristic_;
    SubagentDistiller                    closed_;         // owned closed one-shot

    /// Guards last_standing_id_ (written on the standing path, read by tests/UI).
    mutable std::mutex                   id_mutex_;
    mutable std::string                  last_standing_id_;
};

// =============================================================================
// install_standing_selection — startup wiring (AC6)
// =============================================================================

/// Install the S1 decider + the StandingSelector distiller into @p registry's
/// existing ToolSubagentEnvelope, upgrading the closed-only S4 wiring to the
/// full closed-vs-standing selection.
///
/// No-op when cfg.distill.enabled is false → the envelope stays pure
/// pass-through and dispatch behaves byte-identically to S7.  When
/// @p supervisor is null the StandingSelector still installs but its standing
/// path is unavailable (closed-identical), so the envelope is exactly the S4
/// closed distiller — a safe superset of install_subagent_distillation().  The
/// S7 seam itself (ToolSubagentEnvelope, ToolRegistry::dispatch) is NOT modified.
void install_standing_selection(ToolRegistry&            registry,
                                const config::Config&    cfg,
                                agents::AgentSupervisor* supervisor);

} // namespace batbox::tools
