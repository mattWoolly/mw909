// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Pattern <-> Standard MIDI File (docs/design/architecture.md §5.7).
// Message-thread only: these functions allocate (std::vector).
//
// writeMidiFile: a format-0 file holding ONE measure of one variation, on
// channel 10 at 480 PPQ. Notes sit where the sequencer would play them:
// shuffle (per track, with the scale's rule), flam (grace note, then the main
// note the pattern's flamMs later) and sub-steps are rendered as real
// positions. Velocities are the step classes with the TOTAL ACCENT row folded
// in (promotedLevel: weak / normal / accent = 48 / 96 / 127); a flam's grace
// note is 0.6 x the main velocity, which is how a notes-only import finds
// flams again. Probability, timing jitter, the grace level and the sub-step
// level ramp cannot be expressed in notes; every hit is written.
//
// Optionally (on by default) the file also carries the exact pattern in a
// sequencer-specific meta event (FF 7F, non-commercial id 7D, "MW90"), so
// readMidiFile restores an mw909 pattern bit for bit, probability included.
// Without it the writer leaves a short marker event ("MW9C") that says the
// velocities are the three classes above. DAWs ignore both.
//
// readMidiFile: accepts format 0/1/2 files with a metrical time base. With the
// embedded pattern present it restores the pattern from it. Otherwise it
// rebuilds steps on the grid of `out.scale`: the length comes from the end of
// the longest track (1..64 steps); the level from the velocity: in a file
// with the class marker <72 weak, <112 normal, else accent; in any other file
// the original machine's rule, velocity >= 65 (kAccentVelocityThreshold) is
// an accent and anything below is normal. Per voice it takes each step's
// onset (odd steps may be up to two thirds of a step late: shuffle), claims
// the notes that onset produced (n evenly spaced sub-step notes, up to 4; for
// a flam, the main note one flam spacing after a grace of 0.6 x its velocity,
// the spacing being the most common such distance in the file), cyclically so
// hits the writer wrapped to the top return to the last steps; leftover notes
// go to their nearest step. Files without such grace/main pairs fall back to
// "a clearly louder note within half a step is the flam's main note". Notes
// alone stay lossy where two hits of one key share a tick, or a flam is longer
// than the measure. The TOTAL ACCENT row, per-track length and shuffle are not
// recovered from notes.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mw909/MidiMap.h"
#include "mw909/Voices.h"
#include "mw909/seq/Pattern.h"

namespace mw909::seq {

inline constexpr int kMidiFilePpq = 480;
inline constexpr std::uint8_t kMidiVelocityWeak = kVelocityWeak;      // 48
inline constexpr std::uint8_t kMidiVelocityNormal = kVelocityNormal;  // 96
inline constexpr std::uint8_t kMidiVelocityAccent = kVelocityAccent;  // 127

struct MidiFileOptions {
    bool includeTempo = true;       // write a tempo meta event
    double bpm = 120.0;             // tempo, and the time base that turns the pattern's flamMs into ticks
    float swingScale = 1.0f;        // global shuffle multiplier (Settings::swingScale)
    bool oneStepNotes = false;      // note length: one step (true) or a 1/16 note (false)
    bool embedPattern = true;       // carry the exact pattern in a sequencer-specific meta event
    int accentNote = -1;            // 0..127: also write the accent row as notes on this key
};

// `noteMap[voice]` is the MIDI note of each voice (0..127); kVoiceNote
// (MidiMap.h) is the standard map.
std::vector<std::uint8_t> writeMidiFile(const Pattern& pattern, int variation,
                                        const std::array<std::uint8_t, kNumVoices>& noteMap,
                                        const MidiFileOptions& options);

// Reads into `out` (variation `variation`, which is cleared first; the other
// variation is kept). `noteMap` names the note of each voice; in addition,
// every note of the original machine's receive map (kNoteToVoice: 35 BD,
// 40 SD, 44 CH, ...) is read as its voice when `noteMap` does not claim it.
// Returns false, leaving `out` untouched, when the bytes are not a
// well-formed metrical MIDI file.
bool readMidiFile(const std::vector<std::uint8_t>& bytes, const std::array<std::uint8_t, kNumVoices>& noteMap,
                  Pattern& out, int variation = 0);

}  // namespace mw909::seq
