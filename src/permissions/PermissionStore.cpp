// src/permissions/PermissionStore.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::permissions::PermissionStore.
// See include/batbox/permissions/PermissionStore.hpp for the full contract.
//
// Strategy:
//   The in-memory vectors (allow_, deny_, ask_) are always a complete mirror
//   of the settings file at the time of the last successful load.
//
//   Every mutation follows a read-modify-write cycle:
//     1. Reload current settings from disk (picks up concurrent writers).
//     2. Apply the mutation in memory.
//     3. Write back atomically via config::write_settings() (tmp + rename).
//     4. Update the in-memory mirror on success.
//
//   This guarantees that a slow concurrent writer never silently loses its
//   changes: whichever process renames last wins, and each writer re-reads
//   the current file before writing.
// ---------------------------------------------------------------------------

#include <batbox/permissions/PermissionStore.hpp>
#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Paths.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace batbox::permissions {

// ===========================================================================
// Construction
// ===========================================================================

PermissionStore::PermissionStore(std::filesystem::path settings_path)
    : settings_path_(std::move(settings_path))
{
    auto res = batbox::config::load_settings(settings_path_);
    if (res) {
        allow_ = std::move(res->permissions_allow);
        deny_  = std::move(res->permissions_deny);
        ask_   = std::move(res->permissions_ask);
        // last_load_error_ stays empty → success
    } else {
        // File missing is not an error (load_settings returns Ok with defaults).
        // Only genuine IO errors or malformed JSON land here.
        last_load_error_ = res.error();
        // allow_/deny_/ask_ stay empty — safe default.
    }
}

// static
std::filesystem::path PermissionStore::default_path() {
    return batbox::paths::config_dir() / "settings.json";
}

// ===========================================================================
// Read accessors
// ===========================================================================

const std::vector<std::string>& PermissionStore::allow_rules() const noexcept {
    return allow_;
}

const std::vector<std::string>& PermissionStore::deny_rules() const noexcept {
    return deny_;
}

const std::vector<std::string>& PermissionStore::ask_rules() const noexcept {
    return ask_;
}

std::vector<PermissionRule> PermissionStore::rules() const {
    std::vector<PermissionRule> result;
    result.reserve(allow_.size() + deny_.size() + ask_.size());

    for (const auto& p : allow_) {
        result.emplace_back(p, PermissionRule::Kind::Allow);
    }
    for (const auto& p : deny_) {
        result.emplace_back(p, PermissionRule::Kind::Deny);
    }
    for (const auto& p : ask_) {
        result.emplace_back(p, PermissionRule::Kind::Ask);
    }

    return result;
}

const std::string& PermissionStore::last_load_error() const noexcept {
    return last_load_error_;
}

const std::filesystem::path& PermissionStore::settings_path() const noexcept {
    return settings_path_;
}

// ===========================================================================
// Internal: mutate_and_persist
// ===========================================================================

batbox::Result<void>
PermissionStore::mutate_and_persist(
    std::function<bool(std::vector<std::string>&,
                       std::vector<std::string>&,
                       std::vector<std::string>&)> mutate_fn)
{
    // Step 1: Reload current state from disk (handles concurrent writers).
    auto load_res = batbox::config::load_settings(settings_path_);
    batbox::config::Settings s;
    if (load_res) {
        s = std::move(*load_res);
    } else {
        // If we can't read the file, start from our in-memory snapshot rather
        // than losing all existing rules.  The write will replace whatever is
        // on disk — acceptable for the atomic-write guarantee.
        s.permissions_allow = allow_;
        s.permissions_deny  = deny_;
        s.permissions_ask   = ask_;
    }

    // Step 2: Apply the mutation.  If it returns false the mutation was a no-op
    //         (e.g. duplicate add), so we skip the write.
    const bool changed = mutate_fn(s.permissions_allow,
                                   s.permissions_deny,
                                   s.permissions_ask);
    if (!changed) {
        return {};  // No-op → ok, no disk write needed.
    }

    // Step 3: Atomically write back.
    auto write_res = batbox::config::write_settings(settings_path_, s);
    if (!write_res) {
        return batbox::Err(write_res.error());
    }

    // Step 4: Update in-memory mirror to match what we wrote.
    allow_ = std::move(s.permissions_allow);
    deny_  = std::move(s.permissions_deny);
    ask_   = std::move(s.permissions_ask);

    return {};
}

// ===========================================================================
// Mutations
// ===========================================================================

batbox::Result<void> PermissionStore::add_allow_rule(std::string_view pattern) {
    const std::string pat(pattern);
    return mutate_and_persist(
        [&pat](std::vector<std::string>& allow,
               std::vector<std::string>& /*deny*/,
               std::vector<std::string>& /*ask*/) -> bool {
            // Dedup: if already present, this is a no-op.
            if (std::find(allow.begin(), allow.end(), pat) != allow.end()) {
                return false;  // no change
            }
            allow.push_back(pat);
            return true;
        });
}

batbox::Result<void> PermissionStore::add_deny_rule(std::string_view pattern) {
    const std::string pat(pattern);
    return mutate_and_persist(
        [&pat](std::vector<std::string>& /*allow*/,
               std::vector<std::string>& deny,
               std::vector<std::string>& /*ask*/) -> bool {
            if (std::find(deny.begin(), deny.end(), pat) != deny.end()) {
                return false;
            }
            deny.push_back(pat);
            return true;
        });
}

batbox::Result<void> PermissionStore::add_ask_rule(std::string_view pattern) {
    const std::string pat(pattern);
    return mutate_and_persist(
        [&pat](std::vector<std::string>& /*allow*/,
               std::vector<std::string>& /*deny*/,
               std::vector<std::string>& ask) -> bool {
            if (std::find(ask.begin(), ask.end(), pat) != ask.end()) {
                return false;
            }
            ask.push_back(pat);
            return true;
        });
}

batbox::Result<void> PermissionStore::remove_rule(std::string_view pattern) {
    const std::string pat(pattern);
    return mutate_and_persist(
        [&pat](std::vector<std::string>& allow,
               std::vector<std::string>& deny,
               std::vector<std::string>& ask) -> bool {
            bool changed = false;

            // Remove from allow
            {
                const auto it = std::remove(allow.begin(), allow.end(), pat);
                if (it != allow.end()) {
                    allow.erase(it, allow.end());
                    changed = true;
                }
            }
            // Remove from deny
            {
                const auto it = std::remove(deny.begin(), deny.end(), pat);
                if (it != deny.end()) {
                    deny.erase(it, deny.end());
                    changed = true;
                }
            }
            // Remove from ask
            {
                const auto it = std::remove(ask.begin(), ask.end(), pat);
                if (it != ask.end()) {
                    ask.erase(it, ask.end());
                    changed = true;
                }
            }

            return changed;
        });
}

} // namespace batbox::permissions
