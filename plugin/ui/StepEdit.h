// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The step-editing rules shared by the sequencer's step keys and the GRID
// view (plugin.md §7): a click cycles Off -> Normal -> Accent -> Off
// (shift-click toggles Weak); in FLAM / SUB / PROB mode a click edits that
// property of an active step; a drag paints the first step's result onto the
// steps it passes. Pure functions on seq::Step.
//
// With AUTHENTIC editing on, the cycle skips what the original machine cannot
// hold (weak steps; accents outside BD, SD, the toms and CH), so a click never
// lands on a state seq::applyHardwareRules would undo.

#pragma once

#include "UiCommon.h"
#include "mw909/seq/Pattern.h"

namespace mw909::ui::stepedit {

inline bool isAccentTrack(int track) noexcept { return track >= seq::kAccentTrack; }

inline bool hardwareAccent(int track) noexcept {
    return track < kNumVoices && kVoiceInfo[static_cast<std::size_t>(track)].hardwareAccent;
}

inline void clearModifiers(seq::Step& s) noexcept {
    s.setFlam(false);
    s.setSubSteps(1);
    s.setProbabilityCode(0);
}

// The step after a click in `mode` on `track`.
inline seq::Step clicked(seq::Step s, StepMode mode, int track, bool shift, bool authentic = false) noexcept {
    using seq::StepLevel;
    if (isAccentTrack(track)) {
        s.setLevel(s.on() ? StepLevel::Off : StepLevel::Accent);
        clearModifiers(s);
        return s;
    }
    switch (mode) {
        case StepMode::Level:
            if (shift && !authentic) {
                s.setLevel(s.level() == StepLevel::Weak ? StepLevel::Off : StepLevel::Weak);
            } else {
                switch (s.level()) {
                    case StepLevel::Off: s.setLevel(StepLevel::Normal); break;
                    case StepLevel::Weak: s.setLevel(StepLevel::Normal); break;
                    case StepLevel::Normal:
                        s.setLevel(authentic && !hardwareAccent(track) ? StepLevel::Off : StepLevel::Accent);
                        break;
                    case StepLevel::Accent: s.setLevel(StepLevel::Off); break;
                }
            }
            if (!s.on()) clearModifiers(s);  // a cleared step forgets its modifiers
            return s;
        case StepMode::Flam:
            if (!s.on()) s.setLevel(StepLevel::Normal);
            else s.setFlam(!s.flam());
            return s;
        case StepMode::Sub: {
            if (!s.on()) { s.setLevel(StepLevel::Normal); return s; }
            int n = s.subSteps() + (shift ? -1 : 1);
            if (n > 4) n = 1;
            if (n < 1) n = 4;
            s.setSubSteps(n);
            return s;
        }
        case StepMode::Prob: {
            if (!s.on()) { s.setLevel(StepLevel::Normal); return s; }
            int c = s.probabilityCode() + (shift ? -1 : 1);
            if (c > 7) c = 0;
            if (c < 0) c = 7;
            s.setProbabilityCode(c);
            return s;
        }
    }
    return s;
}

// Paint the result of a click (`brush`) onto another step: in LEVEL mode the
// level is copied (clearing modifiers when off); in the other modes the
// property is copied onto active steps only.
inline seq::Step painted(seq::Step target, seq::Step brush, StepMode mode, int track) noexcept {
    if (isAccentTrack(track) || mode == StepMode::Level) {
        target.setLevel(brush.level());
        if (!target.on()) clearModifiers(target);
        return target;
    }
    if (!target.on()) return target;
    if (mode == StepMode::Flam) target.setFlam(brush.flam());
    else if (mode == StepMode::Sub) target.setSubSteps(brush.subSteps());
    else if (mode == StepMode::Prob) target.setProbabilityCode(brush.probabilityCode());
    return target;
}

// Short badge for a step in a mode ("F", "x3", "75%"); empty when stock.
inline juce::String badge(seq::Step s, StepMode mode, int track) {
    if (!s.on() || isAccentTrack(track)) return {};
    if (mode == StepMode::Flam) return s.flam() ? "F" : juce::String();
    if (mode == StepMode::Sub) return s.subSteps() > 1 ? "x" + juce::String(s.subSteps()) : juce::String();
    if (mode == StepMode::Prob)
        return s.probabilityCode() > 0 ? juce::String(juce::roundToInt(s.probability() * 100.0f)) + "%" : juce::String();
    juce::String b;
    if (s.flam()) b << "F";
    if (s.subSteps() > 1) b << (b.isEmpty() ? "" : " ") << "x" << s.subSteps();
    if (s.probabilityCode() > 0) b << (b.isEmpty() ? "" : " ") << "?";
    return b;
}

inline const char* scaleName(seq::Scale s) noexcept {
    switch (s) {
        case seq::Scale::Sixteenth: return "1/16";
        case seq::Scale::ThirtySecond: return "1/32";
        case seq::Scale::EighthTriplet: return "1/8T";
        case seq::Scale::SixteenthTriplet: return "1/16T";
        case seq::Scale::Eighth: return "1/8";
        case seq::Scale::kNumScales: break;
    }
    return "?";
}

// Default randomiser densities per track (voice order, then the accent row).
inline std::array<float, seq::kNumTracks> defaultDensities() noexcept {
    return {{0.40f, 0.28f, 0.10f, 0.10f, 0.10f, 0.12f, 0.12f, 0.45f, 0.12f, 0.06f, 0.20f, 0.25f}};
}

// A step popup (right-click), shared by keys and grid.
void showStepMenu(Context& ctx, int patternIndex, int variation, int track, int step, juce::Component& target);

}  // namespace mw909::ui::stepedit
