# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# The JUCE plugin shell (docs/design/plugin.md): one juce_add_plugin target
# exporting VST3 / LV2 / Standalone, plus CLAP through clap-juce-extensions.
# JUCE and the CLAP shim are fetched by the top-level CMakeLists.

# The product name lives here and nowhere else: the plugin name, the bundle and
# CLAP ids, the LV2 URI and the preset directory all derive from it.
set(MW_PRODUCT_NAME "mw909")
string(TOLOWER "${MW_PRODUCT_NAME}" MW_PRODUCT_ID)
set(MW_PLUGIN_CODE "mw99")

# One line to drop a format (JUCE's LV2 export is the least mature of the three).
# CLAP comes from clap-juce-extensions below. Buses: one stereo main output and
# 15 stereo individual outs "Out 2".."Out 16", disabled by default (the
# processor declares them, plugin.md §1).
set(MW909_PLUGIN_FORMATS VST3 LV2 Standalone)

juce_add_plugin(mw909
    PRODUCT_NAME                "${MW_PRODUCT_NAME}"
    VERSION                     "${PROJECT_VERSION}"
    COMPANY_NAME                "mwAudio"
    BUNDLE_ID                   "com.mwaudio.${MW_PRODUCT_ID}"
    PLUGIN_MANUFACTURER_CODE    mwAu
    PLUGIN_CODE                 ${MW_PLUGIN_CODE}
    DESCRIPTION                 "Classic hybrid drum machine with step sequencer"
    FORMATS                     ${MW909_PLUGIN_FORMATS}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    # The editor has text-entry fields (value readouts, preset names).
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Instrument Drum
    LV2URI                      "https://github.com/mattWoolly/${MW_PRODUCT_NAME}"
    NEEDS_CURL                  FALSE
    NEEDS_WEB_BROWSER           FALSE
    # Installing into ~/.vst3 etc. is a separate, explicit step on Linux.
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

# Shell sources and (when enabled) the editor sources under plugin/ui are
# globbed, so adding a file needs no CMake edit.
#
# MW909_BUILD_EDITOR selects the editor: ON (the default) compiles plugin/ui
# into the plugin and the plugin tests and uses mw909Editor; OFF builds the
# shell alone with the minimal stand-in editor (MW909_USE_PLACEHOLDER_EDITOR=1).
option(MW909_BUILD_EDITOR "Compile the editor sources under plugin/ui and use the real editor" ON)
file(GLOB MW909_SHELL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/state/*.cpp")
set(MW909_UI_SOURCES "")
if(MW909_BUILD_EDITOR)
    file(GLOB_RECURSE MW909_UI_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp")
endif()

target_sources(mw909 PRIVATE ${MW909_SHELL_SOURCES} ${MW909_UI_SOURCES})

target_include_directories(mw909 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
if(MW909_BUILD_EDITOR)
    target_include_directories(mw909 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui")
endif()
# The core is built and tested under its own warning set. JUCE's recommended
# flags add -Wfloat-equal, -Wswitch-enum and -Wsign-conversion, which flag
# deliberate code in the core's headers (exact "did this parameter change"
# compares); seen from the shell those headers are a dependency, not shell code.
target_include_directories(mw909 SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/libs/mw909-core/include")

target_compile_definitions(mw909
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
    PRIVATE
        MW909_PRODUCT_NAME="${MW_PRODUCT_NAME}"
        MW909_VERSION_STRING="${PROJECT_VERSION}"
        MW909_USE_PLACEHOLDER_EDITOR=$<IF:$<BOOL:${MW909_BUILD_EDITOR}>,0,1>)

target_link_libraries(mw909
    PRIVATE
        mw909_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# JUCE's LTO flag is a bare -flto, which links its partitions one at a time
# (and says so in a warning). Let the linker use the available cores.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(mw909 PUBLIC $<$<CONFIG:Release>:-flto=auto>)
endif()

# Do not set CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES here. With it the CLAP
# wrapper splits a host block at parameter events into several processBlock
# calls that all report the transport position of the block START, so the
# sequencer in Host Sync would see the play head jump back at every split and
# re-anchor each time. Unsplit blocks (the default) are what it expects.
clap_juce_extensions_plugin(
    TARGET          mw909
    CLAP_ID         "com.mwaudio.${MW_PRODUCT_ID}"
    CLAP_FEATURES   instrument drum-machine stereo)

# The LV2 target's POST_BUILD step runs juce_lv2_helper, which dlopens the
# freshly linked plugin. In the TSan presets both are TSan builds, and TSan
# cannot start under this kernel's ASLR, so the step fails (exit 66) unless it
# runs under the configured emulator (`setarch -R`), as the tests do. CMake
# prepends a target's CROSSCOMPILING_EMULATOR to custom commands only when
# CMAKE_CROSSCOMPILING is on, and reads it per directory at generate time:
# set here it affects this directory's custom commands only (just that step;
# juceaide is imported and the VST3 helper runs through cmake).
if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING TRUE)
endif()
