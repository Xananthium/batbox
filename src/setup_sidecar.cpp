// src/setup_sidecar.cpp
// =============================================================================
// batbox::cmd::setup_sidecar — `batbox setup-sidecar` subcommand.
//
// Installs (or upgrades) the Python Scrapling sidecar virtual-environment.
//
// Steps performed:
//   1. Resolve python interpreter
//      BATBOX_SIDECAR_PYTHON  (env)  — explicit override
//      python3 from PATH             — default
//      → Validate version >= 3.10 via `python -c "import sys; ..."`
//
//   2. Resolve venv directory
//      BATBOX_SIDECAR_VENV    (env)  — explicit override
//      ~/.batbox/sidecar/.venv       — default
//      → Create parent directories if absent.
//
//   3. Resolve python-sidecar source directory
//      BATBOX_SIDECAR_SOURCE  (env)  — explicit override (good for tests)
//      project_root()/python-sidecar — dev/repo workflow (walk up from cwd)
//      → Must contain pyproject.toml; hard-fail if absent.
//
//   4. Create venv — skip if already healthy (pyvenv.cfg exists AND
//      <venv>/bin/python responds to --version).  Force-recreate if corrupt.
//
//   5. Upgrade pip inside the venv — `<venv-python> -m pip install --upgrade pip`
//      (quiet; failure is non-fatal: warn and continue).
//
//   6. Install sidecar package — `<venv-python> -m pip install -e <source_dir>`
//      Runs unconditionally so every call upgrades the editable install.
//      Captures stderr and includes it in any failure message.
//
//   7. Probe the installation — `<venv-python> -m scrapling_server --help`
//      Exit 0 means the package is importable and wired up.
//
// Exit codes:
//   0  — success
//   1  — failure (error message already written to stderr with remediation)
//
// Environment variables (all optional):
//   BATBOX_SIDECAR_PYTHON   path to python interpreter (default: python3)
//   BATBOX_SIDECAR_VENV     path to venv directory    (default: ~/.batbox/sidecar/.venv)
//   BATBOX_SIDECAR_SOURCE   path to python-sidecar/   (default: project_root()/python-sidecar)
// =============================================================================

#include "setup_sidecar.hpp"

#include <batbox/core/Logging.hpp>
#include <batbox/core/Paths.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace batbox::cmd {

namespace {

// ---------------------------------------------------------------------------
// Shell-quote a single token for use in a POSIX sh command string.
// Wraps the value in single-quotes and escapes any embedded single-quotes.
// ---------------------------------------------------------------------------
std::string shell_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''"; // end-quote, escaped quote, re-open quote
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

// ---------------------------------------------------------------------------
// run_capture — run a command via popen, capturing stdout+stderr.
//
// Returns:
//   {exit_code, combined_output}
//
// Redirects stderr to stdout (2>&1) so we capture diagnostic messages.
// Trims trailing whitespace from the captured output.
// ---------------------------------------------------------------------------
struct RunResult {
    int         exit_code;
    std::string output;
};

RunResult run_capture(const std::string& cmd) {
    BATBOX_LOG_DEBUG("setup-sidecar: running: {}", cmd);

    std::string full_cmd = cmd + " 2>&1";
    FILE* fp = ::popen(full_cmd.c_str(), "r");
    if (!fp) {
        int err = errno;
        return {-1, std::string("popen failed: ") + std::strerror(err)};
    }

    std::ostringstream buf;
    std::array<char, 4096> chunk{};
    while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), fp)) {
        buf << chunk.data();
    }
    int status = ::pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1; // NOLINT

    std::string output = buf.str();
    // Trim trailing whitespace / newlines
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                                output.back() == ' '  || output.back() == '\t')) {
        output.pop_back();
    }

    if (exit_code != 0) {
        BATBOX_LOG_DEBUG("setup-sidecar: exit {} — output: {}", exit_code, output);
    }
    return {exit_code, output};
}

// ---------------------------------------------------------------------------
// run_visible — run a command, streaming its output to stdout in real time.
// Returns the exit code.
// ---------------------------------------------------------------------------
int run_visible(const std::string& cmd) {
    BATBOX_LOG_DEBUG("setup-sidecar: running (visible): {}", cmd);
    int rc = std::system(cmd.c_str()); // NOLINT(cert-env33-c)
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1; // NOLINT
}

// ---------------------------------------------------------------------------
// resolve_python — $BATBOX_SIDECAR_PYTHON, else "python3"
// ---------------------------------------------------------------------------
std::string resolve_python() {
    const char* env_val = std::getenv("BATBOX_SIDECAR_PYTHON");
    if (env_val && *env_val != '\0') {
        return env_val;
    }
    return "python3";
}

// ---------------------------------------------------------------------------
// validate_python_version — ensure interpreter is >= 3.10
//
// Returns true on success. On failure writes a clear error to stderr.
// ---------------------------------------------------------------------------
bool validate_python_version(const std::string& python) {
    std::string cmd =
        shell_quote(python) +
        " -c \"import sys; v=sys.version_info; print(f'{v.major}.{v.minor}');"
        " sys.exit(0 if (v.major,v.minor)>=(3,10) else 1)\"";

    auto [rc, out] = run_capture(cmd);
    if (rc == 0) {
        BATBOX_LOG_INFO("setup-sidecar: python version {}", out);
        std::cout << "  python version : " << out << " (OK)\n";
        return true;
    }

    if (rc == 1 && !out.empty() && out.find('.') != std::string::npos) {
        // Interpreter found but version < 3.10
        std::cerr << "setup-sidecar: python interpreter is version " << out
                  << " but batbox requires Python >= 3.10.\n"
                  << "  Remediation:\n"
                  << "    • Install Python 3.10+ (e.g. brew install python@3.12)\n"
                  << "    • Set BATBOX_SIDECAR_PYTHON=/path/to/python3.12\n";
    } else {
        // Interpreter not found or some other error
        std::cerr << "setup-sidecar: could not run python interpreter: "
                  << shell_quote(python) << "\n"
                  << "  output: " << out << "\n"
                  << "  Remediation:\n"
                  << "    • Install Python 3.10+: brew install python@3.12\n"
                  << "    • Set BATBOX_SIDECAR_PYTHON to the full path of your python3 binary\n";
    }
    return false;
}

// ---------------------------------------------------------------------------
// resolve_venv_dir — $BATBOX_SIDECAR_VENV, else ~/.batbox/sidecar/.venv
// ---------------------------------------------------------------------------
fs::path resolve_venv_dir() {
    const char* env_val = std::getenv("BATBOX_SIDECAR_VENV");
    if (env_val && *env_val != '\0') {
        return fs::path(env_val);
    }
    return batbox::paths::home_dir() / ".batbox" / "sidecar" / ".venv";
}

// ---------------------------------------------------------------------------
// resolve_source_dir — $BATBOX_SIDECAR_SOURCE, else project_root()/python-sidecar
//
// Returns empty path if nothing is found (caller must hard-fail).
// ---------------------------------------------------------------------------
fs::path resolve_source_dir() {
    const char* env_val = std::getenv("BATBOX_SIDECAR_SOURCE");
    if (env_val && *env_val != '\0') {
        return fs::path(env_val);
    }
    // Walk up from cwd to find the repo root, then locate python-sidecar/
    fs::path root = batbox::paths::project_root();
    fs::path candidate = root / "python-sidecar";
    return candidate;
}

// ---------------------------------------------------------------------------
// venv_is_healthy — check pyvenv.cfg exists and venv's python responds
// ---------------------------------------------------------------------------
bool venv_is_healthy(const fs::path& venv_dir) {
    if (!fs::exists(venv_dir / "pyvenv.cfg")) {
        return false;
    }
    fs::path venv_python = venv_dir / "bin" / "python";
    if (!fs::exists(venv_python)) {
        return false;
    }
    auto [rc, _] = run_capture(shell_quote(venv_python.string()) + " --version");
    return rc == 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// setup_sidecar — public entry point (see setup_sidecar.hpp)
// ---------------------------------------------------------------------------
int setup_sidecar() {
    batbox::log::init_logging();
    BATBOX_LOG_INFO("setup-sidecar: starting");

    // -----------------------------------------------------------------------
    // Banner
    // -----------------------------------------------------------------------
    std::cout << "batbox setup-sidecar\n"
              << "====================\n";

    // -----------------------------------------------------------------------
    // Step 1 — resolve + validate Python interpreter
    // -----------------------------------------------------------------------
    const std::string python = resolve_python();
    std::cout << "  python         : " << python << "\n";
    BATBOX_LOG_INFO("setup-sidecar: python interpreter = {}", python);

    if (!validate_python_version(python)) {
        BATBOX_LOG_ERROR("setup-sidecar: python version check failed for: {}", python);
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 2 — resolve venv directory; create parent dirs
    // -----------------------------------------------------------------------
    const fs::path venv_dir = resolve_venv_dir();
    std::cout << "  venv           : " << venv_dir.string() << "\n";
    BATBOX_LOG_INFO("setup-sidecar: venv = {}", venv_dir.string());

    // Create parent directory (~/.batbox/sidecar/) if it doesn't exist
    std::error_code ec;
    fs::create_directories(venv_dir.parent_path(), ec);
    if (ec) {
        std::cerr << "setup-sidecar: cannot create venv parent directory "
                  << venv_dir.parent_path().string() << ": " << ec.message() << "\n"
                  << "  Remediation: check permissions on "
                  << (batbox::paths::home_dir() / ".batbox").string() << "\n";
        BATBOX_LOG_ERROR("setup-sidecar: create_directories failed: {}", ec.message());
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 3 — resolve python-sidecar source directory
    // -----------------------------------------------------------------------
    const fs::path source_dir = resolve_source_dir();
    std::cout << "  sidecar source : " << source_dir.string() << "\n";
    BATBOX_LOG_INFO("setup-sidecar: source = {}", source_dir.string());

    if (!fs::exists(source_dir / "pyproject.toml")) {
        std::cerr << "setup-sidecar: python-sidecar source not found at "
                  << source_dir.string() << "\n"
                  << "  Expected to find: " << (source_dir / "pyproject.toml").string() << "\n"
                  << "  Remediation:\n"
                  << "    • Run from the batbox repository root (contains python-sidecar/)\n"
                  << "    • Or set BATBOX_SIDECAR_SOURCE=/path/to/python-sidecar\n";
        BATBOX_LOG_ERROR("setup-sidecar: pyproject.toml not found in {}", source_dir.string());
        return 1;
    }

    std::cout << "\n";

    // -----------------------------------------------------------------------
    // Step 4 — create venv (skip if healthy; recreate if corrupt)
    // -----------------------------------------------------------------------
    if (venv_is_healthy(venv_dir)) {
        std::cout << "[1/4] venv already healthy — skipping creation\n";
        BATBOX_LOG_INFO("setup-sidecar: venv already healthy, skipping creation");
    } else {
        // Remove any partial/corrupt venv before recreating
        if (fs::exists(venv_dir)) {
            std::cout << "[1/4] venv appears corrupt — removing and recreating ...\n"
                      << std::flush;
            BATBOX_LOG_WARN("setup-sidecar: corrupt venv at {}, removing", venv_dir.string());
            fs::remove_all(venv_dir, ec);
            if (ec) {
                std::cerr << "setup-sidecar: failed to remove corrupt venv at "
                          << venv_dir.string() << ": " << ec.message() << "\n"
                          << "  Remediation: manually delete " << venv_dir.string()
                          << " and re-run\n";
                BATBOX_LOG_ERROR("setup-sidecar: remove_all failed: {}", ec.message());
                return 1;
            }
        } else {
            std::cout << "[1/4] creating venv ...\n" << std::flush;
        }

        std::string create_cmd =
            shell_quote(python) + " -m venv " + shell_quote(venv_dir.string());
        auto [rc, out] = run_capture(create_cmd);
        if (rc != 0) {
            std::cerr << "setup-sidecar: failed to create venv (exit " << rc << ")\n"
                      << "  output: " << out << "\n"
                      << "  Remediation:\n"
                      << "    • Confirm Python 3.10+ is installed: python3 --version\n"
                      << "    • Confirm the 'venv' module is available: python3 -m venv --help\n"
                      << "    • On Debian/Ubuntu: sudo apt install python3-venv\n"
                      << "    • Set BATBOX_SIDECAR_PYTHON to an interpreter that has venv\n";
            BATBOX_LOG_ERROR("setup-sidecar: venv creation failed (exit {}): {}", rc, out);
            return 1;
        }
        std::cout << "      venv created\n";
        BATBOX_LOG_INFO("setup-sidecar: venv created at {}", venv_dir.string());
    }

    const fs::path venv_python = venv_dir / "bin" / "python";

    // -----------------------------------------------------------------------
    // Step 5 — upgrade pip (non-fatal: warn and continue)
    // -----------------------------------------------------------------------
    std::cout << "[2/4] upgrading pip ...\n" << std::flush;
    {
        std::string upgrade_cmd =
            shell_quote(venv_python.string()) +
            " -m pip install --upgrade pip --quiet --disable-pip-version-check";
        auto [rc, out] = run_capture(upgrade_cmd);
        if (rc != 0) {
            std::cout << "      WARNING: pip upgrade failed (exit " << rc
                      << ") — continuing anyway\n";
            BATBOX_LOG_WARN("setup-sidecar: pip upgrade failed (exit {}): {}", rc, out);
        } else {
            std::cout << "      pip up-to-date\n";
            BATBOX_LOG_INFO("setup-sidecar: pip upgraded");
        }
    }

    // -----------------------------------------------------------------------
    // Step 6 — pip install -e <source_dir>
    // -----------------------------------------------------------------------
    std::cout << "[3/4] installing sidecar package (pip install -e) ...\n"
              << std::flush;
    BATBOX_LOG_INFO("setup-sidecar: running pip install -e {}", source_dir.string());

    {
        std::string install_cmd =
            shell_quote(venv_python.string()) +
            " -m pip install -e " + shell_quote(source_dir.string()) +
            " --quiet --disable-pip-version-check";
        // Use run_visible so the user sees progress for potentially long downloads
        int rc = run_visible(install_cmd);
        if (rc != 0) {
            // Re-run capturing output to surface in the error message
            auto [rc2, out2] = run_capture(install_cmd);
            std::cerr << "setup-sidecar: pip install -e failed (exit " << rc << ")\n"
                      << "  pip output:\n";
            // Indent each line of pip output for readability
            std::istringstream lines(out2);
            std::string line;
            while (std::getline(lines, line)) {
                std::cerr << "    " << line << "\n";
            }
            std::cerr << "  Remediation:\n"
                      << "    • Check network connectivity\n"
                      << "    • Check " << (source_dir / "pyproject.toml").string() << " is valid\n"
                      << "    • Try manually: "
                      << venv_python.string() << " -m pip install -e "
                      << source_dir.string() << "\n";
            BATBOX_LOG_ERROR("setup-sidecar: pip install -e failed (exit {})", rc);
            return 1;
        }
        std::cout << "      package installed\n";
        BATBOX_LOG_INFO("setup-sidecar: pip install -e succeeded");
    }

    // -----------------------------------------------------------------------
    // Step 7 — probe: verify scrapling_server is importable
    // -----------------------------------------------------------------------
    std::cout << "[4/4] probing sidecar (scrapling_server --help) ...\n"
              << std::flush;
    {
        std::string probe_cmd =
            shell_quote(venv_python.string()) + " -m scrapling_server --help";
        auto [rc, out] = run_capture(probe_cmd);
        if (rc != 0) {
            std::cerr << "setup-sidecar: sidecar probe failed (exit " << rc << ")\n"
                      << "  output: " << out << "\n"
                      << "  Remediation:\n"
                      << "    • Run: " << venv_python.string()
                      << " -m scrapling_server --help\n"
                      << "    • Inspect venv: " << venv_python.string()
                      << " -c \"import scrapling_server; print(scrapling_server.__file__)\"\n"
                      << "    • Re-run `batbox setup-sidecar` to reinstall\n";
            BATBOX_LOG_ERROR("setup-sidecar: probe failed (exit {}): {}", rc, out);
            return 1;
        }
        std::cout << "      probe OK\n";
        BATBOX_LOG_INFO("setup-sidecar: probe passed");
    }

    // -----------------------------------------------------------------------
    // Success summary
    // -----------------------------------------------------------------------
    std::cout << "\nbatbox setup-sidecar: done\n"
              << "  sidecar venv   : " << venv_dir.string() << "\n"
              << "  sidecar python : " << venv_python.string() << "\n"
              << "\n"
              << "  The sidecar will start automatically when you use WebFetch,\n"
              << "  WebSearch, or WebSelect tools.\n"
              << "  To re-run setup (upgrade): batbox setup-sidecar\n";

    BATBOX_LOG_INFO("setup-sidecar: completed successfully, venv={}", venv_dir.string());
    return 0;
}

} // namespace batbox::cmd
