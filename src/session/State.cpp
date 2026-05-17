// src/session/State.cpp
// ---------------------------------------------------------------------------
// batbox::config StateStore — lightweight ~/.batbox/state.json persistence.
// (TUI-FLOW-T10)
//
// Implements:
//   read_last_seen_changelog_version()  — reads from state.json, returns nullopt
//                                         on any error or missing key.
//   write_last_seen_changelog_version() — writes to state.json, preserving other
//                                         keys.  Creates the file if absent.
// ---------------------------------------------------------------------------

#include "batbox/config/StateStore.hpp"
#include "batbox/core/Json.hpp"
#include "batbox/core/Paths.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace batbox::config {

namespace {

/// Return the path to ~/.batbox/state.json.
std::filesystem::path state_json_path() {
    return batbox::paths::config_dir() / "state.json";
}

/// Try to load the state JSON object from disk.
/// Returns an empty JSON object on any error.
batbox::Json load_state() {
    const auto path = state_json_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return batbox::Json::object();
    }
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return batbox::Json::object();
    }
    try {
        batbox::Json j;
        f >> j;
        if (j.is_object()) return j;
    } catch (...) {
        // Parse error or unexpected type — return empty object.
    }
    return batbox::Json::object();
}

/// Write the JSON object back to ~/.batbox/state.json.
/// Silently ignores all errors.
void save_state(const batbox::Json& state) {
    const auto path = state_json_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Ignore ec — if mkdir fails we just won't write, which is acceptable.
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return;
    try {
        f << state.dump(2) << '\n';
    } catch (...) {
        // Serialisation error — silently ignore.
    }
}

} // namespace

// ---------------------------------------------------------------------------
// read_last_seen_changelog_version
// ---------------------------------------------------------------------------

std::optional<std::string> read_last_seen_changelog_version() {
    const auto state = load_state();
    const auto key   = "last_seen_changelog_version";
    if (!state.contains(key)) return std::nullopt;
    const auto& val = state.at(key);
    if (!val.is_string()) return std::nullopt;
    return val.get<std::string>();
}

// ---------------------------------------------------------------------------
// write_last_seen_changelog_version
// ---------------------------------------------------------------------------

void write_last_seen_changelog_version(std::string version) {
    auto state = load_state();
    state["last_seen_changelog_version"] = std::move(version);
    save_state(state);
}

} // namespace batbox::config
