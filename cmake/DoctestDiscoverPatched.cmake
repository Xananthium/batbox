# cmake/DoctestDiscoverPatched.cmake
#
# BatBox local patch: overwrites the vcpkg-installed doctestAddTests.cmake with
# our patched version (cmake/DoctestAddTestsPatched.cmake) at CMake configure
# time.  This ensures CTest discovery handles test names that contain "[", "]",
# or ";" (e.g. CSI-u escape sequences like \e[27;2;13~) without CMake's list
# parser merging multiple test names into one malformed add_test() call.
#
# Why overwrite-at-configure rather than function-override:
#   The upstream doctest.cmake registers an add_custom_command for each test
#   target that embeds the path to doctestAddTests.cmake at CMake generate time.
#   Those paths are baked into build/tests/CMakeFiles/*/build.make and are NOT
#   updated when we merely redefine doctest_discover_tests() in this file —
#   CMake's Makefile generator caches build.make and only re-generates it when
#   target properties change.  Overwriting the installed script directly ensures
#   ALL targets (old and new) pick up the fix without any build.make churn.
#
# Durability: this file lives in our repo under cmake/ and is included from
# tests/CMakeLists.txt (right after `include(doctest)`).  On every `cmake`
# reconfigure — including after `vcpkg install` overwrites the installed script
# — this file runs again and restores our patched version.  The fix survives
# vcpkg reinstall because the cmake reconfigure that follows reinstall will
# re-apply the patch before any build step runs.

# Locate the vcpkg-installed doctestAddTests.cmake. The installed tree lives
# under a triplet-named subdir (x64-linux, arm64-osx, x64-windows, …), so the
# triplet must NOT be hardcoded or the patch silently no-ops off-macOS and CTest
# discovery breaks on any test whose name carries a '[' ']' or ';' (CSI-u
# escapes). Derive the path from vcpkg's own variables; fall back to a glob.
set(_vcpkg_installed_root "${CMAKE_BINARY_DIR}/vcpkg_installed")
if(DEFINED VCPKG_INSTALLED_DIR)
  set(_vcpkg_installed_root "${VCPKG_INSTALLED_DIR}")
endif()

set(_upstream_script "")
if(DEFINED VCPKG_TARGET_TRIPLET)
  set(_candidate
      "${_vcpkg_installed_root}/${VCPKG_TARGET_TRIPLET}/share/doctest/doctestAddTests.cmake")
  if(EXISTS "${_candidate}")
    set(_upstream_script "${_candidate}")
  endif()
endif()
if(NOT _upstream_script)
  file(GLOB _doctest_add_tests
       "${_vcpkg_installed_root}/*/share/doctest/doctestAddTests.cmake")
  if(_doctest_add_tests)
    list(GET _doctest_add_tests 0 _upstream_script)
  endif()
endif()
if(NOT _upstream_script)
  # Nothing on disk yet — keep a sensible path so the not-found warning below
  # points somewhere real for the operator.
  set(_upstream_script
      "${_vcpkg_installed_root}/${VCPKG_TARGET_TRIPLET}/share/doctest/doctestAddTests.cmake")
endif()
set(_patched_script
    "${CMAKE_SOURCE_DIR}/cmake/DoctestAddTestsPatched.cmake")

if(EXISTS "${_upstream_script}" AND EXISTS "${_patched_script}")
  # Compare checksums so we only write when actually different (avoids
  # unnecessary rebuild triggers on every configure).
  file(MD5 "${_upstream_script}" _upstream_md5)
  file(MD5 "${_patched_script}"  _patched_md5)
  if(NOT "${_upstream_md5}" STREQUAL "${_patched_md5}")
    file(COPY_FILE "${_patched_script}" "${_upstream_script}")
    message(STATUS "BatBox: patched doctestAddTests.cmake installed (semicolon/bracket fix)")
  else()
    message(STATUS "BatBox: doctestAddTests.cmake already patched — no update needed")
  endif()
elseif(NOT EXISTS "${_upstream_script}")
  message(WARNING
    "BatBox: vcpkg_installed doctestAddTests.cmake not found at '${_upstream_script}'. "
    "Run vcpkg install, then re-run cmake.")
elseif(NOT EXISTS "${_patched_script}")
  message(FATAL_ERROR
    "BatBox patch file missing: '${_patched_script}'. "
    "This file must be present in the repository.")
endif()
