// src/tools/AskUserQuestionTool.cpp
//
// Implementation of batbox::tools::AskUserQuestionTool.
//
// Blueprint contract (task CPP 5.20):
//   AskUserQuestionTool::run — condition_variable wait on user response;
//   when prompt_fn_ is wired (TUI or test), delegates to it; otherwise
//   performs an interactive readline-style stdin prompt (TTY required).
//
// TUI integration point:
//   App::init() should call:
//     registry.register_tool(std::make_unique<AskUserQuestionTool>(
//         [tui_ctx](const QuestionSpec& s) { return tui_ctx.show_modal(s); }));
//   Until then, the no-arg constructor is used and the stdin path is active.

#include <batbox/tools/AskUserQuestionTool.hpp>
#include <batbox/core/Json.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

// POSIX isatty
#include <unistd.h>

namespace batbox::tools {

// =============================================================================
// Construction
// =============================================================================

AskUserQuestionTool::AskUserQuestionTool(PromptFn prompt_fn)
    : prompt_fn_(std::move(prompt_fn))
{}

// =============================================================================
// ITool identity
// =============================================================================

std::string_view AskUserQuestionTool::name() const {
    return "AskUserQuestion";
}

std::string_view AskUserQuestionTool::description() const {
    return "Asks the user one or more multiple-choice questions and returns "
           "their selections; use to gather preferences, clarify ambiguity, "
           "or offer implementation choices.";
}

// =============================================================================
// OpenAI tool schema
// =============================================================================

Json AskUserQuestionTool::schema_json() const {
    // option sub-object schema
    Json option_schema = {
        {"type", "object"},
        {"properties", {
            {"label",       {{"type", "string"}, {"description", "Short option text shown to the user."}}},
            {"description", {{"type", "string"}, {"description", "Optional one-sentence elaboration shown beneath the label."}}}
        }},
        {"required", Json::array({"label"})}
    };

    // question sub-object schema
    Json question_schema = {
        {"type", "object"},
        {"properties", {
            {"question",    {{"type", "string"}, {"description", "The question text displayed to the user."}}},
            {"header",      {{"type", "string"}, {"description", "Optional section header shown above the picker."}}},
            {"multiSelect", {{"type", "boolean"}, {"description", "When true, the user may select more than one option."}}},
            {"options",     {
                {"type", "array"},
                {"description", "The choices presented to the user (2–4 elements)."},
                {"items", option_schema},
                {"minItems", 2},
                {"maxItems", 4}
            }}
        }},
        {"required", Json::array({"question", "options"})}
    };

    return {
        {"name",        "AskUserQuestion"},
        {"description", description()},
        {"parameters", {
            {"type", "object"},
            {"properties", {
                {"questions", {
                    {"type", "array"},
                    {"description", "Array of 1–4 questions to ask the user in sequence."},
                    {"items", question_schema},
                    {"minItems", 1},
                    {"maxItems", 4}
                }}
            }},
            {"required", Json::array({"questions"})}
        }}
    };
}

// =============================================================================
// Private helpers
// =============================================================================

// static
std::string AskUserQuestionTool::parse_questions(
    const Json&               args,
    std::vector<QuestionSpec>& out)
{
    // Presence check
    if (!args.contains("questions") || !args["questions"].is_array()) {
        return "missing or invalid 'questions' array";
    }

    const auto& qs = args["questions"];

    if (qs.empty() || qs.size() > 4) {
        return "questions must contain 1-4 entries";
    }

    out.reserve(qs.size());

    for (std::size_t qi = 0; qi < qs.size(); ++qi) {
        const auto& q = qs[qi];

        if (!q.is_object()) {
            return "questions[" + std::to_string(qi) + "] must be an object";
        }

        // Required: question
        if (!q.contains("question") || !q["question"].is_string()) {
            return "questions[" + std::to_string(qi) + "].question is required (string)";
        }

        // Required: options
        if (!q.contains("options") || !q["options"].is_array()) {
            return "questions[" + std::to_string(qi) + "].options must be an array";
        }

        const auto& opts = q["options"];
        if (opts.size() < 2 || opts.size() > 4) {
            return "each question must have 2-4 options";
        }

        QuestionSpec spec;
        spec.question     = q["question"].get<std::string>();
        spec.header       = q.value("header", std::string{});
        spec.multi_select = q.value("multiSelect", false);

        spec.labels.reserve(opts.size());
        spec.descriptions.reserve(opts.size());

        for (std::size_t oi = 0; oi < opts.size(); ++oi) {
            const auto& opt = opts[oi];
            if (!opt.is_object() || !opt.contains("label") || !opt["label"].is_string()) {
                return "questions[" + std::to_string(qi)
                    + "].options[" + std::to_string(oi)
                    + "].label is required (string)";
            }
            spec.labels.push_back(opt["label"].get<std::string>());
            spec.descriptions.push_back(opt.value("description", std::string{}));
        }

        out.push_back(std::move(spec));
    }

    return {};  // success
}

// static
std::vector<std::string> AskUserQuestionTool::prompt_via_stdin(
    const QuestionSpec& spec)
{
    // Print header if present
    if (!spec.header.empty()) {
        std::cout << "\n=== " << spec.header << " ===\n";
    }

    std::cout << "\n" << spec.question << "\n";
    std::cout << (spec.multi_select
        ? "(Enter comma-separated numbers, or leave blank to skip)\n"
        : "(Enter the number of your choice, or leave blank to skip)\n");

    for (std::size_t i = 0; i < spec.labels.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << spec.labels[i];
        if (!spec.descriptions[i].empty()) {
            std::cout << " — " << spec.descriptions[i];
        }
        std::cout << "\n";
    }
    std::cout << "> " << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
        // EOF / stream error → cancellation
        return {};
    }

    // Trim whitespace
    const auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char c){ return !std::isspace(c); }));
    };
    const auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
    };
    ltrim(line);
    rtrim(line);

    if (line.empty()) {
        return {};  // user skipped
    }

    std::vector<std::string> chosen;

    if (spec.multi_select) {
        // Parse comma-separated indices
        std::istringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            ltrim(token);
            rtrim(token);
            if (token.empty()) continue;
            try {
                int idx = std::stoi(token);
                if (idx >= 1 && static_cast<std::size_t>(idx) <= spec.labels.size()) {
                    const std::string& lbl = spec.labels[static_cast<std::size_t>(idx - 1)];
                    // Deduplicate
                    if (std::find(chosen.begin(), chosen.end(), lbl) == chosen.end()) {
                        chosen.push_back(lbl);
                    }
                }
                // Silently ignore out-of-range indices
            } catch (...) {
                // Non-numeric token: ignore
            }
        }
    } else {
        // Single-select: parse one index
        try {
            int idx = std::stoi(line);
            if (idx >= 1 && static_cast<std::size_t>(idx) <= spec.labels.size()) {
                chosen.push_back(spec.labels[static_cast<std::size_t>(idx - 1)]);
            }
            // Out-of-range → empty (treated as no answer)
        } catch (...) {
            // Non-numeric input → empty (treated as no answer)
        }
    }

    return chosen;
}

// =============================================================================
// Execution
// =============================================================================

ToolResult AskUserQuestionTool::run(const Json& args, ToolContext& ctx) {
    // --- Cancellation check (fast path) ---
    if (ctx.is_cancelled()) {
        return ToolResult::error("cancelled");
    }

    // --- Headless mode guard ---
    // When no TUI callback is wired and stdin is not a terminal,
    // interactive prompts cannot be presented.
    if (!prompt_fn_ && !::isatty(STDIN_FILENO)) {
        return ToolResult::error("AskUserQuestion not available in headless mode");
    }

    // --- Parse and validate questions ---
    std::vector<QuestionSpec> specs;
    std::string parse_err = parse_questions(args, specs);
    if (!parse_err.empty()) {
        return ToolResult::error(parse_err);
    }

    // --- Collect answers ---
    Json answers = Json::array();

    for (const QuestionSpec& spec : specs) {
        // Poll cancellation before each blocking call
        if (ctx.is_cancelled()) {
            return ToolResult::error("cancelled");
        }

        std::vector<std::string> selections;

        if (prompt_fn_) {
            // TUI / test path: delegate to the injected callback
            selections = prompt_fn_(spec);
        } else {
            // Terminal stdin path (TTY verified above)
            selections = prompt_via_stdin(spec);
        }

        // Build the per-question answer entry
        Json entry;
        entry["question"] = spec.question;

        if (selections.empty()) {
            // User cancelled or gave no selection for this question
            entry["answer"] = "(no answer provided)";
        } else if (spec.multi_select) {
            entry["answer"] = selections;
        } else {
            // Single-select: exactly one label
            entry["answer"] = selections.front();
        }

        answers.push_back(std::move(entry));
    }

    // Build the result body — compact JSON for the model, structured payload
    // for TUI consumers that want to inspect the array directly.
    Json result_obj = {{"answers", answers}};
    std::string body = result_obj.dump(2);

    return ToolResult::ok(std::move(body), std::move(result_obj));
}

} // namespace batbox::tools
