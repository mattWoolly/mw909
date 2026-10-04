# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# mw909-rombuild: builds the factory 6-bit ROM images from the CC0 source
# recordings in assets/rom/sources/ and writes libs/mw909-core/src/rom/RomData.cpp.
# Not part of the normal build output; run it by hand when a source changes:
#   cmake --build <build-dir> --target mw909-rombuild
#   <build-dir>/tools/rombuild/mw909-rombuild assets/rom/sources libs/mw909-core/src/rom/RomData.cpp
# Also configurable on its own: cmake -S tools/rombuild -B <dir> && cmake --build <dir>
# (the core is added EXCLUDE_FROM_ALL there, so only the ROM library and the
# tool are built, whatever state the rest of the core is in).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(mw909_rombuild LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../libs/mw909-core" mw909-core EXCLUDE_FROM_ALL)
endif()

add_executable(mw909-rombuild main.cpp)
target_link_libraries(mw909-rombuild PRIVATE mw909_rom)
target_compile_options(mw909-rombuild PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                                              -ffp-contract=off)
