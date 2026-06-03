// include/batbox/tools/SubagentDistiller.hpp
//
// batbox::tools::SubagentDistiller — the S4 closed tool-subagent distiller
// (DIS-980).  Fills the IResultDistiller hook the S7 envelope left as an
// IdentityDistiller, WITHOUT touching the seam (installed via
// ToolRegistry::envelope().set_distiller).
//
// What it does (closed mode):
//   When the ThresholdEngulfDecider (S1) engulfs a too-big tool result, this
//   distills it to the golden line by running a ONE-SHOT call against a LOCAL
//   OpenAI-compatible endpoint (cfg.distill.*, the 3090s) — which is
//   deliberately SEPARATE from cfg.api (the main, often cloud, model).  The
//   local model is FORCED to answer through the report_gold structured contract
//   (report_gold is the only tool offered and tool_choice pins it), so the
//   parent never heuristically parses free text.  The distilled answer is
//   returned as the ToolResult the model actually sees.
//
// Why a single-shot Provider call rather than a full SubAgent window
// (the design decision the issue asks to surface + justify):
//   * Closed mode is one request → one response → discard.  A single-shot
//     OpenAiCompatibleProvider call (built from the S8 provider core) is the
//     lightest faithful realisation of "engulf in a subagent window, distill,
//     throw the window away."
//   * The hard constraint is the ENDPOINT: the distiller must hit a LOCAL
//     endpoint that is NOT cfg.api.  AgentSpec only overrides the model NAME,
//     not the base_url, so a full SubAgent spawned via AgentSupervisor cannot
//     reach a different endpoint without deeper surgery into the conversation
//     construction path.  Synthesising a throwaway Config whose api.* points at
//     cfg.distill.* and driving an OpenAiCompatibleProvider against it reaches
//     the local endpoint cleanly and stays hermetically testable against a fake
//     local server.
//   * Trade-off acknowledged: a standing SubAgent window is more reusable for
//     the later WARM mode (S2/S3).  When standing mode lands it will graduate to
//     a real SubAgent; report_gold (the contract) is shared by both paths.
//
// CLOSED lifecycle (S1+S4 scope): the call is one-shot, used to distill and then
// discarded — no standing window is retained.  follow_up_ok is CAPTURED into the
// distilled payload but NOT acted upon here (it is a signal into the future
// closed-vs-standing selection, S2/S3 + step 8).
//
// ROBUSTNESS (decisive — AC5): if the local endpoint is unreachable, errors,
// times out, returns no report_gold call, or the context is already cancelled,
// distill() returns the ORIGINAL result unchanged — it never loses data and
// never throws out of distill().  A broken local endpoint must not break the
// parent turn.

#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/tools/ToolSubagentEnvelope.hpp>

#include <string_view>

namespace batbox::tools {

class ToolRegistry;  // forward — for install_subagent_distillation()

// =============================================================================
// SubagentDistiller
// =============================================================================

class SubagentDistiller final : public IResultDistiller {
public:
    /// @param cfg  The live runtime config.  Held by const-ref; the caller MUST
    ///             keep it alive for the distiller's lifetime (same contract the
    ///             tools/registry already have at startup).  distill() reads
    ///             cfg.distill.* at call time, so hot-reloads are picked up.
    explicit SubagentDistiller(const config::Config& cfg) noexcept : cfg_(cfg) {}

    [[nodiscard]] ToolResult distill(std::string_view tool_name,
                                     const Json&      args,
                                     ToolResult       result,
                                     ToolContext&     ctx) const override;

private:
    /// The engulf → one-shot-local-call → harvest-report_gold path.  Returns the
    /// distilled gold ToolResult on success; THROWS on EVERY failure mode (fired
    /// cancel token, transport/HTTP/parse error, absent/wrong report_gold call,
    /// unparseable args, missing 'answer', or any provider exception) so that
    /// distill() can converge them all on its single fallback return (AC5).
    /// Reads @p result but never consumes it — the raw stays intact in distill().
    [[nodiscard]] ToolResult run_distillation(std::string_view  tool_name,
                                              const Json&       args,
                                              const ToolResult& result,
                                              ToolContext&      ctx) const;

    const config::Config& cfg_;
};

// =============================================================================
// install_subagent_distillation — startup wiring (AC6)
// =============================================================================

/// Install the real S1 decider + S4 distiller into @p registry's existing
/// ToolSubagentEnvelope via set_decider / set_distiller.
///
/// No-op when cfg.distill.enabled is false → the envelope stays pure
/// pass-through (PassThroughDecider + IdentityDistiller) and dispatch behaves
/// byte-identically to S7.  The S7 seam itself (ToolSubagentEnvelope,
/// ToolRegistry::dispatch) is NOT modified.
void install_subagent_distillation(ToolRegistry& registry, const config::Config& cfg);

} // namespace batbox::tools
