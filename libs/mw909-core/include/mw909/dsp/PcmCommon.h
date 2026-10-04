// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Parts and laws shared by the hi-hat and cymbal voices: the reconstruction
// filter values, the accent law of the anti-log VCAs, the kernel length per
// quality, and parameter sanitising.

#pragma once

#include <cmath>
#include <cstdint>

#include "mw909/Voices.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/PcmBlep.h"

namespace mw909::dsp::pcm {

// AA kernel length (host samples) for a host rate and VoiceSetup::oversample.
// The PCM chain never oversamples: the quality buys a longer (sharper) kernel
// (32 / 48 / 64 taps for oversample 1 / 2 / 4), and the host rate sets a floor
// and a ceiling. Stop band (>= 90 dB) from 0.688 / 0.594 / 0.563 / 0.547 fs
// and pass band (-0.1 dB) to 0.369 / 0.434 / 0.456 / 0.467 fs for 16 / 32 /
// 48 / 64 taps.
//  * floor: every image that would fold back to 20 kHz or below must lie in
//    the stop band, so 44.1 kHz needs 64 taps (clean to 19.98 kHz) and 48 kHz
//    48 taps (clean to 21.0 kHz), whatever the quality;
//  * ceiling: from 88.2 kHz the fold-back lands far above the audio band, so
//    the longest kernel is 48 taps, and from 150 kHz 16 taps (clean to
//    0.31 fs = 60 kHz at 192 kHz) or 32 at High. That keeps prepare() (it
//    builds the kernel, on the audio thread when the engine is reconfigured)
//    cheap where blocks are short.
constexpr int kernelTaps(int oversample, double sampleRate) noexcept {
    int taps = oversample <= 1 ? 32 : (oversample == 2 ? 48 : 64);
    if (!(sampleRate >= 0.0)) sampleRate = 48000.0;   // NaN
    if (sampleRate < 49300.0 && taps < 48) taps = 48;  // 0.4063 fs >= 20 kHz from 49.3 kHz
    if (sampleRate < 45800.0) taps = 64;               // 0.4373 fs >= 20 kHz from 45.8 kHz
    if (sampleRate >= 150000.0) taps = oversample <= 1 ? 16 : 32;
    else if (sampleRate >= 88200.0 && taps > 48) taps = 48;
    return taps;
}

// The frequency up to which a kernel of `taps` keeps fold-back >= 90 dB down.
constexpr double kernelCleanHz(int taps, double sampleRate) noexcept {
    const double stop = taps <= 16 ? 0.688 : (taps <= 32 ? 0.594 : (taps <= 48 ? 0.563 : 0.547));
    return (1.0 - stop) * sampleRate;
}

// The largest latency (host samples) any quality gives at this rate: the
// engine's common latency for the sample voices.
constexpr int maxKernelLatency(double sampleRate) noexcept { return kernelTaps(4, sampleRate) / 2; }

// The two unity-gain Sallen-Key low-passes after every VCA (research/02 §6.1,
// identical in all three circuits): R 5.6k/5.6k with C 1200p (feedback) /
// 1000p (ground), then C 2700p / 390p. f0 = 1/(2π R √(C1 C2)), Q = ½√(C1/C2).
// SCHEMATIC values; ±5 % capacitors and resistors under unit variance.
inline void reconstructionFilter(std::uint32_t seed, std::uint32_t idBase, float variance, AnalogLowpass2& s1,
                                 AnalogLowpass2& s2) noexcept {
    auto cs = [&](std::uint32_t id) { return static_cast<double>(componentScale(seed, idBase + id, 0.05f, variance)); };
    const double r = 5600.0;
    const double r1 = r * cs(0), r2 = r * cs(1), c1 = 1200e-12 * cs(2), c2 = 1000e-12 * cs(3);
    const double r3 = r * cs(4), r4 = r * cs(5), c3 = 2700e-12 * cs(6), c4 = 390e-12 * cs(7);
    auto sk = [](double ra, double rb, double cf, double cg) {
        AnalogLowpass2 s;
        s.f0 = 1.0 / (kTwoPi * std::sqrt(ra * rb * cf * cg));
        s.q = std::sqrt(ra * rb * cf * cg) / (cg * (ra + rb));
        return s;
    };
    s1 = sk(r1, r2, c1, c2);
    s2 = sk(r3, r4, c3, c4);
}

// The DAC coupling capacitor's corner (C157/C127/C128 10 µF into the VCA
// node). The DAC's source resistance is unknown (research/02 §4.1, F6):
// ASSUMED ≈ 4.7 kΩ, i.e. 3.4 Hz.
inline constexpr double kDacCouplingHz = 3.4;

// Output coupling into the level stage: C (10 µF, or 0.01 µF on the late hat)
// with R444/R445/R446 22k.
constexpr double outputCouplingHz(double capFarads) noexcept { return 1.0 / (kTwoPi * 22000.0 * capFarads); }

// Accent law of the anti-log VCAs (research/02 §4.5, §5.4): the accent line
// enters the control node through 4.7k and shifts the gain in the exponential
// domain, so gain ∝ exp(k·V_acc). k is FITTED per voice to the measured
// TOTAL ACCENT span 0 -> 100 % (research/05 §0.11, §10.3), which on the accent
// DAC is code kNormalCode -> 63.
inline constexpr double kAccentSpanVolts =
    static_cast<double>(AccentDac::volts(AccentDac::kMaxCode) - AccentDac::volts(AccentDac::kNormalCode));
inline constexpr double kNormalAccentVolts = static_cast<double>(AccentDac::volts(AccentDac::kNormalCode));

// Gain relative to a normal hit for accent-line volts v, for a voice whose
// full-accent span is spanDb.
inline double accentGain(double volts, double spanDb) noexcept {
    const double v = clamp(volts, 0.0, static_cast<double>(AccentDac::kVolts));
    return std::exp((v - kNormalAccentVolts) * (spanDb / kAccentSpanVolts) * 0.11512925464970229);
}

// Accent-line volts from a Hit: non-finite -> a normal hit, clamped 0..5 V.
inline float sanitiseAcc(float volts) noexcept {
    if (!std::isfinite(volts)) return AccentDac::volts(AccentDac::kNormalCode);
    return clamp(volts, 0.0f, AccentDac::kVolts);
}
inline float sanitisePitch(float semis) noexcept { return std::isfinite(semis) ? clamp(semis, -24.0f, 24.0f) : 0.0f; }

// One knob: non-finite -> its default, then clamped to its range.
inline float sanitiseKnob(float x, const KnobInfo& info) noexcept {
    if (!std::isfinite(x)) return info.def;
    return clamp(x, info.min, info.max);
}
inline VoiceParams sanitiseParams(const VoiceParams& p, int voice) noexcept {
    VoiceParams out{};
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voice)];
    for (int i = 0; i < kMaxKnobs; ++i) {
        const auto k = static_cast<std::size_t>(i);
        out.k[k] = i < info.numKnobs ? sanitiseKnob(p.k[k], info.knobs[k]) : 0.0f;
    }
    return out;
}
inline PcmSource sourceOf(const VoiceParams& p) noexcept {
    const int s = static_cast<int>(std::lround(p.k[static_cast<std::size_t>(knob::PcmSource)]));
    return static_cast<PcmSource>(clamp(s, 0, static_cast<int>(PcmSource::kCount) - 1));
}

// Audio (A, logarithmic) pot taper: fraction of the track resistance at
// rotation p, 10 % at mid-rotation (typical A taper; research/02 F7).
inline double audioTaper(double p) noexcept {
    constexpr double k = 4.394449154672439;   // 2 ln 9: (e^(k/2) - 1)/(e^k - 1) = 0.1
    return (std::exp(k * clamp(p, 0.0, 1.0)) - 1.0) / (std::exp(k) - 1.0);
}

}  // namespace mw909::dsp::pcm
