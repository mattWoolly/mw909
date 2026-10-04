# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# The JUCE-free core, in three targets so each layer builds and tests alone:
#   mw909_dsp   INTERFACE  dsp primitives, the 11 voices and the fx headers
#   mw909_rom   STATIC     ROM images (factory data, romFromAudio); mw909_dsp links it
#   mw909_fx    STATIC     channel strip, FX buses, master chain (if they need .cpp files)
#   mw909_seq   STATIC     pattern model, sequencer, MIDI file I/O
#   mw909_core  STATIC     Engine + Machine + factory content (links all of the above)
# Zero dependencies beyond the C++20 standard library.

add_library(mw909_dsp INTERFACE)
target_include_directories(mw909_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(mw909_dsp INTERFACE cxx_std_20)

set(MW909_CORE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
# FP discipline: deterministic renders. No fast-math, no FMA contraction.
set(MW909_CORE_FP -ffp-contract=off)
target_compile_options(mw909_dsp INTERFACE ${MW909_CORE_FP})

file(GLOB MW909_ROM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rom/*.cpp")
if(MW909_ROM_SOURCES)
    add_library(mw909_rom STATIC ${MW909_ROM_SOURCES})
    target_include_directories(mw909_rom PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(mw909_rom PUBLIC cxx_std_20)
    target_compile_options(mw909_rom PRIVATE ${MW909_CORE_WARNINGS} ${MW909_CORE_FP})
    set_target_properties(mw909_rom PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(mw909_dsp INTERFACE mw909_rom)
endif()

file(GLOB MW909_FX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fx/*.cpp")
if(MW909_FX_SOURCES)
    add_library(mw909_fx STATIC ${MW909_FX_SOURCES})
    target_link_libraries(mw909_fx PUBLIC mw909_dsp)
    target_compile_options(mw909_fx PRIVATE ${MW909_CORE_WARNINGS} ${MW909_CORE_FP})
    set_target_properties(mw909_fx PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

file(GLOB MW909_SEQ_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/seq/*.cpp")
if(MW909_SEQ_SOURCES)
    add_library(mw909_seq STATIC ${MW909_SEQ_SOURCES})
    target_include_directories(mw909_seq PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(mw909_seq PUBLIC cxx_std_20)
    target_compile_options(mw909_seq PRIVATE ${MW909_CORE_WARNINGS} ${MW909_CORE_FP})
    # PIC: these static libs are linked into the plugin shared objects.
    set_target_properties(mw909_seq PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

file(GLOB MW909_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(MW909_CORE_SOURCES AND TARGET mw909_seq)
    add_library(mw909_core STATIC ${MW909_CORE_SOURCES})
    target_link_libraries(mw909_core PUBLIC mw909_dsp mw909_seq)
    if(TARGET mw909_fx)
        target_link_libraries(mw909_core PUBLIC mw909_fx)
    endif()
    target_compile_options(mw909_core PRIVATE ${MW909_CORE_WARNINGS} ${MW909_CORE_FP})
    set_target_properties(mw909_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
