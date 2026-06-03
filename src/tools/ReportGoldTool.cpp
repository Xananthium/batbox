// src/tools/ReportGoldTool.cpp
//
// Implementation of batbox::tools::ReportGoldTool.
// See include/batbox/tools/ReportGoldTool.hpp for the full contract.

#include <batbox/tools/ReportGoldTool.hpp>

#include <string>
#include <utility>

namespace batbox::tools {

// =============================================================================
// name / description
// =============================================================================

std::string_view ReportGoldTool::name() const {
    return "report_gold";
}

std::string_view ReportGoldTool::description() const {
    return "Report ONLY the single golden line distilled from a tool's output: "
           "the answer to the question that produced it. Call this exactly once, "
           "with nothing more and nothing less.";
}

// =============================================================================
// schema_json — OpenAI tools[*].function object
// =============================================================================

Json ReportGoldTool::schema_json() const {
    return Json{
        {"name",        "report_gold"},
        {"description", "Report ONLY the single golden line distilled from a tool's "
                        "output: the answer to the question that produced it. Call "
                        "this exactly once, with nothing more and nothing less."},
        {"parameters", Json{
            {"type", "object"},
            {"properties", Json{
                {"answer", Json{
                    {"type",        "string"},
                    {"description", "The distilled golden line — the answer, nothing more."}
                }},
                {"confidence", Json{
                    {"type",        "number"},
                    {"description", "Optional self-rated confidence in [0,1]."}
                }},
                {"follow_up_ok", Json{
                    {"type",        "boolean"},
                    {"description", "Optional: true if this output is worth keeping warm "
                                    "for follow-up questions."}
                }}
            }},
            {"required", Json::array({"answer"})}
        }}
    };
}

// =============================================================================
// parse — shared shape parser (backs run() and the distiller's harvest)
// =============================================================================

std::optional<ReportGold> ReportGoldTool::parse(const Json& args) {
    if (!args.is_object()) {
        return std::nullopt;
    }

    const auto it = args.find("answer");
    if (it == args.end() || !it->is_string()) {
        return std::nullopt;
    }
    std::string answer = it->get<std::string>();
    if (answer.empty()) {
        return std::nullopt;
    }

    ReportGold gold;
    gold.answer = std::move(answer);

    if (const auto c = args.find("confidence");
        c != args.end() && c->is_number()) {
        gold.confidence = c->get<double>();
    }
    if (const auto f = args.find("follow_up_ok");
        f != args.end() && f->is_boolean()) {
        gold.follow_up_ok = f->get<bool>();
    }
    return gold;
}

// =============================================================================
// run — parse args and surface the structured result
// =============================================================================

ToolResult ReportGoldTool::run(const Json& args, ToolContext& /*ctx*/) {
    const auto gold = parse(args);
    if (!gold.has_value()) {
        return ToolResult::error(
            "report_gold requires a non-empty 'answer' string argument.");
    }

    Json payload = Json::object();
    payload["answer"] = gold->answer;
    if (gold->confidence.has_value())   payload["confidence"]   = *gold->confidence;
    if (gold->follow_up_ok.has_value()) payload["follow_up_ok"] = *gold->follow_up_ok;

    return ToolResult::ok(gold->answer, std::move(payload));
}

} // namespace batbox::tools
