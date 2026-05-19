// src/tui/AskqPromptFactory.cpp
// =============================================================================
// PEXT3 1.6 — make_askq_prompt_fn implementation
//
// Factory that returns the PromptFn closure installed into
// TuiCallbacks::askq_prompt_fn.  See header for the full contract.
// =============================================================================

#include <batbox/tui/AskqPromptFactory.hpp>
#include <batbox/tui/Events.hpp>

#include <batbox/core/Logging.hpp>

namespace batbox::tui {

batbox::tools::AskUserQuestionTool::PromptFn
make_askq_prompt_fn(bool nuclear,
                    std::weak_ptr<batbox::tui::QuestionCard> question_card,
                    batbox::tui::ScreenManager& screen_mgr)
{
    // -------------------------------------------------------------------------
    // Nuclear short-circuit path (PEXT3 1.6 load-bearing fix for the user-
    // reported bug: --nuclear still shows AskUserQuestion modals).
    //
    // When nuclear is true, return a zero-capture closure that:
    //   - returns {} immediately (no answer / auto-declined)
    //   - posts NO modal events to the FTXUI event loop
    //   - logs at INFO level so the auto-decline is traceable in batbox.log
    //
    // Spec pseudocode (from pext3-tasks.md 1.6):
    //   if (nuclear) {
    //       return [](const batbox::tools::QuestionSpec&) -> std::vector<std::string> {
    //           BATBOX_LOG_INFO("nuclear: ask-user-question auto-declined without modal");
    //           return {};
    //       };
    //   }
    // -------------------------------------------------------------------------
    if (nuclear) {
        return [](const batbox::tools::QuestionSpec& /*spec*/)
                   -> std::vector<std::string> {
            BATBOX_LOG_INFO("nuclear: ask-user-question auto-declined without modal");
            return {};
        };
    }

    // -------------------------------------------------------------------------
    // Normal interactive path.
    //
    // Mirrors the inline lambda previously at src/App.cpp:1083-1136.
    // Captured by value: question_card_weak (copy of weak_ptr, no shared
    // ownership), screen_mgr (by ref — lifetime guaranteed by App::run() stack
    // frame which outlives the event loop, same as the original inline lambda).
    //
    // Implementation steps:
    //   1. Lock the weak_ptr; return {} if the card was destroyed.
    //   2. Build QuestionShowPayload from QuestionSpec (field-by-field mapping).
    //   3. Pre-load the payload into the card via set_spec() (thread-safe).
    //   4. Post make_question_show_event(payload) to wake the FTXUI render loop.
    //   5. Block on QuestionCard::await_user_answer() (condition_variable handoff).
    //   6. Post make_question_resolved_event(resolved) to clear the overlay.
    //   7. Map QuestionResolvedPayload → vector<string>.
    // -------------------------------------------------------------------------
    return [question_card_weak = std::move(question_card),
            &screen_mgr](const batbox::tools::QuestionSpec& spec)
               -> std::vector<std::string> {
        auto qcard = question_card_weak.lock();
        if (!qcard) {
            // Card has been destroyed — return empty (no answer).
            return {};
        }

        // Build QuestionShowPayload from QuestionSpec.
        batbox::tui::QuestionShowPayload payload;
        payload.header             = spec.header;
        payload.question           = spec.question;
        payload.multi_select       = spec.multi_select;
        payload.labels             = spec.labels;
        payload.descriptions       = spec.descriptions;
        payload.allow_freeform     = false;
        payload.allow_escape_hatch = false;
        payload.callback           = nullptr;

        // Pre-load the spec into the card so it is ready for the first render
        // frame even before the event loop processes the event.
        qcard->set_spec(payload);

        // Post make_question_show_event (carries payload) — this event has the
        // form "batbox.question-show:TOKEN" which differs from the plain
        // Events::QuestionShow sentinel, so it falls through to
        // ChatView::OnEvent where extract_question_show() extracts the payload,
        // calls set_spec() again (idempotent), and sets show_question_card_ = true.
        screen_mgr.post_event(batbox::tui::make_question_show_event(payload));

        // Block this worker thread until the user resolves or cancels.
        // QuestionCard::await_user_answer() uses an internal condition_variable
        // that OnEvent() notifies on Enter/Esc — no polling required.
        batbox::tui::QuestionResolvedPayload resolved = qcard->await_user_answer();

        // Post QuestionResolved so ChatView clears show_question_card_ and
        // the overlay disappears on the next render frame.
        screen_mgr.post_event(batbox::tui::make_question_resolved_event(resolved));

        // Map resolved payload → vector<string> for AskUserQuestionTool::run().
        // Cancelled or empty selection → empty vector (tool formats as
        // "(no answer provided)").
        if (resolved.cancelled) {
            return {};
        }
        return resolved.chosen_labels;
    };
}

} // namespace batbox::tui
