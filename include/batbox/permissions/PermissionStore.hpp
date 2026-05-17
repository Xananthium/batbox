// include/batbox/permissions/PermissionStore.hpp
// ---------------------------------------------------------------------------
// batbox::permissions::PermissionStore — persistent allow/deny/ask rule list.
//
// PermissionStore wraps batbox::config::load_settings() / write_settings() to
// provide a typed CRUD interface for the three permission arrays in
// ~/.batbox/settings.json:
//
//   "permissions": {
//     "allow": [...],
//     "deny":  [...],
//     "ask":   [...]
//   }
//
// All mutation operations (add_*_rule, remove_rule) perform a read-modify-write
// cycle that ends with an atomic file rename — the same guarantee provided by
// write_settings().
//
// Deduplication: adding a pattern that already exists in the target list is a
// silent no-op (returns Ok without touching the file).
//
// Construction:
//   PermissionStore is constructed with the path to the settings file.
//   It immediately loads the current rules from disk.  If the file is absent
//   the store starts empty (not an error).  If the file exists but is
//   malformed the store starts empty and records the diagnostic in
//   last_load_error().
//
//   Use the static PermissionStore::default_path() to get
//   batbox::paths::config_dir() / "settings.json".
// ---------------------------------------------------------------------------

#pragma once

#include <batbox/core/Result.hpp>
#include <batbox/permissions/PermissionRule.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace batbox::permissions {

// ===========================================================================
// PermissionStore
// ===========================================================================

class PermissionStore {
public:
    // ---- Construction -------------------------------------------------------

    /// Construct and immediately load rules from `settings_path`.
    ///
    /// - Missing file    → ok, store starts empty.
    /// - Malformed JSON  → store starts empty; last_load_error() contains the
    ///                     diagnostic.
    /// - IO error        → same as malformed: store starts empty.
    explicit PermissionStore(std::filesystem::path settings_path);

    /// Returns batbox::paths::config_dir() / "settings.json".
    [[nodiscard]] static std::filesystem::path default_path();

    // ---- Read accessors -----------------------------------------------------

    /// Return the raw patterns in permissions.allow (as stored in settings.json).
    [[nodiscard]] const std::vector<std::string>& allow_rules() const noexcept;

    /// Return the raw patterns in permissions.deny.
    [[nodiscard]] const std::vector<std::string>& deny_rules() const noexcept;

    /// Return the raw patterns in permissions.ask.
    [[nodiscard]] const std::vector<std::string>& ask_rules() const noexcept;

    /// Return all rules merged into a typed vector (allow → deny → ask order).
    [[nodiscard]] std::vector<PermissionRule> rules() const;

    /// If the most recent load_settings() call returned an error, the message
    /// is stored here.  Empty string when the last load succeeded.
    [[nodiscard]] const std::string& last_load_error() const noexcept;

    // ---- Mutations ----------------------------------------------------------

    /// Add `pattern` to permissions.allow.
    ///
    /// - If `pattern` is already in the allow list: no-op, returns Ok.
    /// - Persists to disk atomically via write_settings().
    ///
    /// Blueprint contract name: add_allow_rule
    [[nodiscard]] batbox::Result<void> add_allow_rule(std::string_view pattern);

    /// Add `pattern` to permissions.deny.
    ///
    /// Blueprint contract name: add_deny_rule
    [[nodiscard]] batbox::Result<void> add_deny_rule(std::string_view pattern);

    /// Add `pattern` to permissions.ask.
    [[nodiscard]] batbox::Result<void> add_ask_rule(std::string_view pattern);

    /// Remove `pattern` from ALL three lists (allow, deny, ask).
    ///
    /// If the pattern does not appear in any list: no-op, returns Ok (no write).
    /// Otherwise removes all occurrences and persists atomically.
    [[nodiscard]] batbox::Result<void> remove_rule(std::string_view pattern);

    // ---- Settings path accessor --------------------------------------------

    [[nodiscard]] const std::filesystem::path& settings_path() const noexcept;

private:
    std::filesystem::path settings_path_;

    // In-memory mirror of the three arrays.
    std::vector<std::string> allow_;
    std::vector<std::string> deny_;
    std::vector<std::string> ask_;

    // Diagnostic from the last load_settings() call (empty = success).
    std::string last_load_error_;

    // ---- Internal helpers ---------------------------------------------------

    /// Read-modify-write: reloads from disk, applies `mutate`, writes back.
    /// This serialises concurrent callers through the filesystem rename.
    [[nodiscard]] batbox::Result<void>
    mutate_and_persist(
        std::function<bool(std::vector<std::string>&,   // allow
                           std::vector<std::string>&,   // deny
                           std::vector<std::string>&)>  // ask
        mutate_fn);
};

} // namespace batbox::permissions
