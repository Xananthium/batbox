// src/permissions/PermissionRule.cpp
// ---------------------------------------------------------------------------
// Implementation of PermissionRule helpers.
// See include/batbox/permissions/PermissionRule.hpp for the contract.
// ---------------------------------------------------------------------------

#include <batbox/permissions/PermissionRule.hpp>

namespace batbox::permissions {

std::string_view kind_to_string(PermissionRule::Kind kind) noexcept {
    switch (kind) {
        case PermissionRule::Kind::Allow: return "allow";
        case PermissionRule::Kind::Deny:  return "deny";
        case PermissionRule::Kind::Ask:   return "ask";
    }
    return "allow";
}

} // namespace batbox::permissions
