// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// A compile-time guard: Xlib.h #defines Bool, Status, None, True, False,
// Success and more, and the headless UI tools include it before the shell
// headers (they install their own X error handler). Any shell header that
// uses one of those names as an identifier breaks those tools; this file
// includes them all in that order so the plugin tests catch it first.

#if defined(__has_include)
 #if __has_include(<X11/Xlib.h>)
  #include <X11/Xlib.h>
 #endif
#endif

#include <juce_audio_processors/juce_audio_processors.h>

#include "MidiLearn.h"
#include "ParameterIDs.h"
#include "Parameters.h"
#include "PresetManager.h"
#include "mw909Processor.h"
#include "state/PatternModel.h"
#include "state/StateCodec.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("shell headers compile after Xlib.h", "[headers]") {
    CHECK(static_cast<int>(mw909::plugin::ParamKind::Toggle) == 1);
}
