// src/tools/NotepadReadTool.cpp
//
// Implementation of batbox::tools::NotepadReadTool (DIS-981, S6).

#include <batbox/tools/NotepadReadTool.hpp>
#include <batbox/tools/ToolContext.hpp>
#include <batbox/tools/ToolResult.hpp>
#include <batbox/core/Json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace batbox::tools {

namespace {
constexpr std::size_t k_default_max_chars = 8192;
}

std::string_view NotepadReadTool::name() const {
    return "notepad_read";
}

std::string_view NotepadReadTool::description() const {
    return "Read or grep your working notepad. Pass a 'query' to return only "
           "the entries that mention it (e.g. \"auth\"); omit it to read the "
           "whole pad. Read-only.";
}

Json NotepadReadTool::schema_json() const {
    return Json{
        {"name",        "notepad_read"},
        {"description", std::string(description())},
        {"parameters", Json{
            {"type",       "object"},
            {"properties", Json{
                {"query", Json{
                    {"type",        "string"},
                    {"description", "Case-insensitive substring. Returns only "
                                    "notepad entries containing it. Omit to "
                                    "return the whole pad."}
                }},
                {"max_chars", Json{
                    {"type",        "integer"},
                    {"description", "Maximum characters to return (default 8192)."},
                    {"minimum",     1}
                }}
            }},
            {"required",             Json::array()},
            {"additionalProperties", false}
        }}
    };
}

ToolResult NotepadReadTool::run(const Json& args, ToolContext& ctx) {
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    std::string query;
    auto it_query = args.find("query");
    if (it_query != args.end() && it_query->is_string()) {
        query = it_query->get<std::string>();
    }

    std::size_t max_chars = k_default_max_chars;
    auto it_max = args.find("max_chars");
    if (it_max != args.end() && it_max->is_number_integer()) {
        const long long v = it_max->get<long long>();
        if (v > 0) max_chars = static_cast<std::size_t>(v);
    }

    const std::string key = NotepadStore::session_key(ctx.session_id, ctx.agent_id);

    // grep() with an empty query returns the whole (bounded) pad — so this one
    // path serves both "read" and "grep".
    const std::string out = store_.grep(key, query, max_chars);

    if (out.empty()) {
        const std::string note = query.empty()
            ? "(notepad is empty)"
            : "(no notepad entries match \"" + query + "\")";
        return ToolResult::ok(note, Json{{"matches", 0}, {"query", query}});
    }

    return ToolResult::ok(out, Json{{"chars", out.size()}, {"query", query}});
}

} // namespace batbox::tools
