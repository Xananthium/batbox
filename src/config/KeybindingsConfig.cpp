// src/config/KeybindingsConfig.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::load_keybindings.
//
// Reads ~/.batbox/keybindings.json (or any caller-supplied path).
// The file is a flat JSON object: { "action_name": "KeyDescriptor", ... }.
//
// Merge semantics:
//   1. Start with default_keybindings() (built-in defaults).
//   2. If the file exists: parse it, iterate entries.
//      - Known action name + string value → overwrite the default.
//      - Unknown action name             → BATBOX_LOG_WARN + skip.
//      - Non-string value                → BATBOX_LOG_WARN + skip.
// 3. Missing file → return defaults unchanged (not an error).
// 4. Unreadable / malformed / non-object → return Err.
// ---------------------------------------------------------------------------

#include <batbox/config/KeybindingsConfig.hpp>
#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace batbox::config {

// ============================================================================
// Recognised action names
// ============================================================================
namespace {

/// Set of all valid keybinding action names understood by batbox.
/// Any name not in this set is treated as unknown and triggers a warning.
static const std::unordered_map<std::string, std::string>& known_actions_defaults() {
    static const std::unordered_map<std::string, std::string> kDefaults = {
        { "send",          "Ctrl+Enter"  },
        { "cancel",        "Escape"      },
        { "cycle_mode",    "Shift+Tab"   },  // locked UX affordance
        { "newline",       "Shift+Enter" },
        { "history_up",    "Up"          },
        { "history_down",  "Down"        },
        { "clear",         "Ctrl+L"      },
        { "vim_toggle",    "Ctrl+G"      },  // UI-D8: moved off Escape (Cancel); Ctrl+G also available as /vim
    };
    return kDefaults;
}

/// Return true if 'action' is a recognised action name.
static bool is_known_action(const std::string& action) {
    return known_actions_defaults().count(action) != 0;
}

} // anonymous namespace

// ============================================================================
// default_keybindings()
// ============================================================================

KeybindingMap default_keybindings() {
    return known_actions_defaults();
}

// ============================================================================
// load_keybindings()
// ============================================================================

[[nodiscard]]
batbox::Result<KeybindingMap, std::string>
load_keybindings(std::filesystem::path path) {
    // Start with built-in defaults.
    KeybindingMap result = default_keybindings();

    // Missing file is not an error — return defaults unchanged.
    if (!std::filesystem::exists(path)) {
        return result;
    }

    // Attempt to open and read the file.
    std::ifstream file(path);
    if (!file.is_open()) {
        std::string msg = "KeybindingsConfig: cannot open '";
        msg += path.string();
        msg += "': ";
        msg += std::strerror(errno);
        return batbox::Err(std::move(msg));
    }

    std::ostringstream buf;
    buf << file.rdbuf();
    const std::string raw = buf.str();

    // Parse JSON via batbox::parse() (nlohmann path).
    auto parse_result = batbox::parse(raw);
    if (!parse_result.has_value()) {
        std::string msg = "KeybindingsConfig: '";
        msg += path.string();
        msg += "': JSON parse error: ";
        msg += parse_result.error();
        return batbox::Err(std::move(msg));
    }

    const batbox::Json& root = parse_result.value();

    // Top-level must be a JSON object.
    if (!root.is_object()) {
        std::string msg = "KeybindingsConfig: '";
        msg += path.string();
        msg += "': expected a JSON object at the top level, got ";
        msg += root.type_name();
        return batbox::Err(std::move(msg));
    }

    // Iterate entries: overlay known actions, skip unknown with a warning.
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& action = it.key();

        // Validate action name.
        if (!is_known_action(action)) {
            BATBOX_LOG_WARN(
                "KeybindingsConfig: '{}': unknown action '{}' — skipping",
                path.string(), action);
            continue;
        }

        // Value must be a string.
        if (!it.value().is_string()) {
            BATBOX_LOG_WARN(
                "KeybindingsConfig: '{}': action '{}' has a non-string value ({}) — skipping",
                path.string(), action, it.value().type_name());
            continue;
        }

        // Overlay the default with the user's binding.
        result[action] = it.value().template get<std::string>();
    }

    return result;
}

} // namespace batbox::config
