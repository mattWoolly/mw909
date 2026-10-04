// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Internal: timing arithmetic shared by the sequencer and the MIDI file
// writer, so a written file puts every note where the sequencer plays it.

#pragma once

#include <cmath>

#include "mw909/seq/Pattern.h"

namespace mw909::seq::detail {

inline constexpr double kShuffleTicksPerBeat = 96.0;

constexpr Scale validScale(Scale s) noexcept {
    return static_cast<unsigned>(s) < static_cast<unsigned>(Scale::kNumScales) ? s : Scale::Sixteenth;
}

// Shuffle delay of step `s` of a measure of `len` steps, in quarter notes
// (architecture.md §5.3): only off-beat (0-based odd) steps move;
// t = stored ticks x swingScale; 16th: t/96 beat; 32nd: 2*floor(t/4)/96 (the
// hardware's level pairs 1=2, 3=4, 5=6, 7); 8th (extension): 2t/96; triplet
// scales: none. The last step of an odd-length pattern is never delayed (with
// 0-based odd steps it is an even step anyway; the check states the rule).
inline double shuffleQn(Scale scale, int ticks, float swingScale, int s, int len) noexcept {
    if ((s & 1) == 0 || ((len & 1) != 0 && s == len - 1)) return 0.0;
    const int tk = ticks < 0 ? 0 : (ticks > kMaxShuffleTicks ? kMaxShuffleTicks : ticks);
    const double t = static_cast<double>(tk) * static_cast<double>(swingScale);
    switch (validScale(scale)) {
        case Scale::Sixteenth: return t / kShuffleTicksPerBeat;
        case Scale::ThirtySecond: return 2.0 * std::floor(t / 4.0) / kShuffleTicksPerBeat;
        case Scale::Eighth: return 2.0 * t / kShuffleTicksPerBeat;
        default: return 0.0;
    }
}

}  // namespace mw909::seq::detail
