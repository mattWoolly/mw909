// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// MIDI note <-> voice mapping and velocity <-> dynamics conventions.
// Receive: the original machine's map (research/04 §5.1), which every
// emulation of it shares, plus General-MIDI aliases that never collide with
// it. Transmit: the General-MIDI-preferred member of each pair, which round-
// trips through any receiver using the same map.

#pragma once

#include <array>
#include <cstdint>

#include "mw909/Voices.h"

namespace mw909 {

// The note each voice sends (MIDI out, MIDI file export).
inline constexpr std::array<std::uint8_t, kNumVoices> kVoiceNote{{
    36,  // BD
    38,  // SD
    43,  // LT
    47,  // MT
    50,  // HT
    37,  // RS
    39,  // CP
    42,  // CH
    46,  // OH
    49,  // CR
    51,  // RD
}};

inline constexpr std::int8_t kNoVoice = -1;

// note -> voice (or kNoVoice).
inline constexpr std::array<std::int8_t, 128> kNoteToVoice = [] {
    std::array<std::int8_t, 128> map{};
    for (auto& m : map) m = kNoVoice;
    map[35] = BD; map[36] = BD;
    map[37] = RS;
    map[38] = SD; map[40] = SD;
    map[39] = CP;
    map[41] = LT; map[43] = LT;
    map[42] = CH; map[44] = CH;
    map[45] = MT; map[47] = MT;
    map[46] = OH;
    map[48] = HT; map[50] = HT;
    map[49] = CR; map[52] = CR; map[55] = CR; map[57] = CR;
    map[51] = RD; map[53] = RD; map[59] = RD;
    return map;
}();

// Velocities written for the step classes (MIDI out and file export). The
// original transmits 64..96 ("accent min-max"); these use the full range so
// the classes survive a round trip through any receiver.
inline constexpr std::uint8_t kVelocityWeak = 48;
inline constexpr std::uint8_t kVelocityNormal = 96;
inline constexpr std::uint8_t kVelocityAccent = 127;   // per-instrument and/or total accent
inline constexpr int kAccentVelocityThreshold = 65;    // TAP WRITE from MIDI: velocity >= this records an accent

// HostNote mode: note kPatternNoteBase + i plays pattern i (i = 0..95 -> notes 24..119).
inline constexpr int kPatternNoteBase = 24;

// Chromatic bass-drum zone: this note plays the stock pitch.
inline constexpr int kBassRootNote = 36;

// Velocity -> accent line.
//   velocity-sensitive: 1..96 scales the accent code linearly up to the
//     normal code (so 48 ~ weak); 97..127 moves from normal to the full
//     accent (per-instrument + total at the current TOTAL ACCENT knob).
//   fixed: three classes (weak / normal / accent) by velocity range.
constexpr Hit velocityToHit(int velocity, float totalAccentKnob, bool sensitive) noexcept {
    const int v = velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
    if (!sensitive) {
        if (v < 72) return Hit::weak();
        if (v < 112) return Hit::normal();
        return Hit::accented(true, true, totalAccentKnob);
    }
    const int normal = AccentDac::kNormalCode;
    if (v <= 96) {
        const int code = (normal * v + 48) / 96;
        return Hit::fromCode(code < 1 ? 1 : code);
    }
    const Hit full = Hit::accented(true, true, totalAccentKnob);
    const float t = static_cast<float>(v - 96) / 31.0f;
    const float acc = AccentDac::volts(normal) + (full.acc - AccentDac::volts(normal)) * t;
    return {acc, 0.0f};
}

}  // namespace mw909
