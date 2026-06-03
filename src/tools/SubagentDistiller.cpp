// src/tools/SubagentDistiller.cpp
//
// Implementation of batbox::tools::SubagentDistiller + install_subagent_distillation.
// See include/batbox/tools/SubagentDistiller.hpp for the full contract and the
// design rationale (single-shot Provider call vs full SubAgent window).

#include <batbox/tools/SubagentDistiller.hpp>

#include <batbox/core/Logging.hpp>
#include <batbox/inference/ChatRequest.hpp>
#include <batbox/inference/ChatResponse.hpp>
#include <batbox/inference/Provider.hpp>
#include <batbox/tools/ReportGoldTool.hpp>
#include <batbox/tools/ThresholdEngulfDecider.hpp>
#include <batbox/tools/ToolRegistry.hpp>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace batbox::tools {

namespace {

/// Thrown by run_distillation() for every "soft" failure — an expected,
/// recoverable condition (cancelled, unreachable endpoint, no/wrong report_gold,
/// unparseable args, missing answer).  distill() catches it (it derives from
/// std::exception) and converges it on the single fallback return.  The message
/// preserves the per-path reason for the warn log so AC5's per-path granularity
/// is not lost by the single-convergence structure.
struct DistillFallback final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Build the tight distiller prompt.  It carries the INTENT (tool name + the
/// arguments the tool was dispatched with) plus the raw output, so the local
/// model knows what question the big blob was meant to answer.
std::string build_distiller_prompt(std::string_view   tool_name,
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
        "You are a distillation subagent. A tool was called and produced a large "
        "output. Read the output below and report ONLY the golden line that answers "
        "the question the call was meant to answer — nothing more, nothing less. "
        "You MUST respond by calling the report_gold tool; do not write any other "
        "text.\n\n";
    prompt += "Tool: ";
    prompt += std::string(tool_name);
    prompt += "\nArguments (the intent): ";
    prompt += args_str;
    prompt += "\n\n--- BEGIN TOOL OUTPUT ---\n";
    prompt += raw_body;
    prompt += "\n--- END TOOL OUTPUT ---\n";
    return prompt;
}

} // namespace

// =============================================================================
// run_distillation — the engulf → one-shot local call → harvest path.
//
// Returns the distilled gold ToolResult on success.  THROWS (DistillFallback for
// soft/expected failures, or whatever the provider/JSON layer throws for hard
// ones) on EVERY failure mode, so distill() can converge them all on one return.
// Reads `result` but never consumes it — the raw stays whole back in distill().
// =============================================================================

ToolResult SubagentDistiller::run_distillation(std::string_view  tool_name,
                                               const Json&       args,
                                               const ToolResult& result,
                                               ToolContext&      ctx) const {
    // extract_gold() runs the one-shot report_gold call and converges EVERY
    // failure mode on std::nullopt (it never throws).  Re-raise here so distill()
    // keeps its single-convergence fallback structure (AC5).  This keeps the
    // closed path's behaviour and ToolResult payload BYTE-IDENTICAL to before.
    const auto gold = extract_gold(tool_name, args, result, ctx);
    if (!gold.has_value()) {
        throw DistillFallback("report_gold extraction failed (see extract_gold)");
    }

    // -------------------------------------------------------------------------
    // Success: the distilled golden line becomes the result the model sees.
    // follow_up_ok is captured into the payload but NOT acted upon (S2/S3).
    // -------------------------------------------------------------------------
    Json payload = Json::object();
    payload["distilled"]      = true;
    payload["original_bytes"] = result.body.size();
    if (gold->confidence.has_value())   payload["confidence"]   = *gold->confidence;
    if (gold->follow_up_ok.has_value()) payload["follow_up_ok"] = *gold->follow_up_ok;

    return ToolResult{gold->answer, /*is_error=*/false, std::move(payload)};
}

// =============================================================================
// extract_gold — the one-shot local report_gold harvest (shared contract).
//
// Runs the SAME single synchronous report_gold call distill() has always run,
// against the LOCAL distill endpoint (cfg.distill.*, NOT cfg.api), and returns
// the parsed ReportGold.  Converges EVERY failure mode (cancellation, unreachable
// endpoint, HTTP/transport/parse error, absent/wrong report_gold call,
// unparseable args, missing answer, any thrown exception) on std::nullopt.
// NEVER throws — distill() re-raises nullopt as DistillFallback to keep its
// single-convergence structure; StandingSelector (DIS-1007) reads the returned
// follow_up_ok / confidence for the confirm-after decision.
// =============================================================================

std::optional<ReportGold> SubagentDistiller::extract_gold(std::string_view  tool_name,
                                                          const Json&       args,
                                                          const ToolResult& result,
                                                          ToolContext&      ctx) const noexcept {
    try {
        // Cancellation already requested → do not even reach the endpoint.
        if (ctx.cancel_token.is_cancelled()) {
            return std::nullopt;
        }

        // ---------------------------------------------------------------------
        // Throwaway Config pointed at the LOCAL distill endpoint.  This is the
        // crux: the provider must hit cfg.distill.*, NOT cfg.api (the main /
        // cloud model).  Built from defaults so we only override the api fields
        // the Provider/Client read; `local` outlives `provider` below.
        // ---------------------------------------------------------------------
        config::Config local = config::Config::load_default();
        local.api.base_url            = cfg_.distill.base_url;
        local.api.api_key             = cfg_.distill.api_key;
        local.api.default_model       = cfg_.distill.model;
        local.api.request_timeout_sec = cfg_.distill.request_timeout_sec;
        local.api.max_tokens          = cfg_.distill.max_tokens;
        local.api.provider_hint.clear();  // auto-detect (ollama/openai) from base_url

        // report_gold is the ONLY tool offered and tool_choice pins it → the
        // local model is forced to emit through the structured handoff.
        ReportGoldTool report_tool;
        const Json     gold_schema = report_tool.schema_json();

        inference::ToolDef gold_def;
        gold_def.type        = "function";
        gold_def.name        = "report_gold";
        gold_def.description = gold_schema.value("description", std::string{});
        gold_def.schema      = gold_schema.contains("parameters")
                                   ? gold_schema.at("parameters")
                                   : Json::object();

        inference::WireMessage user_msg;
        user_msg.role    = "user";
        user_msg.content = build_distiller_prompt(tool_name, args, result.body);

        inference::ChatRequest req;
        req.model      = cfg_.distill.model;
        req.messages.push_back(std::move(user_msg));
        req.tools.push_back(std::move(gold_def));
        req.tool_choice = inference::ChatRequest::tool_choice_function("report_gold");
        req.max_tokens  = cfg_.distill.max_tokens;
        req.stream      = false;  // closed / one-shot
        req.stream_options_include_usage = std::nullopt;

        // One-shot call against the local endpoint.  Client::chat forces
        // stream=false and surfaces transport / HTTP / parse failures as Err.
        inference::OpenAiCompatibleProvider provider{local};
        auto resp = provider.chat(req);

        // Cancelled during the call → discard the (possibly partial) work.
        if (ctx.cancel_token.is_cancelled()) {
            return std::nullopt;
        }
        if (!resp.has_value()) {
            return std::nullopt;
        }

        // Harvest the report_gold structured output.
        const inference::ChatResponse& r = resp.value();
        if (!r.tool_calls.has_value() || r.tool_calls->empty()) {
            return std::nullopt;
        }

        const inference::WireToolCall* gold_call = nullptr;
        for (const auto& tc : *r.tool_calls) {
            if (tc.function.name == "report_gold") {
                gold_call = &tc;
                break;
            }
        }
        if (gold_call == nullptr) {
            return std::nullopt;
        }

        Json       parsed_args = Json::parse(gold_call->function.arguments);
        const auto gold        = ReportGoldTool::parse(parsed_args);
        if (!gold.has_value()) {
            return std::nullopt;
        }
        return gold;
    } catch (...) {
        // ANY failure (provider/JSON exception, OOM, etc.) → nullopt.  Never
        // throw out of extract_gold — its callers depend on the no-throw guarantee.
        return std::nullopt;
    }
}

// =============================================================================
// distill — single-convergence robustness wrapper (AC5).
//
// `result` is taken BY VALUE and is NEVER mutated below: the raw output is held
// intact for the whole function, so there is no window where a mid-distillation
// failure has already discarded the raw.  EVERY failure mode — a fired cancel
// token, a transport/HTTP/parse error from the local endpoint, an absent or
// wrong report_gold call, unparseable report_gold args, a missing 'answer', or
// ANY thrown exception (std or otherwise) — converges on the SINGLE fallback
// `return result;` below.  There is exactly ONE other return statement: the
// success path (`return run_distillation(...)`), reached only when a complete
// distilled gold line is in hand.  This makes "never lose data, never throw out
// of distill()" provable BY INSPECTION (one convergence point), not just by
// enumerating the unit tests.
// =============================================================================

ToolResult SubagentDistiller::distill(std::string_view tool_name,
                                      const Json&      args,
                                      ToolResult       result,
                                      ToolContext&     ctx) const {
    try {
        return run_distillation(tool_name, args, result, ctx);  // success — the only non-fallback return
    } catch (const std::exception& e) {
        BATBOX_LOG_WARN("distill: returning raw output ({})", e.what());
    } catch (...) {
        BATBOX_LOG_WARN("distill: returning raw output (unknown non-std failure)");
    }
    return result;  // <<<<< AC5: THE SINGLE FALLBACK CONVERGENCE POINT >>>>>
}

// =============================================================================
// install_subagent_distillation — startup wiring (AC6)
// =============================================================================

void install_subagent_distillation(ToolRegistry& registry, const config::Config& cfg) {
    if (!cfg.distill.enabled) {
        // Pure pass-through → byte-identical to S7.  The S7 seam is untouched.
        return;
    }
    registry.envelope().set_decider(
        std::make_shared<ThresholdEngulfDecider>(cfg.distill.max_tool_response_size));
    registry.envelope().set_distiller(
        std::make_shared<SubagentDistiller>(cfg));
}

} // namespace batbox::tools
