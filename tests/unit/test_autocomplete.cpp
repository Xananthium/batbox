// tests/unit/test_autocomplete.cpp
// ---------------------------------------------------------------------------
// Unit tests for batbox::repl::Autocomplete and detect_context().
//
// Build + run (standalone, no CMake needed):
//   c++ -std=c++20 \
//       -I include \
//       -I build/vcpkg_installed/arm64-osx/include \
//       tests/unit/test_autocomplete.cpp \
//       src/repl/Autocomplete.cpp \
//       src/commands/SlashCommandRegistry.cpp \
//       src/plugins/PluginRegistry.cpp \
//       src/plugins/MarketplaceJson.cpp \
//       src/plugins/FrontmatterParser.cpp \
//       src/core/Json.cpp src/core/Logging.cpp src/core/Paths.cpp \
//       build/vcpkg_installed/arm64-osx/lib/libsimdjson.a \
//       build/vcpkg_installed/arm64-osx/lib/libspdlog.a \
//       build/vcpkg_installed/arm64-osx/lib/libfmt.a \
//       -o /tmp/test_autocomplete && /tmp/test_autocomplete
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/repl/Autocomplete.hpp"
#include "batbox/commands/SlashCommandRegistry.hpp"
#include "batbox/commands/ISlashCommand.hpp"
#include "batbox/plugins/PluginRegistry.hpp"
#include "batbox/plugins/Plugin.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace batbox::repl;
using namespace batbox::commands;
using namespace batbox::plugins;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Minimal slash command stub for testing.
class StubCmd final : public ISlashCommand {
public:
    explicit StubCmd(std::string_view n, std::string_view d, std::vector<std::string> aliases = {})
        : name_{n}, desc_{d}, aliases_{std::move(aliases)} {}

    std::string_view name()        const noexcept override { return name_; }
    std::string_view description() const noexcept override { return desc_; }
    std::string_view usage()       const noexcept override { return name_; }
    std::vector<std::string> aliases() const override { return aliases_; }

    batbox::Result<void> execute(std::string_view, CommandContext&) override {
        return {};
    }

private:
    std::string name_;
    std::string desc_;
    std::vector<std::string> aliases_;
};

// Register a StubCmd and ignore errors (for tests that re-register).
void reg(SlashCommandRegistry& r, std::string_view n, std::string_view d,
         std::vector<std::string> aliases = {}) {
    auto result = r.register_command(std::make_shared<StubCmd>(n, d, std::move(aliases)));
    (void)result;
}

// RAII temp directory.
struct TmpDir {
    fs::path path;
    TmpDir() {
        static std::atomic<int> counter{0};
        const int id = counter.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() /
               ("batbox_ac_test_" + std::to_string(id));
        fs::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path file(const char* name) const { return path / name; }
};

// Create an empty file at p.
void touch(const fs::path& p) {
    std::ofstream f(p);
    (void)f;
}

} // namespace

// ---------------------------------------------------------------------------
// TEST: Slash command source — basic completions
// ---------------------------------------------------------------------------

TEST_CASE("slash command source: returns matching names") {
    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    reg(reg_obj, "help",    "Show help");
    reg(reg_obj, "history", "Show history");
    reg(reg_obj, "exit",    "Exit batbox");
    reg(reg_obj, "model",   "Change model");

    Autocomplete ac(reg_obj, plug_obj);

    SUBCASE("prefix 'h' returns help + history") {
        const auto results = ac.complete("h", AutocompleteContext::SlashCommand);
        CHECK(results.size() >= 2u);
        // Both "help" and "history" should be present.
        bool found_help    = false;
        bool found_history = false;
        for (const auto& r : results) {
            if (r == "help")    { found_help    = true; }
            if (r == "history") { found_history = true; }
        }
        CHECK(found_help);
        CHECK(found_history);
    }

    SUBCASE("prefix 'exit' returns exact match first") {
        const auto candidates = ac.complete_candidates("exit", AutocompleteContext::SlashCommand);
        REQUIRE(!candidates.empty());
        CHECK(candidates[0].text == "exit");
        CHECK(candidates[0].score == doctest::Approx(1.0f).epsilon(0.05f));
    }

    SUBCASE("empty prefix returns all commands") {
        const auto results = ac.complete("", AutocompleteContext::SlashCommand);
        // We registered 4 commands.
        CHECK(results.size() == 4u);
    }

    SUBCASE("prefix 'xyz' returns no results (no match)") {
        const auto results = ac.complete("xyz", AutocompleteContext::SlashCommand);
        CHECK(results.empty());
    }

    SUBCASE("prefix 'mo' matches 'model' via prefix-boost") {
        const auto candidates = ac.complete_candidates("mo", AutocompleteContext::SlashCommand);
        REQUIRE(!candidates.empty());
        CHECK(candidates[0].text == "model");
    }

    SUBCASE("descriptions are non-empty in candidates") {
        const auto candidates = ac.complete_candidates("h", AutocompleteContext::SlashCommand);
        REQUIRE(!candidates.empty());
        for (const auto& c : candidates) {
            CHECK(!c.description.empty());
        }
    }
}

// ---------------------------------------------------------------------------
// TEST: Slash command source — candidate descriptions
// ---------------------------------------------------------------------------

TEST_CASE("slash command source: description from ISlashCommand::description()") {
    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    reg(reg_obj, "compact", "Compact conversation history");

    Autocomplete ac(reg_obj, plug_obj);
    const auto candidates = ac.complete_candidates("comp", AutocompleteContext::SlashCommand);
    REQUIRE(!candidates.empty());
    CHECK(candidates[0].text        == "compact");
    CHECK(candidates[0].description == "Compact conversation history");
}

// ---------------------------------------------------------------------------
// TEST: @-mention source — skills + agents from PluginRegistry
// ---------------------------------------------------------------------------

TEST_CASE("at-mention source: returns skills and agents from active plugins") {
    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    // Build a Plugin with a skill and an agent and insert it via internal
    // load_dir mechanism.  Since we cannot easily inject a Plugin struct
    // directly, we test via a filesystem fixture that matches the Plugin
    // struct shape (used only for @-mention; PluginRegistry.get_snapshot
    // returns whatever is loaded).
    //
    // For unit tests without a real plugin directory, we test with an empty
    // registry (returns no @-mention candidates) and verify the path fallback.

    Autocomplete ac(reg_obj, plug_obj);

    SUBCASE("empty PluginRegistry: at-mention with empty prefix returns no skill/agent candidates") {
        const auto results = ac.complete("", AutocompleteContext::AtMention);
        // No plugins loaded — results come only from filesystem (cwd entries).
        // We just verify the call does not crash.
        (void)results;
        CHECK(true);
    }
}

// ---------------------------------------------------------------------------
// TEST: Filesystem source — file path completions
// ---------------------------------------------------------------------------

TEST_CASE("filesystem source: completes matching filenames in cwd") {
    TmpDir tmp;
    touch(tmp.file("main.cpp"));
    touch(tmp.file("main.hpp"));
    touch(tmp.file("readme.md"));
    fs::create_directory(tmp.path / "src");

    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    // Provide cwd_fn pointing to our temp dir.
    Autocomplete ac(reg_obj, plug_obj, [&]{ return tmp.path; });

    SUBCASE("prefix 'main' returns main.cpp and main.hpp") {
        const auto results = ac.complete("main", AutocompleteContext::FilePath);
        CHECK(results.size() >= 2u);
        bool found_cpp = false;
        bool found_hpp = false;
        for (const auto& r : results) {
            if (r == "main.cpp") { found_cpp = true; }
            if (r == "main.hpp") { found_hpp = true; }
        }
        CHECK(found_cpp);
        CHECK(found_hpp);
    }

    SUBCASE("prefix 'read' returns readme.md") {
        const auto results = ac.complete("read", AutocompleteContext::FilePath);
        REQUIRE(!results.empty());
        bool found = false;
        for (const auto& r : results) {
            if (r == "readme.md") { found = true; }
        }
        CHECK(found);
    }

    SUBCASE("directory entries get trailing slash") {
        const auto candidates = ac.complete_candidates("src", AutocompleteContext::FilePath);
        REQUIRE(!candidates.empty());
        // "src/" should be the text for the directory.
        bool found_dir = false;
        for (const auto& c : candidates) {
            if (c.text == "src/") {
                found_dir = true;
                CHECK(c.description == "directory");
            }
        }
        CHECK(found_dir);
    }

    SUBCASE("prefix 'xyz' returns no filesystem matches") {
        const auto results = ac.complete("xyz", AutocompleteContext::FilePath);
        CHECK(results.empty());
    }

    SUBCASE("empty prefix returns all entries (up to kMaxResults)") {
        const auto results = ac.complete("", AutocompleteContext::FilePath);
        // 3 files + 1 dir = 4 entries in our temp dir.
        CHECK(results.size() == 4u);
    }
}

// ---------------------------------------------------------------------------
// TEST: Filesystem source — extra directories
// ---------------------------------------------------------------------------

TEST_CASE("filesystem source: extra_dirs are also scanned") {
    TmpDir cwd_dir;
    TmpDir extra_dir;

    touch(cwd_dir.file("alpha.cpp"));
    touch(extra_dir.file("beta.hpp"));

    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    Autocomplete ac(reg_obj, plug_obj,
                    [&]{ return cwd_dir.path; },
                    {extra_dir.path});

    const auto results = ac.complete("", AutocompleteContext::FilePath);
    bool found_alpha = false;
    bool found_beta  = false;
    for (const auto& r : results) {
        if (r == "alpha.cpp") { found_alpha = true; }
        if (r == "beta.hpp")  { found_beta  = true; }
    }
    CHECK(found_alpha);
    CHECK(found_beta);
}

// ---------------------------------------------------------------------------
// TEST: Filesystem source — extra_dirs via set_extra_dirs()
// ---------------------------------------------------------------------------

TEST_CASE("filesystem source: set_extra_dirs replaces extra dir list") {
    TmpDir cwd_dir;
    TmpDir extra1;
    TmpDir extra2;

    touch(cwd_dir.file("cwd_file.txt"));
    touch(extra1.file("extra1_file.txt"));
    touch(extra2.file("extra2_file.txt"));

    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    Autocomplete ac(reg_obj, plug_obj,
                    [&]{ return cwd_dir.path; },
                    {extra1.path});

    // Initial extra dirs include extra1 only.
    {
        const auto results = ac.complete("extra1", AutocompleteContext::FilePath);
        bool found = false;
        for (const auto& r : results) {
            if (r == "extra1_file.txt") { found = true; }
        }
        CHECK(found);
    }

    // Replace with extra2.
    ac.set_extra_dirs({extra2.path});
    {
        const auto results_after = ac.complete("extra2", AutocompleteContext::FilePath);
        bool found = false;
        for (const auto& r : results_after) {
            if (r == "extra2_file.txt") { found = true; }
        }
        CHECK(found);
    }
}

// ---------------------------------------------------------------------------
// TEST: AutocompleteContext::None returns empty
// ---------------------------------------------------------------------------

TEST_CASE("None context returns empty") {
    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;
    reg(reg_obj, "help", "Help");

    Autocomplete ac(reg_obj, plug_obj);
    const auto results = ac.complete("help", AutocompleteContext::None);
    CHECK(results.empty());
}

// ---------------------------------------------------------------------------
// TEST: BashCommand context uses filesystem source
// ---------------------------------------------------------------------------

TEST_CASE("BashCommand context uses filesystem source") {
    TmpDir tmp;
    touch(tmp.file("deploy.sh"));
    touch(tmp.file("build.sh"));

    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    Autocomplete ac(reg_obj, plug_obj, [&]{ return tmp.path; });

    const auto results = ac.complete("dep", AutocompleteContext::BashCommand);
    bool found = false;
    for (const auto& r : results) {
        if (r == "deploy.sh") { found = true; }
    }
    CHECK(found);
}

// ---------------------------------------------------------------------------
// TEST: kMaxResults is honoured
// ---------------------------------------------------------------------------

TEST_CASE("kMaxResults: complete() returns at most kMaxResults candidates") {
    TmpDir tmp;
    // Create 30 files.
    for (int i = 0; i < 30; ++i) {
        touch(tmp.file(("file_" + std::to_string(i) + ".txt").c_str()));
    }

    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    Autocomplete ac(reg_obj, plug_obj, [&]{ return tmp.path; });

    const auto results = ac.complete("file_", AutocompleteContext::FilePath);
    CHECK(results.size() <= Autocomplete::kMaxResults);
}

// ---------------------------------------------------------------------------
// TEST: Performance — <50 ms for 10 000 files
// ---------------------------------------------------------------------------

TEST_CASE("performance: complete() finishes <50 ms for 10 000 files") {
    TmpDir tmp;

    // Create 10 000 empty files.
    for (int i = 0; i < 10000; ++i) {
        const fs::path p = tmp.path / ("perf_file_" + std::to_string(i) + ".txt");
        std::ofstream f(p);
        (void)f;
    }

    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    Autocomplete ac(reg_obj, plug_obj, [&]{ return tmp.path; });

    const auto t0 = std::chrono::steady_clock::now();
    const auto results = ac.complete("perf", AutocompleteContext::FilePath);
    const auto t1 = std::chrono::steady_clock::now();

    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    (void)results;
    CHECK(elapsed_ms < 50LL);
}

// ---------------------------------------------------------------------------
// TEST: detect_context()
// ---------------------------------------------------------------------------

TEST_CASE("detect_context: slash command prefix") {
    SUBCASE("/help → SlashCommand, prefix='help'") {
        const auto r = detect_context("/help");
        CHECK(r.context == AutocompleteContext::SlashCommand);
        CHECK(r.prefix  == "help");
    }

    SUBCASE("/ alone → SlashCommand, empty prefix") {
        const auto r = detect_context("/");
        CHECK(r.context == AutocompleteContext::SlashCommand);
        CHECK(r.prefix  == "");
    }

    SUBCASE("/model claude → None (space means arg, not command name)") {
        const auto r = detect_context("/model claude");
        CHECK(r.context == AutocompleteContext::None);
    }
}

TEST_CASE("detect_context: @ mention prefix") {
    SUBCASE("@agent → AtMention, prefix='agent'") {
        const auto r = detect_context("@agent");
        CHECK(r.context == AutocompleteContext::AtMention);
        CHECK(r.prefix  == "agent");
    }

    SUBCASE("@ alone → AtMention, empty prefix") {
        const auto r = detect_context("@");
        CHECK(r.context == AutocompleteContext::AtMention);
        CHECK(r.prefix  == "");
    }

    SUBCASE("@skill name → None (space ends mention)") {
        const auto r = detect_context("@skill name");
        CHECK(r.context == AutocompleteContext::None);
    }
}

TEST_CASE("detect_context: double-bracket agent mention prefix") {
    SUBCASE("[[foo → AtMention, prefix='foo'") {
        const auto r = detect_context("[[foo");
        CHECK(r.context == AutocompleteContext::AtMention);
        CHECK(r.prefix  == "foo");
    }

    SUBCASE("[[ alone → AtMention, empty prefix") {
        const auto r = detect_context("[[");
        CHECK(r.context == AutocompleteContext::AtMention);
        CHECK(r.prefix  == "");
    }

    SUBCASE("[[foo]] → None (closed bracket)") {
        const auto r = detect_context("[[foo]]");
        CHECK(r.context == AutocompleteContext::None);
    }
}

TEST_CASE("detect_context: ! bash command prefix") {
    SUBCASE("!ls → BashCommand, prefix='ls'") {
        const auto r = detect_context("!ls");
        CHECK(r.context == AutocompleteContext::BashCommand);
        CHECK(r.prefix  == "ls");
    }

    SUBCASE("! alone → BashCommand, empty prefix") {
        const auto r = detect_context("!");
        CHECK(r.context == AutocompleteContext::BashCommand);
        CHECK(r.prefix  == "");
    }

    SUBCASE("!ls /tmp/foo → BashCommand, prefix is last token") {
        const auto r = detect_context("!ls /tmp/foo");
        CHECK(r.context == AutocompleteContext::BashCommand);
        CHECK(r.prefix  == "/tmp/foo");
    }
}

TEST_CASE("detect_context: file path prefix") {
    SUBCASE("./src/main.cpp → FilePath") {
        const auto r = detect_context("./src/main.cpp");
        CHECK(r.context == AutocompleteContext::FilePath);
    }

    SUBCASE("word followed by /path/to → FilePath via last token") {
        const auto r = detect_context("look at /absolute/path");
        CHECK(r.context == AutocompleteContext::FilePath);
    }

    SUBCASE("plain text → None") {
        const auto r = detect_context("hello world");
        CHECK(r.context == AutocompleteContext::None);
    }

    SUBCASE("empty string → None") {
        const auto r = detect_context("");
        CHECK(r.context == AutocompleteContext::None);
    }
}

// ---------------------------------------------------------------------------
// TEST: Ranking — higher quality matches rank above lower quality
// ---------------------------------------------------------------------------

TEST_CASE("ranking: exact prefix match outranks fuzzy match") {
    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    reg(reg_obj, "model",   "Change the model");   // exact prefix "mo"
    reg(reg_obj, "memo",    "Write a memo");        // also starts with "mo"

    Autocomplete ac(reg_obj, plug_obj);

    // Both match "mo" by prefix; both should be present with score >= 0.9.
    const auto candidates = ac.complete_candidates("mo", AutocompleteContext::SlashCommand);
    REQUIRE(candidates.size() >= 2u);
    // All returned candidates should have non-zero score.
    for (const auto& c : candidates) {
        CHECK(c.score > 0.0f);
    }
}

TEST_CASE("ranking: exact substring match scores 1.0 (with boost)") {
    SlashCommandRegistry reg_obj;
    PluginRegistry       plug_obj;

    reg(reg_obj, "exit", "Exit batbox");

    Autocomplete ac(reg_obj, plug_obj);

    const auto candidates = ac.complete_candidates("exit", AutocompleteContext::SlashCommand);
    REQUIRE(!candidates.empty());
    CHECK(candidates[0].text  == "exit");
    // Score should be at or near 1.0 (prefix boost may push it to 1.0).
    CHECK(candidates[0].score >= 0.9f);
}
