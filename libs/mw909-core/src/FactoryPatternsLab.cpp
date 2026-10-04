// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 7, "Rhythm Lab": experiments with what the extended
// sequencer can do beyond the original.
//
//   Bank I   group 1  Polymeter: tracks with their own loop lengths (3, 5, 7, 9, 15)
//            group 2  The 32nd scale and ratchets (sub-steps with level ramps)
//            group 3  Triplet grids, 7/8, 5/4 and the 8th-note scale
//   Bank II  group 1  Probability: grooves that never quite repeat (deterministically)

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: Polymeter ------------------------------------------------------------
constexpr Group kPoly = group(std::to_array<PatternDef>({
    {"Three Over Four", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "RS@ x..;"
     "CH@ .x;"
     "CP ....|x...|....|x...",
     "RS x.x"},
    {"Five Four Seven", 16, S16, 0,
     "BD@ X..x.;"
     "SD ....|x...|....|x...;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "LT@ ..x...x;"
     "HT@ x....x..."},
    {"Shifting Clave", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "RS@ x..x..x...x.x..;"
     "CH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...",
     "CP ....|x...|....|x..o"},
    {"Odd Hats", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CH@ x.xo.;"
     "CP ....|x...|....|x...;"
     "RD@ x..x..x"},
    {"Triplet Polymeter", 12, T8, 0,
     "BD X..|X..|X..|X..;"
     "SD ...|x..|...|x..;"
     "CH x.x|x.x|x.x|x.x;"
     "RS@ x.x.x"},
}),
    // A three-step snare loop against the bar. The loop runs on from measure
    // to measure (step (measure x 16 + step) mod 3, architecture §5.3), so
    // the snare lands on 3, 6, 9, 12, 15 only when the fill falls on a
    // measure that is a multiple of 3, and on other steps otherwise: each
    // auto fill comes out a little different, on purpose.
    {"Polymeter Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD@ ..x;"
     "LT ....|....|....|.xxX;"
     "CH ..x.|..x.|....|...."});

// ---- Bank I, group 2: 32nds and ratchets ---------------------------------------------------------
constexpr Group kRatchet = group(std::to_array<PatternDef>({
    {"Thirty-Second Hats", 32, S32, 0,
     "BD X.......|X.......|X.......|X.......;"
     "SD ........|X.......|........|X.......;"
     "CH x.x.x.x.|x.x.xxxx|x.x.x.x.|x.xxx.xx;"
     "AC x.......|........|x.......|........",
     "CH x.x.x.x.|x.x.x.x.|x.x.xxxx|xxxxxxxx"},
    {"Ratchet Groove", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CH x2.x.|x3.x.|x2.x.|x4.x.;"
     "CP ....|x...|....|x...;"
     "RS ..x2.|....|..x3.|....",
     "CH x2.x.|x3.x.|x2.x4.|x4x3x2x",
     24, 100, 60, 100},
    {"Glitch Thirty-Two", 32, S32, 0,
     "BD X..x....|..x.....|X.......|x..x.x..;"
     "SD ........|X.......|........|X..x....;"
     "CH x.x?.x.x?.|x.x?.x.x?.|x.x?.x.x?.|x.x?.x.x?.;"
     "RS ......x?.|........|...x?....|.......x?"},
    {"Roll Call", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x.o4o4;"
     "CH x.x.|x.x.|x.x.|x.x.",
     "SD ....|x...|..o3.|x.o4x4",
     24, 100, 30, 100},
}),
    {"Ratchet Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|....|x2x2x2x2|x3x3x4x4;"
     "CH x.x.|x.x.|....|....",
     {}, 24, 100, 40, 100});

// ---- Bank I, group 3: Triplet and odd grids -------------------------------------------------------
// 16th triplets: 24 steps make one bar. 8th triplets: 12. The 8th scale: 16
// steps make two bars.
constexpr Group kOdd = group(std::to_array<PatternDef>({
    {"Half-Time Triplets", 24, T16, 0,
     "BD X.....|...x..|......|..x...;"
     "SD ......|......|X.....|......;"
     "CH x.x.x.|x.x.x.|x.x.x.|xxxxxx",
     "CH x.x.x.|x.x.x.|x.xxx.|x2x2x2x3x3x3"},
    {"Triplet Groove", 12, T8, 0,
     "BD X..|...|X.x|...;"
     "SD ...|X..|...|X..;"
     "CH x.x|x.x|x.x|x.x;"
     "RS ..x|...|..x|...",
     "BD X..|..x|X.x|..."},
    {"Seven Eight Lock", 14, S16, 0,
     "BD X.....|x...|x...;"
     "SD ...x..|..x.|..x.;"
     "CH xoxoxo|xoxo|xoxo;"
     "RS ..x...|...x|....",
     "SD ...x..|..x.|..xo"},
    {"Five Four Swing", 20, S16, 8,
     "BD X...|....|..x.|X...|....;"
     "SD ....|X...|....|....|X...;"
     "CH x.x.|x.x.|x.x.|x.x.|x.x.",
     "BD X...|....|..x.|X..x|...."},
    {"Eighth-Note Grid", 16, S8, 6,
     "BD x...|x...|x...|x...;"
     "SD ..x.|..x.|..x.|..x.;"
     "CH xxxx|xxxx|xxxx|xxxx",
     "CH xxxx|xxxx|xxxx|xxx2x2"},
}),
    {"Triplet Roll Fill", 12, T8, 0,
     "BD X..|...|X..|...;"
     "SD ...|o.o|.o.|...;"
     "HT ...|x2x2x|...|...;"
     "MT ...|...|x2xx|...;"
     "LT ...|...|...|x3x3X;"
     "CH x.x|...|...|...;"
     "AC ...|...|...|..x",
     {}, 24, 100, 50, 100});

// ---- Bank II, group 1: Probability ------------------------------------------------------------------
constexpr Group kChance = group(std::to_array<PatternDef>({
    {"Dice Groove", 16, S16, 4,
     "BD X...|X...|X...|X..x?;"
     "CH xox?o|xox?o|xox?o|xox?o;"
     "RS ..x??.|...x??|..x??.|.x??..;"
     "CP ....|x...|....|x..."},
    {"Ghost Machine", 16, S16, 6,
     "BD X...|..o?.|X...|..o?.;"
     "SD ....|X..o?|.o?..|X..o?;"
     "CH x.x.|x.x.|x.x.|x.x."},
    {"Generative Toms", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "LT x?...|.x??..|..x?.|...x?;"
     "MT ..x?.|x?...|...x??|.x?..;"
     "HT .x??..|..x?.|x??...|..x?.;"
     "CH ..x.|..x.|..x.|..x."},
    {"Probability Ride", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "RD xo?xo?|xo?xo?|xo?xo?|xo?xo?;"
     "CP ....|x...|....|x...;"
     "RS ....|..x??.|....|.x??.."},
}),
    {"Chance Fill", 16, S16, 4,
     "BD X...|X...|X...|o?...;"
     "SD ....|..o?.|o?.o?x?|x2?x3?X4X4;"
     "LT ....|....|.x??..|..x?.;"
     "CH x.x?.|x.x?.|....|....",
     {}, 24, 100, 50, 100});

constexpr BankDefs kDefs = memory(kPoly, kRatchet, kOdd, kChance, kNoGroup, kNoGroup);

constexpr MemoryDef kMemory{
    {"Rhythm Lab", "Experimental", 120.0f},
    false,
    &kDefs,
    {{
        "1.1*4 1.3*4 1.4*3 1.16 1.2*4",
        "2.2*4 2.4*3 2.16 2.1*4",
        "3.2*4 3.5 3.2 3.16 3.3*4",
        "",
        "4.1*4 4.2*4 4.3*3 4.16 4.4*4",
        "",
        "",
        "",
    }},
    "1.1 1.3 1.4 1.16",
};

static_assert(checkMemory(kMemory) == 0, "Rhythm Lab: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& labMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
