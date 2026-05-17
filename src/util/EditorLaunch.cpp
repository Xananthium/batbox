// src/util/EditorLaunch.cpp
// ---------------------------------------------------------------------------
// batbox::util — resolve_editor(), binary_accessible(), edit_string_in_editor()
// See include/batbox/util/EditorLaunch.hpp for the contract.
//
// Blueprint contract: resolve_editor (task TUI-FIX-T7)
// ---------------------------------------------------------------------------

#include "batbox/util/EditorLaunch.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>       // std::getenv, std::system
#include <cstring>       // std::strlen
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#  include <io.h>        // _access
#  define F_OK 0
#  define X_OK 1
#  define batbox_access _access
#else
#  include <unistd.h>    // access, mkstemp, unlink, close
#  define batbox_access ::access
#endif

// Pull in FTXUI's ScreenInteractive for the WithRestoredIO call.
// We only need this in the .cpp — the header forward-declares it.
#include <ftxui/component/screen_interactive.hpp>

namespace batbox::util {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

/// Return the last path component of `path` (basename equivalent).
/// Works for both "/usr/bin/vi" → "vi" and bare "nano" → "nano".
[[nodiscard]]
static std::string path_basename(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

} // anonymous namespace

// =============================================================================
// binary_accessible()
// =============================================================================

bool binary_accessible(const std::string& binary) {
    // If binary contains a path separator, test it directly.
    if (binary.find('/') != std::string::npos
#ifdef _WIN32
        || binary.find('\\') != std::string::npos
#endif
    ) {
        return batbox_access(binary.c_str(), X_OK) == 0;
    }

    // Walk $PATH entries.
    const char* path_env = std::getenv("PATH");
    if (!path_env || path_env[0] == '\0') {
        return false;
    }

    std::string_view path_view(path_env);
#ifdef _WIN32
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif

    while (!path_view.empty()) {
        const auto delim = path_view.find(kSep);
        const auto dir   = (delim == std::string_view::npos)
                           ? path_view
                           : path_view.substr(0, delim);

        if (!dir.empty()) {
            // Construct full path and test with access().
            std::string candidate;
            candidate.reserve(dir.size() + 1 + binary.size());
            candidate.append(dir);
            candidate += '/';
            candidate.append(binary);
            if (batbox_access(candidate.c_str(), X_OK) == 0) {
                return true;
            }
        }

        if (delim == std::string_view::npos) break;
        path_view = path_view.substr(delim + 1);
    }

    return false;
}

// =============================================================================
// resolve_editor()
// =============================================================================

std::string resolve_editor() {
    // 1. Honour $EDITOR unless it resolves to vi/vim.
    const char* env = std::getenv("EDITOR");
    if (env && env[0] != '\0') {
        const std::string editor_str(env);
        const std::string base = path_basename(editor_str);
        if (base != "vi" && base != "vim") {
            return editor_str;
        }
        // $EDITOR is vi/vim — fall through to preferred fallbacks below.
    }

    // 2–3. Check preferred fallbacks in order.
    const std::array<const char*, 2> preferred = { "nano", "pico" };
    for (const char* candidate : preferred) {
        if (binary_accessible(candidate)) {
            return std::string(candidate);
        }
    }

    // 4. Last resort: vi (always available on POSIX).
    return "vi";
}

// =============================================================================
// edit_string_in_editor()
// =============================================================================

std::string edit_string_in_editor(const std::string& text,
                                  ftxui::ScreenInteractive* screen) {
    // -------------------------------------------------------------------------
    // 1. Create a uniquely-named temp file and write `text` into it.
    // -------------------------------------------------------------------------
    std::string tmp_path;
    int fd = -1;

#ifdef _WIN32
    // On Windows, use a temp file in the temp directory.
    char tmp_buf[MAX_PATH + 16];
    {
        char tmp_dir[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmp_dir) == 0) {
            return text;  // Cannot determine temp dir — return original.
        }
        char tmp_name[MAX_PATH];
        if (GetTempFileNameA(tmp_dir, "bbx", 0, tmp_name) == 0) {
            return text;
        }
        tmp_path = tmp_name;
        fd = _open(tmp_name, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IWRITE);
    }
#else
    {
        // mkstemp — POSIX, creates the file and returns an open fd.
        char tmp_template[] = "/tmp/batbox_edit_XXXXXX";
        fd = ::mkstemp(tmp_template);
        if (fd == -1) {
            return text;  // Cannot create temp file — return original unchanged.
        }
        tmp_path = tmp_template;
    }
#endif

    // Write content to the temp file, then close the fd before exec-ing the editor
    // (some editors refuse to open a file that is already open).
    {
        std::ofstream out;
#ifndef _WIN32
        // Wrap the raw fd in an ofstream via the file descriptor path trick.
        ::close(fd);
        fd = -1;
        out.open(tmp_path);
#else
        ::_close(fd);
        fd = -1;
        out.open(tmp_path);
#endif
        if (!out.is_open()) {
#ifndef _WIN32
            ::unlink(tmp_path.c_str());
#endif
            return text;
        }
        out << text;
    }

    // -------------------------------------------------------------------------
    // 2. Resolve editor and build the shell command.
    // -------------------------------------------------------------------------
    const std::string editor = resolve_editor();
    // Quote the path in case it contains spaces.
    const std::string cmd = editor + " \"" + tmp_path + "\"";

    // -------------------------------------------------------------------------
    // 3. Run the editor, suspending the FTXUI screen if one is provided.
    // -------------------------------------------------------------------------
    int rc = 0;
    if (screen != nullptr) {
        // WithRestoredIO suspends FTXUI raw mode, runs the closure on the
        // current thread (synchronously), then restores raw mode.
        screen->WithRestoredIO([&cmd, &rc]() {
            rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c)
        })();
    } else {
        rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c)
    }

    // -------------------------------------------------------------------------
    // 4. Read the (possibly modified) temp file back.
    // -------------------------------------------------------------------------
    std::string result = text;  // default: return original on any error
    if (rc == 0 || rc != -1) {
        // Even if editor exited non-zero we still read back the content —
        // the user may have saved before a crash / non-zero exit.
        std::ifstream in(tmp_path);
        if (in.is_open()) {
            std::ostringstream ss;
            ss << in.rdbuf();
            result = ss.str();
        }
    }

    // -------------------------------------------------------------------------
    // 5. Clean up temp file.
    // -------------------------------------------------------------------------
#ifndef _WIN32
    ::unlink(tmp_path.c_str());
#else
    ::DeleteFileA(tmp_path.c_str());
#endif

    return result;
}

} // namespace batbox::util
