# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# One Catch2 executable per tests/unit/<prefix>_<name>.cpp. The prefix picks the
# library under test: dsp_ -> mw909_dsp (+ mw909_rom), fx_ -> mw909_fx,
# seq_ -> mw909_seq, core_ -> mw909_core.

file(GLOB MW909_UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")

foreach(test_src IN LISTS MW909_UNIT_TESTS)
    get_filename_component(test_name "${test_src}" NAME_WE)
    string(REGEX MATCH "^[a-z]+" test_prefix "${test_name}")
    if(test_prefix STREQUAL "dsp")
        set(test_lib mw909_dsp)
    elseif(test_prefix STREQUAL "fx")
        if(TARGET mw909_fx)
            set(test_lib mw909_fx)
        else()
            set(test_lib mw909_dsp)
        endif()
    elseif(test_prefix STREQUAL "seq")
        set(test_lib mw909_seq)
    elseif(test_prefix STREQUAL "core")
        set(test_lib mw909_core)
    else()
        message(FATAL_ERROR "tests/unit/${test_name}.cpp: prefix must be dsp_, fx_, seq_ or core_")
    endif()
    if(NOT TARGET ${test_lib})
        continue()
    endif()

    set(test_target "mw909_test_${test_name}")
    add_executable(${test_target} "${test_src}")
    target_link_libraries(${test_target} PRIVATE ${test_lib} Catch2::Catch2WithMain)
    target_include_directories(${test_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
    target_compile_definitions(${test_target} PRIVATE
        MW909_REF_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/.deps/ref-samples"
        MW909_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    catch_discover_tests(${test_target} TEST_PREFIX "${test_name}: ")
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/golden/CMakeLists.txt")
    add_subdirectory(golden)
endif()

if(MW909_BUILD_PLUGIN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugin/CMakeLists.txt")
    add_subdirectory(plugin)
    if(TARGET mw909 AND MW909_BUILD_EDITOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/CMakeLists.txt")
        add_subdirectory(ui)
    endif()
endif()
