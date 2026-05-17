// include/batbox/permissions/PermissionRule.hpp
// ---------------------------------------------------------------------------
// batbox::permissions::PermissionRule — one entry in a settings.json
// permissions.allow / permissions.deny / permissions.ask array.
//
// Wire format (settings.json):
//   "permissions": {
//     "allow": ["Bash(git *)", "Read(./src/**)"],
//     "deny":  ["Bash(rm -rf *)"],
//     "ask":   []
//   }
//
// A PermissionRule pairs the raw pattern string with the list it came from.
// Serialisation helpers (to_json / from_json) are NOT provided here because
// the settings file stores allow/deny/ask as separate arrays of strings rather
// than an array of tagged objects.  PermissionStore owns the mapping.
//
// JSON round-trip is handled by PermissionStore, which reads and writes the
// three separate arrays via batbox::config::load_settings() / write_settings().
// ---------------------------------------------------------------------------

#pragma once

#include <string>
#include <string_view>

namespace batbox::permissions {

// ===========================================================================
// PermissionRule
// ===========================================================================

/// One entry from a permissions.allow / permissions.deny / permissions.ask
/// array in settings.json.
///
/// `pattern` is the raw rule string, e.g. "Bash(npm test:*)" or "Read(./src/**)".
/// `kind`    indicates which list this rule belongs to.
struct PermissionRule {
    /// Which list this rule belongs to.
    enum class Kind {
        Allow,  ///< Auto-approved without prompt.
        Deny,   ///< Always blocked.
        Ask,    ///< Always prompt regardless of other rules.
    };

    /// Raw rule string, e.g. "Bash(npm test:*)" or "Read(./src/**)".
    std::string pattern;

    /// Which list this rule belongs to.
    Kind kind{Kind::Allow};

    // ---- Convenience constructors ------------------------------------------

    PermissionRule() = default;

    PermissionRule(std::string_view p, Kind k)
        : pattern(p), kind(k) {}

    // ---- Comparison (for deduplication + testing) --------------------------

    [[nodiscard]] bool operator==(const PermissionRule& o) const noexcept {
        return kind == o.kind && pattern == o.pattern;
    }
    [[nodiscard]] bool operator!=(const PermissionRule& o) const noexcept {
        return !(*this == o);
    }
};

// ---------------------------------------------------------------------------
// kind_to_string / kind_from_string — human-readable helpers
// ---------------------------------------------------------------------------

/// Returns "allow", "deny", or "ask".
[[nodiscard]] std::string_view kind_to_string(PermissionRule::Kind kind) noexcept;

} // namespace batbox::permissions
