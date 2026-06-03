// src/tools/StandingSelector.cpp
//
// Implementation of batbox::tools::StandingSelector + install_standing_selection
// (DIS-1007).  See include/batbox/tools/StandingSelector.hpp for the full
// contract and the predict-ahead / confirm-after design rationale.

#include <batbox/tools/StandingSelector.hpp>

#include <batbox/agents/AgentSpec.hpp>
#include <batbox/agents/AgentSupervisor.hpp>
#include <batbox/core/CancelToken.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/tools/ThresholdEngulfDecider.hpp>
#include <batbox/tools/ToolRegistry.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace batbox::tools {

namespace {

/// Thrown by run_standing() on every standing-path failure so distill() can
/// converge them all on the SINGLE closed fallback (AC5), mirroring
/// SubagentDistiller::distill's DistillFallback structure.  The message carries
/// the per-path reason for the warn log so AC5 granularity is not lost.
struct StandingFallback final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Confirm-after threshold: a follow_up_ok==true answer whose confidence is at
/// or above this is a TRIVIALLY-HIGH-CONFIDENCE LOOKUP — the subagent is sure,
/// so there is nothing to follow up on; do NOT keep the window warm.  Below it,
/// follow_up_ok==true keeps warm; and low confidence (see kLowConfidence) leans
/// keep-warm even harder.  Documented in the header's confirm-after rules.
constexpr double kTrivialLookupConfidence = 0.95;

/// At or below this confidence, an answer is uncertain enough that a follow-up
/// is likely worthwhile → lean keep-warm regardless of the follow_up_ok hint.
constexpr double kLowConfidence = 0.40;

/// Build the standing subagent's first-turn prompt.  Same INTENT framing as the
/// closed distiller (tool name + args + raw output) but instructs the warm
/// window to keep its engulfed context available for follow-up questions.
std::string build_standing_prompt(std::string_view   tool_name,
                                  const Json&        args,
                                  const std::string& raw_body) {
    std::string args_str;
    try {
        args_str = args.dump();
    } catch (...) {
        args_str = "{}";
    }

    std::string prompt;
    prompt.reserve(raw_body.size() + 512);
    prompt +=
        "You are an investigation subagent. A tool produced a large/broad output "
        "that the parent is likely to ask follow-up questions about. Read the "
        "output below, keep it in mind, and report the golden line that answers "
        "the question the call was meant to answer. Respond by calling the "
        "report_gold tool.\n\n";
    prompt += "Tool: ";
    prompt += std::string(tool_name);
    prompt += "\nArguments (the intent): ";
    prompt += args_str;
    prompt += "\n\n--- BEGIN TOOL OUTPUT ---\n";
    prompt += raw_body;
    prompt += "\n--- END TOOL OUTPUT ---\n";
    return prompt;
}

/// CONFIRM-AFTER: given the report_gold signals from the standing first turn,
/// decide whether to keep the warm window (promote) or close it.
///   follow_up_ok == false                    → close (the subagent says it's done)
///   confidence present and <= kLowConfidence → keep warm (uncertain → follow up)
///   follow_up_ok == true and confidence >= kTrivialLookupConfidence
///                                            → close (trivially-sure lookup)
///   follow_up_ok == true                     → keep warm
///   follow_up_ok absent                      → close (no positive keep-warm hint)
[[nodiscard]] bool should_keep_warm(const ReportGold& gold) noexcept {
    // Low confidence leans keep-warm even if the subagent did not set follow_up_ok.
    if (gold.confidence.has_value() && *gold.confidence <= kLowConfidence) {
        return true;
    }
    if (!gold.follow_up_ok.has_value() || !*gold.follow_up_ok) {
        return false;  // no keep-warm hint, or explicitly done
    }
    // follow_up_ok == true: keep warm UNLESS it is a trivially-high-confidence
    // lookup (the subagent is sure → nothing to follow up on).
    if (gold.confidence.has_value() && *gold.confidence >= kTrivialLookupConfidence) {
        return false;
    }
    return true;
}

/// Build the gold ToolResult the parent sees (closed-equivalent shape: same
/// payload keys the closed distiller emits, so downstream consumers — and the S6
/// notepad sink — are path-agnostic).
[[nodiscard]] ToolResult make_gold_result(const ReportGold& gold,
                                          std::size_t       original_bytes) {
    Json payload = Json::object();
    payload["distilled"]      = true;
    payload["original_bytes"] = original_bytes;
    payload["standing"]       = true;  // provenance: this gold came via the standing path
    if (gold.confidence.has_value())   payload["confidence"]   = *gold.confidence;
    if (gold.follow_up_ok.has_value()) payload["follow_up_ok"] = *gold.follow_up_ok;
    return ToolResult{gold.answer, /*is_error=*/false, std::move(payload)};
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

StandingSelector::StandingSelector(const config::Config&                cfg,
                                   agents::AgentSupervisor*             supervisor,
                                   std::shared_ptr<ISelectionHeuristic> heuristic)
    : cfg_(cfg)
    , supervisor_(supervisor)
    , heuristic_(heuristic ? std::move(heuristic)
                           : std::make_shared<ShapeSelectionHeuristic>())
    , closed_(cfg) {}

std::string StandingSelector::last_standing_id() const {
    std::lock_guard<std::mutex> lock{id_mutex_};
    return last_standing_id_;
}

// =============================================================================
// run_standing — the standing path (spawn warm window → first-turn gold →
// confirm-after → promote-or-evict).  THROWS StandingFallback on every failure
// so distill() converges on the closed fallback (AC5).
// =============================================================================

ToolResult StandingSelector::run_standing(std::string_view  tool_name,
                                          const Json&       args,
                                          const ToolResult& result,
                                          ToolContext&      ctx) const {
    if (supervisor_ == nullptr) {
        // No pool → standing is structurally unavailable.  Fall back to closed.
        throw StandingFallback("no supervisor (standing unavailable)");
    }
    if (ctx.cancel_token.is_cancelled()) {
        throw StandingFallback("cancelled before standing dispatch");
    }

    // -------------------------------------------------------------------------
    // First-turn gold via the SHARED report_gold contract.  We reuse the closed
    // distiller's extract_gold (the single source of truth for the gold harvest)
    // so closed and standing speak the identical contract.  This is the
    // first-turn distillation; it never throws (nullopt on any failure).
    // -------------------------------------------------------------------------
    const auto gold = closed_.extract_gold(tool_name, args, result, ctx);
    if (!gold.has_value()) {
        throw StandingFallback("standing first-turn gold extraction failed");
    }

    // -------------------------------------------------------------------------
    // CONFIRM-AFTER: read follow_up_ok / confidence and decide keep-warm vs close.
    // -------------------------------------------------------------------------
    ToolResult gold_result = make_gold_result(*gold, result.body.size());

    if (!should_keep_warm(*gold)) {
        // (3) FOLLOW_UP_OK-CANCEL: investigation predicted, but the subagent says
        // it is done (or is a trivially-sure lookup).  Do NOT spawn/keep a warm
        // window; return the gold as the closed-equivalent result.
        {
            std::lock_guard<std::mutex> lock{id_mutex_};
            last_standing_id_.clear();
        }
        BATBOX_LOG_INFO(
            "StandingSelector: FOLLOW_UP_OK-CANCEL (investigation predicted, "
            "follow_up_ok=false/trivial-lookup) → closed-equivalent gold for tool '{}'",
            std::string(tool_name));
        return gold_result;
    }

    // -------------------------------------------------------------------------
    // KEEP WARM: dispatch a real warm SubAgent on the SAME local distill endpoint
    // (use_distill_endpoint → cfg.distill.*), then promote it so its window stays
    // alive for follow-up interrogation.  Spawn/promote failures throw → closed
    // fallback (AC5); a promotion past the bound evicts the LRU losslessly (AC4).
    // -------------------------------------------------------------------------
    agents::AgentSpec spec;
    spec.name        = "standing-distill";
    spec.description = "warm investigation window (DIS-1007)";
    spec.endpoint    = agents::EndpointOverride{};
    spec.endpoint->use_distill_endpoint = true;  // point at cfg.distill.* (local 3090)

    const std::string prompt = build_standing_prompt(tool_name, args, result.body);

    // spawn() takes the CancelToken by value (move-only).  A standing window is
    // INTENDED to outlive this single tool dispatch (that is the whole point of
    // "standing") and is owned + cancelled by the supervisor — via cancel(),
    // LRU eviction, or the supervisor destructor, all of which fire the agent's
    // OWN child source independently of the parent context token.  So we hand
    // spawn() a fresh never-cancelled token rather than chaining a child of the
    // dispatch-scoped ctx token (whose stop-state would not outlive distill()).
    std::string id;
    try {
        id = supervisor_->spawn(spec, prompt, /*parent_id=*/"", batbox::CancelToken{});
    } catch (const std::exception& e) {
        throw StandingFallback(std::string("spawn failed: ") + e.what());
    }
    if (id.empty()) {
        throw StandingFallback("spawn returned empty id");
    }

    // promote() is no-throw and refuses dead/exited agents (DIS-1001 guard); it
    // registers the window in the LRU pool, evicting the least-recently-
    // interrogated if the bound is exceeded (AC4 — lossless: gold is already out).
    supervisor_->promote(id);

    {
        std::lock_guard<std::mutex> lock{id_mutex_};
        last_standing_id_ = id;
    }

    // (2) PROMOTE: investigation + keep-warm → warm window registered.
    BATBOX_LOG_INFO(
        "StandingSelector: PROMOTE (investigation + keep-warm) → standing window '{}' "
        "kept warm for tool '{}' (standing_count={})",
        id, std::string(tool_name), supervisor_->standing_count());

    return gold_result;
}

// =============================================================================
// distill — single-convergence robustness wrapper (AC5).
//
// `result` is taken BY VALUE and is NEVER mutated below, so the raw output is
// held intact for the whole function — a mid-standing failure never discards it.
//
// Branch:
//   CLOSED predicted  → delegate verbatim to the owned SubagentDistiller (AC1
//                       byte-identical: same gold, no standing window created).
//   STANDING predicted → try run_standing(); ANY failure converges on the closed
//                       one-shot fallback; if THAT also fails the closed distiller
//                       itself returns the original result unchanged.  Never
//                       throws, never hangs (AC5).
// =============================================================================

ToolResult StandingSelector::distill(std::string_view tool_name,
                                     const Json&      args,
                                     ToolResult       result,
                                     ToolContext&     ctx) const {
    const DispatchMode mode = heuristic_->classify(tool_name, args, result);

    if (mode == DispatchMode::Closed) {
        // (1) CLOSED: no investigation signal → the one-shot, byte-identical to S4.
        {
            std::lock_guard<std::mutex> lock{id_mutex_};
            last_standing_id_.clear();
        }
        BATBOX_LOG_INFO(
            "StandingSelector: CLOSED (no investigation signal) → one-shot distill "
            "for tool '{}'", std::string(tool_name));
        return closed_.distill(tool_name, args, std::move(result), ctx);
    }

    // STANDING predicted — try the standing path; converge every failure on the
    // closed fallback.  The closed distiller is itself fail-closed (it returns
    // the original result if the local endpoint is unreachable), so this is the
    // double-fallback the contract requires: standing → closed → original.
    try {
        return run_standing(tool_name, args, result, ctx);  // success — the only standing return
    } catch (const std::exception& e) {
        // (4) standing-path FALLBACK → closed one-shot.
        {
            std::lock_guard<std::mutex> lock{id_mutex_};
            last_standing_id_.clear();
        }
        BATBOX_LOG_WARN(
            "StandingSelector: standing path failed ({}) → closed fallback for tool '{}'",
            e.what(), std::string(tool_name));
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock{id_mutex_};
            last_standing_id_.clear();
        }
        BATBOX_LOG_WARN(
            "StandingSelector: standing path failed (unknown non-std failure) → "
            "closed fallback for tool '{}'", std::string(tool_name));
    }
    return closed_.distill(tool_name, args, std::move(result), ctx);
}

// =============================================================================
// install_standing_selection — startup wiring (AC6)
// =============================================================================

void install_standing_selection(ToolRegistry&            registry,
                                const config::Config&    cfg,
                                agents::AgentSupervisor* supervisor) {
    if (!cfg.distill.enabled) {
        // Pure pass-through → byte-identical to S7.  The S7 seam is untouched.
        return;
    }
    registry.envelope().set_decider(
        std::make_shared<ThresholdEngulfDecider>(cfg.distill.max_tool_response_size));
    registry.envelope().set_distiller(
        std::make_shared<StandingSelector>(cfg, supervisor));
}

} // namespace batbox::tools
