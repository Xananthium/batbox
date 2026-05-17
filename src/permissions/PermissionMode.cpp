// src/permissions/PermissionMode.cpp
//
// Implementation of PermissionMode helpers.
// See include/batbox/permissions/PermissionMode.hpp for the contract.

#include <batbox/permissions/PermissionMode.hpp>

#include <algorithm>
#include <array>
#include <string>

namespace batbox::permissions {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// ASCII-only lowercasing of a single character.
constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// Return a lowercase copy of s (ASCII-only; sufficient for mode names).
std::string to_lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(ascii_lower(c));
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// to_string
// ---------------------------------------------------------------------------

std::string_view to_string(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::Default:     return "default";
        case PermissionMode::Plan:        return "plan";
        case PermissionMode::AcceptEdits: return "acceptedits";
        case PermissionMode::Nuclear:     return "nuclear";
    }
    // Unreachable for a well-formed enum value, but the compiler may warn
    // without an explicit return outside the switch on some targets.
    return "default";
}

// ---------------------------------------------------------------------------
// mode_from_string
// ---------------------------------------------------------------------------

batbox::Result<PermissionMode> mode_from_string(std::string_view s) {
    const std::string lower = to_lower_ascii(s);

    // Canonical names
    if (lower == "default")     return PermissionMode::Default;
    if (lower == "plan")        return PermissionMode::Plan;
    if (lower == "acceptedits") return PermissionMode::AcceptEdits;
    if (lower == "nuclear")     return PermissionMode::Nuclear;

    // AcceptEdits aliases
    if (lower == "accept-edits" || lower == "accept_edits") {
        return PermissionMode::AcceptEdits;
    }

    // Nuclear deprecated aliases — still parse successfully;
    // callers that care about deprecation check the alias themselves
    // (e.g. by comparing the original input against the canonical name returned
    // by to_string before invoking this function).
    if (lower == "skip-permissions"
        || lower == "dangerously-skip-permissions"
        || lower == "skip_permissions") {
        return PermissionMode::Nuclear;
    }

    return batbox::Err("unknown permission mode: '" + std::string(s) + "'");
}

// ---------------------------------------------------------------------------
// cycle_next
// ---------------------------------------------------------------------------

PermissionMode cycle_next(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::Default:     return PermissionMode::Plan;
        case PermissionMode::Plan:        return PermissionMode::AcceptEdits;
        case PermissionMode::AcceptEdits: return PermissionMode::Nuclear;
        case PermissionMode::Nuclear:     return PermissionMode::Default;
    }
    return PermissionMode::Default;
}

// ---------------------------------------------------------------------------
// Banner helpers
// ---------------------------------------------------------------------------

bool requires_banner(PermissionMode mode) noexcept {
    return mode == PermissionMode::Nuclear;
}

std::string_view banner_text(PermissionMode mode) noexcept {
    if (mode == PermissionMode::Nuclear) {
        return "\xe2\x98\xa2\xef\xb8\x8f NUCLEAR MODE \xe2\x80\x94 ALL PERMISSIONS BYPASSED";
        //       ^^^^^^^^^^^^^^^^^^^^  ☢️ (U+2622 U+FE0F)    ^^^^^^^^^^^  — (U+2014 em dash)
    }
    return {};
}

} // namespace batbox::permissions
