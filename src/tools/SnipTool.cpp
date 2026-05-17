// src/tools/SnipTool.cpp
// ---------------------------------------------------------------------------
// Implementation of SnipTool — named snippet save/load/list/delete backed
// by ~/.batbox/snippets/<name>.txt files.
//
// See include/batbox/tools/SnipTool.hpp for the full contract.
// ---------------------------------------------------------------------------

#include <batbox/tools/SnipTool.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::tools {

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr std::string_view kToolName        = "Snip";
constexpr std::string_view kStorageSubdir   = ".batbox/snippets";
constexpr std::string_view kSnippetExt      = ".txt";

// ---------------------------------------------------------------------------
// expand_tilde
//
// Resolves a leading "~/" in a path string to the user's home directory.
// Uses HOME environment variable on POSIX; falls back to a temp path if
// HOME is not set (should not occur in production).
// ---------------------------------------------------------------------------
fs::path expand_tilde(std::string_view p) {
    if (p.empty() || p[0] != '~') {
        return fs::path(p);
    }
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return fs::path(home) / fs::path(std::string(p.substr(2)));
    }
    // Fallback: use temp directory — avoids a hard crash but should not
    // happen in a properly configured shell environment.
    return fs::temp_directory_path() / "batbox_snippets_fallback";
}

// ---------------------------------------------------------------------------
// first_line
//
// Returns the first non-empty line of `text`, truncated to 80 characters,
// or "(empty)" if the content is blank.
// ---------------------------------------------------------------------------
std::string first_line(const std::string& text) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim trailing whitespace for a clean preview.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            if (line.size() > 80) {
                line.resize(77);
                line += "...";
            }
            return line;
        }
    }
    return "(empty)";
}

} // anonymous namespace

// ===========================================================================
// SnipTool — static helpers
// ===========================================================================

fs::path SnipTool::snippets_dir() {
    return expand_tilde("~/" + std::string(kStorageSubdir));
}

std::string SnipTool::ensure_snippets_dir() {
    const fs::path dir = snippets_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return "Snip: cannot create snippets directory '"
             + dir.string() + "': " + ec.message();
    }
    return {};
}

std::string SnipTool::validate_name(const std::string& name) {
    if (name.empty()) {
        return "Snip: 'name' must not be empty.";
    }
    if (name[0] == '.') {
        return "Snip: 'name' must not start with '.'.";
    }
    for (char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c))
                     || c == '_'
                     || c == '-'
                     || c == '.';
        if (!ok) {
            return std::string("Snip: 'name' contains invalid character '")
                 + c + "'. Only [a-zA-Z0-9_.-] are allowed.";
        }
    }
    // Reject path-separator characters and ".." component explicitly
    // (belt-and-suspenders — already caught by the char loop above on POSIX,
    // but explicit for clarity and Windows forward-slash support).
    if (name.find('/') != std::string::npos
     || name.find('\\') != std::string::npos
     || name == "..") {
        return "Snip: 'name' must not contain path separators or '..'.";
    }
    return {};
}

fs::path SnipTool::snippet_path(const std::string& name) {
    return snippets_dir() / (name + std::string(kSnippetExt));
}

// ===========================================================================
// SnipTool — action implementations
// ===========================================================================

ToolResult SnipTool::action_save(const std::string& name,
                                  const std::string& content) {
    // Ensure storage directory exists.
    if (const std::string err = ensure_snippets_dir(); !err.empty()) {
        return ToolResult::error(err);
    }

    const fs::path target = snippet_path(name);

    // Write to a temporary file in the same directory, then rename atomically
    // (same strategy as WriteTool to avoid partial writes on crash).
    const fs::path tmp = target.parent_path()
                       / (std::string(".snip_tmp_") + name + std::string(kSnippetExt));

    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) {
            return ToolResult::error(
                "Snip: cannot open temporary file '" + tmp.string() + "' for writing.");
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            return ToolResult::error(
                "Snip: write failed for snippet '" + name + "'.");
        }
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Rename failed (e.g. cross-device); attempt copy-then-remove.
        fs::copy_file(tmp, target,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove(tmp);  // Best-effort cleanup.
            return ToolResult::error(
                "Snip: cannot save snippet '" + name + "': " + ec.message());
        }
        fs::remove(tmp);
    }

    const std::string path_str = target.string();
    Json payload{{"path", path_str}, {"name", name}, {"bytes", content.size()}};
    return ToolResult::ok("Saved snippet '" + name + "' → " + path_str, std::move(payload));
}

ToolResult SnipTool::action_load(const std::string& name,
                                  ToolContext& ctx) {
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    const fs::path path = snippet_path(name);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return ToolResult::error("Snip: snippet '" + name + "' does not exist.");
    }
    if (!fs::is_regular_file(path, ec) || ec) {
        return ToolResult::error(
            "Snip: '" + name + "' exists but is not a regular file.");
    }

    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        return ToolResult::error(
            "Snip: cannot open snippet '" + name + "' for reading.");
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    if (!in && !in.eof()) {
        return ToolResult::error("Snip: read error for snippet '" + name + "'.");
    }

    const std::string content = buf.str();
    Json payload{{"name", name}, {"content", content}, {"bytes", content.size()}};
    return ToolResult::ok(content, std::move(payload));
}

ToolResult SnipTool::action_list(ToolContext& ctx) {
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    const fs::path dir = snippets_dir();
    std::error_code ec;

    if (!fs::exists(dir, ec) || ec) {
        return ToolResult::ok("No snippets saved yet. Use action=\"save\" to create one.");
    }
    if (!fs::is_directory(dir, ec) || ec) {
        return ToolResult::error(
            "Snip: snippets path '" + dir.string() + "' exists but is not a directory.");
    }

    // Collect all .txt files in the directory (non-recursive).
    struct SnipEntry {
        std::string name;
        std::string preview;
        fs::file_time_type mtime;
    };
    std::vector<SnipEntry> entries;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) { break; }
        if (!entry.is_regular_file()) { continue; }
        const fs::path& p = entry.path();
        if (p.extension() != kSnippetExt) { continue; }

        // Skip temp files left over from a crashed save.
        const std::string fname = p.filename().string();
        if (fname.rfind(".snip_tmp_", 0) == 0) { continue; }

        const std::string snip_name = p.stem().string();

        // Read first line for preview.
        std::string preview;
        {
            std::ifstream in(p, std::ios::in | std::ios::binary);
            if (in) {
                std::ostringstream buf;
                buf << in.rdbuf();
                preview = first_line(buf.str());
            } else {
                preview = "(unreadable)";
            }
        }

        auto mtime = entry.last_write_time(ec);
        if (ec) { mtime = fs::file_time_type{}; ec.clear(); }

        entries.push_back({snip_name, preview, mtime});
    }

    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // Sort alphabetically by name for deterministic output.
    std::sort(entries.begin(), entries.end(),
              [](const SnipEntry& a, const SnipEntry& b) {
                  return a.name < b.name;
              });

    if (entries.empty()) {
        return ToolResult::ok("No snippets saved yet. Use action=\"save\" to create one.");
    }

    std::ostringstream body;
    Json names_json = Json::array();
    for (const auto& e : entries) {
        body << e.name << ": " << e.preview << '\n';
        names_json.push_back(e.name);
    }

    Json payload{{"snippets", std::move(names_json)}, {"count", entries.size()}};
    return ToolResult::ok(body.str(), std::move(payload));
}

ToolResult SnipTool::action_delete(const std::string& name) {
    const fs::path path = snippet_path(name);
    std::error_code ec;

    if (!fs::exists(path, ec) || ec) {
        return ToolResult::error("Snip: snippet '" + name + "' does not exist.");
    }

    fs::remove(path, ec);
    if (ec) {
        return ToolResult::error(
            "Snip: cannot delete snippet '" + name + "': " + ec.message());
    }

    Json payload{{"name", name}, {"path", path.string()}};
    return ToolResult::ok("Deleted snippet '" + name + "'.", std::move(payload));
}

// ===========================================================================
// SnipTool — ITool implementation
// ===========================================================================

std::string_view SnipTool::name() const {
    return kToolName;
}

std::string_view SnipTool::description() const {
    return "Save, load, list, or delete named text/code snippets "
           "stored in ~/.batbox/snippets/.";
}

Json SnipTool::schema_json() const {
    return Json{
        {"name",        "Snip"},
        {"description", "Save, load, list, or delete named text/code snippets "
                        "stored in ~/.batbox/snippets/."},
        {"parameters", Json{
            {"type",     "object"},
            {"properties", Json{
                {"action", Json{
                    {"type",        "string"},
                    {"enum",        Json::array({"save", "load", "list", "delete"})},
                    {"description", "Operation to perform: "
                                    "save=persist content, load=retrieve content, "
                                    "list=show all snippets, delete=remove a snippet."}
                }},
                {"name", Json{
                    {"type",        "string"},
                    {"description", "Snippet identifier. Required for save, load, delete. "
                                    "Only [a-zA-Z0-9_.-] characters are allowed."}
                }},
                {"content", Json{
                    {"type",        "string"},
                    {"description", "Text content to save. Required for action=save."}
                }}
            }},
            {"required", Json::array({"action"})}
        }}
    };
}

ToolResult SnipTool::run(const Json& args, ToolContext& ctx) {
    try {
        // ------------------------------------------------------------------
        // 1. Check cancellation up front.
        // ------------------------------------------------------------------
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        // ------------------------------------------------------------------
        // 2. Extract and validate 'action'.
        // ------------------------------------------------------------------
        const auto action_it = args.find("action");
        if (action_it == args.end() || !action_it->is_string()) {
            return ToolResult::error(
                "Snip: required argument 'action' is missing or not a string.");
        }
        const std::string action = action_it->get<std::string>();

        // Validate action value.
        if (action != "save" && action != "load"
         && action != "list" && action != "delete") {
            return ToolResult::error(
                "Snip: unknown action '" + action
                + "'. Expected one of: save, load, list, delete.");
        }

        // ------------------------------------------------------------------
        // 3. Plan-mode gate: mutating actions are not allowed in Plan mode.
        // ------------------------------------------------------------------
        if (ctx.is_plan_mode() && (action == "save" || action == "delete")) {
            return ToolResult::error(
                "Snip: action '" + action + "' is not allowed in Plan mode.");
        }

        // ------------------------------------------------------------------
        // 4. Dispatch 'list' early — it doesn't need a name.
        // ------------------------------------------------------------------
        if (action == "list") {
            return action_list(ctx);
        }

        // ------------------------------------------------------------------
        // 5. Extract and validate 'name' (required for save/load/delete).
        // ------------------------------------------------------------------
        const auto name_it = args.find("name");
        if (name_it == args.end() || !name_it->is_string()) {
            return ToolResult::error(
                "Snip: argument 'name' is required for action='" + action + "'.");
        }
        const std::string snip_name = name_it->get<std::string>();

        if (const std::string err = validate_name(snip_name); !err.empty()) {
            return ToolResult::error(err);
        }

        // ------------------------------------------------------------------
        // 6. Dispatch to the appropriate action.
        // ------------------------------------------------------------------
        if (action == "load") {
            return action_load(snip_name, ctx);
        }

        if (action == "delete") {
            return action_delete(snip_name);
        }

        // action == "save"
        const auto content_it = args.find("content");
        if (content_it == args.end() || !content_it->is_string()) {
            return ToolResult::error(
                "Snip: argument 'content' is required for action='save'.");
        }
        const std::string content = content_it->get<std::string>();

        return action_save(snip_name, content);

    } catch (const std::exception& ex) {
        return ToolResult::error(
            std::string("Snip: unexpected error: ") + ex.what());
    } catch (...) {
        return ToolResult::error("Snip: unknown error.");
    }
}

} // namespace batbox::tools
