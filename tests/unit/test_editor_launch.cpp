// tests/unit/test_editor_launch.cpp
// ---------------------------------------------------------------------------
// Unit tests for batbox::util::resolve_editor() — TUI-FIX-T7
//
// Tests cover:
//   1. EDITOR=nano           → "nano"  (not vi/vim, use as-is)
//   2. EDITOR=vi             → fallback (nano if available, else pico, else vi)
//   3. EDITOR=vim            → fallback (same as above)
//   4. EDITOR=/usr/bin/nano  → "/usr/bin/nano" (full path, not vi/vim basename)
//   5. EDITOR unset + nano available   → "nano"
//   6. binary_accessible: known binary → true
//   7. binary_accessible: garbage name → false
//
// Note: tests 2, 3, and 5 depend on whether "nano" or "pico" is installed on
// the test machine.  On macOS and most Linux distros nano is present.  The
// tests assert the result is NOT vi/vim when nano is available.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "batbox/util/EditorLaunch.hpp"

#include <cstdlib>    // setenv / unsetenv / putenv
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers: portable setenv / unsetenv wrappers
// ---------------------------------------------------------------------------

namespace {

#ifdef _WIN32
void set_env(const char* name, const char* value) {
    std::string kv = std::string(name) + "=" + value;
    _putenv(kv.c_str());
}
void unset_env(const char* name) {
    std::string kv = std::string(name) + "=";
    _putenv(kv.c_str());
}
#else
void set_env(const char* name, const char* value) {
    ::setenv(name, value, /*overwrite=*/1);
}
void unset_env(const char* name) {
    ::unsetenv(name);
}
#endif

/// RAII guard: saves $EDITOR, sets it to `val` in ctor, restores in dtor.
struct EditorGuard {
    std::string saved;
    bool        was_set;

    explicit EditorGuard(const char* val) {
        const char* e = std::getenv("EDITOR");
        was_set = (e != nullptr);
        if (was_set) saved = e;
        if (val) {
            set_env("EDITOR", val);
        } else {
            unset_env("EDITOR");
        }
    }
    ~EditorGuard() {
        if (was_set) {
            set_env("EDITOR", saved.c_str());
        } else {
            unset_env("EDITOR");
        }
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// binary_accessible() tests
// ---------------------------------------------------------------------------

TEST_CASE("binary_accessible: sh is present on POSIX") {
#ifndef _WIN32
    CHECK(batbox::util::binary_accessible("sh"));
#endif
}

TEST_CASE("binary_accessible: garbage name returns false") {
    CHECK_FALSE(batbox::util::binary_accessible("__batbox_no_such_binary_xyz__"));
}

// ---------------------------------------------------------------------------
// resolve_editor() — $EDITOR set to a non-vi editor
// ---------------------------------------------------------------------------

TEST_CASE("resolve_editor: EDITOR=nano returns nano") {
    EditorGuard g("nano");
    const std::string ed = batbox::util::resolve_editor();
    CHECK(ed == "nano");
}

TEST_CASE("resolve_editor: EDITOR=emacs returns emacs") {
    EditorGuard g("emacs");
    const std::string ed = batbox::util::resolve_editor();
    CHECK(ed == "emacs");
}

TEST_CASE("resolve_editor: EDITOR=/usr/bin/nano returns /usr/bin/nano") {
    EditorGuard g("/usr/bin/nano");
    const std::string ed = batbox::util::resolve_editor();
    // Full path with non-vi basename — returned as-is.
    CHECK(ed == "/usr/bin/nano");
}

// ---------------------------------------------------------------------------
// resolve_editor() — $EDITOR is vi or vim → must fall back
// ---------------------------------------------------------------------------

TEST_CASE("resolve_editor: EDITOR=vi falls back (not vi when nano available)") {
    EditorGuard g("vi");
    const std::string ed = batbox::util::resolve_editor();
    if (batbox::util::binary_accessible("nano")) {
        // Nano is available — should prefer it over vi.
        CHECK(ed == "nano");
    } else if (batbox::util::binary_accessible("pico")) {
        CHECK(ed == "pico");
    } else {
        // No alternative found — last-resort vi is acceptable.
        CHECK(ed == "vi");
    }
}

TEST_CASE("resolve_editor: EDITOR=vim falls back (same logic as vi)") {
    EditorGuard g("vim");
    const std::string ed = batbox::util::resolve_editor();
    if (batbox::util::binary_accessible("nano")) {
        CHECK(ed == "nano");
    } else if (batbox::util::binary_accessible("pico")) {
        CHECK(ed == "pico");
    } else {
        CHECK(ed == "vi");
    }
}

TEST_CASE("resolve_editor: EDITOR=/usr/bin/vim falls back (vim basename)") {
    EditorGuard g("/usr/bin/vim");
    const std::string ed = batbox::util::resolve_editor();
    // Basename is "vim" → should trigger fallback.
    if (batbox::util::binary_accessible("nano")) {
        CHECK(ed == "nano");
    } else if (batbox::util::binary_accessible("pico")) {
        CHECK(ed == "pico");
    } else {
        CHECK(ed == "vi");
    }
}

// ---------------------------------------------------------------------------
// resolve_editor() — $EDITOR unset
// ---------------------------------------------------------------------------

TEST_CASE("resolve_editor: EDITOR unset prefers nano over vi when nano available") {
    EditorGuard g(nullptr);  // unset
    const std::string ed = batbox::util::resolve_editor();
    if (batbox::util::binary_accessible("nano")) {
        CHECK(ed == "nano");
    } else if (batbox::util::binary_accessible("pico")) {
        CHECK(ed == "pico");
    } else {
        // Neither nano nor pico available — last resort vi.
        CHECK(ed == "vi");
    }
}

TEST_CASE("resolve_editor: result is always non-empty") {
    EditorGuard g(nullptr);
    CHECK_FALSE(batbox::util::resolve_editor().empty());
}
