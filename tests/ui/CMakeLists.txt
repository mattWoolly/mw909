# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# mw909_ui_tool: a JUCE console app that compiles the plugin shell and editor
# sources directly and drives the real mw909Editor headlessly (software
# renderer, under xvfb-run):
#   screenshots <dir>   PNGs of PANEL / GRID / MIXER / FX at 0.6x, 1.0x, 2.0x, plus
#                       editor states (menus open, strip expanded, effects, modes)
#   stress <n>          open / close / resize the editor n times while an audio
#                       thread runs processBlock (200; 40 in sanitizer trees)
#   idle <seconds>      CPU time of an open, idle editor with the 30 Hz timer running
#   interact            scripted mouse interaction on every control kind
#   wiring              every plugin.md §7 control path, driven by mouse / menu keys,
#                       checked at the processor (params, PatternModel, MidiLearn, presets)
#   lifetime <n>        close the editor with popup menus, alert windows, a knob
#                       drag, a step paint or a MIDI drag-out in flight (audio running)
#   perf <seconds>      frames/s and dirty area per frame, stopped and playing
# Each is registered as a CTest "ui: ..." test when xvfb-run (or a display) is
# available. tests/CMakeLists.txt adds this directory when the editor is built
# (MW909_BUILD_EDITOR).

if(NOT TARGET mw909 OR NOT TARGET mw909_core)
    message(STATUS "mw909_ui_tool: plugin target not built, skipped")
    return()
endif()

juce_add_console_app(mw909_ui_tool
    PRODUCT_NAME "mw909_ui_tool"
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

file(GLOB MW909_UI_TOOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
file(GLOB MW909_SHELL_SOURCES_FOR_UI CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/plugin/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/plugin/src/state/*.cpp")
file(GLOB_RECURSE MW909_UI_SOURCES_FOR_UI CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/plugin/ui/*.cpp")

target_sources(mw909_ui_tool PRIVATE
    ${MW909_UI_TOOL_SOURCES}
    ${MW909_SHELL_SOURCES_FOR_UI}
    ${MW909_UI_SOURCES_FOR_UI})

target_include_directories(mw909_ui_tool PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_SOURCE_DIR}/plugin/src"
    "${CMAKE_SOURCE_DIR}/plugin/ui"
    "${CMAKE_SOURCE_DIR}/plugin")
target_include_directories(mw909_ui_tool SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/libs/mw909-core/include")

target_compile_definitions(mw909_ui_tool PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # The tool pumps the message loop itself (runDispatchLoopUntil).
    JUCE_MODAL_LOOPS_PERMITTED=1
    MW909_PRODUCT_NAME="mw909"
    MW909_VERSION_STRING="${PROJECT_VERSION}"
    # The real editor (mw909Processor.cpp falls back to the stand-in without it).
    MW909_USE_PLACEHOLDER_EDITOR=0)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(mw909_ui_tool PRIVATE X11::X11)
endif()

target_link_libraries(mw909_ui_tool PRIVATE
    mw909_core
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Headless runs: prefer xvfb-run; fall back to a live DISPLAY.
find_program(MW909_XVFB_RUN xvfb-run)
if(MW909_XVFB_RUN)
    # A virtual screen large enough for the 2.0x window (2560 x 1520): popup
    # menus are kept within the screen, so a smaller one would push them aside.
    set(_mw909_ui_launcher "${MW909_XVFB_RUN}" -a -s "-screen 0 2600x1600x24")
elseif(DEFINED ENV{DISPLAY})
    set(_mw909_ui_launcher "")
else()
    message(STATUS "mw909_ui_tool: no xvfb-run and no DISPLAY, UI tests not registered")
    return()
endif()
# The tool is started through the launcher, not as a target, so CTest does not
# add the configured emulator (the TSan presets' `setarch -R`) by itself.
list(PREPEND _mw909_ui_launcher ${CMAKE_CROSSCOMPILING_EMULATOR})

# Sanitizer trees (the asan-plugin / tsan-plugin presets: Debug, ASan or TSan)
# open and paint the editor some twenty times slower than Release: 200
# open / close cycles took 715 s under ASan, past the 600 s timeout, and longer
# under TSan. They run 40 cycles, which still goes through every view, scale
# range, program change, state round trip and song-editor path several times.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[a-z,]*(address|thread)")
    set(_mw909_ui_stress_cycles 40)
else()
    set(_mw909_ui_stress_cycles 200)
endif()
set(_mw909_ui_stress_test "ui: open / close / resize the editor ${_mw909_ui_stress_cycles} times with audio running")

set(_mw909_screens_dir "${CMAKE_BINARY_DIR}/screens")
add_test(NAME "ui: screenshots of PANEL / GRID / MIXER / FX at 0.6x 1.0x 2.0x"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> screenshots "${_mw909_screens_dir}")
add_test(NAME "${_mw909_ui_stress_test}"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> stress ${_mw909_ui_stress_cycles})
add_test(NAME "ui: idle editor CPU with the 30 Hz timer"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> idle 5)
add_test(NAME "ui: scripted interaction on every control kind"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> interact)
add_test(NAME "ui: every editor control path reaches the processor"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> wiring)
add_test(NAME "ui: editor closed with menus, dialogs and drags in flight"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> lifetime 96)
add_test(NAME "ui: repaint rate and dirty area, stopped and playing"
    COMMAND ${_mw909_ui_launcher} $<TARGET_FILE:mw909_ui_tool> perf 3)
set_tests_properties(
    "ui: screenshots of PANEL / GRID / MIXER / FX at 0.6x 1.0x 2.0x"
    "${_mw909_ui_stress_test}"
    "ui: idle editor CPU with the 30 Hz timer"
    "ui: scripted interaction on every control kind"
    "ui: every editor control path reaches the processor"
    "ui: editor closed with menus, dialogs and drags in flight"
    "ui: repaint rate and dirty area, stopped and playing"
    PROPERTIES LABELS ui TIMEOUT 600 RUN_SERIAL TRUE)
