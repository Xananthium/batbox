# =============================================================================
# cmake/Sanitizers.cmake
# =============================================================================
# Provides batbox_enable_sanitizers(<target>) — call this on any CMake target
# to inject the compiler/linker flags chosen by BATBOX_SANITIZERS.
#
# Usage:
#   cmake -B build -DBATBOX_SANITIZERS=address
#   cmake -B build -DBATBOX_SANITIZERS=address+undefined
#   cmake -B build -DBATBOX_SANITIZERS=thread           # Linux only
#   cmake -B build -DBATBOX_SANITIZERS=address+undefined+leak  # Linux only
#
# Accepted values for BATBOX_SANITIZERS:
#   OFF                      (default) — no instrumentation
#   address                  — AddressSanitizer (ASan)
#   undefined                — UndefinedBehaviorSanitizer (UBSan)
#   thread                   — ThreadSanitizer (TSan) — Linux only
#   address+undefined        — ASan + UBSan (most useful combination)
#   address+undefined+leak   — ASan + UBSan + LeakSanitizer (Linux only)
#
# Recommended environment variables when running an instrumented binary:
#
#   ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:check_initialization_order=1
#   UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
#   TSAN_OPTIONS=second_deadlock_stack=1:halt_on_error=1
#
# Notes:
#   - TSan is incompatible with ASan/LSan; they cannot be combined.
#   - TSan is SKIPPED on macOS (Apple Clang lacks reliable arm64 TSan runtime).
#   - LeakSanitizer requires Linux; ignored silently on macOS.
#   - Flags are applied with PRIVATE visibility (does not infect dependents).
#   - Always adds -fno-omit-frame-pointer and -g for usable stack traces.
# =============================================================================

# Guard against double-inclusion
if(DEFINED _BATBOX_SANITIZERS_CMAKE_INCLUDED)
  return()
endif()
set(_BATBOX_SANITIZERS_CMAKE_INCLUDED TRUE)

# ---------------------------------------------------------------------------
# Internal helper: validate and parse the BATBOX_SANITIZERS option value
# into boolean component variables scoped to the caller.
# ---------------------------------------------------------------------------
function(_batbox_parse_sanitizers_option out_asan out_ubsan out_tsan out_lsan)
  # Normalise: strip whitespace, lower-case
  string(STRIP "${BATBOX_SANITIZERS}" _san_raw)
  string(TOLOWER "${_san_raw}" _san)

  # Short-circuit for the common OFF / empty case
  if(_san STREQUAL "off" OR _san STREQUAL "" OR NOT BATBOX_SANITIZERS)
    set(${out_asan}  FALSE PARENT_SCOPE)
    set(${out_ubsan} FALSE PARENT_SCOPE)
    set(${out_tsan}  FALSE PARENT_SCOPE)
    set(${out_lsan}  FALSE PARENT_SCOPE)
    return()
  endif()

  # Accepted token set
  set(_valid_tokens address undefined thread leak)

  # Split on '+' separator
  string(REPLACE "+" ";" _san_tokens "${_san}")

  # Validate each token
  foreach(_tok IN LISTS _san_tokens)
    if(NOT _tok IN_LIST _valid_tokens)
      message(FATAL_ERROR
        "[Sanitizers.cmake] Unknown sanitizer token: '${_tok}'.\n"
        "Accepted tokens (combine with '+'): address undefined thread leak\n"
        "Accepted full values: OFF  address  undefined  thread  "
        "address+undefined  address+undefined+leak\n"
        "Example: -DBATBOX_SANITIZERS=address+undefined")
    endif()
  endforeach()

  # Detect components
  set(_want_asan  FALSE)
  set(_want_ubsan FALSE)
  set(_want_tsan  FALSE)
  set(_want_lsan  FALSE)

  if("address" IN_LIST _san_tokens)
    set(_want_asan TRUE)
  endif()
  if("undefined" IN_LIST _san_tokens)
    set(_want_ubsan TRUE)
  endif()
  if("thread" IN_LIST _san_tokens)
    set(_want_tsan TRUE)
  endif()
  if("leak" IN_LIST _san_tokens)
    set(_want_lsan TRUE)
  endif()

  # Reject illegal combinations: TSan cannot coexist with ASan or LSan
  if(_want_tsan AND (_want_asan OR _want_lsan))
    message(FATAL_ERROR
      "[Sanitizers.cmake] ThreadSanitizer (thread) is INCOMPATIBLE with "
      "AddressSanitizer (address) and LeakSanitizer (leak).\n"
      "Use -DBATBOX_SANITIZERS=thread by itself.")
  endif()

  set(${out_asan}  ${_want_asan}  PARENT_SCOPE)
  set(${out_ubsan} ${_want_ubsan} PARENT_SCOPE)
  set(${out_tsan}  ${_want_tsan}  PARENT_SCOPE)
  set(${out_lsan}  ${_want_lsan}  PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Public API: batbox_enable_sanitizers(<target>)
#
# Applies the sanitizer flags selected by BATBOX_SANITIZERS to <target>.
# No-op when BATBOX_SANITIZERS is OFF or unset.
# ---------------------------------------------------------------------------
function(batbox_enable_sanitizers target)
  _batbox_parse_sanitizers_option(_asan _ubsan _tsan _lsan)

  # Nothing to do when all components are disabled
  if(NOT _asan AND NOT _ubsan AND NOT _tsan AND NOT _lsan)
    return()
  endif()

  # ------------------------------------------------------------------
  # Platform guards
  # ------------------------------------------------------------------

  # TSan: not reliably shipped by Apple Clang for arm64 macOS
  if(APPLE AND _tsan)
    message(WARNING
      "[Sanitizers.cmake] ThreadSanitizer is not reliably supported on "
      "macOS/arm64 with Apple Clang. Skipping TSan for target '${target}'.")
    return()
  endif()

  # LSan: standalone leak sanitizer not available on macOS
  if(APPLE AND _lsan)
    message(STATUS
      "[Sanitizers.cmake] LeakSanitizer is not available on macOS. "
      "Ignoring 'leak' token for target '${target}'.")
    set(_lsan FALSE)
  endif()

  # ------------------------------------------------------------------
  # Build the -fsanitize= flag value
  # ------------------------------------------------------------------
  set(_san_components "")
  if(_asan)
    list(APPEND _san_components "address")
  endif()
  if(_ubsan)
    list(APPEND _san_components "undefined")
  endif()
  if(_tsan)
    list(APPEND _san_components "thread")
  endif()
  if(_lsan)
    list(APPEND _san_components "leak")
  endif()

  list(JOIN _san_components "," _san_joined)
  set(_fsanitize "-fsanitize=${_san_joined}")

  # ------------------------------------------------------------------
  # Common diagnostic flags (always added alongside any sanitizer)
  # ------------------------------------------------------------------
  set(_diag_flags
    -fno-omit-frame-pointer   # Required for readable stack traces
    -g                        # Debug info — essential for symbolisation
  )

  # UBSan: request verbose diagnostics and abort-on-error by default
  set(_ubsan_extra "")
  if(_ubsan)
    list(APPEND _ubsan_extra -fno-sanitize-recover=undefined)
  endif()

  # ------------------------------------------------------------------
  # Apply to target with PRIVATE visibility
  # PRIVATE: flags do not propagate to consumers of this target
  # ------------------------------------------------------------------
  target_compile_options(${target} PRIVATE
    ${_fsanitize}
    ${_diag_flags}
    ${_ubsan_extra}
  )

  target_link_options(${target} PRIVATE
    ${_fsanitize}
  )

  # ------------------------------------------------------------------
  # Informational summary
  # ------------------------------------------------------------------
  message(STATUS
    "[Sanitizers.cmake] '${target}': enabled sanitizers -> ${_san_joined}")
endfunction()

# =============================================================================
# Wire-up block (intentional contract handoff — NOT a stub)
# =============================================================================
# TODO(CPP A.X wire-up): Once src/CMakeLists.txt declares the 'batbox' target,
# add the following call in the top-level CMakeLists.txt AFTER the
# add_subdirectory chain:
#
#   batbox_enable_sanitizers(batbox)
#
# This is the documented handoff point to the App-wiring task (CPP A.X).
# The function batbox_enable_sanitizers() is fully implemented and ready;
# it simply has no target to attach to until CPP A.X lands.
# =============================================================================
