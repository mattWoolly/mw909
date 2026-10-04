# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# mw909-render      JUCE-free CLI: voice / pattern renders, MIDI export and
#                   import, WAV compare, benchmark, factory listing.
# mw909-wavcompare  golden-render comparer with diagnostics (tests/golden).
# Both link the core only (the comparer not even that).

set(MW909_TOOL_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

if(CMAKE_BUILD_TYPE)
    set(_mw909_build_type "${CMAKE_BUILD_TYPE}")
else()
    set(_mw909_build_type "multi-config")
endif()

add_library(mw909_tool_wav STATIC Wav.cpp WavCompareCore.cpp)
target_include_directories(mw909_tool_wav PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(mw909_tool_wav PUBLIC cxx_std_20)
target_compile_options(mw909_tool_wav PRIVATE ${MW909_TOOL_WARNINGS})

if(TARGET mw909_core)
    add_executable(mw909-render
        main.cpp
        Common.cpp
        Render.cpp
        Bench.cpp
        PatternText.cpp)
    target_link_libraries(mw909-render PRIVATE mw909_core mw909_tool_wav)
    target_compile_options(mw909-render PRIVATE ${MW909_TOOL_WARNINGS} -ffp-contract=off)
    target_compile_definitions(mw909-render PRIVATE
        MW909_VERSION="${PROJECT_VERSION}"
        MW909_BUILD_TYPE="${_mw909_build_type}")
endif()

add_executable(mw909-wavcompare WavCompare.cpp)
target_link_libraries(mw909-wavcompare PRIVATE mw909_tool_wav)
target_compile_options(mw909-wavcompare PRIVATE ${MW909_TOOL_WARNINGS})
