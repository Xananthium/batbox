// ---------------------------------------------------------------------------
// src/sidecar/SidecarManager.cpp
//
// SidecarManager implementation.
//
// Design notes:
//
//   Port selection:
//     bind(0) on 127.0.0.1 lets the kernel pick a free ephemeral port.
//     We immediately getsockname() to read the assigned port, then close()
//     the socket before passing the port to posix_spawn.  There is a
//     brief TOCTOU window between close and bind-in-child, but this is
//     acceptable — the alternative (keeping the socket open and having the
//     child inherit it) would require SO_REUSEPORT and is more complex.
//
//   posix_spawn vs fork+exec:
//     posix_spawn is preferred because:
//       - It is async-signal-safe (no fork-after-threads deadlock risk).
//       - posix_spawn_file_actions_t handles fd setup declaratively.
//       - POSIX_SPAWN_SETPGROUP sets the child process group atomically.
//     We use posix_spawnp() so that a bare "python3" argv[0] resolves via
//     the child's PATH (which already has the venv prefix prepended).
//
//   Stderr reader:
//     A background std::thread reads the child stderr pipe and forwards
//     each line to the [sidecar] spdlog logger at INFO level.  The thread
//     exits when it reads EOF (child closed its end of the pipe).
//
//   Healthcheck loop:
//     Polls every 100 ms using ScraplingClient::healthz() (500 ms timeout
//     each).  Uses a monotonic clock for startup_timeout_sec enforcement.
//
//   Shutdown sequence:
//     1. Best-effort POST /shutdown (1 s timeout via ScraplingClient)
//     2. wait up to 200 ms for the process to self-exit
//     3. killpg(pgid, SIGTERM) + wait up to 2 s
//     4. killpg(pgid, SIGKILL)
//     5. waitpid() — reap zombie
//     6. Join stderr reader thread
// ---------------------------------------------------------------------------

#include "batbox/sidecar/SidecarManager.hpp"

#include <batbox/core/Json.hpp>
#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>
#include <batbox/core/Result.hpp>
#include <batbox/sidecar/ScraplingClient.hpp>

#include <cpr/cpr.h>
#include <spdlog/spdlog.h>

// POSIX headers
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// extern environ: the parent process environment as a null-terminated array.
extern char** environ;

namespace batbox::sidecar {

namespace {

// Module logger for [sidecar] prefix — acquired once per translation unit.
inline std::shared_ptr<spdlog::logger> sidecar_log() {
    static auto lg = batbox::log::get("sidecar");
    return lg;
}

// Convenience: expand a tilde-prefixed path to its absolute form.
// Falls back to the original string if expansion fails.
std::string expand_path(const std::string& p) {
    try {
        return batbox::paths::expand_tilde(p).string();
    } catch (...) {
        return p;
    }
}

// Wait (polling) up to `max_wait` for the child pid to exit.
// Returns true if the process is gone within the deadline.
bool wait_for_exit(int pid, std::chrono::milliseconds max_wait) noexcept {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + max_wait;
    while (clock::now() < deadline) {
        int status = 0;
        pid_t r = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (r > 0 || r == -1) {
            return true;  // exited or already reaped
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// sidecar_post_json_raw — free function POST helper for request<Req,Resp>
//
// Used by the template in SidecarManager.hpp so it doesn't depend on the
// private ScraplingClient::post_json method.  Mirrors the same cancellation
// and error-classification logic.
// ---------------------------------------------------------------------------

Result<std::string> sidecar_post_json_raw(
    uint16_t            port,
    std::string_view    endpoint,
    const batbox::Json& body,
    CancelToken         ct)
{
    if (ct.is_cancelled()) {
        return batbox::Err(std::string("cancelled"));
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port) +
                            std::string(endpoint);
    const std::string body_str = batbox::dump(body);

    auto cancel_flag = std::make_shared<std::atomic_bool>(false);
    auto cancel_handle = ct.on_cancel([cancel_flag]() noexcept {
        cancel_flag->store(true, std::memory_order_relaxed);
    });

    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetHeader(cpr::Header{
        {"Content-Type", "application/json"},
        {"Accept",       "application/json"},
        {"Expect",       ""},
    });
    session.SetBody(cpr::Body{body_str});
    session.SetTimeout(cpr::Timeout{30000});
    session.SetCancellationParam(cancel_flag);

    cpr::Response resp = session.Post();

    if (resp.error.code != cpr::ErrorCode::OK) {
        if (resp.error.code == cpr::ErrorCode::ABORTED_BY_CALLBACK ||
            cancel_flag->load(std::memory_order_relaxed)) {
            return batbox::Err(std::string("cancelled"));
        }
        return batbox::Err(resp.error.message);
    }

    if (resp.status_code < 200 || resp.status_code >= 300) {
        return batbox::Err("sidecar error " + std::to_string(resp.status_code) +
                           ": " + resp.text);
    }

    return resp.text;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

SidecarManager::SidecarManager(const batbox::config::SidecarConfig& cfg)
    : python_bin_(cfg.python.string())
    , venv_dir_(expand_path(cfg.venv.string()))
    , startup_timeout_sec_(cfg.startup_timeout_sec)
    , state_(SidecarState::Cold)
{
    sidecar_log()->debug("SidecarManager created: python={} venv={} timeout={}s",
                         python_bin_, venv_dir_, startup_timeout_sec_);
}

SidecarManager::~SidecarManager() {
    // Wait for any in-flight prewarm to complete before tearing down the
    // process — otherwise the background thread may access freed state.
    if (prewarm_future_.valid()) {
        sidecar_log()->debug("SidecarManager::~SidecarManager: waiting for prewarm future");
        (void)prewarm_future_.get();
    }
    // Best-effort cleanup on destruction.  If the caller forgot to call
    // shutdown() we still kill and reap the child to avoid zombies.
    //
    // Guard condition: child_pid_ > 0 catches the normal case; the second arm
    // catches the race where try_reap_child() already set child_pid_ to -1
    // (child died naturally and was reaped non-blockingly) but the stderr
    // reader thread has not yet exited and been joined.  Without this check,
    // the std::thread destructor would fire std::terminate() (SIGABRT) because
    // the thread is still joinable.
    if (child_pid_.load(std::memory_order_relaxed) > 0 ||
        stderr_reader_thread_.joinable()) {
        sidecar_log()->debug("SidecarManager::~SidecarManager: shutdown on destruction");
        shutdown();
    }
}

// ---------------------------------------------------------------------------
// pick_free_port — bind(0) to get a kernel-assigned ephemeral port
// ---------------------------------------------------------------------------

uint16_t SidecarManager::pick_free_port() noexcept {
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return 0;
    }

    // Allow re-use so the port can be bound again quickly by the child.
    const int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = 0; // kernel picks
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(sock, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(sock);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        ::close(sock);
        return 0;
    }

    const uint16_t port = ntohs(addr.sin_port);
    ::close(sock);
    return port;
}

// ---------------------------------------------------------------------------
// build_envp — overlay VIRTUAL_ENV / PATH / PYTHONUNBUFFERED / SCRAPLING_PORT
// ---------------------------------------------------------------------------

std::vector<std::string>
SidecarManager::build_envp(const std::string& venv_dir, uint16_t port) {
    const std::string venv_bin = venv_dir + "/bin";

    // Start with a copy of the parent environment, filtering out keys we
    // will override.
    static const std::array<std::string, 3> kOverrideKeys{
        "VIRTUAL_ENV", "PYTHONHOME", "SCRAPLING_PORT"
    };

    std::vector<std::string> env_strings;
    env_strings.reserve(64);

    // Grab the original PATH so we can prepend the venv bin.
    std::string orig_path;
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        const auto eq_pos = entry.find('=');
        if (eq_pos == std::string::npos) {
            env_strings.push_back(entry);
            continue;
        }
        const std::string key = entry.substr(0, eq_pos);

        // Capture original PATH before filtering.
        if (key == "PATH") {
            orig_path = entry.substr(eq_pos + 1);
        }

        // Filter keys we will override.
        bool skip = false;
        for (const auto& k : kOverrideKeys) {
            if (key == k) { skip = true; break; }
        }
        if (!skip) {
            env_strings.push_back(std::move(entry));
        }
    }

    // Prepend venv/bin to PATH.
    const std::string new_path = venv_bin + ":" + orig_path;
    // Remove any existing PATH entry we already copied (it wasn't filtered
    // since PATH is not in kOverrideKeys) and replace.
    env_strings.erase(
        std::remove_if(env_strings.begin(), env_strings.end(),
            [](const std::string& s) {
                return s.size() >= 5 && s.substr(0, 5) == "PATH=";
            }),
        env_strings.end());
    env_strings.push_back("PATH=" + new_path);

    // Add the override variables.
    env_strings.push_back("VIRTUAL_ENV=" + venv_dir);
    env_strings.push_back("PYTHONUNBUFFERED=1");
    env_strings.push_back("SCRAPLING_PORT=" + std::to_string(port));
    // Explicitly unset PYTHONHOME so the venv's python isn't confused.
    // (We already filtered it above; no need to add an empty assignment.)

    return env_strings;
}

// ---------------------------------------------------------------------------
// start_stderr_reader — background thread: pipe fd → spdlog
// ---------------------------------------------------------------------------

void SidecarManager::start_stderr_reader(int fd) {
    stderr_reader_thread_ = std::thread([fd]() {
        auto lg = batbox::log::get("sidecar");
        constexpr std::size_t kBufSize = 4096;
        std::array<char, kBufSize> buf{};
        std::string line_buf;

        while (true) {
            const ssize_t n = ::read(fd, buf.data(), buf.size() - 1);
            if (n <= 0) {
                // EOF or error — flush any remaining line.
                if (!line_buf.empty()) {
                    lg->info("{}", line_buf);
                }
                break;
            }
            buf[static_cast<std::size_t>(n)] = '\0';
            line_buf += std::string_view(buf.data(), static_cast<std::size_t>(n));

            // Emit complete lines.
            std::size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                lg->info("{}", line_buf.substr(0, pos));
                line_buf.erase(0, pos + 1);
            }
        }
        ::close(fd);
    });
}

// ---------------------------------------------------------------------------
// try_reap_child — non-blocking waitpid
// ---------------------------------------------------------------------------

bool SidecarManager::try_reap_child() noexcept {
    const int pid = child_pid_.load(std::memory_order_relaxed);
    if (pid <= 0) return true;

    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (r > 0) {
        child_pid_.store(-1, std::memory_order_relaxed);
        sidecar_log()->debug("sidecar child pid={} reaped (status={})", pid, status);
        return true;
    }
    if (r == -1 && errno == ECHILD) {
        child_pid_.store(-1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// kill_and_reap
// ---------------------------------------------------------------------------

void SidecarManager::kill_and_reap(std::chrono::milliseconds sigterm_grace) noexcept {
    const int pid  = child_pid_.load(std::memory_order_relaxed);
    const int pgid = child_pgid_.load(std::memory_order_relaxed);

    if (pid <= 0) return;

    // SIGTERM to the entire process group.
    if (pgid > 0) {
        ::killpg(static_cast<pid_t>(pgid), SIGTERM);
    } else {
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    }

    // Wait for voluntary exit.
    if (!wait_for_exit(pid, sigterm_grace)) {
        // Log inside try-catch: spdlog can theoretically throw under memory
        // pressure, and this function is marked noexcept.  An uncaught throw
        // here would call std::terminate() (SIGABRT) instead of propagating.
        try {
            sidecar_log()->warn("sidecar did not exit after SIGTERM in {}ms; sending SIGKILL",
                                sigterm_grace.count());
        } catch (...) {}
        if (pgid > 0) {
            ::killpg(static_cast<pid_t>(pgid), SIGKILL);
        } else {
            ::kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }

    // Final reap — blocking.
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid), &status, 0);
    child_pid_.store(-1, std::memory_order_relaxed);
    try {
        sidecar_log()->debug("sidecar child pid={} reaped after kill", pid);
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// do_spawn — pick port, build argv+envp, posix_spawn
// ---------------------------------------------------------------------------

Result<void> SidecarManager::do_spawn() {
    // Step 1: pick a free port.
    const uint16_t port = pick_free_port();
    if (port == 0) {
        return batbox::Err(std::string("failed to pick a free port for sidecar"));
    }
    port_.store(port, std::memory_order_release);

    sidecar_log()->info("sidecar: spawning on port {}", port);

    // Step 2: build argv.
    // argv: [python_bin, "-m", "scrapling_server", "--port", "<N>", nullptr]
    const std::string port_str = std::to_string(port);
    std::vector<std::string> argv_strs = {
        python_bin_, "-m", "scrapling_server", "--port", port_str
    };
    std::vector<char*> argv;
    argv.reserve(argv_strs.size() + 1);
    for (auto& s : argv_strs) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    // Step 3: build envp.
    auto env_strings = build_envp(venv_dir_, port);
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    // Step 4: set up file actions — redirect stderr to a pipe we own.
    //         stdout is also redirected to stderr pipe so all output is captured.
    int stderr_pipe[2]{-1, -1};
    if (::pipe(stderr_pipe) != 0) {
        return batbox::Err(std::string("pipe() failed: ") + std::strerror(errno));
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Close read end in child.
    posix_spawn_file_actions_addclose(&fa, stderr_pipe[0]);
    // Redirect child's stdout and stderr to write end of pipe.
    posix_spawn_file_actions_adddup2(&fa, stderr_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, stderr_pipe[1], STDERR_FILENO);
    // Close the original write fd in child (it's been dup2'd).
    posix_spawn_file_actions_addclose(&fa, stderr_pipe[1]);

    // Step 5: spawn attributes — set process group.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0); // new pgid = child pid

    // Step 6: posix_spawnp (resolves python_bin_ via PATH in envp).
    pid_t child_pid = -1;
    const int spawn_rc = ::posix_spawnp(
        &child_pid,
        argv[0],      // file (resolved via PATH)
        &fa,
        &attr,
        argv.data(),
        envp.data()
    );

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    // Close write end in parent; we only keep the read end.
    ::close(stderr_pipe[1]);

    if (spawn_rc != 0) {
        ::close(stderr_pipe[0]);
        return batbox::Err(std::string("posix_spawnp failed: ") + std::strerror(spawn_rc));
    }

    child_pid_.store(static_cast<int>(child_pid), std::memory_order_release);
    // The child's pgid equals its pid (POSIX_SPAWN_SETPGROUP with pgroup=0).
    child_pgid_.store(static_cast<int>(child_pid), std::memory_order_release);
    stderr_read_fd_ = stderr_pipe[0];

    sidecar_log()->info("sidecar: spawned pid={} pgid={} port={}",
                        child_pid, child_pid, port);

    // Step 7: start stderr reader thread.
    start_stderr_reader(stderr_pipe[0]);
    stderr_read_fd_ = -1; // ownership transferred to reader thread

    return {};
}

// ---------------------------------------------------------------------------
// wait_for_healthy — poll /healthz until ready or timeout
// ---------------------------------------------------------------------------

Result<void> SidecarManager::wait_for_healthy(CancelToken ct) {
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::seconds(startup_timeout_sec_);

    const uint16_t port = port_.load(std::memory_order_acquire);
    ScraplingClient client(port);

    constexpr auto kPollInterval = std::chrono::milliseconds(100);

    while (true) {
        if (ct.is_cancelled()) {
            // CPP 7.6: On first Ctrl+C, leave state as Starting so the sidecar
            // process continues booting in the background.  The caller's cancel
            // token fires (tool call returns Cancelled), but the background
            // prewarm_async future will eventually resolve to Running or
            // CrashedRestarting independently.  Do NOT transition → Disabled here.
            return batbox::Err(std::string("cancelled"));
        }

        if (client.healthz()) {
            // Transition Starting → Running.
            if (state_.try_transition(SidecarState::Starting, SidecarState::Running)) {
                sidecar_log()->info("sidecar: running on port {}", port);
                return {};
            }
            // Another thread may have already transitioned (e.g. shutdown).
            const auto cur = state_.current();
            if (cur == SidecarState::Running) {
                return {}; // already running — we're good
            }
            return batbox::Err(std::string("sidecar state changed unexpectedly during healthcheck: ") +
                               std::string(to_string(cur)));
        }

        // Check if the child died before we got a healthy response.
        if (try_reap_child()) {
            (void)state_.try_transition(SidecarState::Starting, SidecarState::CrashedRestarting);
            return batbox::Err(std::string("sidecar process exited during startup"));
        }

        if (clock::now() >= deadline) {
            (void)state_.try_transition(SidecarState::Starting, SidecarState::CrashedRestarting);
            return batbox::Err(std::string("sidecar startup timeout (") +
                               std::to_string(startup_timeout_sec_) + "s)");
        }

        std::this_thread::sleep_for(kPollInterval);
    }
}

// ---------------------------------------------------------------------------
// ensure_started
// ---------------------------------------------------------------------------

Result<void> SidecarManager::ensure_started(CancelToken ct) {
    // Fast path: already running.
    if (state_.current() == SidecarState::Running) {
        return {};
    }

    // Disabled state: cannot start.
    if (state_.current() == SidecarState::Disabled) {
        return batbox::Err(std::string("sidecar is disabled"));
    }

    // Prewarm path: if a background prewarm is in-flight, wait for it first.
    // If the prewarm succeeded the state is already Running; if it failed we
    // fall through to the normal cold-start path below (graceful degradation).
    if (prewarm_future_.valid()) {
        sidecar_log()->debug("ensure_started: consuming prewarm future");
        auto prewarm_res = prewarm_future_.get(); // blocks until prewarm done
        if (prewarm_res.has_value()) {
            // Prewarm brought us to Running.
            if (state_.current() == SidecarState::Running) {
                return {};
            }
        } else {
            sidecar_log()->warn("ensure_started: prewarm failed ({}); attempting cold start",
                                prewarm_res.error());
            // State may be CrashedRestarting or Cold — fall through to normal path.
        }
    }

    // CrashedRestarting: attempt a restart if under the cap.
    if (state_.current() == SidecarState::CrashedRestarting) {
        const int attempts = restart_count_.load(std::memory_order_relaxed);
        if (attempts >= kMaxRestarts) {
            (void)state_.try_transition(SidecarState::CrashedRestarting, SidecarState::Disabled);
            return batbox::Err(std::string("sidecar restart cap reached (") +
                               std::to_string(kMaxRestarts) + ")");
        }
        // Try to win the CrashedRestarting→Starting transition.
        if (!state_.try_transition(SidecarState::CrashedRestarting, SidecarState::Starting)) {
            // Another thread won; fall through to the poll loop below.
        } else {
            restart_count_.fetch_add(1, std::memory_order_relaxed);
            sidecar_log()->warn("sidecar: restart attempt {}/{}", attempts + 1, kMaxRestarts);
            // Kill and clean up the previous child before spawning a new one.
            kill_and_reap(std::chrono::milliseconds(500));
            if (stderr_reader_thread_.joinable()) {
                stderr_reader_thread_.join();
            }
            auto spawn_res = do_spawn();
            if (!spawn_res.has_value()) {
                (void)state_.try_transition(SidecarState::Starting, SidecarState::CrashedRestarting);
                return batbox::Err(spawn_res.error());
            }
            return wait_for_healthy(std::move(ct));
        }
    }

    // Cold: attempt Cold→Starting.
    if (state_.current() == SidecarState::Cold) {
        if (state_.try_transition(SidecarState::Cold, SidecarState::Starting)) {
            auto spawn_res = do_spawn();
            if (!spawn_res.has_value()) {
                (void)state_.try_transition(SidecarState::Starting, SidecarState::CrashedRestarting);
                return batbox::Err(spawn_res.error());
            }
            return wait_for_healthy(std::move(ct));
        }
        // Another thread won Cold→Starting; fall through to poll.
    }

    // Poll until Running or a terminal state (Starting driven by another thread).
    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::seconds(startup_timeout_sec_ + 5);

    while (clock::now() < deadline) {
        if (ct.is_cancelled()) {
            return batbox::Err(std::string("cancelled"));
        }
        const auto cur = state_.current();
        if (cur == SidecarState::Running) {
            return {};
        }
        if (cur == SidecarState::Disabled) {
            return batbox::Err(std::string("sidecar disabled"));
        }
        if (cur == SidecarState::CrashedRestarting) {
            // Recurse to handle the restart attempt.
            return ensure_started(std::move(ct));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return batbox::Err(std::string("timed out waiting for sidecar to become ready"));
}

// ---------------------------------------------------------------------------
// prewarm_async — launch background ensure_started
// ---------------------------------------------------------------------------

void SidecarManager::prewarm_async(CancelToken ct,
                                   std::function<void(std::string_view)> status_cb)
{
    // No-op if a prewarm is already in-flight or if the sidecar is already
    // running (or disabled).
    if (prewarm_future_.valid()) {
        sidecar_log()->debug("prewarm_async: already in-flight, ignoring second call");
        return;
    }
    const auto cur = state_.current();
    if (cur == SidecarState::Running || cur == SidecarState::Disabled) {
        sidecar_log()->debug("prewarm_async: state={}, skipping", to_string(cur));
        return;
    }

    prewarm_status_cb_ = std::move(status_cb);

    // Fire the status callback with "prewarming" to update the status line.
    if (prewarm_status_cb_) {
        prewarm_status_cb_("prewarming");
    }

    sidecar_log()->info("sidecar: prewarm launched (background)");

    // Launch the prewarm on a dedicated thread.
    // We capture `this` — safe because wait_prewarm() / destructor join
    // the future before SidecarManager is destroyed.
    prewarm_future_ = std::async(std::launch::async, [this, ct = std::move(ct)]() mutable -> Result<void> {
        auto res = this->ensure_started(std::move(ct));
        if (res.has_value()) {
            sidecar_log()->info("sidecar: prewarm complete — Running on port {}", this->port());
            if (this->prewarm_status_cb_) {
                this->prewarm_status_cb_("ready");
            }
        } else {
            sidecar_log()->warn("sidecar: prewarm failed: {}", res.error());
            if (this->prewarm_status_cb_) {
                this->prewarm_status_cb_("failed: " + res.error());
            }
        }
        return res;
    });
}

// ---------------------------------------------------------------------------
// wait_prewarm — join background prewarm future
// ---------------------------------------------------------------------------

Result<void> SidecarManager::wait_prewarm() {
    if (!prewarm_future_.valid()) {
        return {}; // no prewarm was launched
    }
    sidecar_log()->debug("wait_prewarm: waiting for prewarm future");
    return prewarm_future_.get();
}

// ---------------------------------------------------------------------------
// abort_startup — CPP 7.6: second Ctrl+C kills the process group
//
// Sends SIGTERM to the sidecar process group and transitions the state to
// CrashedRestarting (from Starting or Running).  Called by the App-level
// SIGINT double-tap handler when the user presses Ctrl+C twice within 2 s
// while the sidecar is still booting.
//
// The sidecar process will be reaped lazily: the next call to ensure_started()
// will see CrashedRestarting, call kill_and_reap(), and attempt a restart (up
// to kMaxRestarts).
//
// This method is async-signal-safe in the sense that it does not call any
// signal-unsafe operations itself — it is called from the main thread AFTER
// the signal handler has set a flag.  SIGTERM delivery is async-signal-safe.
// ---------------------------------------------------------------------------

void SidecarManager::abort_startup() noexcept {
    const int pid  = child_pid_.load(std::memory_order_relaxed);
    const int pgid = child_pgid_.load(std::memory_order_relaxed);

    if (pid <= 0) {
        return; // No child to kill.
    }

    sidecar_log()->warn("sidecar: abort_startup() — sending SIGTERM to pgid={} (second Ctrl+C)", pgid);

    if (pgid > 0) {
        ::killpg(static_cast<pid_t>(pgid), SIGTERM);
    } else {
        ::kill(static_cast<pid_t>(pid), SIGTERM);
    }

    // Transition to CrashedRestarting from whichever state we're in.
    // If Starting: the healthcheck poll will either see the process gone or
    // the cancel token fires — either way it ends up in CrashedRestarting.
    // If Running: the process just received SIGTERM; mark it crashed so the
    // next tool call triggers a restart.
    // We attempt both transitions; exactly one will succeed.
    (void)state_.try_transition(SidecarState::Starting, SidecarState::CrashedRestarting);
    (void)state_.try_transition(SidecarState::Running,  SidecarState::CrashedRestarting);
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------

void SidecarManager::shutdown() {
    const auto cur = state_.current();
    // Early return only when there is genuinely nothing to do: state is
    // already Disabled, no child process is running, and the stderr reader
    // thread has already been joined (or was never started).  If the reader
    // thread is still joinable we must proceed so it gets joined below, even
    // if the child was already reaped by try_reap_child().
    if (cur == SidecarState::Disabled &&
        child_pid_.load(std::memory_order_relaxed) <= 0 &&
        !stderr_reader_thread_.joinable()) {
        return; // Nothing to do.
    }

    sidecar_log()->info("sidecar: shutdown initiated (state={})", to_string(cur));

    // Step 1: best-effort POST /shutdown (1 s timeout).
    const uint16_t port = port_.load(std::memory_order_acquire);
    if (port != 0 &&
        (cur == SidecarState::Running || cur == SidecarState::Starting)) {
        ScraplingClient client(port);
        client.shutdown(); // silently ignores errors
    }

    // Step 2: give the process up to 200 ms to self-exit after the HTTP call.
    const int pid = child_pid_.load(std::memory_order_relaxed);
    if (pid > 0) {
        if (!wait_for_exit(pid, std::chrono::milliseconds(200))) {
            // Step 3+4: SIGTERM grace 2 s, then SIGKILL.
            kill_and_reap(std::chrono::milliseconds(2000));
        } else {
            // Process already gone; just reap it.
            int status = 0;
            ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
            child_pid_.store(-1, std::memory_order_relaxed);
        }
    }

    // Step 5: join stderr reader thread.
    if (stderr_reader_thread_.joinable()) {
        stderr_reader_thread_.join();
    }

    // Step 6: transition to Disabled.
    // Try all legal states → Disabled.
    (void)state_.try_transition(SidecarState::Running,           SidecarState::Disabled);
    (void)state_.try_transition(SidecarState::Starting,          SidecarState::Disabled);
    (void)state_.try_transition(SidecarState::CrashedRestarting, SidecarState::Disabled);
    (void)state_.try_transition(SidecarState::Cold,              SidecarState::Disabled);

    sidecar_log()->info("sidecar: shutdown complete");
}

} // namespace batbox::sidecar
