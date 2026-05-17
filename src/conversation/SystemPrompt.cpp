// src/conversation/SystemPrompt.cpp
// =============================================================================
// Implementation of batbox::conversation::compose_system_prompt (CPP 3.8).
//
// Composition order (each section separated by a blank line):
//   [plan-mode prefix]   — only when plan_mode == true
//   [base system prompt] — always
//   [user BATBOX.md]     — ~/.batbox/BATBOX.md, if present
//   [project BATBOX.md]  — <project_root>/BATBOX.md, if present
//
// @import resolution:
//   Each BATBOX.md may contain lines of the form "@import relative-path.md".
//   Those lines are replaced with the content of the referenced file, resolved
//   relative to the directory that contains the importing file.  Imported files
//   are NOT re-scanned (single-level only).  A per-call seen-paths set prevents
//   the same file from being imported more than once even across user + project
//   layers.
// =============================================================================

#include <batbox/conversation/SystemPrompt.hpp>
#include <batbox/core/Paths.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace batbox::conversation {

// =============================================================================
// Public string constants
// =============================================================================

// TUI-PLAN-T4: cards-then-plan workflow.
// The phrase "PLAN MODE" appears below so existing sentinel checks still pass.
const char* const k_plan_mode_prefix =
    "You are in PLAN MODE. Your job: produce a clear, scoped plan for the\n"
    "user to approve BEFORE any file edits or commands run.\n"
    "\n"
    "Workflow:\n"
    "1. If the user's request has ambiguity (file choice, framework, scope,\n"
    "   naming, etc.), call the AskUserQuestion tool to pose multi-choice\n"
    "   clarifying questions. One question at a time, up to 4 total. Each\n"
    "   question MUST include 2-4 distinct options (not yes/no).\n"
    "2. Once you have enough information, draft the plan as a numbered list\n"
    "   of concrete steps (filename:line where applicable).\n"
    "3. Call ExitPlanMode with the plan markdown. The user will see\n"
    "   Approve / Reject / Edit buttons. Do NOT proceed past ExitPlanMode\n"
    "   without approval.\n"
    "\n"
    "Constraints:\n"
    "- You may read files and grep the codebase freely in PLAN MODE\n"
    "  (read-only tools allowed).\n"
    "- You MUST NOT call Write, Edit, or Bash (mutating) tools until\n"
    "  ExitPlanMode is approved.\n"
    "- Keep plans under 10 steps. Decompose larger work into\n"
    "  'phase 1 / phase 2' plans.";

const char* const k_base_system_prompt =
    "You are BatBox, an AI coding assistant running as a local CLI/TUI\n"
    "application. You have no telemetry, no auto-update, and no cloud\n"
    "dependencies beyond the inference endpoint the user has configured.\n"
    "\n"
    "## Core capabilities\n"
    "- Read and write files via the Read and Write tools\n"
    "- Execute shell commands via the Bash tool (with user permission)\n"
    "- Search the filesystem with the Glob tool\n"
    "- Fetch web pages and search the web via WebFetch / WebSearch\n"
    "- Spawn sub-agents for parallel work via the Task tool\n"
    "- Integrate with external tools via MCP servers\n"
    "\n"
    "## Operating principles\n"
    "- Be concise. Prefer code and diffs over lengthy prose explanations.\n"
    "- Ask before destructive operations (delete, overwrite, run as root).\n"
    "- Respect the active permission mode: default, plan, acceptEdits, nuclear.\n"
    "- Session history is persisted locally; no data leaves the machine unless\n"
    "  the user explicitly invokes a web or API tool.\n"
    "- When uncertain about intent, ask a short clarifying question rather than\n"
    "  guessing and potentially causing harm.\n"
    "\n"
    "## Response style\n"
    "- Use markdown formatting for structure (headings, code blocks, lists).\n"
    "- Keep replies proportional to the complexity of the request.\n"
    "- For multi-step operations, show what you are about to do before doing it.\n"
    "\n"
    "## Plan mode\n"
    "Do NOT call the EnterPlanMode tool unless the user has explicitly asked you\n"
    "to plan a multi-step task (e.g. typed /plan or said \"make a plan\") or you\n"
    "are about to undertake a non-trivial change involving more than three file\n"
    "edits and want to confirm scope first. Casual questions, single-file edits,\n"
    "and quick fixes must be handled directly without entering plan mode. Calling\n"
    "EnterPlanMode unsolicited wastes the user's time and will be treated as a\n"
    "misbehaviour.";

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/// Read the entire content of a text file into a string.
/// Returns an empty string if the file cannot be opened (does not throw).
std::string read_file_silent(const std::filesystem::path& p) {
    std::ifstream f{p};
    if (!f.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Resolve @import directives in `source_text`.
///
/// @param source_text   Raw content of a BATBOX.md file.
/// @param source_dir    Directory containing the importing file; used to
///                      resolve relative @import paths.
/// @param seen          Canonical paths already imported in this composition
///                      call; updated as new imports are resolved.  Prevents
///                      the same file from being imported twice and breaks
///                      A→B→A cycles.
///
/// @return The source text with each "@import <path>" line replaced by the
///         content of the referenced file.  Import errors are silently skipped.
std::string resolve_imports(const std::string&                       source_text,
                             const std::filesystem::path&             source_dir,
                             std::unordered_set<std::string>&         seen) {
    std::istringstream in{source_text};
    std::ostringstream out;
    std::string        line;
    bool               first_line = true;

    while (std::getline(in, line)) {
        if (!first_line) {
            out << '\n';
        }
        first_line = false;

        // Detect "@import <relative-path>" — must be at the start of the line.
        if (line.size() > 8 && line.compare(0, 8, "@import ") == 0) {
            const std::string rel = line.substr(8);
            // Strip leading/trailing whitespace from the path token.
            const auto trim_start = rel.find_first_not_of(" \t\r");
            const auto trim_end   = rel.find_last_not_of(" \t\r");
            if (trim_start == std::string::npos) {
                // Blank @import — skip silently.
                continue;
            }
            const std::string import_path_str =
                rel.substr(trim_start, trim_end - trim_start + 1);

            const std::filesystem::path import_path =
                source_dir / import_path_str;

            // Resolve to a canonical path for dedup purposes; fall back to
            // lexically-normal if the file does not exist.
            std::string canon;
            try {
                canon = std::filesystem::canonical(import_path).string();
            } catch (...) {
                canon = import_path.lexically_normal().string();
            }

            // Skip if this file has already been imported (cycle protection).
            if (seen.count(canon)) {
                continue;
            }
            seen.insert(canon);

            // Read the imported file content; skip silently on failure.
            const std::string imported = read_file_silent(import_path);
            if (imported.empty()) {
                continue;
            }

            // Splice in the imported content WITHOUT recursing (single-level).
            out << imported;
            // Ensure the imported block ends with a newline before the next
            // line of the parent file.
            if (!imported.empty() && imported.back() != '\n') {
                out << '\n';
            }
        } else {
            out << line;
        }
    }

    return out.str();
}

/// Load a BATBOX.md file, resolve its @import directives, and return the
/// resulting text.  Returns an empty string if the file does not exist or
/// cannot be read.
///
/// @param batbox_md_path  Absolute path to the BATBOX.md file to load.
/// @param seen            Shared set of already-seen canonical paths.
std::string load_batbox_md(const std::filesystem::path&     batbox_md_path,
                            std::unordered_set<std::string>& seen) {
    // Canonicalise the top-level file path for dedup tracking.
    std::string canon;
    try {
        canon = std::filesystem::canonical(batbox_md_path).string();
    } catch (...) {
        canon = batbox_md_path.lexically_normal().string();
    }

    if (seen.count(canon)) {
        return {};
    }
    seen.insert(canon);

    const std::string raw = read_file_silent(batbox_md_path);
    if (raw.empty()) {
        return {};
    }

    const std::filesystem::path source_dir = batbox_md_path.parent_path();
    return resolve_imports(raw, source_dir, seen);
}

/// Append a non-empty section to `out`, preceded by a blank line separator
/// if `out` is already non-empty.
void append_section(std::string& out, const std::string& section) {
    if (section.empty()) {
        return;
    }
    if (!out.empty()) {
        // Ensure exactly one blank-line separator between sections.
        if (out.back() != '\n') {
            out += '\n';
        }
        out += '\n';
    }
    out += section;
}

} // anonymous namespace

// =============================================================================
// compose_system_prompt()
// =============================================================================

std::string compose_system_prompt(bool                         plan_mode,
                                   const std::filesystem::path& working_dir) {
    std::string                      result;
    std::unordered_set<std::string>  seen;  // canonical paths already imported

    // ---- Layer 0: plan-mode prefix ------------------------------------------
    if (plan_mode) {
        append_section(result, std::string{k_plan_mode_prefix});
    }

    // ---- Layer 1: base BatBox system prompt ---------------------------------
    append_section(result, std::string{k_base_system_prompt});

    // ---- Layer 2: user BATBOX.md  (~/.batbox/BATBOX.md) ---------------------
    {
        const std::filesystem::path user_batbox_md =
            batbox::paths::config_dir() / "BATBOX.md";
        const std::string user_text = load_batbox_md(user_batbox_md, seen);
        append_section(result, user_text);
    }

    // ---- Layer 3: project BATBOX.md -----------------------------------------
    //
    // Walk up from working_dir looking for a BATBOX.md (mirrors project_root()
    // semantics).  We walk manually rather than calling project_root() so that
    // we can use working_dir rather than the process cwd, which is important for
    // sub-agents that have a different working directory.
    {
        namespace fs = std::filesystem;

        fs::path dir = working_dir.empty() ? fs::current_path() : working_dir;
        // Resolve to an absolute path (in case working_dir is relative).
        if (!dir.is_absolute()) {
            try {
                dir = fs::canonical(dir);
            } catch (...) {
                dir = fs::current_path() / dir;
            }
        }

        fs::path project_batbox_md;
        // Walk toward the filesystem root.
        fs::path prev;
        while (dir != prev) {
            const fs::path candidate = dir / "BATBOX.md";
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                project_batbox_md = candidate;
                break;
            }
            prev = dir;
            dir  = dir.parent_path();
        }

        if (!project_batbox_md.empty()) {
            const std::string proj_text = load_batbox_md(project_batbox_md, seen);
            append_section(result, proj_text);
        }
    }

    return result;
}

} // namespace batbox::conversation
