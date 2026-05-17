// include/batbox/conversation/SystemPrompt.hpp
// =============================================================================
// batbox::conversation::SystemPrompt — system prompt composition (CPP 3.8).
//
// Composes the final system prompt sent to the model on every turn by merging:
//
//   Layer 0 — Plan-mode prefix (injected first when plan_mode is true)
//             "You are in Plan Mode. Only read-only tools are available..."
//
//   Layer 1 — Base BatBox system prompt (always present, hardcoded)
//             Describes BatBox, its capabilities, and operating principles.
//
//   Layer 2 — User BATBOX.md  (~/.batbox/BATBOX.md, if present)
//             User-wide customisation / standing instructions.
//
//   Layer 3 — Project BATBOX.md  (<working_dir>/BATBOX.md, or walks up via
//             batbox::paths::project_root(), if present)
//             Per-project instructions, highest-priority user text.
//
// Ordering rationale:
//   Plan-mode prefix must appear first so the model sees its operational
//   constraint before any user content.  Within user-supplied text, project
//   instructions come last (closest to the end of the system prompt) so they
//   take semantic precedence over user-wide defaults.
//
// @import resolution:
//   Each BATBOX.md is scanned for lines of the form:
//       @import relative-path.md
//   The referenced file is resolved relative to the directory containing the
//   importing file and its content is spliced in at that line.  Only a single
//   level of import is supported — imported files are never re-scanned for
//   further @import directives (prevents infinite recursion without a full
//   cycle-detection graph).  Import cycles (A→B→A) that span the top-level
//   call are detected via a seen-paths set and silently skipped.
//
// Caching:
//   compose_system_prompt() is a pure function of its inputs.  Callers that
//   want to avoid repeated filesystem reads across turns should cache the
//   result and invalidate on config hot-reload (observe Config::version_seq).
//
// Thread safety:
//   compose_system_prompt() is safe to call from multiple threads concurrently;
//   it performs only read-only filesystem operations and has no shared state.
//
// Build standalone (from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_system_prompt.cpp \
//       src/conversation/SystemPrompt.cpp \
//       src/core/Paths.cpp \
//       -o /tmp/test_system_prompt && /tmp/test_system_prompt
// =============================================================================

#pragma once

#include <filesystem>
#include <string>

namespace batbox::conversation {

// =============================================================================
// compose_system_prompt()
//
// Builds the complete system-prompt string for one inference turn.
//
// Parameters:
//   plan_mode    — When true, prepend the plan-mode read-only constraint prefix
//                  before all other content.
//   working_dir  — The working directory from which to resolve the project
//                  BATBOX.md.  The function walks up from working_dir looking
//                  for a BATBOX.md file (via batbox::paths::project_root()),
//                  consistent with how other project-root lookups work.
//                  Pass std::filesystem::current_path() when no override is
//                  needed.
//
// Returns:
//   A fully composed system-prompt string ready to be sent as the "system"
//   role message in the ChatRequest.  Never throws — if filesystem reads fail
//   the affected layer is silently omitted and the function continues.
//
// Example:
//   std::string sp = batbox::conversation::compose_system_prompt(
//       plan_mode.is_planning(),   // true → plan-mode prefix included
//       working_dir_               // from Conversation constructor
//   );
// =============================================================================
[[nodiscard]] std::string compose_system_prompt(
    bool                          plan_mode,
    const std::filesystem::path&  working_dir);

// =============================================================================
// k_base_system_prompt
//
// The hardcoded base system prompt text for BatBox.  Exposed as a public
// constant so that tests can verify its presence in composed output without
// duplicating the string.
// =============================================================================
extern const char* const k_base_system_prompt;

// =============================================================================
// k_plan_mode_prefix
//
// The plan-mode constraint prefix prepended when plan_mode == true.  Exposed
// for the same test-accessibility reason as k_base_system_prompt.
// =============================================================================
extern const char* const k_plan_mode_prefix;

} // namespace batbox::conversation
