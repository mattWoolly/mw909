# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# Script-driven CTests, included from the top-level CMakeLists.txt when tests
# are enabled:
#   license_headers          tools/check_license_headers.sh          (LABEL hygiene)
#   branding_audit           scripts/branding-audit.sh --binaries     (LABEL hygiene)
#                            text files, plus strings of everything built in this
#                            tree (plugin artefacts when the plugin is built, the
#                            tools, the test programs), the registered test names
#                            and the factory content names
#   tools: behaviour         tools/mw909-render/tool_checks.sh        (LABEL tools)
#   validators: ...          tools/validators/run_*.sh                (LABEL validators)
# Validators exit 77 when their tool or the plugin artefact is missing, which
# CTest reports as skipped (SKIP_RETURN_CODE 77). They receive this build
# directory through MW909_BUILD_DIR and as their first argument, validate a
# private copy of each artefact, and never leave a process behind.

find_program(MW909_BASH bash)
if(NOT MW909_BASH)
    message(STATUS "bash not found: licence, branding, tool and validator checks not registered")
    return()
endif()

set(_mw909_root "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME license_headers
    COMMAND "${MW909_BASH}" "${_mw909_root}/tools/check_license_headers.sh" "${_mw909_root}")
set_tests_properties(license_headers PROPERTIES LABELS hygiene TIMEOUT 120)

# Script tests start project binaries themselves, so they run under the
# configured emulator too (the TSan presets set it to `setarch -R`: TSan
# cannot map its shadow memory under this kernel's ASLR).
set(_mw909_emu ${CMAKE_CROSSCOMPILING_EMULATOR})

add_test(NAME branding_audit
    COMMAND ${_mw909_emu} "${MW909_BASH}" "${_mw909_root}/scripts/branding-audit.sh" --binaries "${CMAKE_BINARY_DIR}" "${_mw909_root}")
set_tests_properties(branding_audit PROPERTIES LABELS hygiene TIMEOUT 300)

if(TARGET mw909-render AND TARGET mw909-wavcompare)
    add_test(NAME "tools: behaviour"
        COMMAND ${_mw909_emu} "${MW909_BASH}" "${_mw909_root}/tools/mw909-render/tool_checks.sh"
            "$<TARGET_FILE:mw909-render>" "$<TARGET_FILE:mw909-wavcompare>"
            "${CMAKE_BINARY_DIR}/tool-checks" "${_mw909_root}/tests/golden")
    set_tests_properties("tools: behaviour" PROPERTIES LABELS tools TIMEOUT 600)
endif()

foreach(_v IN ITEMS
        "pluginval VST3|run_pluginval.sh|1800"
        "clap-validator CLAP|run_clap_validator.sh|1800"
        "LV2 bundle checks|run_lv2_checks.sh|300")
    string(REPLACE "|" ";" _v "${_v}")
    list(GET _v 0 _name)
    list(GET _v 1 _script)
    list(GET _v 2 _timeout)
    add_test(NAME "validators: ${_name}"
        COMMAND "${MW909_BASH}" "${_mw909_root}/tools/validators/${_script}" "${CMAKE_BINARY_DIR}")
    set_tests_properties("validators: ${_name}" PROPERTIES
        LABELS validators
        SKIP_RETURN_CODE 77
        TIMEOUT ${_timeout}
        RUN_SERIAL TRUE
        ENVIRONMENT "MW909_BUILD_DIR=${CMAKE_BINARY_DIR}")
endforeach()
