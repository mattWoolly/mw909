# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# Third-party dependencies, all pinned. Each one resolves to a local checkout
# under <repo>/.deps/<name> when present (offline / shared between build dirs),
# and falls back to FetchContent at the same pin otherwise.

include(FetchContent)

set(MW909_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps")

function(mw909_use_local_dep name dir)
    string(TOUPPER "${name}" _upper)
    if(EXISTS "${MW909_DEPS_DIR}/${dir}/CMakeLists.txt"
       AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_${_upper})
        set(FETCHCONTENT_SOURCE_DIR_${_upper} "${MW909_DEPS_DIR}/${dir}"
            CACHE PATH "Local checkout of ${name}")
    endif()
endfunction()

# --- Catch2 (tests) ---------------------------------------------------------
macro(mw909_fetch_catch2)
    mw909_use_local_dep(Catch2 Catch2)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endmacro()

# --- JUCE (plugin shell only; the core never sees it) -----------------------
macro(mw909_fetch_juce)
    mw909_use_local_dep(JUCE JUCE)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.13
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endmacro()

# --- clap-juce-extensions (CLAP export) -------------------------------------
macro(mw909_fetch_clap_extensions)
    mw909_use_local_dep(clap-juce-extensions clap-juce-extensions)
    FetchContent_Declare(
        clap-juce-extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG        51a9359315298de632cf44e9d7524940868441e6
    )
    FetchContent_MakeAvailable(clap-juce-extensions)
endmacro()
