# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# Lets a build directory pick up tests/ui without editing tests/CMakeLists.txt:
#   cmake -S . -B <dir> -DCMAKE_PROJECT_mw909_INCLUDE=tests/ui/DeferredInclude.cmake
# The include is deferred to the end of the top-level directory so the
# plugin and JUCE targets exist by then. Once the integrator adds
# `add_subdirectory(ui)` to tests/CMakeLists.txt this file is unnecessary.
if(NOT TARGET mw909_ui_tool)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _mw909_add_ui_tests)
endif()

function(_mw909_add_ui_tests)
    if(NOT TARGET mw909_ui_tool AND TARGET mw909)
        include("${CMAKE_SOURCE_DIR}/tests/ui/CMakeLists.txt")
    endif()
endfunction()
