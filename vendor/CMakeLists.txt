# ---------------------------------------------------------------------------
# vendor/CMakeLists.txt — tree-sitter core + grammar static library
#
# Only active when BATBOX_SYNTAX=ON (default ON, see top-level CMakeLists.txt).
# When BATBOX_SYNTAX=OFF this file is never included by CMake.
#
# Sources are vendored as git submodules under vendor/.  If a submodule has
# not been checked out (i.e., the expected parser.c is absent), configure
# aborts with a clear FATAL_ERROR message instructing the developer to run
# `git submodule update --init --recursive`.
#
# The output of this file is a single STATIC library target:
#   batbox_treesitter
#
# Downstream targets link it and access tree-sitter headers via its
# INTERFACE_INCLUDE_DIRECTORIES — no manual include_directories() needed.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.24)

# Guard: this file must only be included when BATBOX_SYNTAX is ON.
# The top-level CMakeLists.txt already gates add_subdirectory(vendor) behind
# `if(BATBOX_SYNTAX)`, so this is a secondary safety net.
if(NOT BATBOX_SYNTAX)
  return()
endif()

# ---------------------------------------------------------------------------
# Helper macro: assert that a submodule source file exists.
# Emits FATAL_ERROR with actionable instructions if it is missing.
# ---------------------------------------------------------------------------
macro(require_submodule LABEL REL_PATH)
  set(_abs "${CMAKE_CURRENT_SOURCE_DIR}/${REL_PATH}")
  if(NOT EXISTS "${_abs}")
    message(FATAL_ERROR
      "\n"
      "  [batbox] Missing vendored submodule: ${LABEL}\n"
      "  Expected file : ${_abs}\n"
      "\n"
      "  tree-sitter grammar sources are vendored as git submodules.\n"
      "  They are NOT fetched automatically during CMake configure.\n"
      "  Run the following command from the repository root and then\n"
      "  re-run CMake:\n"
      "\n"
      "      git submodule update --init --recursive\n"
      "\n"
      "  If you only need to update a single grammar:\n"
      "      git submodule update --remote vendor/${LABEL}\n"
    )
  endif()
endmacro()

# ---------------------------------------------------------------------------
# Verify all submodules are present before declaring any build targets.
# This produces a single, clear error rather than confusing compile errors.
# ---------------------------------------------------------------------------

# tree-sitter core — lib/src/lib.c is the amalgamation source
require_submodule("tree-sitter"            "tree-sitter/lib/src/lib.c")

# Grammar submodules — each must supply src/parser.c
require_submodule("tree-sitter-c"          "tree-sitter-c/src/parser.c")
require_submodule("tree-sitter-cpp"        "tree-sitter-cpp/src/parser.c")
require_submodule("tree-sitter-python"     "tree-sitter-python/src/parser.c")
require_submodule("tree-sitter-javascript" "tree-sitter-javascript/src/parser.c")
require_submodule("tree-sitter-typescript" "tree-sitter-typescript/src/typescript/src/parser.c")
require_submodule("tree-sitter-rust"       "tree-sitter-rust/src/parser.c")
require_submodule("tree-sitter-go"         "tree-sitter-go/src/parser.c")
require_submodule("tree-sitter-bash"       "tree-sitter-bash/src/parser.c")
require_submodule("tree-sitter-json"       "tree-sitter-json/src/parser.c")
require_submodule("tree-sitter-markdown"   "tree-sitter-markdown/tree-sitter-markdown/src/parser.c")

# ---------------------------------------------------------------------------
# Collect all C sources for the combined static library.
#
# tree-sitter core:
#   lib/src/lib.c  — the official amalgamation (includes all *.c internally)
#
# Grammar sources (per-grammar):
#   src/parser.c    — the generated parser (required for all grammars)
#   src/scanner.c   — optional handwritten external scanner (present for some)
#
# We test for scanner.c existence at configure time rather than GLOB to
# keep the dependency graph deterministic and avoid stale-cache issues.
# ---------------------------------------------------------------------------

set(TREESITTER_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/tree-sitter/lib/src/lib.c"
)

# Macro: add parser.c (required) + scanner.c (optional) for a grammar
macro(add_grammar_sources GRAMMAR_DIR)
  list(APPEND TREESITTER_SOURCES "${GRAMMAR_DIR}/src/parser.c")
  if(EXISTS "${GRAMMAR_DIR}/src/scanner.c")
    list(APPEND TREESITTER_SOURCES "${GRAMMAR_DIR}/src/scanner.c")
  endif()
endmacro()

set(_vendor "${CMAKE_CURRENT_SOURCE_DIR}")

add_grammar_sources("${_vendor}/tree-sitter-c")
add_grammar_sources("${_vendor}/tree-sitter-cpp")
add_grammar_sources("${_vendor}/tree-sitter-python")
add_grammar_sources("${_vendor}/tree-sitter-javascript")
# TypeScript stores its grammar under a nested path
add_grammar_sources("${_vendor}/tree-sitter-typescript/typescript")
add_grammar_sources("${_vendor}/tree-sitter-rust")
add_grammar_sources("${_vendor}/tree-sitter-go")
add_grammar_sources("${_vendor}/tree-sitter-bash")
add_grammar_sources("${_vendor}/tree-sitter-json")
# tree-sitter-markdown stores parsers under named sub-directories
add_grammar_sources("${_vendor}/tree-sitter-markdown/tree-sitter-markdown")

# ---------------------------------------------------------------------------
# Build the combined static library.
# ---------------------------------------------------------------------------

add_library(batbox_treesitter STATIC ${TREESITTER_SOURCES})

# tree-sitter core public API header
target_include_directories(batbox_treesitter
  PUBLIC
    # Consumers get the tree-sitter.h public header
    "${CMAKE_CURRENT_SOURCE_DIR}/tree-sitter/lib/include"
  PRIVATE
    # The lib.c amalgamation needs its own internal headers
    "${CMAKE_CURRENT_SOURCE_DIR}/tree-sitter/lib/src"
)

# tree-sitter is written in C99; enforce this regardless of the project-wide
# CXX standard so the parser sources compile cleanly under strict C++ builds.
set_target_properties(batbox_treesitter PROPERTIES
  C_STANDARD          99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS        OFF
)

# Suppress warnings from generated grammar code.  These sources are upstream
# and we do not control their warning hygiene.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  target_compile_options(batbox_treesitter PRIVATE
    -w   # silence all warnings for vendored C sources
  )
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(batbox_treesitter PRIVATE /W0)
endif()

# Position-independent code — required when batbox_treesitter is linked into
# a shared library or when ASAN is enabled (which uses PIC even for static).
set_target_properties(batbox_treesitter PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

message(STATUS "  [batbox] batbox_treesitter: tree-sitter core + 10 grammars (c, cpp, python, javascript, typescript, rust, go, bash, json, markdown)")
