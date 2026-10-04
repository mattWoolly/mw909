# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# One golden case, run as `cmake -P` by CTest (see CMakeLists.txt here).
# Renders the case into SCRATCH_DIR and compares it with the blessed render.
# A case without a blessed render is reported as skipped (see below).
#
# -D RENDER_BIN   mw909-render
# -D COMPARE_BIN  mw909-wavcompare
# -D CASE_ID      case id
# -D REFERENCE    id whose blessed file to compare against, or "-" (= CASE_ID)
# -D TOL          mw909-wavcompare --tol value, or "-" for 0 (bit-exact: renders
#                 are deterministic across Release and Debug/sanitizer builds,
#                 so any slack would hide low-level regressions; a case that
#                 genuinely needs one sets "tol" in cases.json)
# -D ARGS         mw909-render arguments separated by '|'
# -D BLESSED_DIR  tests/golden/blessed
# -D SCRATCH_DIR  where candidate renders go

foreach(_v RENDER_BIN COMPARE_BIN CASE_ID REFERENCE TOL ARGS BLESSED_DIR SCRATCH_DIR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "RunGoldenCase: -D ${_v}=... is missing")
    endif()
endforeach()

set(_ref "${CASE_ID}")
if(NOT REFERENCE STREQUAL "-")
    set(_ref "${REFERENCE}")
endif()
set(_blessed "${BLESSED_DIR}/${_ref}.wav")
set(_candidate "${SCRATCH_DIR}/${CASE_ID}.wav")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

string(REPLACE "|" ";" _args "${ARGS}")
execute_process(
    COMMAND "${RENDER_BIN}" ${_args} --quiet --out "${_candidate}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "golden ${CASE_ID}: render failed (rc ${_rc})\n${_out}${_err}")
endif()

# No blessed reference yet (a fresh corpus, or a case added since the last
# blessing): the render above still has to succeed, but there is nothing to
# compare against, so the case is reported as SKIPPED, never as passed. The
# marker below is matched by the test's SKIP_REGULAR_EXPRESSION (CMake 3.28's
# `cmake -P` cannot exit with a chosen code such as 77).
if(NOT EXISTS "${_blessed}")
    message("MW909_GOLDEN_SKIPPED: golden ${CASE_ID}: no blessed render ${_blessed}\n"
        "  candidate kept at ${_candidate}\n"
        "  bless it: BLESS_REASON=\"why\" tools/bless_goldens.sh <build-dir>")
    return()
endif()

set(_tol_args --tol 0)
if(NOT TOL STREQUAL "-")
    set(_tol_args --tol "${TOL}")
endif()
execute_process(
    COMMAND "${COMPARE_BIN}" --candidate "${_candidate}" --blessed "${_blessed}" ${_tol_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
message("${_out}${_err}")
if(NOT _rc EQUAL 0)
    string(REPLACE "|" " " _args_print "${ARGS}")
    message(FATAL_ERROR "golden ${CASE_ID}: does not match ${_blessed} (rc ${_rc})\n"
        "  candidate kept at ${_candidate}\n"
        "  render it again: ${RENDER_BIN} ${_args_print} --out <file.wav>")
endif()
