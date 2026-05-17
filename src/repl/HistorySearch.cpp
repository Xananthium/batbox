// src/repl/HistorySearch.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::repl::HistorySearch.
//
// Scoring formula recap (see header for rationale):
//
//   score_match(entry, query):
//     1. Substring hit  → quality = 1.0
//     2. Fuzzy hit (all query chars appear in order in entry):
//          matched_ratio = (float)len(query) / len(query)  [always 1.0 when
//                          all chars found — so the denominator is len(query)]
//          first_match   = index of first matched char in entry
//          last_match    = index of last matched char in entry
//          span          = last_match - first_match + 1
//          gap_penalty   = (float)(span - len(query)) / len(entry)
//          quality       = 1.0 - 0.4 * gap_penalty    [in (0.6, 1.0) range]
//     3. No hit → quality = 0.0
//
//   recency_weight = (float)(1 + deque_index) / history.size()
//     deque_index 0 = oldest, size-1 = newest.
//
//   final_score = quality * recency_weight
//
// Sorting is stable descending by final_score so equal-scored entries
// preserve their relative history order (newest first among equals).
// ---------------------------------------------------------------------------

#include "batbox/repl/HistorySearch.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace batbox::repl {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

HistorySearch::HistorySearch(History& history)
    : history_{history}
    , matches_{}
    , index_{0}
    , active_{false}
{}

// ---------------------------------------------------------------------------
// to_lower — ASCII case fold
// ---------------------------------------------------------------------------

/*static*/ std::string HistorySearch::to_lower(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (const unsigned char c : s) {
        result.push_back(static_cast<char>(std::tolower(c)));
    }
    return result;
}

// ---------------------------------------------------------------------------
// score_match
//
// Returns quality in [0, 1]:
//   1.0  — contiguous substring match
//   (0,1) — fuzzy (all query chars in order, non-contiguous)
//   0.0  — no match
//
// Pre-condition: both strings are already lowercase.
// ---------------------------------------------------------------------------

/*static*/ float HistorySearch::score_match(std::string_view entry_lower,
                                             std::string_view query_lower) noexcept {
    if (query_lower.empty()) {
        // Empty query matches everything with perfect quality.
        return 1.0f;
    }

    if (entry_lower.empty()) {
        return 0.0f;
    }

    // --- Substring check (O(N)) ---
    if (entry_lower.find(query_lower) != std::string_view::npos) {
        return 1.0f;
    }

    // --- Fuzzy check: all query chars in order ---
    const auto qlen = static_cast<int>(query_lower.size());
    const auto elen = static_cast<int>(entry_lower.size());

    int qi = 0;                   // query position
    int first_match_pos = -1;
    int last_match_pos  = -1;

    for (int ei = 0; ei < elen && qi < qlen; ++ei) {
        if (entry_lower[static_cast<std::size_t>(ei)] ==
            query_lower[static_cast<std::size_t>(qi)]) {
            if (first_match_pos == -1) {
                first_match_pos = ei;
            }
            last_match_pos = ei;
            ++qi;
        }
    }

    if (qi < qlen) {
        // Not all query characters found.
        return 0.0f;
    }

    // All chars matched.  Compute gap penalty based on the span of the match.
    const int span       = last_match_pos - first_match_pos + 1;
    const int gaps       = span - qlen;                  // chars between matches
    const float gap_ratio = static_cast<float>(gaps) / static_cast<float>(elen);
    const float quality  = 1.0f - 0.4f * gap_ratio;

    // Clamp to avoid floating-point surprises.
    return (quality < 0.0f) ? 0.0f : (quality > 1.0f ? 1.0f : quality);
}

// ---------------------------------------------------------------------------
// filter_matches
// ---------------------------------------------------------------------------

const std::vector<MatchResult>& HistorySearch::filter_matches(std::string_view query) {
    matches_.clear();
    index_  = 0;
    active_ = true;

    const std::size_t n = history_.size();
    if (n == 0) {
        return matches_;
    }

    const std::string query_lower = to_lower(query);

    // Walk deque from oldest (age = n-1) to newest (age = 0).
    // deque_index is assigned 0..n-1 (oldest→newest) for recency weighting.
    for (std::size_t age = n; age > 0; --age) {
        const std::size_t deque_index = n - age;  // 0 = oldest, n-1 = newest
        const auto entry_opt = history_.at(age - 1);
        if (!entry_opt.has_value()) {
            continue;
        }
        const std::string& text = entry_opt.value();

        const std::string entry_lower = to_lower(text);
        const float quality = score_match(entry_lower, query_lower);

        if (quality == 0.0f && !query_lower.empty()) {
            // No match — skip.
            continue;
        }

        // recency_weight: range (0, 1].  Newest entry gets weight 1.0.
        const float recency_weight =
            static_cast<float>(deque_index + 1) / static_cast<float>(n);

        const float final_score = quality * recency_weight;
        if (final_score > 0.0f) {
            matches_.push_back(MatchResult{text, final_score});
        }
    }

    // Sort descending by score; stable sort preserves relative order for ties.
    std::stable_sort(matches_.begin(), matches_.end(),
                     [](const MatchResult& a, const MatchResult& b) {
                         return a.score > b.score;
                     });

    // Deduplicate by text (keep first / highest-scored occurrence).
    {
        auto it = matches_.begin();
        while (it != matches_.end()) {
            auto dup = std::find_if(it + 1, matches_.end(),
                                    [&it](const MatchResult& r) {
                                        return r.text == it->text;
                                    });
            while (dup != matches_.end()) {
                dup = matches_.erase(dup);
                dup = std::find_if(dup, matches_.end(),
                                   [&it](const MatchResult& r) {
                                       return r.text == it->text;
                                   });
            }
            ++it;
        }
    }

    return matches_;
}

// ---------------------------------------------------------------------------
// next_match
// ---------------------------------------------------------------------------

std::optional<std::string> HistorySearch::next_match() {
    if (matches_.empty()) {
        return std::nullopt;
    }

    const std::string text = matches_[index_].text;

    // Advance and wrap.
    index_ = (index_ + 1) % matches_.size();

    return text;
}

// ---------------------------------------------------------------------------
// selected
// ---------------------------------------------------------------------------

std::optional<std::string> HistorySearch::selected() const {
    if (matches_.empty()) {
        return std::nullopt;
    }
    return matches_[index_].text;
}

// ---------------------------------------------------------------------------
// reset
// ---------------------------------------------------------------------------

void HistorySearch::reset() {
    matches_.clear();
    index_  = 0;
    active_ = false;
}

// ---------------------------------------------------------------------------
// active
// ---------------------------------------------------------------------------

bool HistorySearch::active() const noexcept {
    return active_;
}

// ---------------------------------------------------------------------------
// matches
// ---------------------------------------------------------------------------

const std::vector<MatchResult>& HistorySearch::matches() const noexcept {
    return matches_;
}

} // namespace batbox::repl
