// include/batbox/tools/NotepadStore.hpp
// =============================================================================
// batbox::tools::NotepadStore — the working-memory notepad (DIS-981, S6).
//
// The notepad is batbox's THIRD memory horizon (project_batbox.md):
//   ephemeral (conversation, lost on compaction)
//     → WORKING = THE NOTEPAD (this class)
//       → durable (agentic_memory / PARA, cross-session facts).
//
// It is the agent's own scratch, kept mid-task by its own hand: a running plan,
// distilled findings, "the gold I'll still need 20 turns from now," and
// decisions-and-why.  Its load-bearing property is that it lives OUT-OF-BAND —
// it is NOT a conversation Message, so it survives compaction by construction
// (the Compactor only ever rewrites the message array; it never touches this
// store).  That is precisely what makes aggressive context-trimming SAFE:
// a tool-subagent distills raw output → the agent writes the gold line to the
// pad → the raw can be pruned from context → the knowledge persists.
//
// Store home — disk-backed (NOT in-process, NOT SessionStore):
//   The design soul (project_batbox.md) wants the pad inspectable
//   ("I want to see you working") and crash-surviving.  A plain markdown file
//   per session key is the natural, grep-able, operator-readable artifact, so
//   NotepadStore writes <root>/<key>.md and flushes on every append.
//   We deliberately do NOT reuse:
//     - TodoWriteTool's in-process map (lost on crash, not inspectable), nor
//     - session::SessionStore (JSONL message-array machinery — wrong shape for
//       a freeform append-structured pad).
//   We DO lift TodoWriteTool's session-keying (session_id else agent_id) and
//   its static-mutex thread-safety pattern.
//
// Shape — append with light headers (the report's "fix the shape"):
//   NOT goose's overwrite-the-whole-blob, NOT opencode's status checklist.
//   Each entry is appended (never replaces prior content) under an optional
//   light "## <section>" header.  LEAST_FORCE: the caller jots the nugget; the
//   store enforces no transcript.
//
// Grep-able:
//   read() returns the whole pad; grep() returns only the paragraph blocks
//   matching a query — "what did I note about auth?" — without re-reading the
//   whole pad.  Because every tool routes through the S7 dispatch envelope, a
//   large pad read is automatically eligible for S1+S4 distillation — that
//   comes for free; NotepadStore does not special-case it.
//
// Lifecycle:
//   born at task start (lazily, on first append — a one-line born header is
//   written) → survives compaction (out-of-band, automatic) → archived/exported
//   at task end via archive()/export_pad().  The "notepad → curator → durable
//   facts" ingest is a fleet/Paperclip concern and is OUT OF SCOPE here; this
//   class only exposes the pad (export_pad) so a future integration can harvest
//   it.
//
// Thread-safety:
//   All appends/archives are serialised by a single process-wide mutex (defined
//   in the .cpp).  Reads open-and-read the file and do not lock writers out;
//   the OS guarantees a consistent view of a completed append.
//
// Build standalone (from repo root, x64-linux triplet):
//   c++ -std=c++20 -I include -I build/vcpkg_installed/x64-linux/include \
//       tests/unit/test_notepad_store.cpp \
//       src/tools/NotepadStore.cpp \
//       src/core/Paths.cpp \
//       -o /tmp/test_notepad_store && /tmp/test_notepad_store
// =============================================================================

#pragma once

#include <batbox/core/Result.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

namespace batbox::tools {

// =============================================================================
// NotepadStore
// =============================================================================

class NotepadStore {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /// Construct a store rooted at `root` (the directory that holds <key>.md
    /// pad files).  When `root` is empty, default_root() is used.
    explicit NotepadStore(std::filesystem::path root = {});

    /// Default pad root: $BATBOX_NOTEPAD_DIR when set (used by tests to point
    /// at a temp dir), otherwise batbox::paths::config_dir() / "notepads".
    [[nodiscard]] static std::filesystem::path default_root();

    /// Derive the session key from identity: session_id when non-empty,
    /// otherwise agent_id, otherwise the literal "default".  Mirrors
    /// TodoWriteTool's keying so one task == one pad.
    [[nodiscard]] static std::string session_key(const std::string& session_id,
                                                 const std::string& agent_id);

    // -------------------------------------------------------------------------
    // Write — append a nugget under an optional light header.
    // -------------------------------------------------------------------------

    /// Append `note` to the pad for `key`, under an optional "## <section>"
    /// header.  Creates the pad (with a one-line born header) on first append.
    /// Append-only: prior content is never replaced.  `note` must be non-empty.
    [[nodiscard]] batbox::Result<void> append(const std::string& key,
                                              const std::string& note,
                                              const std::string& section = {});

    // -------------------------------------------------------------------------
    // Read / query
    // -------------------------------------------------------------------------

    /// Full pad text for `key`; empty string when no pad exists yet.
    [[nodiscard]] std::string read(const std::string& key) const;

    /// Grep the pad: return only the paragraph blocks (entries) that contain
    /// `query` (case-insensitive substring), joined, bounded to `max_chars`.
    /// An empty `query` returns the whole pad (bounded).
    [[nodiscard]] std::string grep(const std::string& key,
                                   const std::string& query,
                                   std::size_t        max_chars = 8192) const;

    /// A bounded slice of the pad for per-turn re-injection: the TAIL of the
    /// pad up to `max_chars` (the most recent notes always surface).  When the
    /// pad is larger than the budget, a "(earlier notes truncated)" marker is
    /// prepended.  Empty string when no pad exists — callers skip injection
    /// then, so the prompt cache is never touched by an empty pad.
    [[nodiscard]] std::string reinjection_slice(const std::string& key,
                                                std::size_t        max_chars = 4096) const;

    // -------------------------------------------------------------------------
    // Lifecycle — export / archive at task end.
    // -------------------------------------------------------------------------

    /// Export the full pad (== read()).  Named hook for the future
    /// notepad → curator → durable-facts harvest (curator itself out of scope).
    [[nodiscard]] std::string export_pad(const std::string& key) const;

    /// Archive the pad at task end: move <root>/<key>.md to
    /// <root>/archive/<key>.md (with a numeric suffix if a prior archive
    /// exists).  No-op (ok) when the pad does not exist.
    [[nodiscard]] batbox::Result<void> archive(const std::string& key);

    // -------------------------------------------------------------------------
    // Introspection (inspection / tests)
    // -------------------------------------------------------------------------

    /// Absolute path of the pad file for `key` (the file need not exist yet).
    [[nodiscard]] std::filesystem::path pad_path(const std::string& key) const;

    /// The configured pad root.
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    /// Filesystem-safe filename stem for a session key (sanitises path-unsafe
    /// characters; empty → "default").
    [[nodiscard]] static std::string sanitise_key(const std::string& key);

    std::filesystem::path root_;
};

} // namespace batbox::tools
