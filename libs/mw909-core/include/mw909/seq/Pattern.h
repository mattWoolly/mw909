// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Pattern data model. Everything here is trivially copyable, fixed-size POD so
// patterns can cross threads by value through lock-free queues and be written
// to state as raw, versioned bytes (StateCodec stores sizeof(Pattern) and
// kPatternFormat and refuses blobs that do not match).
//
// The original machine (research/03 §0, §2): 2 banks x 3 groups x 16 patterns,
// 1..16 steps (LAST STEP), four scales, shuffle levels 1..7 and flam intervals
// 1..8 stored per pattern, a total-accent row, per-instrument accent on BD, SD,
// the toms and the closed hat, and 4 tracks (songs) per bank. Everything beyond
// that (64 steps, A/B variations, per-track length and shuffle, weak steps,
// sub-steps, probability, accent and flam on every voice) is the extended
// superset carried over from mw808; the hardware values are a subset of each
// field's range, so a pattern written on the original fits unchanged.

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "mw909/Voices.h"

namespace mw909::seq {

inline constexpr int kMaxSteps = 64;                  // 4 pages of 16; the hardware stops at 16
inline constexpr int kAccentTrack = kNumVoices;       // the TOTAL ACCENT row
inline constexpr int kNumTracks = kNumVoices + 1;     // 11 voices + total accent
inline constexpr int kNumVariations = 2;              // A, B (extension)

inline constexpr int kNumBanks = 2;                   // I, II
inline constexpr int kGroupsPerBank = 3;              // pattern groups 1..3
inline constexpr int kPatternsPerGroup = 16;
inline constexpr int kPatternsPerBank = kGroupsPerBank * kPatternsPerGroup;  // 48
inline constexpr int kNumPatterns = kNumBanks * kPatternsPerBank;           // 96
inline constexpr int kMaxChainLength = 64;            // live pattern chain

inline constexpr int kSongTracksPerBank = 4;          // TRACK 1..4 of each bank
inline constexpr int kNumSongTracks = kNumBanks * kSongTracksPerBank;       // 8
inline constexpr int kMaxSongLength = 256;            // measures per track (hardware: 896 shared per bank)

inline constexpr std::uint32_t kPatternFormat = 1;    // bump when Pattern/SongTrack/Chain layout changes

// Pattern index helpers: index = bank*48 + group*16 + slot.
constexpr int patternIndex(int bank, int group, int slot) noexcept {
    return bank * kPatternsPerBank + group * kPatternsPerGroup + slot;
}
constexpr int bankOf(int index) noexcept { return index / kPatternsPerBank; }
constexpr int groupOf(int index) noexcept { return (index % kPatternsPerBank) / kPatternsPerGroup; }
constexpr int slotOf(int index) noexcept { return index % kPatternsPerGroup; }

// Step length. The first four are the original SCALE positions, in the order
// the SCALE key cycles them (research/03 §0).
enum class Scale : std::uint8_t {
    Sixteenth = 0,         // scale 1: 4 steps per beat (default)
    ThirtySecond,          // scale 2: 8 steps per beat
    EighthTriplet,         // scale 3: 3 steps per beat
    SixteenthTriplet,      // scale 4: 6 steps per beat
    Eighth,                // extension: 2 steps per beat
    kNumScales
};

inline constexpr int kNumHardwareScales = 4;

// Step length in quarter notes.
constexpr double stepLengthQn(Scale s) noexcept {
    switch (s) {
        case Scale::Sixteenth:        return 0.25;
        case Scale::ThirtySecond:     return 0.125;
        case Scale::EighthTriplet:    return 1.0 / 3.0;
        case Scale::SixteenthTriplet: return 1.0 / 6.0;
        case Scale::Eighth:           return 0.5;
        default:                      return 0.25;
    }
}

constexpr bool isTriplet(Scale s) noexcept {
    return s == Scale::EighthTriplet || s == Scale::SixteenthTriplet;
}

enum class StepLevel : std::uint8_t { Off = 0, Weak = 1, Normal = 2, Accent = 3 };

// One step of one track, packed in a byte (same layout as mw808):
//   bits 0-1  level        (StepLevel; Accent = the per-instrument accent;
//                           on the total-accent track any non-zero = accent on)
//   bit  2    flam         (two hits: the step, then the pattern's flam interval later)
//   bits 3-4  sub-steps    (0..3 -> x1..x4 evenly spaced hits inside the step)
//   bits 5-7  probability  (0 = always; n = (8-n)/8 chance, i.e. 87.5% .. 12.5%)
struct Step {
    std::uint8_t bits = 0;

    constexpr StepLevel level() const noexcept { return static_cast<StepLevel>(bits & 0x03U); }
    constexpr bool on() const noexcept { return (bits & 0x03U) != 0U; }
    constexpr bool flam() const noexcept { return (bits & 0x04U) != 0U; }
    constexpr int subSteps() const noexcept { return 1 + ((bits >> 3) & 0x03); }
    constexpr int probabilityCode() const noexcept { return (bits >> 5) & 0x07; }
    constexpr float probability() const noexcept {
        return static_cast<float>(8 - probabilityCode()) / 8.0f;
    }

    constexpr void setLevel(StepLevel l) noexcept {
        bits = static_cast<std::uint8_t>((bits & ~0x03U) | static_cast<std::uint8_t>(l));
    }
    constexpr void setFlam(bool f) noexcept {
        bits = static_cast<std::uint8_t>(f ? (bits | 0x04U) : (bits & ~0x04U));
    }
    constexpr void setSubSteps(int n) noexcept {  // 1..4
        const auto code = static_cast<std::uint8_t>((n < 1 ? 1 : (n > 4 ? 4 : n)) - 1);
        bits = static_cast<std::uint8_t>((bits & ~0x18U) | (code << 3));
    }
    constexpr void setProbabilityCode(int c) noexcept {  // 0..7
        const auto code = static_cast<std::uint8_t>(c < 0 ? 0 : (c > 7 ? 7 : c));
        bits = static_cast<std::uint8_t>((bits & ~0xE0U) | (code << 5));
    }

    constexpr bool operator==(const Step&) const noexcept = default;
};

// Shuffle is stored as the delay of the off-beat steps in 1/96 of a beat
// ("ticks"). The hardware's SHUFFLE keys 1..7 are 0, 2, 4, ... 12 ticks in the
// 16th scale (research/03 §0, measured in research/05 §0.13); see
// shuffleTicksFor() for the other scales.
inline constexpr int kMaxShuffleTicks = 16;
constexpr std::uint8_t shuffleTicksForKey(int key) noexcept {  // key 1..7
    return static_cast<std::uint8_t>(2 * ((key < 1 ? 1 : (key > 7 ? 7 : key)) - 1));
}

// Flam interval in ms, stored per pattern. The hardware's 8 intervals (keys
// 9..16) are 20, 24, ... 48 ms, fixed in time regardless of tempo
// (research/03 §0 CONFLICTING 10..38 vs 20..48; research/05 measured 20..47).
inline constexpr int kMinFlamMs = 1;
inline constexpr int kMaxFlamMs = 100;
constexpr std::uint8_t flamMsForKey(int key) noexcept {  // key 1..8 (panel keys 9..16)
    return static_cast<std::uint8_t>(20 + 4 * ((key < 1 ? 1 : (key > 8 ? 8 : key)) - 1));
}

struct Pattern {
    std::uint8_t length = 16;                    // LAST STEP, 1..kMaxSteps
    Scale scale = Scale::Sixteenth;
    std::uint8_t shuffle = 0;                    // 0..kMaxShuffleTicks, 1/96-beat delay of the off-beat steps
    std::uint8_t flamMs = 24;                    // kMinFlamMs..kMaxFlamMs
    std::uint8_t flamGrace = 100;                // level of the first (grace) flam hit, % of the main hit's ACC, 0..100
    std::uint8_t subStart = 100;                 // sub-step level ramp: first sub-hit, % of ACC, 0..100
    std::uint8_t subEnd = 100;                   //   last sub-hit, % of ACC; linear in between
    std::uint8_t reserved = 0;                   // must be 0
    std::array<std::uint8_t, kNumVoices> trackLength{};   // 0 = follow `length`; else 1..kMaxSteps (polymeter)
    std::array<std::uint8_t, kNumVoices> trackShuffle{};  // 0 = follow `shuffle`; else 1 + ticks (0..kMaxShuffleTicks)
    std::array<std::array<std::array<Step, kMaxSteps>, kNumTracks>, kNumVariations> steps{};

    constexpr Step& at(int variation, int track, int step) noexcept {
        return steps[static_cast<std::size_t>(variation)][static_cast<std::size_t>(track)]
                    [static_cast<std::size_t>(step)];
    }
    constexpr const Step& at(int variation, int track, int step) const noexcept {
        return steps[static_cast<std::size_t>(variation)][static_cast<std::size_t>(track)]
                    [static_cast<std::size_t>(step)];
    }
    // Effective loop length of a track (voice tracks may be shorter/longer than
    // the pattern; the total-accent track always follows the pattern).
    constexpr int lengthOf(int track) const noexcept {
        if (track < kNumVoices && trackLength[static_cast<std::size_t>(track)] != 0)
            return trackLength[static_cast<std::size_t>(track)];
        return length;
    }
    // Effective shuffle of a track, in ticks.
    constexpr int shuffleOf(int track) const noexcept {
        if (track < kNumVoices && trackShuffle[static_cast<std::size_t>(track)] != 0)
            return trackShuffle[static_cast<std::size_t>(track)] - 1;
        return shuffle;
    }

    constexpr bool operator==(const Pattern&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Pattern>);

// Live pattern chain (the panel's "press several pattern keys"). length == 0
// means no chain.
struct Chain {
    std::uint8_t length = 0;
    std::array<std::uint8_t, kMaxChainLength> entries{};    // global pattern indices
    constexpr bool operator==(const Chain&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<Chain>);

// One TRACK (song): a list of measures, each naming the pattern it plays.
// Entries are global pattern indices; the hardware keeps a track inside its
// own bank, the extension allows any pattern.
struct SongTrack {
    std::uint16_t length = 0;                                // 0..kMaxSongLength measures; 0 = empty
    std::array<std::uint8_t, kMaxSongLength> entries{};
    constexpr bool operator==(const SongTrack&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<SongTrack>);

struct PatternBank {
    std::array<Pattern, kNumPatterns> patterns{};
    Chain chain{};
    std::array<SongTrack, kNumSongTracks> tracks{};
};

// How A/B variations play (extension; the hardware has none).
enum class VariationMode : std::uint8_t { A = 0, AB = 1, B = 2 };

// Auto fill-in interval, in measures (pattern passes). The fill replaces the
// last measure of each group. Manual = only when requested. (Extension.)
enum class AutoFill : std::uint8_t { Manual = 0, Every16, Every12, Every8, Every4, Every2 };

constexpr int autoFillMeasures(AutoFill f) noexcept {
    switch (f) {
        case AutoFill::Every16: return 16;
        case AutoFill::Every12: return 12;
        case AutoFill::Every8:  return 8;
        case AutoFill::Every4:  return 4;
        case AutoFill::Every2:  return 2;
        default:                return 0;
    }
}

// What the clock walks through.
enum class PlayMode : std::uint8_t {
    Pattern = 0,   // loop the selected pattern (queued selections take over at the measure end)
    Chain,         // walk PatternBank::chain
    Track          // TRACK PLAY: walk PatternBank::tracks[Settings::songTrack]
};

}  // namespace mw909::seq
