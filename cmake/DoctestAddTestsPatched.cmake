# Distributed under the OSI-approved BSD 3-Clause License.
# See https://cmake.org/licensing for details.
#
# BatBox local patch — fixes CTest discovery for test names that contain "["
# "]", or ";" (e.g. CSI-u escape sequences like \e[27;2;13~).
#
# Root cause (two layers):
#
#   1. LINE-SPLIT BUG: The upstream script splits the --list-test-cases output
#      into lines by doing string(REPLACE "\n" ";" output), then iterates with
#      foreach(line ${output}).  CMake's list parser treats unmatched "[" as
#      opening a bracket-group that absorbs subsequent elements until it finds
#      a matching "]", merging multiple test names into one malformed entry.
#
#   2. ADD_COMMAND BUG: The add_command() helper uses foreach(_arg ${ARGN}),
#      which also performs unquoted list expansion.  Even if the line-split
#      were fixed, passing a test name that contains "[" through ARGN would
#      merge it with the subsequent arguments (executable path, --test-case=
#      flag, etc.) into a single malformed script token.
#
# Fix:
#   - Line-split: replace "[", "]", and ";" with unique ASCII placeholders
#     BEFORE the newline→";" conversion, then restore them inside the foreach
#     body so that set(test ...) holds the real test name.
#   - add_test / set_tests_properties: bypass add_command() entirely and
#     append directly to the `script` variable using bracket-quoted
#     [==[...]==] literals.  Plain string operations on `script` see "[" as
#     ordinary characters, so no list-parsing occurs.
#
# Durability: this file lives in cmake/ and is copied over the vcpkg-installed
# doctestAddTests.cmake at configure time by cmake/DoctestDiscoverPatched.cmake.
# A vcpkg reinstall will overwrite the installed copy; the next cmake configure
# re-applies the patch before any build step runs.

set(prefix "${TEST_PREFIX}")
set(suffix "${TEST_SUFFIX}")
set(spec ${TEST_SPEC})
set(extra_args ${TEST_EXTRA_ARGS})
set(properties ${TEST_PROPERTIES})
set(add_labels ${TEST_ADD_LABELS})
set(junit_output_dir "${TEST_JUNIT_OUTPUT_DIR}")
set(script)
set(suite)
set(tests)

function(add_command NAME)
  set(_args "")
  foreach(_arg ${ARGN})
    if(_arg MATCHES "[^-./:a-zA-Z0-9_]")
      set(_args "${_args} [==[${_arg}]==]") # form a bracket_argument
    else()
      set(_args "${_args} ${_arg}")
    endif()
  endforeach()
  set(script "${script}${NAME}(${_args})\n" PARENT_SCOPE)
endfunction()

# Run test executable to get list of available tests
if(NOT EXISTS "${TEST_EXECUTABLE}")
  message(FATAL_ERROR
    "Specified test executable '${TEST_EXECUTABLE}' does not exist"
  )
endif()

if("${spec}" MATCHES .)
  set(spec "--test-case=${spec}")
endif()

execute_process(
  COMMAND ${TEST_EXECUTOR} "${TEST_EXECUTABLE}" ${spec} --list-test-cases
  OUTPUT_VARIABLE output
  RESULT_VARIABLE result
  WORKING_DIRECTORY "${TEST_WORKING_DIR}"
)
if(NOT ${result} EQUAL 0)
  message(FATAL_ERROR
    "Error running test executable '${TEST_EXECUTABLE}':\n"
    "  Result: ${result}\n"
    "  Output: ${output}\n"
  )
endif()

# PATCH (layer 1 — line-split bug):
# Protect "[", "]", and ";" with ASCII placeholders before converting newlines
# to ";" for foreach iteration.  Without this, unmatched "[" in test names
# (such as the "\e[27" in CSI-u escape sequences) are treated as bracket-group
# openers by CMake's list parser, absorbing all subsequent elements until a
# matching "]" is found and merging multiple test names into one malformed entry.
set(_SEMI_PH  "XBATBOX_SEMI_PHX")
set(_LB_PH    "XBATBOX_LBRACK_PHX")
set(_RB_PH    "XBATBOX_RBRACK_PHX")
string(REPLACE "[" "${_LB_PH}"   output "${output}")
string(REPLACE "]" "${_RB_PH}"   output "${output}")
string(REPLACE ";" "${_SEMI_PH}" output "${output}")
string(REPLACE "\n" ";" output "${output}")

# Parse output
foreach(line ${output})
  # Restore the original characters; set(test ...) now holds the verbatim name.
  string(REPLACE "${_LB_PH}"   "[" line "${line}")
  string(REPLACE "${_RB_PH}"   "]" line "${line}")
  string(REPLACE "${_SEMI_PH}" ";" line "${line}")

  if("${line}" STREQUAL "" OR
     "${line}" STREQUAL "===============================================================================" OR
     "${line}" MATCHES [==[^\[doctest\] ]==])
    continue()
  endif()
  set(test "${line}")
  set(labels "")
  if(${add_labels})
    # get test suite that test belongs to
    execute_process(
      COMMAND ${TEST_EXECUTOR} "${TEST_EXECUTABLE}" --test-case=${test} --list-test-suites
      OUTPUT_VARIABLE labeloutput
      RESULT_VARIABLE labelresult
      WORKING_DIRECTORY "${TEST_WORKING_DIR}"
    )
    if(NOT ${labelresult} EQUAL 0)
      message(FATAL_ERROR
        "Error running test executable '${TEST_EXECUTABLE}':\n"
        "  Result: ${labelresult}\n"
        "  Output: ${labeloutput}\n"
      )
    endif()

    string(REPLACE "\n" ";" labeloutput "${labeloutput}")
    foreach(labelline ${labeloutput})
      if("${labelline}" STREQUAL "===============================================================================" OR "${labelline}" MATCHES [==[^\[doctest\] ]==])
        continue()
      endif()
      list(APPEND labels ${labelline})
    endforeach()
  endif()

  if(NOT "${junit_output_dir}" STREQUAL "")
    # turn testname into a valid filename by replacing all special characters with "-"
    string(REGEX REPLACE "[/\\:\"|<>\\[\\];]" "-" test_filename "${test}")
    set(TEST_JUNIT_OUTPUT_PARAM "--reporters=junit" "--out=${junit_output_dir}/${prefix}${test_filename}${suffix}.xml")
  else()
    unset(TEST_JUNIT_OUTPUT_PARAM)
  endif()

  # escape commas to handle test cases with commas inside the name (upstream fix)
  string(REPLACE "," "\\," test_name "${test}")

  # PATCH (layer 2 — add_command bug):
  # Bypass add_command() for add_test and set_tests_properties.  add_command()
  # passes arguments through ARGN (a CMake list), so a test name containing "["
  # would merge with subsequent ARGN elements before the bracket quoting runs.
  # Instead, directly append bracket-quoted [==[...]==] tokens to `script`.
  # Plain string(APPEND) treats "[" as an ordinary character — no list-parsing.
  set(_full_test_name "${prefix}${test}${suffix}")

  # Build add_test() line
  string(APPEND script "add_test(")
  string(APPEND script " [==[${_full_test_name}]==]")
  if(NOT "${TEST_EXECUTOR}" STREQUAL "")
    string(APPEND script " [==[${TEST_EXECUTOR}]==]")
  endif()
  string(APPEND script " [==[${TEST_EXECUTABLE}]==]")
  string(APPEND script " [==[--test-case=${test_name}]==]")
  if(TEST_JUNIT_OUTPUT_PARAM)
    foreach(_jp ${TEST_JUNIT_OUTPUT_PARAM})
      string(APPEND script " [==[${_jp}]==]")
    endforeach()
  endif()
  if(extra_args)
    foreach(_ea ${extra_args})
      string(APPEND script " [==[${_ea}]==]")
    endforeach()
  endif()
  string(APPEND script ")\n")

  # Build set_tests_properties() line
  string(APPEND script "set_tests_properties(")
  string(APPEND script " [==[${_full_test_name}]==]")
  string(APPEND script " PROPERTIES")
  string(APPEND script " WORKING_DIRECTORY [==[${TEST_WORKING_DIR}]==]")
  if(properties)
    foreach(_p ${properties})
      string(APPEND script " [==[${_p}]==]")
    endforeach()
  endif()
  string(APPEND script " LABELS")
  if(labels)
    foreach(_l ${labels})
      string(APPEND script " [==[${_l}]==]")
    endforeach()
  endif()
  string(APPEND script ")\n")

  unset(labels)
  list(APPEND tests "${_full_test_name}")
endforeach()

# Create a list of all discovered tests, which users may use to e.g. set
# properties on the tests
add_command(set ${TEST_LIST} ${tests})

# Write CTest script
file(WRITE "${CTEST_FILE}" "${script}")
