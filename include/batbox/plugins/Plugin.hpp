// include/batbox/plugins/Plugin.hpp
// =============================================================================
// batbox::plugins::Plugin — loaded plugin metadata + assets.
// batbox::plugins::Skill  — a loaded skill (prompt + optional script).
// batbox::plugins::Agent  — a loaded agent definition.
// batbox::plugins::Command — a loaded user-defined slash command.
//
// These structs are the in-memory representation produced by PluginLoader
// (CPP 11.4) and consumed by PluginRegistry (this task, CPP 11.3).
//
// Design notes (per ned-cpp.md §2.C11):
//   - Plugin::disabled mirrors the plugins.disabled[] list in settings.json.
//     PluginRegistry::active_plugins() filters disabled == true entries out.
//   - McpServerConfig is a type alias for McpServerSpec (from MarketplaceJson.hpp)
//     so that Plugin.hpp does not re-invent that struct.
//   - Skill::source is a discriminator: "user-dir" for skills loaded from
//     ~/.batbox/skills/, or "plugin:<name>" for skills bundled with a plugin.
//   - operator== is defaulted for easy test assertions.
//
// Build (standalone — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_plugin_registry.cpp \
//       src/plugins/PluginRegistry.cpp \
//       src/core/Json.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       src/plugins/MarketplaceJson.cpp src/plugins/FrontmatterParser.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_plugin_registry && /tmp/test_plugin_registry
// =============================================================================

#pragma once

#include <batbox/plugins/MarketplaceJson.hpp>
#include <batbox/plugins/SkillLoader.hpp>  // batbox::plugins::Skill (canonical definition)

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace batbox::plugins {

// ============================================================================
// McpServerConfig — alias for the parsed MCP server spec from MarketplaceJson.
// ============================================================================

/// In Plugin contexts McpServerConfig is the same struct as McpServerSpec;
/// the alias lets Plugin.hpp stay independent of the marketplace parser name.
using McpServerConfig = McpServerSpec;

// ============================================================================
// Agent — a loaded agent definition.
// ============================================================================

/// An agent definition loaded from an agents/*.md file.
///
/// Shares the same frontmatter shape as Skill; the source discriminator
/// follows the same convention ("user-dir" | "plugin:<name>").
struct Agent {
    std::string                        name;
    std::string                        description;
    std::optional<std::string>         model;
    std::vector<std::string>           allowed_tools;
    std::string                        prompt_body;
    std::optional<fs::path>            script_path;
    std::string                        source;  ///< "user-dir" | "plugin:<name>"

    bool operator==(const Agent&) const = default;
};

// ============================================================================
// Command — a loaded user-defined slash command.
// ============================================================================

/// A slash command loaded from a commands/*.md file.
///
/// The markdown body becomes the command prompt template; frontmatter may
/// carry name and description overrides.
struct Command {
    std::string name;         ///< slash-command trigger (without the leading /)
    std::string description;  ///< one-line description shown in the picker
    std::string body;         ///< full markdown body (the command prompt template)
    std::string source;       ///< "user-dir" | "plugin:<name>"

    bool operator==(const Command&) const = default;
};

// ============================================================================
// Plugin — the top-level loaded plugin struct.
// ============================================================================

/// Fully-loaded plugin: marketplace.json metadata + all resolved assets.
///
/// A Plugin is constructed by PluginLoader after:
///   1. Finding and parsing marketplace.json (name, version, description, author,
///      mcp_servers).
///   2. Enumerating and parsing skills/*.md, agents/*.md, commands/*.md relative
///      to `dir`.
///
/// disabled flag:
///   Mirrors the plugins.disabled[] entry in settings.json.  PluginRegistry
///   exposes only enabled plugins through active_plugins(); the full list
///   (including disabled) is available via all_plugins().
///
/// author:
///   Not part of the current marketplace.json schema but reserved for future
///   use; set to empty string when absent.
struct Plugin {
    std::string                    name;         ///< from marketplace.json "name" (REQUIRED)
    std::string                    description;  ///< from marketplace.json "description"
    std::string                    version;      ///< from marketplace.json "version"
    std::string                    author;       ///< reserved; empty when absent
    fs::path                       dir;          ///< absolute path to the plugin directory
    std::vector<Skill>             skills;
    std::vector<Agent>             agents;
    std::vector<Command>           commands;
    std::vector<McpServerConfig>   mcp_servers;  ///< keyed-by-name entries flattened to a vector
    bool                           disabled = false;

    bool operator==(const Plugin&) const = default;
};

} // namespace batbox::plugins
