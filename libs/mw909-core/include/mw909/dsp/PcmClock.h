// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Sample clocks of the four 6-bit voices, shared by the voices (playback) and
// by romFromAudio (the rate a ROM image is built at, so that a source plays at
// its natural pitch with the knobs at their defaults).
//
// Each voice's ROM counter is clocked by a gated two-gate CMOS RC astable
// divided by two (research/02 §2). The rates below are MEASURED on an original
// unit from the exact sinc null of the zero-order hold (research/05 §0.9, §5.1):
//
//   hi-hat         31.9 kHz (fixed R492 12k / C160 470p; the service notes say
//                  "about 60 kHz divided by two" = 30 kHz: CONFLICTING, the
//                  measurement wins and unit variance covers the spread)
//   crash / ride   f = fmax / (1 + (Rpot/Rfix)·(1 − k)),  k = TUNE pot 0..1,
//                  fmax = 56.6 kHz and Rpot/Rfix = 1.0 (measured law
//                  f ≈ 56.6k / (2 − k): 28.3 kHz at 0, 37.7 kHz at centre).
//
// The schematic parts (R443/R441 6.8k fixed + VR25/VR27 10k(B)) predict
// Rpot/Rfix = 1.47, a 2.47:1 range (15.7 semitones) instead of the measured
// 2.0:1 (12 semitones). The cause is unknown (the serial-number change of the
// oscillator's series resistor, pot end resistance, a trim). mw909 follows the
// measurement; kCymbalPotRatioSchematic documents the alternative.

#pragma once

#include "mw909/dsp/Rom.h"

namespace mw909::dsp::pcm {

inline constexpr double kHatClockHz = 31900.0;          // MEASURED, research/05 §5.1
inline constexpr double kCymbalClockMaxHz = 56600.0;    // MEASURED fit, TUNE fully clockwise
inline constexpr double kCymbalPotRatio = 1.0;          // MEASURED fit: Rpot/Rfix
inline constexpr double kCymbalPotRatioSchematic = 10.0 / 6.8;  // schematic: VR 10k(B) / R 6.8k (not used)

// Crash/ride clock at TUNE pot position k (0..1, linear B-taper pot).
constexpr double cymbalClockHz(double k, double maxHz = kCymbalClockMaxHz,
                               double potRatio = kCymbalPotRatio) noexcept {
    const double kk = k < 0.0 ? 0.0 : (k > 1.0 ? 1.0 : k);
    return maxHz / (1.0 + potRatio * (1.0 - kk));
}

inline constexpr double kCymbalClockCentreHz = cymbalClockHz(0.5);   // 37.73 kHz

// The rate a ROM region is built at: the region's clock with every knob at
// its default (hat tune 0 st; crash/ride TUNE at centre).
constexpr double romBuildRateHz(RomRegion region) noexcept {
    return (region == RomRegion::HatOpen || region == RomRegion::HatClosed) ? kHatClockHz
                                                                            : kCymbalClockCentreHz;
}

// Region bounds in ROM words.
struct RomSpan {
    int start = 0;
    int end = kRomWords;   // exclusive
    constexpr int length() const noexcept { return end - start; }
};

constexpr RomSpan romSpan(RomRegion region) noexcept {
    switch (region) {
        case RomRegion::HatOpen: return {kHatOpenStart, kHatOpenEnd};
        case RomRegion::HatClosed: return {kHatClosedStart, kHatClosedEnd};
        case RomRegion::Crash:
        case RomRegion::Ride: break;
    }
    return {0, kRomWords};
}

}  // namespace mw909::dsp::pcm
