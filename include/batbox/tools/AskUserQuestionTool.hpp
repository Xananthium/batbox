// include/batbox/tools/AskUserQuestionTool.hpp
//
// batbox::tools::AskUserQuestionTool — pause the agent and surface a set of
// multiple-choice questions to the user.
//
// Contract (blueprints table, task CPP 5.20):
//
//   Tool name       : "AskUserQuestion"
//   Arguments:
//     questions (array, 1–4 elements)
//       Each element:
//         question  (string, required) — the question text
//         header    (string, optional) — section header shown above the picker
//         multiSelect (bool, optional, default false)
//                                      — allow more than one option to be chosen
//         options   (array, 2–4 elements)
//           Each element:
//             label       (string, required) — option text
//             description (string, optional) — one-sentence elaboration
//
//   Returns JSON object:
//     {
//       "answers": [
//         { "question": "<text>", "answer": "<label>"  }   // single-select
//         { "question": "<text>", "answer": ["<label>", …] }  // multi-select
//       ]
//     }
//   User cancellation for a question (EOF / empty input): answer = "(no answer provided)"
//
//   Errors:
//     - Non-TTY stdin with no prompt_fn wired:
//         ToolResult::error("AskUserQuestion not available in headless mode")
//     - 0 or more than 4 questions: ToolResult::error("questions must contain 1–4 entries")
//     - Option count outside [2,4]: ToolResult::error("each question must have 2–4 options")
//     - ctx.cancel_token fired: ToolResult::error("cancelled")
//
//   TUI integration:
//     The constructor accepts an optional PromptFn callback:
//       using PromptFn = std::function<std::vector<std::string>(
//           const QuestionSpec&)>;
//     When wired (non-null), PromptFn is called instead of the stdin path.
//     When null (default), the tool falls back to a terminal stdin prompt.
//     The FTXUI ModalPicker will be wired here once component C3 is ready.
//
//   Permission gate:
//     is_read_only()           = false  (blocks; has side-effects on agent flow)
//     requires_confirmation()  = false  (IS the confirmation/interaction mechanism)
//
// Blueprint contract: batbox::tools::AskUserQuestionTool (task CPP 5.20)

#pragma once

#include <batbox/tools/ITool.hpp>
#include <batbox/core/Json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace batbox::tools {

// =============================================================================
// QuestionSpec — parsed representation of one question element
// =============================================================================

/// Parsed form of a single element in the `questions` array.
struct QuestionSpec {
    std::string              question;     ///< The question text (required).
    std::string              header;       ///< Section header (empty if absent).
    bool                     multi_select; ///< True → allow multiple selections.
    std::vector<std::string> labels;       ///< Option labels (2–4 entries).
    std::vector<std::string> descriptions; ///< Parallel descriptions (may be empty strings).
};

// =============================================================================
// AskUserQuestionTool
// =============================================================================

/// Implements the "AskUserQuestion" tool: pauses the agent, presents 1–4
/// multiple-choice questions to the user, and returns their selections as a
/// JSON answers array.
///
/// TUI integration: inject a PromptFn at construction to replace the stdin
/// path.  When PromptFn is null, the tool uses a terminal readline-style prompt
/// and returns error if stdin is not a TTY.
class AskUserQuestionTool final : public ITool {
public:
    // -------------------------------------------------------------------------
    // PromptFn — TUI callback type
    // -------------------------------------------------------------------------

    /// Callback invoked once per question to obtain the user's selection.
    ///
    /// @param spec  The fully-parsed question (header, labels, multi_select, …).
    ///
    /// @returns  Zero or more label strings chosen by the user.
    ///           An empty vector means the user cancelled / gave no answer.
    ///
    /// The callback MUST NOT throw; return an empty vector on cancellation.
    using PromptFn = std::function<std::vector<std::string>(const QuestionSpec&)>;

    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct with no TUI callback — uses stdin fallback (errors in headless).
    AskUserQuestionTool() = default;

    /// Construct with a custom prompt callback (used by TUI and by tests).
    /// @param prompt_fn  Called once per question; may not be null when supplied.
    explicit AskUserQuestionTool(PromptFn prompt_fn);

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "AskUserQuestion".
    [[nodiscard]] std::string_view name() const override;

    /// Returns a one-sentence description surfaced to the model in the schema.
    [[nodiscard]] std::string_view description() const override;

    // -------------------------------------------------------------------------
    // OpenAI tool schema
    // -------------------------------------------------------------------------

    /// Returns the full OpenAI tools[*].function JSON object for this tool.
    [[nodiscard]] Json schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Process all questions and collect user answers.
    ///
    /// Validates args, then for each question either:
    ///   a) Invokes prompt_fn_ (when wired) — used by TUI and tests.
    ///   b) Falls back to an interactive stdin prompt (TTY required).
    ///
    /// Polls ctx.cancel_token before each question.
    ///
    /// @returns ToolResult::ok(json_body)   — JSON answers array on success.
    ///          ToolResult::error(message)  — validation failure, headless mode,
    ///                                        or cancellation.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — this tool has side-effects on agent execution flow.
    [[nodiscard]] bool is_read_only() const override { return false; }

    /// false — this tool IS the interaction/confirmation mechanism; no outer
    /// confirmation prompt should be shown before invoking it.
    [[nodiscard]] bool requires_confirmation() const override { return false; }

private:
    /// Optional TUI/test callback.  Null → use stdin path.
    PromptFn prompt_fn_;

    /// Parse and validate the `questions` array from args.
    /// Returns an error string on failure, empty string on success.
    [[nodiscard]] static std::string parse_questions(
        const Json&                  args,
        std::vector<QuestionSpec>&   out);

    /// Prompt via stdin (terminal fallback).
    /// Returns selected labels (empty = user cancelled).
    /// Caller must have verified stdin is a TTY before invoking.
    [[nodiscard]] static std::vector<std::string> prompt_via_stdin(
        const QuestionSpec& spec);
};

} // namespace batbox::tools
