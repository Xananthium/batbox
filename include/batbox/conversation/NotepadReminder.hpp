// include/batbox/conversation/NotepadReminder.hpp
// =============================================================================
// batbox::conversation::NotepadReminder — per-turn notepad re-injection
// (DIS-981, S6).  The goose `get_moim` / opencode `reminders.apply` equivalent.
//
// The working notepad (tools::NotepadStore) must be surfaced to the model every
// turn so its own jotted plan/findings/decisions stay in view.  CACHE
// DISCIPLINE is the load-bearing constraint here:
//
//   The notepad changes every turn.  It MUST NOT go into the cached
//   system-prompt prefix (compose_system_prompt) — that would bust the KV/
//   prefix cache every single turn and fight batbox's cache-preservation ethos
//   ("only ever mutate the tail").  So the pad is injected as a per-turn TAIL
//   reminder: a trailing message appended AFTER the cached prefix and after all
//   conversation history.  The longest byte-identical prefix (system prompt +
//   stable history) keeps its cache hit; only the appended tail message
//   changes turn to turn.
//
// This file deliberately does NOT touch compose_system_prompt — that function
// stays a pure, cacheable function of (plan_mode, working_dir), unaffected by
// pad mutations (AC4b).
// =============================================================================

#pragma once

#include <batbox/inference/ChatRequest.hpp>

#include <string>

namespace batbox::conversation {

// =============================================================================
// compose_notepad_reminder()
//
// Pure formatter.  Wraps a pad slice in a delimited reminder block.  Returns
// the empty string when `pad_slice` is empty (nothing to inject).
// =============================================================================
[[nodiscard]] std::string compose_notepad_reminder(const std::string& pad_slice);

// =============================================================================
// apply_notepad_reminder()
//
// Append the notepad reminder to a fully-built ChatRequest as the FINAL
// (tail) message, mutating only the tail so the cached prefix is preserved.
//
//   - When `pad_slice` is empty: no-op, returns false (prompt untouched — the
//     cache is never disturbed by an empty pad).
//   - Otherwise: pushes a trailing {role:"system", content:<reminder>} message
//     onto req.messages and returns true.
//
// Injecting as the last message (rather than mutating the first system message)
// is what keeps the cache-preserving property: everything before the reminder
// is byte-identical across turns.
// =============================================================================
bool apply_notepad_reminder(batbox::inference::ChatRequest& req,
                            const std::string&              pad_slice);

} // namespace batbox::conversation
