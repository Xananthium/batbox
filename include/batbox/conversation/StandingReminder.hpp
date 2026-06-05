// include/batbox/conversation/StandingReminder.hpp
// =============================================================================
// batbox::conversation::StandingReminder — per-turn "warm subagents available
// for follow-up" re-injection (DIS-988, S2/S3 AC4).  The goose `get_moim`
// status-surfacing equivalent, built on the SAME cache-disciplined TAIL shape
// as S6's NotepadReminder.
//
// A promoted (standing) subagent keeps its conversation warm and interrogable
// beside the parent loop.  For the parent to actually USE a warm window it must
// SEE which handles are available — so the supervisor's standing list is
// surfaced to the model every turn, exactly like the notepad.
//
// CACHE DISCIPLINE (load-bearing — identical to NotepadReminder):
//   The standing list changes turn to turn (recency reorder, promote, evict).
//   It MUST NOT go into the cached system-prompt prefix (compose_system_prompt)
//   — that would bust the KV/prefix cache every turn.  So it is injected as a
//   per-turn TAIL reminder: a trailing message appended AFTER the cached prefix
//   and all history.  The longest byte-identical prefix keeps its cache hit;
//   only the appended tail changes.  When there are no warm subagents the
//   injection is a no-op (empty pad semantics) so the cache is never disturbed.
//
// LAYERING: this header intentionally takes a local POD (StandingHandle), NOT
// agents::AgentSupervisor::StandingStatus, so the conversation layer keeps no
// dependency on the agents layer.  The App wiring converts standing_status() →
// vector<StandingHandle> in the provider callback.
// =============================================================================

#pragma once

#include <batbox/inference/ChatRequest.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace batbox::conversation {

// =============================================================================
// StandingHandle — one warm subagent's line (layer-local POD).
// =============================================================================
struct StandingHandle {
    std::string id;           ///< Opaque agent handle (interrogate() key).
    std::string name;         ///< Display name (AgentSpec::name).
    std::string status_line;  ///< One-line status (already truncated upstream).
};

/// Default cap on the number of warm-subagent lines surfaced per turn.  Keeps
/// the tail reminder bounded regardless of pool size (AC4 "bounded length").
inline constexpr std::size_t kStandingReminderMaxHandles = 8;

// =============================================================================
// compose_standing_reminder()
//
// Pure formatter.  Wraps up to `max_handles` warm-subagent lines in a delimited
// reminder block.  Returns the empty string when `handles` is empty (nothing to
// inject — the cache stays untouched).  When the pool exceeds `max_handles`, the
// first `max_handles` are shown (callers pass them most-recently-interrogated
// first) and a trailing "(+N more)" note bounds the rest.
// =============================================================================
[[nodiscard]] std::string compose_standing_reminder(
    const std::vector<StandingHandle>& handles,
    std::size_t                        max_handles = kStandingReminderMaxHandles);

// =============================================================================
// apply_standing_reminder()
//
// Append the standing reminder to a fully-built ChatRequest as the FINAL (tail)
// message, mutating only the tail so the cached prefix is preserved.
//
//   - When `handles` is empty: no-op, returns false (prompt and cache untouched).
//   - Otherwise: pushes a trailing {role:"system", content:<reminder>} message
//     onto req.messages and returns true.
// =============================================================================
bool apply_standing_reminder(
    batbox::inference::ChatRequest&    req,
    const std::vector<StandingHandle>& handles,
    std::size_t                        max_handles = kStandingReminderMaxHandles);

} // namespace batbox::conversation
