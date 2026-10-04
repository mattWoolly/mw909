# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# Golden-render regression suite. cases.json is the single source of truth:
# each case is an mw909-render command line. This file registers one CTest per
# case (name "golden_<id>", LABEL golden) that renders into the build tree and
# compares against blessed/<id>.wav with mw909-wavcompare at tolerance 0
# unless the case sets "tol" (a case with no blessed file yet renders, then
# reports SKIPPED). It also writes the
# case list the bless script reads and defines the `bless_goldens` target.
#
# Re-blessing (after an intended sound change) is one command:
#   BLESS_REASON="why" tools/bless_goldens.sh <build-dir>
# or: BLESS_REASON="why" cmake --build <build-dir> --target bless_goldens

if(NOT TARGET mw909-render OR NOT TARGET mw909-wavcompare)
    message(STATUS "golden renders: mw909-render not built (MW909_BUILD_TOOLS=OFF?), suite skipped")
    return()
endif()

set(GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(GOLDEN_CASES_JSON "${GOLDEN_DIR}/cases.json")
set(GOLDEN_BLESSED_DIR "${GOLDEN_DIR}/blessed")
set(GOLDEN_SCRATCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/renders")
set(GOLDEN_CASE_LIST "${CMAKE_CURRENT_BINARY_DIR}/golden_cases.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GOLDEN_CASES_JSON}")

file(READ "${GOLDEN_CASES_JSON}" _json)
string(JSON _count LENGTH "${_json}" cases)
if(_count EQUAL 0)
    message(FATAL_ERROR "tests/golden/cases.json has no cases")
endif()
math(EXPR _last "${_count} - 1")

set(_tab "\t")
set(_tsv "")
set(_ids "")
foreach(i RANGE 0 ${_last})
    string(JSON _id GET "${_json}" cases ${i} id)
    if(NOT _id MATCHES "^[a-z0-9_]+$")
        message(FATAL_ERROR "cases.json: case id '${_id}' must be [a-z0-9_]+")
    endif()
    if(_id IN_LIST _ids)
        message(FATAL_ERROR "cases.json: duplicate case id '${_id}'")
    endif()
    list(APPEND _ids "${_id}")

    string(JSON _ref ERROR_VARIABLE _err GET "${_json}" cases ${i} reference)
    if(_err OR _ref STREQUAL "")
        set(_ref "-")
    endif()
    string(JSON _tol ERROR_VARIABLE _err GET "${_json}" cases ${i} tol)
    if(_err OR _tol STREQUAL "")
        set(_tol "-")
    endif()

    string(JSON _nargs LENGTH "${_json}" cases ${i} args)
    math(EXPR _alast "${_nargs} - 1")
    set(_args_bar "")
    set(_args_tab "")
    foreach(j RANGE 0 ${_alast})
        string(JSON _a GET "${_json}" cases ${i} args ${j})
        string(REPLACE "@GOLDEN_DIR@" "${GOLDEN_DIR}" _a "${_a}")
        if(_a MATCHES "[|\t;]")
            message(FATAL_ERROR "cases.json: case '${_id}': arguments must not contain '|', ';' or tabs")
        endif()
        if(j EQUAL 0)
            set(_args_bar "${_a}")
            set(_args_tab "${_a}")
        else()
            string(APPEND _args_bar "|${_a}")
            string(APPEND _args_tab "${_tab}${_a}")
        endif()
    endforeach()

    string(APPEND _tsv "${_id}${_tab}${_ref}${_tab}${_tol}${_tab}${_args_tab}\n")

    # The emulator (the TSan presets' `setarch -R`) wraps the render as well.
    add_test(NAME "golden_${_id}"
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${CMAKE_COMMAND}"
            "-DRENDER_BIN=$<TARGET_FILE:mw909-render>"
            "-DCOMPARE_BIN=$<TARGET_FILE:mw909-wavcompare>"
            "-DCASE_ID=${_id}"
            "-DREFERENCE=${_ref}"
            "-DTOL=${_tol}"
            "-DARGS=${_args_bar}"
            "-DBLESSED_DIR=${GOLDEN_BLESSED_DIR}"
            "-DSCRATCH_DIR=${GOLDEN_SCRATCH_DIR}"
            -P "${GOLDEN_DIR}/RunGoldenCase.cmake")
    set_tests_properties("golden_${_id}" PROPERTIES
        LABELS golden
        TIMEOUT 300
        SKIP_REGULAR_EXPRESSION "MW909_GOLDEN_SKIPPED"
        SKIP_RETURN_CODE 77)
endforeach()

# Every `reference` must name a case that has its own blessed file. A typo
# would otherwise make the case look for a file that is never blessed, and it
# would report SKIPPED forever while the suite still says 100 % passed.
foreach(i RANGE 0 ${_last})
    string(JSON _id GET "${_json}" cases ${i} id)
    string(JSON _ref ERROR_VARIABLE _err GET "${_json}" cases ${i} reference)
    if(_err OR _ref STREQUAL "" OR _ref STREQUAL "-")
        continue()
    endif()
    if(NOT _ref IN_LIST _ids)
        message(FATAL_ERROR "cases.json: case '${_id}': reference '${_ref}' is not a case id")
    endif()
    if(_ref STREQUAL _id)
        message(FATAL_ERROR "cases.json: case '${_id}' references itself")
    endif()
    list(FIND _ids "${_ref}" _ri)
    string(JSON _rr ERROR_VARIABLE _err GET "${_json}" cases ${_ri} reference)
    if(NOT _err AND NOT _rr STREQUAL "" AND NOT _rr STREQUAL "-")
        message(FATAL_ERROR "cases.json: case '${_id}': reference '${_ref}' is itself a reference case "
            "(it has no blessed file of its own); point at '${_rr}' instead")
    endif()
endforeach()

# Case list for tools/bless_goldens.sh: id, reference, tol, args (tab-separated).
file(WRITE "${GOLDEN_CASE_LIST}" "${_tsv}")
message(STATUS "golden renders: ${_count} cases registered (label golden)")

# The target has already built the tools, so the script must not re-enter the build.
add_custom_target(bless_goldens
    COMMAND "${CMAKE_COMMAND}" -E env BLESS_NO_BUILD=1
            "${CMAKE_SOURCE_DIR}/tools/bless_goldens.sh" "${CMAKE_BINARY_DIR}"
    DEPENDS mw909-render mw909-wavcompare
    USES_TERMINAL
    VERBATIM)
