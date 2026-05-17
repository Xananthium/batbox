// include/batbox/tools/ConfigTool.hpp
// ---------------------------------------------------------------------------
// batbox::tools::ConfigTool — ITool implementation for reading and writing
// BATBOX_* runtime configuration.
//
// Tool name: "Config"
// Slash alias: /config
//
// Actions:
//   get    — return the current value of one config key (redacted for display)
//   set    — validate key, write new value via write_settings, fire ConfigReloadBus
//   list   — return all config fields with current values (secrets redacted)
//   reload — re-run the full config load chain via reload_config()
//
// Schema:
//   action  (string, required) — "get" | "set" | "list" | "reload"
//   key     (string)           — required for get/set; the BATBOX_* key name
//                                (e.g. "BATBOX_DEFAULT_MODEL") or dot-path
//                                short form (e.g. "api.default_model")
//   value   (string)           — required for set; the new value as a string
//
// Security contract:
//   - BATBOX_API_KEY is a read-only field; set attempts return an error.
//   - list and get always return redacted_for_display() output so that
//     api_key is never exposed in the tool response body.
//   - Unknown keys return a ToolResult::error with a descriptive message.
//
// Permission gate:
//   is_read_only()          — false (set and reload mutate state)
//   requires_confirmation() — true  (set/reload require confirmation)
//
// Blueprint contract: batbox::tools::ConfigTool (task CPP 5.23)
// ---------------------------------------------------------------------------
#pragma once

#include <batbox/config/Config.hpp>
#include <batbox/tools/ITool.hpp>

#include <filesystem>
#include <mutex>

namespace batbox::tools {

// =============================================================================
// ConfigTool
// =============================================================================

/// Implements the "Config" tool: get/set/list/reload BATBOX_* config fields.
///
/// The tool holds a reference to the live Config and its config_dir so it can
/// write settings and trigger reload.  The caller is responsible for ensuring
/// the Config reference remains valid for the tool's lifetime.
class ConfigTool final : public ITool {
public:
    /// Construct a ConfigTool bound to a live Config instance.
    ///
    /// @param cfg         Reference to the process-wide Config (shared, guarded
    ///                    externally).
    /// @param cfg_mutex   Mutex that protects 'cfg' for write access.
    /// @param config_dir  Directory containing .env and settings.json; passed to
    ///                    reload_config() and write_settings().  Defaults to
    ///                    cfg.general.config_dir when empty.
    explicit ConfigTool(config::Config&        cfg,
                        std::mutex&            cfg_mutex,
                        std::filesystem::path  config_dir = {});

    // -------------------------------------------------------------------------
    // ITool identity
    // -------------------------------------------------------------------------

    /// Returns "Config".
    [[nodiscard]] std::string_view name()        const override;
    /// Returns a one-sentence description for the model schema.
    [[nodiscard]] std::string_view description() const override;
    /// Returns the full OpenAI tools[*].function JSON object.
    [[nodiscard]] Json             schema_json() const override;

    // -------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------

    /// Dispatch to get/set/list/reload based on args["action"].
    ///
    /// Always uses redacted_for_display() for any output that includes
    /// config values, so api_key is never exposed.
    [[nodiscard]] ToolResult run(const Json& args, ToolContext& ctx) override;

    // -------------------------------------------------------------------------
    // Permission gate hooks
    // -------------------------------------------------------------------------

    /// false — set/reload have side effects; not allowed unconditionally in
    ///         Plan mode.  A get or list call returns quickly without mutation.
    [[nodiscard]] bool is_read_only()          const override { return false; }
    /// true — set and reload require user confirmation.
    [[nodiscard]] bool requires_confirmation() const override { return true;  }

private:
    // -------------------------------------------------------------------------
    // Private dispatch helpers — each returns a ToolResult.
    // -------------------------------------------------------------------------

    [[nodiscard]] ToolResult handle_get(const Json& args) const;
    [[nodiscard]] ToolResult handle_set(const Json& args);
    [[nodiscard]] ToolResult handle_list() const;
    [[nodiscard]] ToolResult handle_reload();

    // -------------------------------------------------------------------------
    // Key resolution
    // -------------------------------------------------------------------------

    /// Resolve a user-supplied key string to the canonical BATBOX_* env-var name.
    /// Accepts both "BATBOX_DEFAULT_MODEL" and "api.default_model" forms.
    /// Returns empty string when the key is unrecognised.
    [[nodiscard]] static std::string resolve_key(std::string_view input);

    /// Returns true when the key identifies a read-only field (e.g. api_key).
    [[nodiscard]] static bool is_read_only_key(std::string_view canonical_key);

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------
    config::Config&       cfg_;
    std::mutex&           cfg_mutex_;
    std::filesystem::path config_dir_;
};

} // namespace batbox::tools
