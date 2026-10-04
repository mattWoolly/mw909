# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# mw909_plugin_tests: a JUCE-linked Catch2 console executable that compiles the
# plugin shell sources directly (no plugin-format wrapper) and drives the
# processor the way a host would. Registered with CTest test by test.

juce_add_console_app(mw909_plugin_tests
    PRODUCT_NAME "mw909_plugin_tests"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

file(GLOB MW909_PLUGIN_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB MW909_SHELL_SOURCES_FOR_TESTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/plugin/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/plugin/src/state/*.cpp")
# The editor sources too when the real editor is built (MW909_BUILD_EDITOR,
# see plugin/CMakeLists.txt), so the test binary links whichever editor the
# processor creates.
set(MW909_UI_SOURCES_FOR_TESTS "")
if(MW909_BUILD_EDITOR)
    file(GLOB_RECURSE MW909_UI_SOURCES_FOR_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/plugin/ui/*.cpp")
endif()

target_sources(mw909_plugin_tests PRIVATE
    ${MW909_PLUGIN_TEST_SOURCES}
    ${MW909_SHELL_SOURCES_FOR_TESTS}
    ${MW909_UI_SOURCES_FOR_TESTS})

target_include_directories(mw909_plugin_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/plugin/src"
    "${CMAKE_SOURCE_DIR}/plugin")
if(MW909_BUILD_EDITOR)
    target_include_directories(mw909_plugin_tests PRIVATE "${CMAKE_SOURCE_DIR}/plugin/ui")
endif()

# Same treatment of the core headers as the plugin target (see plugin/CMakeLists.txt).
target_include_directories(mw909_plugin_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/libs/mw909-core/include")

target_compile_definitions(mw909_plugin_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # One test pumps the message loop to see the real 30 Hz timer do its work.
    JUCE_MODAL_LOOPS_PERMITTED=1
    MW909_PRODUCT_NAME="mw909"
    MW909_VERSION_STRING="${PROJECT_VERSION}"
    MW909_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch"
    MW909_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    MW909_USE_PLACEHOLDER_EDITOR=$<IF:$<BOOL:${MW909_BUILD_EDITOR}>,0,1>)

target_link_libraries(mw909_plugin_tests PRIVATE
    mw909_core
    juce::juce_audio_utils
    Catch2::Catch2WithMain
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The tests compare floats exactly on purpose (restored values, exact silence);
# the shell sources compiled alongside keep the full warning set.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${MW909_PLUGIN_TEST_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-float-equal")
endif()

# The allocation spy (AllocSpy.cpp) replaces operator new/delete and, because
# JUCE's HeapBlock calls malloc directly, also wraps the C allocator at link time.
if(UNIX AND NOT APPLE)
    target_link_options(mw909_plugin_tests PRIVATE
        "LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc,--wrap=memalign")
    target_compile_definitions(mw909_plugin_tests PRIVATE MW909_TEST_WRAPS_MALLOC=1)
endif()

# Every test points the preset manager at a scratch directory: nothing here
# may create or touch ~/.config/mwAudio/mw909 (research/06 L23).
set(MW909_TEST_PRESET_DIR "${CMAKE_CURRENT_BINARY_DIR}/scratch/test-presets")
catch_discover_tests(mw909_plugin_tests TEST_PREFIX "plugin: "
    PROPERTIES ENVIRONMENT "MW909_PRESET_DIR=${MW909_TEST_PRESET_DIR}")

# The cross-thread stress test at full length (label `stress`; the suite above
# runs it for 2 s). Meant for the TSan and ASan lanes as much as for Release.
add_test(NAME "stress: 30 s of audio, host and message threads at once"
    COMMAND mw909_plugin_tests "[stress]")
set_tests_properties("stress: 30 s of audio, host and message threads at once" PROPERTIES
    LABELS stress
    TIMEOUT 900
    ENVIRONMENT "MW909_STRESS_SECONDS=30;MW909_PRESET_DIR=${MW909_TEST_PRESET_DIR}")

# ---------------------------------------------------------------------------
# Host smoke (label `host-smoke`): the Standalone starts, runs for a few
# seconds and quits by itself with exit code 0. The script exits 77 when the
# Standalone is not built or there is no display and no xvfb-run, which CTest
# reports as skipped. (The format validators live in tools/validators.)
# The script starts the Standalone itself, so it runs under the configured
# emulator (the TSan presets' `setarch -R`), like the other script tests.
if(UNIX AND NOT APPLE)
    add_test(NAME "host-smoke: Standalone starts and quits cleanly"
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${CMAKE_CURRENT_SOURCE_DIR}/run_standalone_smoke.sh")
    set_tests_properties("host-smoke: Standalone starts and quits cleanly" PROPERTIES
        LABELS host-smoke
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        ENVIRONMENT "MW909_BUILD_DIR=${CMAKE_BINARY_DIR};MW909_PRESET_DIR=${MW909_TEST_PRESET_DIR}")
endif()
