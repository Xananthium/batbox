// src/tools/SkillTool.cpp
// =============================================================================
// Implementation of batbox::tools::SkillTool.
//
// Invokes a named skill from SkillLoader:
//   1. Validates that "name" arg is present and non-empty.
//   2. Looks up the skill; returns an error result if not found.
//   3. Substitutes every occurrence of $ARGS in the prompt body with the
//      optional "input" argument (empty string when not provided).
//   4. Packages allowed_tools and model override into the structured_payload
//      so the dispatch layer can apply the skill's runtime tool restrictions.
//   5. Returns ToolResult::ok(rendered_body, payload).
//
// Blueprint contract: batbox::tools::SkillTool (CPP 5.19)
// =============================================================================

#include <batbox/tools/SkillTool.hpp>

#include <batbox/core/Json.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>

#include <string>
#include <string_view>

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// replace_all — in-place replacement of all non-overlapping occurrences of
// `from` with `to` inside `s`.  No-op when `from` is empty.
// ---------------------------------------------------------------------------
void replace_all(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size(); // advance past the replacement to avoid infinite loop
    }
}

// ---------------------------------------------------------------------------
// substitute_args — replace every occurrence of $ARGS in body with args_str.
// Modifies body in place; returns the modified string by value for clarity
// at call-sites.
// ---------------------------------------------------------------------------
[[nodiscard]] std::string substitute_args(std::string body,
                                          std::string_view args_str) {
    replace_all(body, "$ARGS", args_str);
    return body;
}

} // anonymous namespace

// =============================================================================
// SkillTool — construction
// =============================================================================

SkillTool::SkillTool(batbox::plugins::SkillLoader& loader)
    : loader_(loader) {}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view SkillTool::name() const {
    return "Skill";
}

std::string_view SkillTool::description() const {
    return "Invoke a named skill: injects the skill's prompt body (with "
           "optional $ARGS substitution) into the conversation as a user "
           "message and honours the skill's allowed_tools list.";
}

Json SkillTool::schema_json() const {
    return Json{
        {"name",        "Skill"},
        {"description", description()},
        {"parameters",  Json{
            {"type",       "object"},
            {"properties", Json{
                {"name", Json{
                    {"type",        "string"},
                    {"description", "The canonical skill name to invoke "
                                    "(e.g. \"remember\", \"debug\"). "
                                    "Must match a loaded skill exactly."}
                }},
                {"input", Json{
                    {"type",        "string"},
                    {"description", "Optional argument string passed to the "
                                    "skill. Every occurrence of $ARGS in the "
                                    "skill body is replaced with this value. "
                                    "Defaults to an empty string when absent."}
                }}
            }},
            {"required", Json::array({"name"})}
        }}
    };
}

// =============================================================================
// ITool execution
// =============================================================================

ToolResult SkillTool::run(const Json& args, ToolContext& ctx) {
    // ------------------------------------------------------------------
    // 0. Cancellation check — bail out fast.
    // ------------------------------------------------------------------
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // ------------------------------------------------------------------
    // 1. Validate and extract "name" argument.
    // ------------------------------------------------------------------
    if (!args.contains("name") || !args["name"].is_string()) {
        return ToolResult::error(
            "Skill: required argument 'name' is missing or not a string.");
    }
    const std::string skill_name = args["name"].get<std::string>();
    if (skill_name.empty()) {
        return ToolResult::error("Skill: 'name' must be a non-empty string.");
    }

    // ------------------------------------------------------------------
    // 2. Extract optional "input" argument (defaults to empty string).
    // ------------------------------------------------------------------
    std::string input_str;
    if (args.contains("input") && args["input"].is_string()) {
        input_str = args["input"].get<std::string>();
    }

    // ------------------------------------------------------------------
    // 3. Look up the skill in the loader.
    // ------------------------------------------------------------------
    const batbox::plugins::Skill* skill = loader_.find(skill_name);
    if (!skill) {
        return ToolResult::error("Unknown skill: " + skill_name);
    }

    // ------------------------------------------------------------------
    // 4. Substitute $ARGS in the prompt body.
    // ------------------------------------------------------------------
    std::string rendered_body = substitute_args(skill->prompt_body, input_str);

    // ------------------------------------------------------------------
    // 5. Build structured_payload with runtime metadata.
    //
    //    allowed_tools — non-null JSON array when the skill declares a
    //                    tool allow-list; the dispatch layer uses this to
    //                    restrict which tools the model may call after the
    //                    skill prompt is injected.
    //    model         — non-null string when the skill requests a model
    //                    override; null otherwise.
    // ------------------------------------------------------------------
    Json payload = Json::object();
    payload["skill_name"] = skill_name;

    if (!skill->allowed_tools.empty()) {
        Json allowed = Json::array();
        for (const std::string& t : skill->allowed_tools) {
            allowed.push_back(t);
        }
        payload["allowed_tools"] = std::move(allowed);
    } else {
        payload["allowed_tools"] = nullptr;
    }

    if (skill->model.has_value()) {
        payload["model"] = skill->model.value();
    } else {
        payload["model"] = nullptr;
    }

    // ------------------------------------------------------------------
    // 6. Return the rendered body with the metadata payload.
    //    The caller is responsible for injecting body as a user message.
    // ------------------------------------------------------------------
    return ToolResult::ok(std::move(rendered_body), std::move(payload));
}

} // namespace batbox::tools
