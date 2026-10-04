// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Pattern editing helpers (docs/design/architecture.md §5.6). Pure functions
// on a Pattern value: no allocation, no globals, deterministic for a seed.
// Meant for the message thread, but nothing here would hurt the audio thread.
//
// Conventions shared by every function:
//  * `variation` is 0 (A) or 1 (B); `track` is 0..kNumTracks-1 (11 voices,
//    then the TOTAL ACCENT row at kAccentTrack). Out-of-range arguments make
//    the call a no-op (it never writes outside the arrays).
//  * Track-wise edits act on the track's own loop length (Pattern::lengthOf),
//    so they respect polymeter: steps beyond that length are left untouched.

#pragma once

#include <array>
#include <cstdint>

#include "mw909/seq/Pattern.h"

namespace mw909::seq {

inline constexpr int kBothVariations = -1;

// Velocity class of a voice hit once the TOTAL ACCENT row is applied, for
// MIDI output and file export (weak / normal / accent = 48 / 96 / 127):
// Weak -> Normal, Normal -> Accent, Accent stays Accent. The sound itself uses
// the accent-DAC codes (architecture.md §5.3), which add up instead.
constexpr StepLevel promotedLevel(StepLevel voiceLevel, bool accentOn) noexcept {
    if (!accentOn || voiceLevel == StepLevel::Off) return voiceLevel;
    if (voiceLevel == StepLevel::Weak) return StepLevel::Normal;
    return StepLevel::Accent;
}

// Silence every step (of one variation, or both). Length, scale, shuffle and
// track lengths are kept.
void clearPattern(Pattern& p, int variation = kBothVariations) noexcept;

// Silence one track of one variation (all 64 cells, not only the loop).
void clearTrack(Pattern& p, int variation, int track) noexcept;

// Copy all 64 cells of a track. When both tracks are voice tracks the
// per-track length and shuffle come along too. Source and destination may be the same
// pattern (and may overlap: the copy goes through a temporary).
void copyTrack(const Pattern& src, int srcVariation, int srcTrack, Pattern& dst, int dstVariation,
               int dstTrack) noexcept;
void copyTrack(Pattern& p, int variation, int fromTrack, int toTrack) noexcept;

// Rotate a track by `steps` (positive = later), wrapping within its length.
void shiftTrack(Pattern& p, int variation, int track, int steps) noexcept;
// Rotate every track (accent row included), each within its own length.
void shiftPattern(Pattern& p, int variation, int steps) noexcept;

// Within the track length: set steps become Off, empty steps become Normal
// with no flam, sub-steps or probability.
void invertTrack(Pattern& p, int variation, int track) noexcept;

// Copy variation `from` onto `to` (A <-> B).
void copyVariation(Pattern& p, int from, int to) noexcept;

// Fill the track (within its length) at random: each step is on with
// probability `density` (0..1). Levels are mostly Normal with some Weak and
// Accent on voice tracks; flam, sub-step and probability bits are always off.
// Writing OH clears the CH steps with the same index where OH is now on, and
// vice versa. That is exact when the hat tracks have one length; with
// different lengths the hats can still meet on later passes (the other hat
// is never wiped wholesale; see the note on polymeter in the .cpp).
void randomizeTrack(Pattern& p, int variation, int track, float density, std::uint32_t seed) noexcept;

// randomizeTrack over all 12 tracks with per-track densities. Same seed and
// densities give the same pattern. Afterwards CH and OH never play on one step
// of any measure, whatever their track lengths: the collisions are resolved per
// residue class of gcd(L_OH, L_CH), so hat lengths with gcd 1 keep only one
// hat.
void randomizePattern(Pattern& p, int variation, const std::array<float, kNumTracks>& densities,
                      std::uint32_t seed) noexcept;

// Double the pattern length by repetition (both variations). Tracks that
// follow the pattern length repeat their content; tracks with their own
// length keep it. Returns false (and changes nothing) above kMaxSteps.
bool expandPattern(Pattern& p) noexcept;

// Euclidean rhythm: `hits` maximally even onsets over the track length,
// rotated by `rotation` steps (positive = later). Set steps are Normal; the
// track is cleared first. hits is clamped to 0..length.
void setEuclidean(Pattern& p, int variation, int track, int hits, int rotation) noexcept;

// --- the original machine's editing rules (architecture.md §5.6) -------------

// The LAST STEP the hardware sets when SCALE changes (research/03 §2.3): 16
// for the 16th and 32nd scales, 12 for both triplet scales. The 8th scale is
// an extension and uses 16.
constexpr int defaultLengthFor(Scale s) noexcept { return isTriplet(s) ? 12 : 16; }

// Change the scale. When the length was the old scale's default it becomes
// the new scale's default (16 <-> 12), as on the hardware; a length the user
// set is kept. Per-track lengths are kept. An invalid scale is a no-op.
void setScale(Pattern& p, Scale scale) noexcept;

// Song tracks (TRACK mode). Measures are 0-based. Each returns false and
// changes nothing when the arguments are out of range or the result would
// exceed kMaxSongLength.
//  * insertMeasure: a measure playing `pattern` (0..kNumPatterns-1) goes in
//    before index `at` (0..length); later measures move up.
//  * deleteMeasure: removes `count` measures from `at` (clipped to the end).
//  * copyMeasures: inserts a copy of measures [from, from + count) before
//    `to` (0..length), moving later measures up, as the hardware's COPY does.
//    Copying into the middle of the source range is refused, as on the
//    hardware (to == from and to == from + count are fine).
bool insertMeasure(SongTrack& t, int at, int pattern) noexcept;
bool deleteMeasure(SongTrack& t, int at, int count = 1) noexcept;
bool copyMeasures(SongTrack& t, int from, int count, int to) noexcept;

// Make a pattern one the original machine could hold ("authentic" editing):
//  * 4 scales (the 8th extension becomes 16th), LAST STEP 1..16, no per-track
//    length or shuffle;
//  * shuffle snapped to the hardware levels (0, 2, ... 12 ticks), flam
//    interval snapped to 20, 24, ... 48 ms;
//  * on every voice step (both variations): no weak level (-> Normal), no
//    sub-steps, no probability; accent only on BD, SD, the toms and CH
//    (elsewhere -> Normal); flam only on BD, SD and the toms and never with
//    an accent (the accent is kept); CH and OH never on one step (the step
//    plays open on the hardware, so CH is cleared);
//  * the TOTAL ACCENT row holds plain on/off cells;
//  * flam grace level and sub-step ramp at 100 % (the original's flam is two
//    triggers at one accent level).
void applyHardwareRules(Pattern& p) noexcept;

// True when applyHardwareRules would change nothing.
bool followsHardwareRules(const Pattern& p) noexcept;

}  // namespace mw909::seq
