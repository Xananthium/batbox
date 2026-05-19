// include/batbox/tui/AskqPromptFactory.hpp
// =============================================================================
// PEXT3 1.6 — make_askq_prompt_fn factory
//
// Free function that returns the AskUserQuestionTool::PromptFn closure to
// install into TuiCallbacks::askq_prompt_fn.
//
// Design (data-abstraction over OO, Karla DRY directive):
//   - Free function returning std::function — no class, no vtable.
//   - When nuclear == true: returns a zero-capture closure that immediately
//     returns {} without posting any modal events.
//   - When nuclear == false: returns the normal interactive closure that
//     posts QuestionShow, blocks on await_user_answer(), then posts
//     QuestionResolved and maps the result to vector<string>.
//   - Closure captures by value only what it needs (weak_ptr + ref where
//     lifetime is guaranteed by App::run() stack frame).
//
// Blueprint contract (blueprints table, task PEXT3 1.6):
//   symbol_name : make_askq_prompt_fn
//   symbol_type : function
//   file_path   : include/batbox/tui/AskqPromptFactory.hpp (declaration)
//                 src/tui/AskqPromptFactory.cpp (definition)
//   signature   : batbox::tools::AskUserQuestionTool::PromptFn
//                 make_askq_prompt_fn(
//                     bool nuclear,
//                     std::weak_ptr<batbox::tui::QuestionCard> question_card,
//                     batbox::tui::ScreenManager& screen_mgr)
// =============================================================================

#pragma once

#include <batbox/tools/AskUserQuestionTool.hpp>
#include <batbox/tui/QuestionCard.hpp>
#include <batbox/tui/Screen.hpp>

#include <memory>

namespace batbox::tui {

/// Returns the PromptFn closure for TuiCallbacks::askq_prompt_fn.
///
/// @param nuclear       When true, returns a closure that immediately returns {}
///                      without rendering or posting any modal events.
///                      When false, returns the normal interactive closure.
/// @param question_card Weak reference to the QuestionCard owned by App::run().
///                      Captured by value in the non-nuclear closure only.
/// @param screen_mgr    Reference to the ScreenManager for event posting.
///                      Only captured (by ref) in the non-nuclear closure;
///                      lifetime guaranteed by App::run() stack frame which
///                      outlives the event loop.
///
/// @returns A callable matching AskUserQuestionTool::PromptFn.
[[nodiscard]] batbox::tools::AskUserQuestionTool::PromptFn
make_askq_prompt_fn(bool nuclear,
                    std::weak_ptr<batbox::tui::QuestionCard> question_card,
                    batbox::tui::ScreenManager& screen_mgr);

} // namespace batbox::tui
