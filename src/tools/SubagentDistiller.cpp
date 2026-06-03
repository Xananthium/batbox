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
#include <string>
#include <utility>

namespace batbox::tools {

namespace {

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
// distill — engulf into a one-shot local call, harvest report_gold, or fall back
// =============================================================================

ToolResult SubagentDistiller::distill(std::string_view tool_name,
                                      const Json&      args,
                                      ToolResult       result,
                                      ToolContext&     ctx) const {
    const std::size_t original_bytes = result.body.size();

    // Cancellation already requested → do not engulf; return the original.
    if (ctx.cancel_token.is_cancelled()) {
        return result;
    }

    try {
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
            return result;
        }
        if (!resp.has_value()) {
            BATBOX_LOG_WARN("distill: local endpoint failed ({}); returning raw output",
                            resp.error());
            return result;  // unreachable / 5xx / parse error → original
        }

        // Harvest the report_gold structured output.
        const inference::ChatResponse& r = resp.value();
        if (!r.tool_calls.has_value() || r.tool_calls->empty()) {
            BATBOX_LOG_WARN("distill: model returned no tool_calls; returning raw output");
            return result;  // report_gold never called → original
        }

        const inference::WireToolCall* gold_call = nullptr;
        for (const auto& tc : *r.tool_calls) {
            if (tc.function.name == "report_gold") {
                gold_call = &tc;
                break;
            }
        }
        if (gold_call == nullptr) {
            BATBOX_LOG_WARN("distill: no report_gold tool_call; returning raw output");
            return result;  // wrong tool → original
        }

        Json parsed_args;
        try {
            parsed_args = Json::parse(gold_call->function.arguments);
        } catch (...) {
            BATBOX_LOG_WARN("distill: report_gold arguments unparseable; returning raw output");
            return result;
        }

        const auto gold = ReportGoldTool::parse(parsed_args);
        if (!gold.has_value()) {
            BATBOX_LOG_WARN("distill: report_gold missing 'answer'; returning raw output");
            return result;  // no usable answer → original
        }

        // ---------------------------------------------------------------------
        // Success: the distilled golden line becomes the result the model sees.
        // follow_up_ok is captured into the payload but NOT acted upon (S2/S3).
        // ---------------------------------------------------------------------
        Json payload = Json::object();
        payload["distilled"]      = true;
        payload["original_bytes"] = original_bytes;
        if (gold->confidence.has_value())   payload["confidence"]   = *gold->confidence;
        if (gold->follow_up_ok.has_value()) payload["follow_up_ok"] = *gold->follow_up_ok;

        std::string answer = gold->answer;
        return ToolResult{std::move(answer), /*is_error=*/false, std::move(payload)};
    } catch (const std::exception& e) {
        // distill() must never throw — any unexpected failure falls back to raw.
        BATBOX_LOG_WARN("distill: unexpected exception ({}); returning raw output", e.what());
        return result;
    } catch (...) {
        BATBOX_LOG_WARN("distill: unexpected non-std exception; returning raw output");
        return result;
    }
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
