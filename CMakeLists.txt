cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# vcpkg toolchain bootstrap
# Must appear BEFORE project() so vcpkg can inject package search paths.
# Users who pass -DCMAKE_TOOLCHAIN_FILE=<path> on the command line take
# precedence; this block only fires when the variable is unset (typical
# developer workflow with vcpkg.json manifest at repo root).
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  include(cmake/VcpkgToolchain.cmake)
endif()

# ---------------------------------------------------------------------------
# Project declaration
# ---------------------------------------------------------------------------
project(batbox
  VERSION 0.1.0
  DESCRIPTION "batbox – terminal AI assistant"
  LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# C++ standard – C++20 required, GNU/MSVC extensions OFF for portability
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ---------------------------------------------------------------------------
# IDE support: export compile_commands.json unconditionally
# ---------------------------------------------------------------------------
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build options
# Defaults: TESTS ON (active dev), SANITIZERS and STATIC OFF
# (opt-in for CI / release builds respectively)
# ---------------------------------------------------------------------------
option(BATBOX_TESTS
  "Build the test suite (requires tests/ subdirectory)"
  ON)

option(BATBOX_SANITIZERS
  "Enable AddressSanitizer + UBSanitizer in Debug builds"
  OFF)

option(BATBOX_STATIC
  "Static-link final binary (Linux/musl targets only)"
  OFF)

# ---------------------------------------------------------------------------
# cmake/ helper modules (sanitizer flags, vcpkg dependency wiring, etc.)
# Included conditionally so the skeleton configures cleanly before those
# files are added in their respective component tasks.
# ---------------------------------------------------------------------------
include(cmake/Sanitizers.cmake   OPTIONAL)
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Static-egress scan (CPP B.8)
# Provides batbox_add_egress_check(<target>) — call this after the batbox
# executable target is defined (CPP A wiring task). Example:
#
#   batbox_add_egress_check(batbox)
#
# The function is intentionally not called here because the `batbox` target
# does not yet exist in this scaffold. This include makes the function
# available so the A-series wiring task can call it after add_executable().
# ---------------------------------------------------------------------------
include(cmake/EgressCheck.cmake)
# batbox_add_egress_check(batbox)    # uncomment in CPP A wiring task

# ---------------------------------------------------------------------------
# Theme header generation (CPP B.9)
# Reads data/themes.json at configure time and generates:
#   ${CMAKE_BINARY_DIR}/generated/include/batbox/themes/Themes.hpp
#
# The generated header provides constexpr BatboxTheme structs and the
# kThemes array for all five palettes (miss-kittin, stock-exchange,
# frank-sinatra, monochrome, classic).
#
# Re-triggers automatically when data/themes.json changes (configure_file
# dependency). No rebuild overhead — header is written at configure time only.
#
# Uncomment the call below once the TUI target exists (CPP 1.X wiring task).
# Then add the include dir to the TUI target:
#
#   batbox_generate_themes_header("${CMAKE_BINARY_DIR}/generated/include")
#   target_include_directories(batbox_tui PRIVATE ${BATBOX_THEMES_INCLUDE_DIR})
# ---------------------------------------------------------------------------
include(cmake/Theme.cmake)
# batbox_generate_themes_header("${CMAKE_BINARY_DIR}/generated/include")

# ---------------------------------------------------------------------------
# Configure summary
# ---------------------------------------------------------------------------
message(STATUS "batbox ${PROJECT_VERSION} — configure summary")
message(STATUS "  C++ standard  : ${CMAKE_CXX_STANDARD}")
message(STATUS "  BATBOX_TESTS      : ${BATBOX_TESTS}")
message(STATUS "  BATBOX_SANITIZERS : ${BATBOX_SANITIZERS}")
message(STATUS "  BATBOX_STATIC     : ${BATBOX_STATIC}")

# ---------------------------------------------------------------------------
# src/ — all component libraries and their subdirectory wiring
# src/CMakeLists.txt owns the full add_subdirectory chain in dependency order:
#   core → theme/config → session/permissions → inference/sidecar/mcp
#   → tools/agents → conversation → skills/commands/plugins → repl → tui
#
# Executable target (batbox) and link step land in CPP A wiring task:
#   add_executable(batbox src/main.cpp src/App.cpp src/setup_sidecar.cpp)
#   target_link_libraries(batbox PRIVATE
#     batbox_tui batbox_repl batbox_commands batbox_plugins batbox_skills
#     batbox_conversation batbox_agents batbox_tools batbox_mcp batbox_sidecar
#     batbox_inference batbox_permissions batbox_session batbox_config
#     batbox_theme batbox_core
#   )
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# tests/ — unit + perf test suite (BATBOX_TESTS=ON, default ON)
# tests/CMakeLists.txt wires Catch2 + batbox_add_unit_test() helper macro.
# Individual test files are added by their per-component CPP N.x tasks.
# ---------------------------------------------------------------------------
enable_testing()
if(BATBOX_TESTS)
  add_subdirectory(tests)
endif()
