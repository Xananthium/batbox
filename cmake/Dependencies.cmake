# cmake/Dependencies.cmake
# ---------------------------------------------------------------------------
# find_package glue for all 12 vcpkg-managed dependencies.
#
# Vcpkg (manifest mode) installs CONFIG packages under the triplet's
# share/ tree. Every call uses CONFIG mode and REQUIRED so missing packages
# produce actionable errors pointing at the vcpkg port.
#
# After finding all deps, an INTERFACE target `batbox::deps` is created so
# individual subdirectory targets can link with a single line:
#
#   target_link_libraries(<target> PRIVATE batbox::deps)
#
# ---------------------------------------------------------------------------

# Guard against double-inclusion (e.g. if someone adds a second include()).
include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Helper macro: wraps find_package and emits a clear "vcpkg port X" message
# when a package is not found, in addition to CMake's built-in REQUIRED error.
# ---------------------------------------------------------------------------
macro(_batbox_find_dep _pkg)
    find_package(${_pkg} ${ARGN} CONFIG REQUIRED)
endmacro()

# ---------------------------------------------------------------------------
# 1. ftxui  (TUI framework)
#    Port: ftxui
#    Exported targets: ftxui::screen  ftxui::dom  ftxui::component
# ---------------------------------------------------------------------------
find_package(ftxui CONFIG REQUIRED)
if(NOT TARGET ftxui::screen)
    message(FATAL_ERROR
        "[batbox] ftxui::screen target not found after find_package(ftxui).\n"
        "Run:  vcpkg install ftxui  (or ensure vcpkg toolchain is active).")
endif()

# ---------------------------------------------------------------------------
# 2. cpr  (HTTP client wrapping libcurl)
#    Port: cpr
#    Exported target: cpr::cpr
# ---------------------------------------------------------------------------
find_package(cpr CONFIG REQUIRED)
if(NOT TARGET cpr::cpr)
    message(FATAL_ERROR
        "[batbox] cpr::cpr target not found after find_package(cpr).\n"
        "Run:  vcpkg install cpr")
endif()

# ---------------------------------------------------------------------------
# 3. simdjson  (high-performance JSON parsing)
#    Port: simdjson
#    Exported target: simdjson::simdjson
# ---------------------------------------------------------------------------
find_package(simdjson CONFIG REQUIRED)
if(NOT TARGET simdjson::simdjson)
    message(FATAL_ERROR
        "[batbox] simdjson::simdjson not found.\n"
        "Run:  vcpkg install simdjson")
endif()

# ---------------------------------------------------------------------------
# 4. nlohmann_json  (request serialisation / convenience JSON)
#    Port: nlohmann-json
#    Exported target: nlohmann_json::nlohmann_json  (header-only INTERFACE)
# ---------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR
        "[batbox] nlohmann_json::nlohmann_json not found.\n"
        "Run:  vcpkg install nlohmann-json")
endif()

# ---------------------------------------------------------------------------
# 5. lexbor  (HTML/CSS parser used by ScraplingClient)
#    Port: lexbor
#    Exported target: lexbor::lexbor_static
# ---------------------------------------------------------------------------
find_package(lexbor CONFIG REQUIRED)
if(NOT TARGET lexbor::lexbor_static)
    message(FATAL_ERROR
        "[batbox] lexbor::lexbor_static not found.\n"
        "Run:  vcpkg install lexbor")
endif()

# ---------------------------------------------------------------------------
# 6. fmt  (formatting library; also pulled in transitively by spdlog)
#    Port: fmt
#    Exported target: fmt::fmt
# ---------------------------------------------------------------------------
find_package(fmt CONFIG REQUIRED)
if(NOT TARGET fmt::fmt)
    message(FATAL_ERROR
        "[batbox] fmt::fmt not found.\n"
        "Run:  vcpkg install fmt")
endif()

# ---------------------------------------------------------------------------
# 7. spdlog  (structured logging)
#    Port: spdlog
#    Exported target: spdlog::spdlog  (compiled library, links fmt::fmt)
#    Note: spdlog::spdlog_header_only also exists; we use the compiled variant
#    so that fmt is not re-compiled into every TU.
# ---------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)
if(NOT TARGET spdlog::spdlog)
    message(FATAL_ERROR
        "[batbox] spdlog::spdlog not found.\n"
        "Run:  vcpkg install spdlog")
endif()

# ---------------------------------------------------------------------------
# 8. unofficial-sqlite3  (embedded SQL database)
#    Port: sqlite3
#    find_package name: unofficial-sqlite3  (vcpkg wraps SQLite in this port)
#    Exported target: unofficial::sqlite3::sqlite3
#    Note: vcpkg's sqlite3 port does NOT use the standard FindSQLite3 MODULE;
#    it ships a CONFIG package named "unofficial-sqlite3".
# ---------------------------------------------------------------------------
find_package(unofficial-sqlite3 CONFIG REQUIRED)
if(NOT TARGET unofficial::sqlite3::sqlite3)
    message(FATAL_ERROR
        "[batbox] unofficial::sqlite3::sqlite3 not found.\n"
        "Run:  vcpkg install sqlite3")
endif()

# ---------------------------------------------------------------------------
# 9. CLI11  (command-line argument parsing)
#    Port: cli11
#    Exported target: CLI11::CLI11  (header-only INTERFACE)
# ---------------------------------------------------------------------------
find_package(CLI11 CONFIG REQUIRED)
if(NOT TARGET CLI11::CLI11)
    message(FATAL_ERROR
        "[batbox] CLI11::CLI11 not found.\n"
        "Run:  vcpkg install cli11")
endif()

# ---------------------------------------------------------------------------
# 10. utf8cpp  (UTF-8 / Unicode string utilities)
#     Port: utfcpp
#     find_package name: utf8cpp  (the config file is utf8cppConfig.cmake,
#     package name inside it is utf8cpp)
#     Exported target: utf8cpp::utf8cpp  (header-only INTERFACE)
# ---------------------------------------------------------------------------
find_package(utf8cpp CONFIG REQUIRED)
if(NOT TARGET utf8cpp::utf8cpp)
    message(FATAL_ERROR
        "[batbox] utf8cpp::utf8cpp not found.\n"
        "Run:  vcpkg install utfcpp")
endif()

# ---------------------------------------------------------------------------
# 11. IXWebSocket  (WebSocket + HTTP client with TLS/SSL)
#     Port: ixwebsocket[ssl]  — ssl feature wires OpenSSL (no Boost)
#     Exported target: ixwebsocket::ixwebsocket
#     SSL note: vcpkg enables OpenSSL via the [ssl] feature declared in
#     vcpkg.json.  The resulting library already carries the correct
#     INTERFACE_LINK_LIBRARIES for the platform TLS backend (-framework
#     Security on macOS; OpenSSL on Linux).  No extra find_package(OpenSSL)
#     is needed here — the imported target pulls it transitively.
# ---------------------------------------------------------------------------
find_package(ixwebsocket CONFIG REQUIRED)
if(NOT TARGET ixwebsocket::ixwebsocket)
    message(FATAL_ERROR
        "[batbox] ixwebsocket::ixwebsocket not found.\n"
        "Run:  vcpkg install \"ixwebsocket[ssl]\"")
endif()

# ---------------------------------------------------------------------------
# 12. doctest  (unit-test framework — linked only when BATBOX_TESTS=ON)
#     Port: doctest
#     Exported target: doctest::doctest  (header-only INTERFACE)
# ---------------------------------------------------------------------------
if(BATBOX_TESTS)
    find_package(doctest CONFIG REQUIRED)
    if(NOT TARGET doctest::doctest)
        message(FATAL_ERROR
            "[batbox] doctest::doctest not found (BATBOX_TESTS=ON).\n"
            "Run:  vcpkg install doctest")
    endif()
endif()

# ---------------------------------------------------------------------------
# batbox::deps  — aggregate INTERFACE target
#
# Every component library (batbox_core, batbox_inference, etc.) can link to
# this single target instead of listing individual dep targets:
#
#   target_link_libraries(batbox_inference PRIVATE batbox::deps)
#
# Only the 11 mandatory runtime deps are included.  doctest is intentionally
# omitted because it is test-only and must be linked selectively in tests/.
# ---------------------------------------------------------------------------
if(NOT TARGET batbox::deps)
    add_library(batbox::deps INTERFACE IMPORTED GLOBAL)
    target_link_libraries(batbox::deps
        INTERFACE
            ftxui::screen
            ftxui::dom
            ftxui::component
            cpr::cpr
            simdjson::simdjson
            nlohmann_json::nlohmann_json
            lexbor::lexbor_static
            fmt::fmt
            spdlog::spdlog
            unofficial::sqlite3::sqlite3
            CLI11::CLI11
            utf8cpp::utf8cpp
            ixwebsocket::ixwebsocket
    )
endif()

message(STATUS "[batbox] Dependencies.cmake: all 12 vcpkg deps resolved")
if(BATBOX_TESTS)
    message(STATUS "[batbox]   doctest::doctest    : available (BATBOX_TESTS=ON)")
endif()
