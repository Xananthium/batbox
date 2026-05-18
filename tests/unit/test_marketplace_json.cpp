// tests/unit/test_marketplace_json.cpp
// ---------------------------------------------------------------------------
// doctest suite for batbox::plugins::parse_marketplace_json and
// batbox::plugins::find_marketplace_in_dir.
//
// Covers:
//   1. Minimal valid marketplace.json (name only)
//   2. Fully-populated marketplace.json (all fields)
//   3. Missing required field 'name' → error
//   4. Unknown field → warning only, not an error (forward-compatibility)
//   5. Both filename variants: .claude-plugin/marketplace.json and
//      .batbox-plugin/marketplace.json
//   6. Malformed JSON → error
//   7. mcpServers: stdio and remote (sse/ws) variants
//   8. Type errors on required fields → error
//
// Build + run (standalone, no CMake needed — from repo root):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_marketplace_json.cpp \
//       src/plugins/MarketplaceJson.cpp \
//       src/core/Json.cpp \
//       src/core/Logging.cpp \
//       src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_marketplace_json && /tmp/test_marketplace_json
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/plugins/MarketplaceJson.hpp>
#include <batbox/core/Json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace batbox;
using namespace batbox::plugins;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

/// Write `content` to a file at `path`, creating parent dirs as needed.
static void write_file(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path);
    ofs << content;
}

/// RAII temp-dir that removes itself on scope exit.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix = "batbox_test_") {
        static std::atomic<int> counter{0};
        const auto unique_id =
            static_cast<unsigned long>(::getpid()) * 1000000UL
            + static_cast<unsigned long>(
                std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
        auto base = fs::temp_directory_path() / (prefix + std::to_string(unique_id));
        path = base / std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec); // ignore errors in destructor
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ---------------------------------------------------------------------------
// TEST SUITE 1 — Minimal valid marketplace.json (name only)
// ---------------------------------------------------------------------------
TEST_SUITE("parse_marketplace_json — minimal valid") {

    TEST_CASE("name-only JSON parses successfully") {
        Json j = {{"name", "my-plugin"}};
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        CHECK(r.value().name == "my-plugin");
        CHECK(r.value().version.empty());
        CHECK(r.value().description.empty());
        CHECK(r.value().skills.empty());
        CHECK(r.value().agents.empty());
        CHECK(r.value().commands.empty());
        CHECK(r.value().mcp_servers.empty());
    }

    TEST_CASE("name + version only") {
        Json j = {{"name", "my-plugin"}, {"version", "2.0.0"}};
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        CHECK(r.value().name == "my-plugin");
        CHECK(r.value().version == "2.0.0");
    }

    TEST_CASE("name + description only") {
        Json j = {{"name", "tool"}, {"description", "A useful tool"}};
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        CHECK(r.value().description == "A useful tool");
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 2 — Fully-populated marketplace.json
// ---------------------------------------------------------------------------
TEST_SUITE("parse_marketplace_json — fully populated") {

    TEST_CASE("all fields populated") {
        Json j = {
            {"name",        "super-plugin"},
            {"version",     "1.2.3"},
            {"description", "Does everything"},
            {"skills",      Json::array({"./skills/foo.md", "./skills/bar.md"})},
            {"agents",      Json::array({"./agents/agent1.md"})},
            {"commands",    Json::array({"./commands/cmd.md", "./commands/cmd2.md"})},
            {"mcpServers",  {
                {"fs", {
                    {"command", "/usr/bin/mcp-fs"},
                    {"args",    Json::array({"/home/user"})},
                    {"env",     {{"TOKEN", "abc123"}}}
                }},
                {"remote", {
                    {"transport", "sse"},
                    {"url",       "https://example.com/mcp"},
                    {"headers",   {{"Authorization", "Bearer tok"}}}
                }}
            }}
        };
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        const Marketplace& m = r.value();

        CHECK(m.name        == "super-plugin");
        CHECK(m.version     == "1.2.3");
        CHECK(m.description == "Does everything");

        REQUIRE(m.skills.size() == 2);
        CHECK(m.skills[0] == fs::path("./skills/foo.md"));
        CHECK(m.skills[1] == fs::path("./skills/bar.md"));

        REQUIRE(m.agents.size() == 1);
        CHECK(m.agents[0] == fs::path("./agents/agent1.md"));

        REQUIRE(m.commands.size() == 2);
        CHECK(m.commands[0] == fs::path("./commands/cmd.md"));

        REQUIRE(m.mcp_servers.size() == 2);

        auto& fs_spec = m.mcp_servers.at("fs");
        CHECK(fs_spec.transport == McpTransport::Stdio);
        CHECK(fs_spec.command   == "/usr/bin/mcp-fs");
        REQUIRE(fs_spec.args.size() == 1);
        CHECK(fs_spec.args[0] == "/home/user");
        CHECK(fs_spec.env.at("TOKEN") == "abc123");

        auto& remote = m.mcp_servers.at("remote");
        CHECK(remote.transport == McpTransport::Sse);
        CHECK(remote.url       == "https://example.com/mcp");
        CHECK(remote.headers.at("Authorization") == "Bearer tok");
    }

    TEST_CASE("mcpServers with ws transport") {
        Json j = {
            {"name", "ws-plugin"},
            {"mcpServers", {
                {"ws-srv", {
                    {"transport", "ws"},
                    {"url",       "wss://ws.example.com/mcp"}
                }}
            }}
        };
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        auto& spec = r.value().mcp_servers.at("ws-srv");
        CHECK(spec.transport == McpTransport::Ws);
        CHECK(spec.url       == "wss://ws.example.com/mcp");
    }

    TEST_CASE("mcpServers with http transport") {
        Json j = {
            {"name", "http-plugin"},
            {"mcpServers", {
                {"http-srv", {
                    {"transport", "http"},
                    {"url",       "https://api.example.com/rpc"}
                }}
            }}
        };
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        CHECK(r.value().mcp_servers.at("http-srv").transport == McpTransport::Http);
    }

    TEST_CASE("mcpServers stdio without args or env") {
        Json j = {
            {"name", "bare-stdio"},
            {"mcpServers", {
                {"srv", {{"command", "mcp-server"}}}
            }}
        };
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        auto& spec = r.value().mcp_servers.at("srv");
        CHECK(spec.transport == McpTransport::Stdio);
        CHECK(spec.command   == "mcp-server");
        CHECK(spec.args.empty());
        CHECK(spec.env.empty());
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 3 — Missing required field errors
// ---------------------------------------------------------------------------
TEST_SUITE("parse_marketplace_json — missing required fields") {

    TEST_CASE("missing 'name' returns error") {
        Json j = {{"version", "1.0.0"}, {"description", "no name"}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("name") != std::string::npos);
    }

    TEST_CASE("empty 'name' returns error") {
        Json j = {{"name", ""}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("name") != std::string::npos);
    }

    TEST_CASE("name is not a string returns error") {
        Json j = {{"name", 42}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("name") != std::string::npos);
    }

    TEST_CASE("input is not an object returns error") {
        Json j = Json::array({"a", "b"});
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("mcpServers remote missing url returns error") {
        Json j = {
            {"name", "bad-remote"},
            {"mcpServers", {
                {"srv", {{"transport", "sse"}}}  // no url
            }}
        };
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("url") != std::string::npos);
    }

    TEST_CASE("mcpServers unknown transport returns error") {
        Json j = {
            {"name", "bad-transport"},
            {"mcpServers", {
                {"srv", {{"transport", "quic"}, {"url", "quic://host"}}}
            }}
        };
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("quic") != std::string::npos);
    }

    TEST_CASE("skills array with non-string element returns error") {
        Json j = {{"name", "p"}, {"skills", Json::array({"good.md", 99})}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("skills") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 4 — Unknown fields are warnings, not errors
// ---------------------------------------------------------------------------
TEST_SUITE("parse_marketplace_json — unknown fields graceful") {

    TEST_CASE("unknown top-level field is ignored, not an error") {
        Json j = {
            {"name",         "plugin-x"},
            {"unknownField", "some value"},
            {"another",      42}
        };
        auto r = parse_marketplace_json(j);
        // Must succeed — unknown fields are just logged as warnings
        REQUIRE(r.has_value());
        CHECK(r.value().name == "plugin-x");
    }

    TEST_CASE("unknown field inside mcpServers entry is ignored") {
        Json j = {
            {"name", "plugin-y"},
            {"mcpServers", {
                {"srv", {
                    {"command",     "mcp-srv"},
                    {"newField2025", "future-feature"}
                }}
            }}
        };
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        CHECK(r.value().mcp_servers.at("srv").command == "mcp-srv");
    }

    TEST_CASE("claude-code real-world fields (owner, author, plugins, metadata) ignored gracefully") {
        // These appear in actual claude-code marketplace.json files
        Json j = {
            {"name",    "claude-code-plugins"},
            {"version", "1.0.0"},
            {"owner",   {{"name", "Anthropic"}, {"email", "support@anthropic.com"}}},
            {"plugins", Json::array({
                {{"name", "agent-sdk-dev"}, {"description", "SDK dev"}, {"source", "./plugins/agent-sdk-dev"}}
            })},
            {"$schema", "https://json.schemastore.org/claude-code-marketplace.json"}
        };
        auto r = parse_marketplace_json(j);
        REQUIRE(r.has_value());
        CHECK(r.value().name    == "claude-code-plugins");
        CHECK(r.value().version == "1.0.0");
        // plugins/owner/$schema are not in our Marketplace struct — they're ignored
        CHECK(r.value().skills.empty());
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 5 — File-based parsing + find_marketplace_in_dir
// ---------------------------------------------------------------------------
TEST_SUITE("parse_marketplace_json from file + find_marketplace_in_dir") {

    TEST_CASE(".claude-plugin/marketplace.json found and parsed") {
        TempDir td;
        const std::string json_content = R"({
            "name": "claude-compat-plugin",
            "version": "0.1.0",
            "description": "Claude compat"
        })";
        write_file(td.path / ".claude-plugin" / "marketplace.json", json_content);

        auto found = find_marketplace_in_dir(td.path);
        REQUIRE(found.has_value());
        CHECK(found.value().filename() == "marketplace.json");

        auto r = parse_marketplace_json(found.value());
        REQUIRE(r.has_value());
        CHECK(r.value().name    == "claude-compat-plugin");
        CHECK(r.value().version == "0.1.0");
    }

    TEST_CASE(".batbox-plugin/marketplace.json found and parsed") {
        TempDir td;
        const std::string json_content = R"({
            "name": "batbox-native-plugin",
            "version": "1.0.0"
        })";
        write_file(td.path / ".batbox-plugin" / "marketplace.json", json_content);

        auto found = find_marketplace_in_dir(td.path);
        REQUIRE(found.has_value());

        auto r = parse_marketplace_json(found.value());
        REQUIRE(r.has_value());
        CHECK(r.value().name == "batbox-native-plugin");
    }

    TEST_CASE(".claude-plugin takes precedence over .batbox-plugin when both exist") {
        TempDir td;
        write_file(td.path / ".claude-plugin" / "marketplace.json",
                   R"({"name":"claude-first"})");
        write_file(td.path / ".batbox-plugin" / "marketplace.json",
                   R"({"name":"batbox-second"})");

        auto found = find_marketplace_in_dir(td.path);
        REQUIRE(found.has_value());

        auto r = parse_marketplace_json(found.value());
        REQUIRE(r.has_value());
        CHECK(r.value().name == "claude-first");
    }

    TEST_CASE("find_marketplace_in_dir returns nullopt when neither file present") {
        TempDir td;
        auto found = find_marketplace_in_dir(td.path);
        CHECK_FALSE(found.has_value());
    }

    TEST_CASE("parse_marketplace_json from non-existent file returns error") {
        auto r = parse_marketplace_json(fs::path("/nonexistent/marketplace.json"));
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("parse_marketplace_json from file with malformed JSON returns error") {
        TempDir td;
        write_file(td.path / ".claude-plugin" / "marketplace.json",
                   "{this is not json");

        auto found = find_marketplace_in_dir(td.path);
        REQUIRE(found.has_value());

        auto r = parse_marketplace_json(found.value());
        CHECK_FALSE(r.has_value());
        CHECK_FALSE(r.error().empty());
    }

    TEST_CASE("parse_marketplace_json from file with full fixture") {
        TempDir td;
        const std::string json_content = R"({
            "name": "full-fixture-plugin",
            "version": "3.0.0",
            "description": "Full fixture test",
            "skills":   ["./skills/one.md", "./skills/two.md"],
            "agents":   ["./agents/bot.md"],
            "commands": ["./commands/go.md"],
            "mcpServers": {
                "local": {
                    "command": "/usr/local/bin/mcp-local",
                    "args": ["--port", "9000"],
                    "env": {"MCP_KEY": "secret"}
                },
                "cloud": {
                    "transport": "sse",
                    "url": "https://mcp.example.com",
                    "headers": {"X-Token": "bearer-123"}
                }
            }
        })";
        write_file(td.path / ".batbox-plugin" / "marketplace.json", json_content);

        auto found = find_marketplace_in_dir(td.path);
        REQUIRE(found.has_value());
        auto r = parse_marketplace_json(found.value());
        REQUIRE(r.has_value());

        const auto& m = r.value();
        CHECK(m.name        == "full-fixture-plugin");
        CHECK(m.version     == "3.0.0");
        CHECK(m.description == "Full fixture test");
        REQUIRE(m.skills.size()   == 2);
        REQUIRE(m.agents.size()   == 1);
        REQUIRE(m.commands.size() == 1);
        REQUIRE(m.mcp_servers.size() == 2);

        auto& local = m.mcp_servers.at("local");
        CHECK(local.transport      == McpTransport::Stdio);
        CHECK(local.command        == "/usr/local/bin/mcp-local");
        CHECK(local.args[0]        == "--port");
        CHECK(local.env.at("MCP_KEY") == "secret");

        auto& cloud = m.mcp_servers.at("cloud");
        CHECK(cloud.transport == McpTransport::Sse);
        CHECK(cloud.url       == "https://mcp.example.com");
        CHECK(cloud.headers.at("X-Token") == "bearer-123");
    }
}

// ---------------------------------------------------------------------------
// TEST SUITE 6 — Type errors on optional fields
// ---------------------------------------------------------------------------
TEST_SUITE("parse_marketplace_json — type errors on optional fields") {

    TEST_CASE("version is not a string returns error") {
        Json j = {{"name", "p"}, {"version", 123}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("version") != std::string::npos);
    }

    TEST_CASE("description is not a string returns error") {
        Json j = {{"name", "p"}, {"description", true}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("description") != std::string::npos);
    }

    TEST_CASE("skills is not an array returns error") {
        Json j = {{"name", "p"}, {"skills", "not-an-array"}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("skills") != std::string::npos);
    }

    TEST_CASE("mcpServers is not an object returns error") {
        Json j = {{"name", "p"}, {"mcpServers", Json::array({"a", "b"})}};
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
        CHECK(r.error().find("mcpServers") != std::string::npos);
    }

    TEST_CASE("mcpServers entry is not an object returns error") {
        Json j = {
            {"name", "p"},
            {"mcpServers", {{"srv", "should-be-object"}}}
        };
        auto r = parse_marketplace_json(j);
        CHECK_FALSE(r.has_value());
    }
}
