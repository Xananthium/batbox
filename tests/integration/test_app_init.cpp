// tests/integration/test_app_init.cpp
// =============================================================================
// Integration tests for CPP A.3 — App::init wiring sequence.
//
// Strategy:
//   The full TUI cannot be driven from a test process (no tty, no FTXUI).
//   We validate the init sequence via two observable surfaces:
//
//   Surface 1 — batbox --print (headless path)
//     The headless path exercises Steps 1–4 of App::init (env load, Config,
//     logging, PermissionMode) then exits before the TUI.  These tests use a
//     fake OpenAI server fixture.
//
//   Surface 2 — BundledSkillsRegistry::all() unit check
//     Directly validates that all() returns >= 13 bundled skills with correct
//     "bundled" source tags and non-empty names.
//
//   Surface 3 — SkillLoader load sequence unit check
//     Validates set_bundled_skills() + load_user_dirs() without touching disk
//     (no actual skill files needed — non-existent dirs are silently skipped).
//
// Tests:
//   1. headless --print exits 0 when env is set up (fake server round-trip)
//   2. headless --print exits 1 when BATBOX_API_BASE_URL is unreachable
//   3. BundledSkillsRegistry::all() returns >= 13 skills
//   4. All bundled skills have non-empty name and source == "bundled"
//   5. SkillLoader: set_bundled_skills sets correct count; load_user_dirs() on
//      non-existent dirs does not crash; size() remains >= bundled count.
//   6. PluginLoader::load_all() on an empty plugin root returns Ok (not Err)
//   7. Config::load_from_env with empty env succeeds (returns Ok with defaults)
//   8. Config::load_from_env with BATBOX_API_KEY set reflects key in Config
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <batbox/config/Config.hpp>
#include <batbox/config/EnvLoader.hpp>
#include <batbox/plugins/PluginLoader.hpp>
#include <batbox/plugins/PluginRegistry.hpp>
#include <batbox/plugins/SkillLoader.hpp>
#include <batbox/skills/BundledSkillsRegistry.hpp>
#include <batbox/conversation/Conversation.hpp>
#include <batbox/inference/Client.hpp>
#include <batbox/session/SessionStore.hpp>


#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// POSIX headers for subprocess management.
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: find fake_openai_server.py
// BATBOX_FIXTURE_DIR injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_fixture_script() {
#ifdef BATBOX_FIXTURE_DIR
    fs::path p = fs::path(BATBOX_FIXTURE_DIR) / "fake_openai_server.py";
    if (fs::exists(p)) return p.string();
#endif
    fs::path dir = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        fs::path candidate = dir / "tests" / "fixtures" / "fake_openai_server.py";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

// ---------------------------------------------------------------------------
// Helper: find the batbox executable.
// BATBOX_BINARY_DIR injected by CMake at compile time.
// ---------------------------------------------------------------------------
static std::string find_batbox_binary() {
#ifdef BATBOX_BINARY_DIR
    fs::path p = fs::path(BATBOX_BINARY_DIR) / "src" / "batbox";
    if (fs::exists(p)) return p.string();
    fs::path p2 = fs::path(BATBOX_BINARY_DIR) / "batbox";
    if (fs::exists(p2)) return p2.string();
#endif
    FILE* f = ::popen("which batbox 2>/dev/null", "r");
    if (f) {
        char buf[512] = {};
        if (std::fgets(buf, sizeof(buf), f)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            ::pclose(f);
            if (!s.empty() && fs::exists(s)) return s;
        }
        ::pclose(f);
    }
    return {};
}

// ---------------------------------------------------------------------------
// FakeServer RAII — forks python3, waits for "READY <port>" on stdout.
// ---------------------------------------------------------------------------
struct FakeServer {
    pid_t pid{-1};
    int   port{0};

    bool start(const std::string& script_path) {
        if (script_path.empty()) return false;

        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        pid = ::fork();
        if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }

        if (pid == 0) {
            ::close(pipefd[0]);
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::close(pipefd[1]);
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
            ::execlp("python3", "python3", script_path.c_str(), nullptr);
            ::_exit(127);
        }

        ::close(pipefd[1]);
        ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        char buf[128] = {};
        std::string accum;
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::read(pipefd[0], buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                accum += buf;
                auto pos = accum.find('\n');
                if (pos != std::string::npos) {
                    std::string line = accum.substr(0, pos);
                    if (line.rfind("READY ", 0) == 0) {
                        port = std::stoi(line.substr(6));
                        ::close(pipefd[0]);
                        return port > 0;
                    }
                    break;
                }
            } else if (n == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::close(pipefd[0]);
        stop();
        return false;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    ~FakeServer() { stop(); }
};

// ---------------------------------------------------------------------------
// RunResult — output of a child process run.
// ---------------------------------------------------------------------------
struct RunResult {
    int         exit_code{-1};
    std::string stdout_text;
    std::string stderr_text;
};

// ---------------------------------------------------------------------------
// run_batbox — spawn batbox with given args and environment, capture output.
// timeout_ms: max ms to wait before sending SIGTERM to the child.
// ---------------------------------------------------------------------------
static RunResult run_batbox(
    const std::string&              binary,
    const std::vector<std::string>& args,
    const std::vector<std::string>& env_vars,
    int                             timeout_ms = 10000)
{
    RunResult result;
    if (binary.empty()) {
        result.stderr_text = "batbox binary not found";
        return result;
    }

    int stdout_pipe[2], stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.stderr_text = "pipe() failed";
        return result;
    }

    pid_t child = ::fork();
    if (child < 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        result.stderr_text = "fork() failed";
        return result;
    }

    if (child == 0) {
        // Child: redirect stdout/stderr.
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);

        // Build argv.
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        // Build envp from current env + injected vars.
        // Use execve with current environment plus overrides.
        for (const auto& kv : env_vars) {
            ::putenv(const_cast<char*>(kv.c_str()));
        }

        ::execv(binary.c_str(), const_cast<char**>(argv.data()));
        ::_exit(127);
    }

    // Parent: close write ends, read from read ends.
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    // Read stdout + stderr with a timeout.
    ::fcntl(stdout_pipe[0], F_SETFL, ::fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, ::fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    bool stdout_done = false, stderr_done = false;
    char buf[4096];

    while ((!stdout_done || !stderr_done) &&
           std::chrono::steady_clock::now() < deadline) {
        if (!stdout_done) {
            ssize_t n = ::read(stdout_pipe[0], buf, sizeof(buf));
            if (n > 0) result.stdout_text.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) stdout_done = true;
        }
        if (!stderr_done) {
            ssize_t n = ::read(stderr_pipe[0], buf, sizeof(buf));
            if (n > 0) result.stderr_text.append(buf, static_cast<std::size_t>(n));
            else if (n == 0) stderr_done = true;
        }
        if (!stdout_done || !stderr_done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // If timed out, kill the child.
    if (std::chrono::steady_clock::now() >= deadline) {
        ::kill(child, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(child, SIGKILL);
    }

    // Drain remaining output.
    {
        ssize_t n;
        while ((n = ::read(stdout_pipe[0], buf, sizeof(buf))) > 0)
            result.stdout_text.append(buf, static_cast<std::size_t>(n));
        while ((n = ::read(stderr_pipe[0], buf, sizeof(buf))) > 0)
            result.stderr_text.append(buf, static_cast<std::size_t>(n));
    }

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    int status = 0;
    ::waitpid(child, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return result;
}

// ===========================================================================
// TEST SUITE: BundledSkillsRegistry
// ===========================================================================

TEST_SUITE("BundledSkillsRegistry") {

    TEST_CASE("all() returns at least 13 bundled skills") {
        auto skills = batbox::skills::BundledSkillsRegistry::all();
        CHECK(skills.size() >= 13);
    }

    TEST_CASE("all() skills have non-empty names") {
        auto skills = batbox::skills::BundledSkillsRegistry::all();
        for (const auto& s : skills) {
            INFO("skill with empty name detected");
            CHECK_FALSE(s.name.empty());
        }
    }

    TEST_CASE("all() skills have source == bundled") {
        auto skills = batbox::skills::BundledSkillsRegistry::all();
        for (const auto& s : skills) {
            INFO("skill: " << s.name);
            CHECK(s.source == "bundled");
        }
    }

    TEST_CASE("all() skills have non-empty prompt_body") {
        auto skills = batbox::skills::BundledSkillsRegistry::all();
        for (const auto& s : skills) {
            INFO("skill: " << s.name);
            CHECK_FALSE(s.prompt_body.empty());
        }
    }
}

// ===========================================================================
// TEST SUITE: SkillLoader init sequence
// ===========================================================================

TEST_SUITE("SkillLoader init") {

    TEST_CASE("set_bundled_skills then size() matches") {
        batbox::plugins::SkillLoader loader;
        auto bundled = batbox::skills::BundledSkillsRegistry::all();
        const std::size_t bundled_count = bundled.size();
        loader.set_bundled_skills(std::move(bundled));
        CHECK(loader.size() == bundled_count);
    }

    TEST_CASE("load_user_dirs() on non-existent dirs does not crash") {
        batbox::plugins::SkillLoader loader;
        auto bundled = batbox::skills::BundledSkillsRegistry::all();
        const std::size_t bundled_count = bundled.size();
        loader.set_bundled_skills(std::move(bundled));
        // load_user_dirs scans ~/.claude/skills, ./.claude/skills, ~/.batbox/skills,
        // ./.batbox/skills — all may be absent; that is fine.
        loader.load_user_dirs();
        // Skills count should be >= bundled count (user dirs may add more).
        CHECK(loader.size() >= bundled_count);
    }

    TEST_CASE("find() returns bundled skill after set_bundled_skills") {
        batbox::plugins::SkillLoader loader;
        auto bundled = batbox::skills::BundledSkillsRegistry::all();
        if (bundled.empty()) {
            MESSAGE("No bundled skills — skipping find() check");
            return;
        }
        const std::string first_name = bundled[0].name;
        loader.set_bundled_skills(std::move(bundled));
        const batbox::plugins::Skill* found = loader.find(first_name);
        REQUIRE(found != nullptr);
        CHECK(found->name == first_name);
        CHECK(found->source == "bundled");
    }
}

// ===========================================================================
// TEST SUITE: Config loading (Steps 2a + 2b of App::init)
// ===========================================================================

TEST_SUITE("Config init") {

    TEST_CASE("load_from_env with empty env returns Ok with defaults") {
        batbox::config::EnvMap empty_env;
        auto result = batbox::config::Config::load_from_env(empty_env);
        REQUIRE(static_cast<bool>(result));
        const auto& cfg = result.value();
        // Default model should be non-empty.
        CHECK_FALSE(cfg.api.default_model.empty());
    }

    TEST_CASE("load_from_env with BATBOX_API_KEY reflects in Config") {
        batbox::config::EnvMap env;
        env["BATBOX_API_KEY"] = "sk-test-init-key-xyz";
        auto result = batbox::config::Config::load_from_env(env);
        REQUIRE(static_cast<bool>(result));
        CHECK(result.value().api.api_key == "sk-test-init-key-xyz");
    }

    TEST_CASE("load_from_env with invalid int value returns Err") {
        batbox::config::EnvMap env;
        env["BATBOX_BASH_TIMEOUT_SEC"] = "not-a-number";
        auto result = batbox::config::Config::load_from_env(env);
        // Invalid int should yield an error.
        CHECK_FALSE(static_cast<bool>(result));
    }
}

// ===========================================================================
// TEST SUITE: PluginLoader (Step 5 of App::init)
// ===========================================================================

TEST_SUITE("PluginLoader init") {

    TEST_CASE("load_all() with empty plugin roots returns Ok") {
        // PluginLoader scans 4 standard roots; if none exist it returns Ok([]).
        batbox::plugins::PluginLoader loader;
        auto result = loader.load_all();
        // Result should be Ok (even with empty/absent roots).
        CHECK(static_cast<bool>(result));
    }

    TEST_CASE("reload(registry) with empty roots succeeds") {
        batbox::plugins::PluginLoader loader;
        batbox::plugins::PluginRegistry registry;
        auto result = loader.reload(registry);
        CHECK(static_cast<bool>(result));
    }
}

// ===========================================================================
// TEST SUITE: headless --print (exercises init Steps 1–4)
// ===========================================================================

TEST_SUITE("App::init headless") {

    TEST_CASE("--print exits 0 with fake server") {
        const std::string fixture = find_fixture_script();
        const std::string binary  = find_batbox_binary();
        if (fixture.empty() || binary.empty()) {
            MESSAGE("Skipping: fixture script or batbox binary not found");
            return;
        }

        FakeServer srv;
        if (!srv.start(fixture)) {
            MESSAGE("Skipping: fake server failed to start");
            return;
        }

        std::string base_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/v1";
        auto res = run_batbox(binary,
            {"--print", "hello"},
            {"BATBOX_API_BASE_URL=" + base_url,
             "BATBOX_API_KEY=test-key-123"});

        CHECK(res.exit_code == 0);
        CHECK_FALSE(res.stdout_text.empty());
    }

    TEST_CASE("--print exits 1 when no API key set and server unreachable") {
        const std::string binary = find_batbox_binary();
        if (binary.empty()) {
            MESSAGE("Skipping: batbox binary not found");
            return;
        }

        // Point to a port that refuses connections.
        auto res = run_batbox(binary,
            {"--print", "hello"},
            {"BATBOX_API_BASE_URL=http://127.0.0.1:1/v1",
             "BATBOX_API_KEY=test-key"},
            5000);

        // Should exit non-zero (1 for inference error).
        CHECK(res.exit_code != 0);
    }
}

// ===========================================================================
// TEST SUITE: UI-D11 (TUI-T10) — session UUID stderr logging
// ===========================================================================

TEST_SUITE("UI-D11 session UUID") {

    TEST_CASE("--print mode does not emit session UUID on stderr") {
        // Acceptance criterion 2: the 'session UUID: <uuid>' line must NOT
        // appear in --print mode output (stdout must not be polluted, and the
        // TUI-only log line must be absent from the headless path entirely).
        const std::string fixture = find_fixture_script();
        const std::string binary  = find_batbox_binary();
        if (fixture.empty() || binary.empty()) {
            MESSAGE("Skipping: fixture script or batbox binary not found");
            return;
        }

        FakeServer srv;
        if (!srv.start(fixture)) {
            MESSAGE("Skipping: fake server failed to start");
            return;
        }

        std::string base_url = "http://127.0.0.1:" + std::to_string(srv.port) + "/v1";
        auto res = run_batbox(binary,
            {"--print", "hello"},
            {"BATBOX_API_BASE_URL=" + base_url,
             "BATBOX_API_KEY=test-key-123"});

        // Must succeed.
        CHECK(res.exit_code == 0);

        // Acceptance criterion 2: no "session UUID: " on stdout.
        CHECK(res.stdout_text.find("session UUID: ") == std::string::npos);

        // Acceptance criterion 2 (extended): the session UUID line must not
        // appear on stderr in --print mode either — TUI-only path is never
        // reached by the headless path.  spdlog INFO lines in --print mode
        // are suppressed by default (no log file sink in CI); but even if
        // they were emitted, the TUI start_session() call is gated behind
        // the args.print_mode early-return so it cannot fire.
        CHECK(res.stderr_text.find("session UUID: ") == std::string::npos);
    }

    TEST_CASE("Conversation::start_session() returns Ok and populates session_id") {
        // Unit test: confirms that start_session() eagerly creates a session
        // and makes session_id() non-empty before any user_message() call.
        batbox::config::EnvMap env;
        env["BATBOX_API_KEY"]      = "sk-test-uuid-check";
        env["BATBOX_API_BASE_URL"] = "http://127.0.0.1:1/v1"; // unreachable; not called
        auto cfg_res = batbox::config::Config::load_from_env(env);
        REQUIRE(static_cast<bool>(cfg_res));
        const auto cfg = cfg_res.value();

        // Use a temp dir for sessions so we do not pollute ~/.batbox/sessions.
        fs::path tmp_dir = fs::temp_directory_path() /
            ("batbox_test_uuid_" + std::to_string(::getpid()));
        fs::create_directories(tmp_dir);

        batbox::inference::Client    client{cfg};
        batbox::session::SessionStore store{tmp_dir};

        batbox::conversation::Conversation conv{
            client,
            store,
            cfg,
            tmp_dir,
            nullptr,   // no on_delta
            nullptr,   // no tool registry
            nullptr    // no permission gate
        };

        // Before start_session(): session_id should be empty.
        CHECK(conv.session_id().empty());

        // Call start_session(): should succeed.
        auto res = conv.start_session();
        CHECK(static_cast<bool>(res));

        // After start_session(): session_id must be non-empty UUID.
        CHECK_FALSE(conv.session_id().empty());
        // UUID is 36 chars: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        CHECK(conv.session_id().size() == 36);

        // Idempotent: calling again should be no-op (same id).
        const std::string first_id = conv.session_id();
        auto res2 = conv.start_session();
        CHECK(static_cast<bool>(res2));
        CHECK(conv.session_id() == first_id);

        // Cleanup.
        fs::remove_all(tmp_dir);
    }
}
