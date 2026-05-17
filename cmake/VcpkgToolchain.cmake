# cmake/VcpkgToolchain.cmake
# ---------------------------------------------------------------------------
# vcpkg bootstrap helper for batbox.
#
# Included by the top-level CMakeLists.txt BEFORE project(), inside a guard:
#
#   if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
#     include(cmake/VcpkgToolchain.cmake)
#   endif()
#
# Resolution order (first match wins):
#   1. $ENV{VCPKG_ROOT}   — honours user/CI environment variable
#   2. <repo-root>/vcpkg/ — sibling clone already present on disk
#   3. Auto-fetch         — only if BATBOX_VCPKG_AUTO_FETCH=ON (default OFF)
#
# If vcpkg cannot be located and BATBOX_VCPKG_AUTO_FETCH is OFF, a
# FATAL_ERROR is emitted with platform-specific install instructions.
#
# The baseline SHA used for the shallow clone is read from vcpkg.json so
# the fetched toolchain always matches the pinned dependency set.
# ---------------------------------------------------------------------------

# This script must only run when CMAKE_TOOLCHAIN_FILE has not been set by
# the caller.  The top-level guard enforces this, but re-check defensively.
if(DEFINED CMAKE_TOOLCHAIN_FILE)
  return()
endif()

# ---------------------------------------------------------------------------
# Option: opt-in network access for first-time bootstrap
# ---------------------------------------------------------------------------
option(BATBOX_VCPKG_AUTO_FETCH
  "Allow cmake to clone vcpkg automatically on first configure (requires git + internet)"
  OFF)

# ---------------------------------------------------------------------------
# Helper: locate vcpkg.cmake inside a candidate root directory
# ---------------------------------------------------------------------------
function(_batbox_vcpkg_toolchain_path OUT_VAR candidate_root)
  set(_tc "${candidate_root}/scripts/buildsystems/vcpkg.cmake")
  if(EXISTS "${_tc}")
    set(${OUT_VAR} "${_tc}" PARENT_SCOPE)
  else()
    set(${OUT_VAR} "" PARENT_SCOPE)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Step 1 — $ENV{VCPKG_ROOT}
# ---------------------------------------------------------------------------
set(_batbox_vcpkg_root "")
set(_batbox_vcpkg_source "")

if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
  set(_env_root "$ENV{VCPKG_ROOT}")
  _batbox_vcpkg_toolchain_path(_tc_candidate "${_env_root}")
  if(_tc_candidate)
    set(_batbox_vcpkg_root   "${_env_root}")
    set(_batbox_vcpkg_source "VCPKG_ROOT env var (${_env_root})")
    message(STATUS "[batbox/vcpkg] Using VCPKG_ROOT env var: ${_env_root}")
  else()
    message(WARNING "[batbox/vcpkg] VCPKG_ROOT is set to '${_env_root}' but "
                    "scripts/buildsystems/vcpkg.cmake was not found there. "
                    "Falling through to sibling-directory and auto-fetch checks.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Step 2 — sibling vcpkg/ directory at repo root
# ---------------------------------------------------------------------------
if(NOT _batbox_vcpkg_root)
  # CMAKE_CURRENT_LIST_DIR is cmake/; repo root is one level up.
  get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
  set(_sibling_root "${_repo_root}/vcpkg")
  _batbox_vcpkg_toolchain_path(_tc_candidate "${_sibling_root}")
  if(_tc_candidate)
    set(_batbox_vcpkg_root   "${_sibling_root}")
    set(_batbox_vcpkg_source "sibling vcpkg/ directory (${_sibling_root})")
    message(STATUS "[batbox/vcpkg] Found sibling vcpkg/: ${_sibling_root}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Step 3 — Auto-fetch (opt-in only)
# ---------------------------------------------------------------------------
if(NOT _batbox_vcpkg_root)
  if(NOT BATBOX_VCPKG_AUTO_FETCH)
    # Neither env var nor sibling dir found; auto-fetch is disabled → fatal.
    message(FATAL_ERROR
      "\n"
      "  [batbox/vcpkg] vcpkg was not found.\n"
      "\n"
      "  Options:\n"
      "  A) Set the VCPKG_ROOT environment variable to your existing vcpkg install:\n"
      "       export VCPKG_ROOT=/path/to/vcpkg\n"
      "       cmake -B build\n"
      "\n"
      "  B) Install vcpkg via your package manager:\n"
      "       macOS (Homebrew):  brew install vcpkg\n"
      "       Debian/Ubuntu:     sudo apt install vcpkg\n"
      "     Then set VCPKG_ROOT as above, or run `vcpkg integrate install`.\n"
      "\n"
      "  C) Clone vcpkg next to this repository (repo root):\n"
      "       git clone https://github.com/microsoft/vcpkg vcpkg\n"
      "       ./vcpkg/bootstrap-vcpkg.sh    # macOS/Linux\n"
      "       cmake -B build\n"
      "\n"
      "  D) Allow cmake to clone and bootstrap vcpkg automatically:\n"
      "       cmake -B build -DBATBOX_VCPKG_AUTO_FETCH=ON\n"
      "     (requires git and internet access; clones a shallow copy into vcpkg/)\n"
    )
  endif()

  # BATBOX_VCPKG_AUTO_FETCH=ON: shallow-clone at the baseline SHA from vcpkg.json
  get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
  set(_fetch_dest "${_repo_root}/vcpkg")

  # Read builtin-baseline from vcpkg.json so the clone matches pinned deps.
  set(_vcpkg_json_path "${_repo_root}/vcpkg.json")
  set(_baseline_sha "")

  if(EXISTS "${_vcpkg_json_path}")
    file(READ "${_vcpkg_json_path}" _vcpkg_json_content)
    string(REGEX MATCH "\"builtin-baseline\"[[:space:]]*:[[:space:]]*\"([a-f0-9]+)\""
           _baseline_match "${_vcpkg_json_content}")
    if(CMAKE_MATCH_1)
      set(_baseline_sha "${CMAKE_MATCH_1}")
    endif()
  endif()

  if(_baseline_sha)
    message(STATUS "[batbox/vcpkg] Auto-fetch: baseline SHA from vcpkg.json = ${_baseline_sha}")
  else()
    message(STATUS "[batbox/vcpkg] Auto-fetch: could not read baseline SHA from vcpkg.json; "
                   "cloning HEAD of main branch instead.")
  endif()

  # Locate git
  find_program(_batbox_git_exe git REQUIRED)
  if(NOT _batbox_git_exe)
    message(FATAL_ERROR
      "[batbox/vcpkg] BATBOX_VCPKG_AUTO_FETCH=ON but git was not found in PATH. "
      "Please install git and retry.")
  endif()

  if(NOT EXISTS "${_fetch_dest}/.git")
    message(STATUS "[batbox/vcpkg] Cloning vcpkg (shallow) into ${_fetch_dest} ...")
    execute_process(
      COMMAND "${_batbox_git_exe}" clone
              --depth 1
              --no-tags
              https://github.com/microsoft/vcpkg
              "${_fetch_dest}"
      RESULT_VARIABLE _clone_result
      OUTPUT_VARIABLE _clone_output
      ERROR_VARIABLE  _clone_error
    )
    if(NOT _clone_result EQUAL 0)
      message(FATAL_ERROR
        "[batbox/vcpkg] git clone failed (exit ${_clone_result}):\n${_clone_error}")
    endif()
    message(STATUS "[batbox/vcpkg] Clone complete.")

    # If we have a baseline SHA, fetch and checkout that exact commit.
    if(_baseline_sha)
      execute_process(
        COMMAND "${_batbox_git_exe}" fetch
                --depth 1
                origin
                "${_baseline_sha}"
        WORKING_DIRECTORY "${_fetch_dest}"
        RESULT_VARIABLE   _fetch_result
        OUTPUT_VARIABLE   _fetch_output
        ERROR_VARIABLE    _fetch_error
      )
      if(_fetch_result EQUAL 0)
        execute_process(
          COMMAND "${_batbox_git_exe}" checkout
                  "${_baseline_sha}"
          WORKING_DIRECTORY "${_fetch_dest}"
          RESULT_VARIABLE   _checkout_result
          OUTPUT_VARIABLE   _checkout_output
          ERROR_VARIABLE    _checkout_error
        )
        if(_checkout_result EQUAL 0)
          message(STATUS "[batbox/vcpkg] Checked out baseline ${_baseline_sha}.")
        else()
          message(WARNING "[batbox/vcpkg] Could not checkout baseline SHA ${_baseline_sha} "
                          "(exit ${_checkout_result}). Using cloned HEAD instead.")
        endif()
      else()
        message(WARNING "[batbox/vcpkg] Could not fetch baseline SHA ${_baseline_sha} "
                        "(exit ${_fetch_result}). Using cloned HEAD instead.")
      endif()
    endif()

    # Bootstrap the vcpkg binary
    if(WIN32)
      set(_bootstrap_script "${_fetch_dest}/bootstrap-vcpkg.bat")
    else()
      set(_bootstrap_script "${_fetch_dest}/bootstrap-vcpkg.sh")
    endif()

    if(EXISTS "${_bootstrap_script}")
      message(STATUS "[batbox/vcpkg] Running bootstrap-vcpkg ...")
      execute_process(
        COMMAND "${_bootstrap_script}" -disableMetrics
        WORKING_DIRECTORY "${_fetch_dest}"
        RESULT_VARIABLE   _bootstrap_result
        OUTPUT_VARIABLE   _bootstrap_output
        ERROR_VARIABLE    _bootstrap_error
      )
      if(NOT _bootstrap_result EQUAL 0)
        message(FATAL_ERROR
          "[batbox/vcpkg] bootstrap-vcpkg failed (exit ${_bootstrap_result}):\n${_bootstrap_error}")
      endif()
      message(STATUS "[batbox/vcpkg] vcpkg bootstrap complete.")
    else()
      message(WARNING "[batbox/vcpkg] bootstrap script not found at ${_bootstrap_script}; "
                      "vcpkg binary may need to be built manually.")
    endif()
  else()
    message(STATUS "[batbox/vcpkg] Auto-fetch: vcpkg/ directory already exists, skipping clone.")
  endif()

  _batbox_vcpkg_toolchain_path(_tc_candidate "${_fetch_dest}")
  if(_tc_candidate)
    set(_batbox_vcpkg_root   "${_fetch_dest}")
    set(_batbox_vcpkg_source "auto-fetched clone (${_fetch_dest})")
    message(STATUS "[batbox/vcpkg] Auto-fetch resolved: ${_fetch_dest}")
  else()
    message(FATAL_ERROR
      "[batbox/vcpkg] Auto-fetch completed but vcpkg.cmake toolchain was not found at "
      "${_fetch_dest}/scripts/buildsystems/vcpkg.cmake. "
      "The clone or bootstrap may have failed partially.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Set CMAKE_TOOLCHAIN_FILE and VCPKG_ROOT in the cache
# ---------------------------------------------------------------------------
set(CMAKE_TOOLCHAIN_FILE
    "${_batbox_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
    CACHE FILEPATH "vcpkg toolchain (set by cmake/VcpkgToolchain.cmake)")

set(VCPKG_ROOT
    "${_batbox_vcpkg_root}"
    CACHE PATH "Root of the vcpkg installation used by batbox")

message(STATUS "[batbox/vcpkg] Toolchain source : ${_batbox_vcpkg_source}")
message(STATUS "[batbox/vcpkg] CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")
