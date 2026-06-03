// src/tools/NotepadAppendTool.cpp
//
// Implementation of batbox::tools::NotepadAppendTool (DIS-981, S6).

#include <batbox/tools/NotepadAppendTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <string>
#include <string_view>

namespace batbox::tools {

std::string_view NotepadAppendTool::name() const {
    return "notepad_append";
}

std::string_view NotepadAppendTool::description() const {
    return "Jot a nugget into your working notepad — your own running plan, a "
           "distilled finding, or a decision-and-why worth keeping. The pad "
           "lives outside the conversation, survives compaction, and is shown "
           "back to you each turn. Append-only; jot the nugget, not the "
           "transcript.";
}

Json NotepadAppendTool::schema_json() const {
    return Json{
        {"name",        "notepad_append"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"note", Json{
                    {"type",        "string"},
                    {"description", "The nugget to jot. A single finding, plan "
                                    "line, or decision-and-why. Must not be empty."},
                    {"minLength",   1}
                }},
                {"section", Json{
                    {"type",        "string"},
                    {"description", "Optional light header to group this note "
                                    "under (e.g. \"plan\", \"auth findings\", "
                                    "\"decisions\")."}
                }}
            }},
            {"required",             Json::array({"note"})},
            {"additionalProperties", false}
        }}
    };
}

ToolResult NotepadAppendTool::run(const Json& args, ToolContext& ctx) {
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    auto it_note = args.find("note");
    if (it_note == args.end() || !it_note->is_string()) {
        return ToolResult::error(
            "notepad_append: 'note' argument must be a non-empty string.");
    }
    const std::string note = it_note->get<std::string>();
    if (note.empty()) {
        return ToolResult::error("notepad_append: 'note' must not be empty.");
    }

    std::string section;
    auto it_section = args.find("section");
    if (it_section != args.end() && it_section->is_string()) {
        section = it_section->get<std::string>();
    }

    const std::string key = NotepadStore::session_key(ctx.session_id, ctx.agent_id);

    auto res = store_.append(key, note, section);
    if (!res) {
        return ToolResult::error(res.error());
    }

    const std::string pad = store_.read(key);
    Json payload = Json{
        {"appended",  true},
        {"section",   section},
        {"pad_chars", pad.size()},
    };
    return ToolResult::ok("noted (" + std::to_string(pad.size()) +
                          " chars on the pad)", payload);
}

} // namespace batbox::tools
