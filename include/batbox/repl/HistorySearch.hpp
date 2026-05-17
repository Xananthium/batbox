// include/batbox/repl/HistorySearch.hpp
// ---------------------------------------------------------------------------
// Ctrl+R fuzzy / substring history search for batbox REPL.
//
// Design overview
// ---------------
//  • HistorySearch wraps a History& (non-owning) and provides incremental
//    query-narrowing on every keypress.
//  • filter_matches(query) re-runs a full ranked search from scratch on the
//    current history entries.  This is intentional: history is bounded to
//    10 000 entries (worst case) and a full re-scan per keypress is O(N·|q|)
//    which is well under 1 ms on modern hardware.
//  • Scoring is a product of recency weight × match quality, so a recent
//    exact-match outscores an older fuzzy match.
//
// Match quality scoring
// ----------------------
//  Each candidate is scored by score_match(entry, query) which returns a
//  floating-point value in [0, 1]:
//    0.0  — no match at all (entry is excluded from results)
//    >0.0 — partial score (fuzzy: all query chars appear in order)
//    1.0  — maximum (query is a contiguous substring of entry,
//            case-insensitive)
//
//  Fuzzy scoring (all chars found in order, but not contiguous):
//    base = (matched_chars / query_length)
//    gap_penalty = (total_gaps / entry_length)
//    quality = base * (1.0 - 0.4 * gap_penalty)
//
//  Recency weight:
//    The deque index of the most-recent entry is (size-1).
//    recency = (1.0 + index) / size   → range (0, 1]
//
//  Final score = quality * recency_weight
//
// API
// ---
//  HistorySearch(history)          — construct; does not start a search.
//  filter_matches(query)           — run/re-run search; returns ranked vector.
//  next_match()                    — cycle forward through current matches
//                                    (wraps around); returns nullopt if empty.
//  selected() const                — current selection text, or nullopt.
//  reset()                         — clear selection and matches.
//  active() const                  — true when a search session is in progress.
//
// Thread safety
// -------------
//  Same as History: single-thread only.  Call from the REPL input thread.
//
// No FTXUI dependency — pure logic; the TUI overlay consumes the results.
// ---------------------------------------------------------------------------
#pragma once

#include "batbox/repl/History.hpp"

#include <optional>
#include <string>
#include <vector>
#include <cstddef>

namespace batbox::repl {

// ---------------------------------------------------------------------------
// MatchResult — one ranked hit returned by filter_matches().
// ---------------------------------------------------------------------------
struct MatchResult {
    std::string text;   ///< The history entry text (verbatim).
    float       score;  ///< Composite recency×quality score in (0, 1].
};

// ---------------------------------------------------------------------------
// HistorySearch
// ---------------------------------------------------------------------------
class HistorySearch {
public:
    // -----------------------------------------------------------------------
    // Constructor
    //
    // history — reference to the History object this search operates over.
    //           The reference must outlive the HistorySearch instance.
    // -----------------------------------------------------------------------
    explicit HistorySearch(History& history);

    // Non-copyable (reference member).
    HistorySearch(const HistorySearch&)            = delete;
    HistorySearch& operator=(const HistorySearch&) = delete;

    // Move-constructible; move-assign deleted (reference member).
    HistorySearch(HistorySearch&&)            = default;
    HistorySearch& operator=(HistorySearch&&) = delete;

    ~HistorySearch() = default;

    // -----------------------------------------------------------------------
    // filter_matches(query)
    //
    // Scans all history entries, scores each one against query, sorts by
    // descending score, and returns the ranked vector.
    //
    // Matching is case-insensitive (ASCII fold only — Unicode not normalised).
    //
    // Empty query: every entry is returned ordered by recency (most recent
    // first), all with score == 1.0.
    //
    // Side effects:
    //   • Replaces the internal match list.
    //   • Resets the cycling index to 0 (first / best match selected).
    //   • Sets active() == true.
    //
    // Returns the same ranked vector for the caller's convenience.
    // -----------------------------------------------------------------------
    const std::vector<MatchResult>& filter_matches(std::string_view query);

    // -----------------------------------------------------------------------
    // next_match()
    //
    // Steps to the next match in the ranked list (Ctrl+R repeat).
    //
    // Behaviour:
    //   • If there are no matches, returns std::nullopt.
    //   • Cycles forward through matches, wrapping from last back to first.
    //   • Each call advances the index by one.
    //
    // Returns the text of the newly selected match.
    // -----------------------------------------------------------------------
    std::optional<std::string> next_match();

    // -----------------------------------------------------------------------
    // selected() const
    //
    // Returns the text of the currently selected match (the entry at the
    // current cycling index), or std::nullopt when there are no matches.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<std::string> selected() const;

    // -----------------------------------------------------------------------
    // reset()
    //
    // Clears the match list, resets the cycling index, and sets active() to
    // false.  Call on Escape or after the user commits a selection.
    // -----------------------------------------------------------------------
    void reset();

    // -----------------------------------------------------------------------
    // active() const
    //
    // Returns true after filter_matches() has been called at least once
    // without a subsequent reset().  Used by the TUI to show/hide the overlay.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool active() const noexcept;

    // -----------------------------------------------------------------------
    // matches() const — read-only view of the current match list.
    // -----------------------------------------------------------------------
    [[nodiscard]] const std::vector<MatchResult>& matches() const noexcept;

private:
    // -----------------------------------------------------------------------
    // score_match(entry_lower, query_lower)
    //
    // Returns composite match quality in [0, 1]:
    //   0.0  — no match
    //   1.0  — exact substring
    //   (0,1) — fuzzy (all chars appear in order, not contiguous)
    //
    // Both strings must be pre-lowercased by the caller.
    // -----------------------------------------------------------------------
    [[nodiscard]] static float score_match(std::string_view entry_lower,
                                           std::string_view query_lower) noexcept;

    // -----------------------------------------------------------------------
    // to_lower(s) — ASCII case fold to a new string.
    // -----------------------------------------------------------------------
    [[nodiscard]] static std::string to_lower(std::string_view s);

    History&                history_;   ///< Non-owning reference to History.
    std::vector<MatchResult> matches_;  ///< Current ranked match list.
    std::size_t              index_;    ///< Cycling index into matches_.
    bool                     active_;   ///< Whether a session is active.
};

} // namespace batbox::repl
