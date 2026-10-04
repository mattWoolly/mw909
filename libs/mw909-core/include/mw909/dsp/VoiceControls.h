// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Control-value hygiene and pot tapers shared by the analog voices.
// Header-only, allocation-free.

#pragma once

#include <cmath>
#include <cstddef>

#include "mw909/Voices.h"
#include "mw909/dsp/Math.h"

namespace mw909::dsp {

// A knob value from the host: NaN takes the knob's default, anything else
// (infinities included) is clamped to the knob's range (architecture §1).
inline float saneKnob(float x, const KnobInfo& info) noexcept {
    if (std::isnan(x)) return info.def;
    return clamp(x, info.min, info.max);
}

inline VoiceParams saneParams(const VoiceParams& p, int voice) noexcept {
    VoiceParams out = p;
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voice)];
    for (int i = 0; i < kMaxKnobs; ++i) {
        const auto k = static_cast<std::size_t>(i);
        out.k[k] = i < info.numKnobs ? saneKnob(p.k[k], info.knobs[k]) : 0.0f;
    }
    return out;
}

// A per-hit pitch offset in semitones: non-finite is 0, the rest is limited.
inline double saneSemis(float s, double limit = 48.0) noexcept {
    return std::isfinite(s) ? clamp(static_cast<double>(s), -limit, limit) : 0.0;
}

// VoiceSetup hygiene: the engine may hand a corrupt variance or rate.
inline VoiceSetup saneSetup(const VoiceSetup& s) noexcept {
    VoiceSetup out = s;
    out.sampleRate = std::isfinite(s.sampleRate) ? clamp(s.sampleRate, 8000.0, 768000.0) : 48000.0;
    out.variance = std::isfinite(s.variance) ? clamp(s.variance, 0.0f, 1.0f) : 0.0f;
    return out;
}

// Extended (non-hardware) time knobs: 0.5 is the stock circuit, the travel is
// x0.25 .. x4 of the time constant.
inline double timeKnobScale(double knob) noexcept { return std::exp2(4.0 * (clamp(knob, 0.0, 1.0) - 0.5)); }

// Extended amount knobs: 0.5 is stock (x1), 0 removes the effect, 1 doubles it.
inline double amountKnobScale(double knob) noexcept { return 2.0 * clamp(knob, 0.0, 1.0); }

// Piecewise-linear interpolation of a table over equally spaced knob points
// 0, 1/(n-1), .., 1.
template <std::size_t N>
inline double knobTable(const double (&table)[N], double knob) noexcept {
    static_assert(N >= 2);
    const double p = clamp(knob, 0.0, 1.0) * static_cast<double>(N - 1);
    const auto i = static_cast<std::size_t>(std::min(p, static_cast<double>(N - 2)));
    const double f = p - static_cast<double>(i);
    return table[i] + (table[i + 1] - table[i]) * f;
}

}  // namespace mw909::dsp
