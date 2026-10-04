// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// A small human-writable text format for mw909 patterns (both variations),
// the live chain and the song tracks, used by mw909-render and the golden
// cases. Full description in kPatternTextHelp below (also printed by
// `mw909-render help pattern-format`).

#pragma once

#include <array>
#include <string>
#include <vector>

#include "mw909/seq/Pattern.h"

namespace mw909::tools {

struct PatternText
{
    seq::PatternBank bank{};                       // every pattern the file defines; the rest empty
    std::vector<int> defined;                      // indices defined, in file order
    std::array<std::string, seq::kNumPatterns> names{};
    double tempo = 0.0;                            // from a `tempo` line; 0 when absent

    int first() const { return defined.empty() ? 0 : defined.front(); }
};

// Parses `text`. On failure returns false and sets `error` to "line N: ...".
bool parsePatternText(const std::string& text, PatternText& out, std::string& error);
bool loadPatternText(const std::string& path, PatternText& out, std::string& error);

// Text of a scale as the format writes it: "16" "32" "8t" "16t" "8".
const char* scaleText(seq::Scale scale);

// Writes one pattern in the same format (round-trips through parsePatternText
// into pattern slot `index`).
std::string formatPatternText(const seq::Pattern& pattern, int index, const std::string& name,
                              double tempo);

inline constexpr const char* kPatternTextHelp = R"(PATTERN TEXT FORMAT

  UTF-8/ASCII, one statement per line. '#' starts a comment (to the end of
  the line); blank lines are ignored. Keywords and track names are
  case-insensitive; step characters are not. A file holds one or more
  patterns, and optionally the live chain and song tracks.

  Pattern addresses: 0..95, or the hardware's BANK-GROUP-SLOT label,
  I-1-1 .. I-3-16 (bank I) and II-1-1 .. II-3-16 (bank II).

  File statements:
    tempo <bpm>            suggested tempo (20..400); --tempo wins
    pattern <addr>         following statements edit this pattern. Without
                           it, everything goes to pattern I-1-1 (0). The
                           first pattern defined is the one rendered unless
                           --pattern picks another.
    chain <addr>...        the live chain (PLAY MODE chain), up to 64 entries
    song <1..8> <addr>...  song track 1..8 (TRACK PLAY; 1..4 = bank I's
                           TRACK 1..4, 5..8 = bank II's). An entry may repeat:
                           I-1-1*4 is four measures of I-1-1. Up to 256.

  Pattern statements (each optional):
    name <text>            display name (informational)
    length <1..64>         LAST STEP, pattern length in steps   (default 16)
    scale <s>              16 | 32 | 8t | 16t | 8               (default 16)
                           16 = sixteenth (hardware scale 1), 32 = thirty-
                           second (2), 8t = eighth triplet (3), 16t =
                           sixteenth triplet (4), 8 = eighth (extension).
                           Also accepted: 16th, 32nd, 8th, s1..s4.
    shuffle <0..16>        delay of the off-beat steps in 1/96-beat ticks
    shufflekey <1..7>      the hardware SHUFFLE key: 0, 2, 4 .. 12 ticks
    flam <1..100>          flam interval in ms                  (default 24)
    flamkey <1..8>         the hardware FLAM keys 9..16: 20, 24 .. 48 ms
    flamgrace <0..100>     grace-hit level, % of the main hit   (default 100)
    substeps <0..100> <0..100>   sub-step level ramp, first and last hit, %
    tracklength <TR> <n>   polymeter: track TR loops every n steps (1..64)
    trackshuffle <TR> <t>  track TR has its own shuffle, t ticks (0..16)
    variation A|B          following track lines fill variation A or B

  Track lines:  <TR> <steps>
    TR is a voice code  BD SD LT MT HT RS CP CH OH CR RD
    or AC for the TOTAL ACCENT row.

    <steps> is one token per step, starting at step 1. Spaces and '|' are
    ignored, so  x---|x---|x---|x---  is fine. Steps not written are off; at
    most 64 steps per line.

      -  or  .     off
      o            weak (extension)
      x            normal
      X            accent (the instrument's own accent)
    On the AC row any of o x X switches the total accent on for that step.

    A step character may be followed by modifiers in parentheses:
      (f)          flam: grace hit, then the main hit one flam interval later
      (2) (3) (4)  sub-steps: that many evenly spaced hits inside the step
      (pN)         probability code N = 0..7, chance (8-N)/8 (0 = always)
    Modifiers combine, e.g.  x(f)  x(3)  x(f2p4)

  Example:
    tempo    124
    pattern  I-1-1
    name     Four on the floor
    shufflekey 3
    BD  x---x---x---x---
    SD  ----X-------x(f)---
    CH  x-x-x-x-x-x-x(3)-x-
    OH  --x---x---x---x-
    AC  x-------x-------
    pattern  I-1-2
    BD  x--x--x-x--x--x-
    song 1  I-1-1*3 I-1-2
)";

} // namespace mw909::tools
