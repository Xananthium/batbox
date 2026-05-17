// src/config/SettingsLoader.cpp
// ---------------------------------------------------------------------------
// Implementation of batbox::config::load_settings() and write_settings().
//
// Uses nlohmann::json for both parsing and serialisation — ergonomic for this
// small file (< 1 KB typical) where simdjson's zero-copy advantage is moot.
//
// Atomic write guarantees:
//   POSIX std::filesystem::rename() over a tmp file on the same filesystem is
//   an atomic replace.  We write to "<path>.tmp", flush, close, then rename.
//   On failure at any stage we attempt to remove the tmp file before returning
//   the error so we don't leave stale temps on disk.
// ---------------------------------------------------------------------------

#include <batbox/config/SettingsLoader.hpp>
#include <batbox/core/Json.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace batbox::config {

namespace {

// ---------------------------------------------------------------------------
// parse_string_array()
// ---------------------------------------------------------------------------
// Safely extract a JSON array of strings; returns empty vector on anything
// unexpected (absent key, not-array, non-string elements are skipped).
std::vector<std::string> parse_string_array(const batbox::Json& obj,
                                             const std::string& key)
{
    std::vector<std::string> result;
    if (!obj.is_object()) return result;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return result;
    for (const auto& elem : *it) {
        if (elem.is_string()) {
            result.push_back(elem.get<std::string>());
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// parse_string_field()
// ---------------------------------------------------------------------------
// Safely extract a single string field; returns empty string on absent/non-string.
std::string parse_string_field(const batbox::Json& obj, const std::string& key)
{
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// ---------------------------------------------------------------------------
// settings_to_json()
// ---------------------------------------------------------------------------
// Serialise a Settings struct to a nlohmann::json object.
batbox::Json settings_to_json(const Settings& s)
{
    batbox::Json j = batbox::Json::object();

    // permissions sub-object — always emit, even if arrays are empty, so the
    // file is self-documenting for the user.
    batbox::Json perms = batbox::Json::object();
    perms["allow"] = s.permissions_allow;
    perms["deny"]  = s.permissions_deny;
    perms["ask"]   = s.permissions_ask;
    j["permissions"] = std::move(perms);

    // theme — omit if empty (default will be used by the reader).
    if (!s.theme.empty()) {
        j["theme"] = s.theme;
    }

    // plugins sub-object.
    batbox::Json plugins = batbox::Json::object();
    plugins["disabled"] = s.plugins_disabled;
    j["plugins"] = std::move(plugins);

    // output_style — omit if empty.
    if (!s.output_style.empty()) {
        j["output_style"] = s.output_style;
    }

    return j;
}

} // anonymous namespace

// ============================================================================
// load_settings()
// ============================================================================

batbox::Result<Settings, std::string>
load_settings(std::filesystem::path path)
{
    // Missing file is not an error — return default Settings.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Settings{};
    }

    // Open for reading.
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return batbox::Err("settings: cannot open '" + path.string() + "': " +
                           std::strerror(errno));
    }

    // Read entire file content.
    std::ostringstream buf;
    buf << ifs.rdbuf();
    const std::string content = buf.str();
    ifs.close();

    // Parse with nlohmann — gives detailed error with byte offset on failure.
    batbox::Json j;
    try {
        j = batbox::Json::parse(content);
    } catch (const nlohmann::json::parse_error& e) {
        return batbox::Err(std::string("settings: JSON parse error in '") +
                           path.string() + "': " + e.what());
    } catch (const std::exception& e) {
        return batbox::Err(std::string("settings: failed to read '") +
                           path.string() + "': " + e.what());
    }

    if (!j.is_object()) {
        return batbox::Err("settings: root of '" + path.string() +
                           "' must be a JSON object");
    }

    Settings s;

    // permissions sub-object.
    if (j.contains("permissions") && j["permissions"].is_object()) {
        const auto& perms = j["permissions"];
        s.permissions_allow = parse_string_array(perms, "allow");
        s.permissions_deny  = parse_string_array(perms, "deny");
        s.permissions_ask   = parse_string_array(perms, "ask");
    }

    // theme.
    s.theme = parse_string_field(j, "theme");

    // plugins.disabled.
    if (j.contains("plugins") && j["plugins"].is_object()) {
        s.plugins_disabled = parse_string_array(j["plugins"], "disabled");
    }

    // output_style.
    s.output_style = parse_string_field(j, "output_style");

    return s;
}

// ============================================================================
// write_settings()
// ============================================================================

batbox::Result<void, std::string>
write_settings(std::filesystem::path path, const Settings& s)
{
    // Ensure parent directory exists (create_directories is idempotent).
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return batbox::Err("settings: cannot create directory '" +
                               parent.string() + "': " + ec.message());
        }
    }

    // Derive tmp path.
    const std::filesystem::path tmp_path = std::filesystem::path(path.string() + ".tmp");

    // Step 1: write to tmp.
    {
        std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            return batbox::Err("settings: cannot open tmp file '" +
                               tmp_path.string() + "': " + std::strerror(errno));
        }

        const batbox::Json j = settings_to_json(s);
        // pretty(j) → 4-space indented; add trailing newline for unix convention.
        ofs << batbox::pretty(j) << '\n';
        ofs.flush();

        if (!ofs.good()) {
            std::error_code ec2;
            std::filesystem::remove(tmp_path, ec2); // best-effort cleanup
            return batbox::Err("settings: write error on tmp file '" +
                               tmp_path.string() + "'");
        }
    } // ofs closes here (RAII)

    // Step 2: atomic rename tmp → target.
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(tmp_path, ec2); // best-effort cleanup
        return batbox::Err("settings: rename '" + tmp_path.string() +
                           "' → '" + path.string() + "': " + ec.message());
    }

    return {};
}

} // namespace batbox::config
