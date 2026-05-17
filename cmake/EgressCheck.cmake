# =============================================================================
# cmake/EgressCheck.cmake — BatBox static-egress post-build scan
#
# PURPOSE:
#   Provides batbox_add_egress_check(<target>) which attaches a POST_BUILD
#   custom command that runs cmake/egress_scan.sh against the built binary.
#   Also registers a CTest test so `ctest` re-runs the scan independently.
#
# RATIONALE:
#   BatBox must never compile in telemetry, analytics, or update-distribution
#   hostnames (pmdraft.md §Privacy — "Static-Egress Test"). This cmake module
#   enforces that policy at build time and at test time so CI fails fast.
#
# USAGE:
#   include(cmake/EgressCheck.cmake)
#   batbox_add_egress_check(batbox)          # call after the target is defined
#
#   The function is a no-op when BATBOX_TESTS=OFF so release-only configurations
#   that skip the test suite still get the POST_BUILD check. Wait — actually
#   the POST_BUILD strip+scan always runs so that every build is gated; only
#   the `ctest` registration is gated by BATBOX_TESTS.
#
# DEPENDENCIES:
#   cmake/egress_scan.sh  — must exist in CMAKE_SOURCE_DIR/cmake/
#   strip                 — used to remove debug symbols before scan (so we
#                           scan only compiled-in strings, not DWARF comments)
#
# CROSS-PLATFORM:
#   - macOS: strip is /usr/bin/strip; egress_scan.sh uses /usr/bin/strings
#   - Linux: strip is from GNU binutils; egress_scan.sh prefers llvm-strings
#            or falls back to GNU strings
# =============================================================================

cmake_minimum_required(VERSION 3.24)

# Guard against double inclusion
if(DEFINED _BATBOX_EGRESS_CHECK_INCLUDED)
    return()
endif()
set(_BATBOX_EGRESS_CHECK_INCLUDED TRUE)

# Resolve the scan script path once (absolute, relative to source root)
set(_EGRESS_SCAN_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/egress_scan.sh")

# Verify the script exists at configure time so the error is clear
if(NOT EXISTS "${_EGRESS_SCAN_SCRIPT}")
    message(FATAL_ERROR
        "[EgressCheck] cmake/egress_scan.sh not found at: ${_EGRESS_SCAN_SCRIPT}\n"
        "This file must exist before EgressCheck.cmake is included."
    )
endif()

# =============================================================================
# batbox_add_egress_check(<target>)
#
# Attaches a POST_BUILD command that:
#   1. Strips the binary into a temporary file to remove debug symbol sections
#      (so DWARF comments mentioning forbidden hosts don't cause false positives).
#   2. Runs egress_scan.sh against the stripped copy.
#   3. Removes the stripped copy.
#
# Also registers a CTest test named "egress_static_<target>" when BATBOX_TESTS=ON.
# =============================================================================
function(batbox_add_egress_check target)
    # Validate argument
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "[EgressCheck] batbox_add_egress_check: '${target}' is not a CMake target. "
            "Call this function after add_executable() or add_library()."
        )
    endif()

    # Path where the stripped binary will be written temporarily
    set(_STRIPPED_BINARY "${CMAKE_BINARY_DIR}/${target}.egress-stripped")

    # -------------------------------------------------------------------------
    # POST_BUILD: strip → scan → clean up stripped copy
    #
    # We use two separate COMMAND entries so CMake echoes them separately and
    # failure attribution is clear.
    #
    # The strip command uses `|| true` fallback because:
    #   - On macOS, `strip` returns non-zero for some binary types.
    #   - If strip fails we still run the scan (unstripped is conservative;
    #     false positives from DWARF are possible but won't cause false negatives).
    # -------------------------------------------------------------------------
    add_custom_command(TARGET ${target} POST_BUILD
        COMMENT "[egress-check] Stripping and scanning ${target} for forbidden hostnames..."

        # Step 1: strip debug symbols into a temporary file
        COMMAND
            ${CMAKE_COMMAND} -E cmake_echo_color --blue
            "[egress-check] stripping $<TARGET_FILE:${target}> -> ${_STRIPPED_BINARY}"
        COMMAND
            ${CMAKE_COMMAND} -E env
            strip "$<TARGET_FILE:${target}>" -o "${_STRIPPED_BINARY}"
            || ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${target}>" "${_STRIPPED_BINARY}"

        # Step 2: run the egress scan on the stripped binary
        COMMAND
            "${_EGRESS_SCAN_SCRIPT}" "${_STRIPPED_BINARY}"

        # Step 3: remove the stripped copy (cleanup; non-fatal if it fails)
        COMMAND
            ${CMAKE_COMMAND} -E remove -f "${_STRIPPED_BINARY}"

        VERBATIM
    )

    # -------------------------------------------------------------------------
    # CTest registration (only when BATBOX_TESTS=ON)
    #
    # This allows `ctest -R egress` to re-run the scan independently of a full
    # build. It scans the unstripped binary directly because ctest runs
    # post-configure, not post-build. The scan is conservative (unstripped =
    # more strings visible = stricter check).
    # -------------------------------------------------------------------------
    if(BATBOX_TESTS)
        add_test(
            NAME    "egress_static_${target}"
            COMMAND "${_EGRESS_SCAN_SCRIPT}" "$<TARGET_FILE:${target}>"
        )
        set_tests_properties("egress_static_${target}" PROPERTIES
            LABELS              "egress;security;static-analysis"
            TIMEOUT             30
            FAIL_REGULAR_EXPRESSION "FAIL"
        )
    endif()

    message(STATUS
        "[EgressCheck] POST_BUILD egress scan registered for target '${target}'"
    )
endfunction()
