// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 3, "Acid & Electro": the straight 16th closed hats with
// accented off-beats that sit under a squelching bass line, electro and
// freestyle, and four-bar acid arrangements.
//
//   Bank I   group 1  Acid lines
//            group 2  Electro, freestyle and bass
//            group 3  Long-form acid: 64- and 32-step patterns, ratchets, polymeter

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: Acid lines ------------------------------------------------
constexpr Group kAcid = group(std::to_array<PatternDef>({
    {"Acid Line 4", 16, S16, 0,
     "BD X...|X...|X...|X..o;"
     "CH xx.X|xx.X|xx.X|xx.X;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...;"
     "RS ...o|..o.|...o|.o..;"
     "AC ..x.|..x.|..x.|..x.",
     "CH xx.X|xx.X|xx.X|xx2.X"},
    {"Acid Line 5", 16, S16, 0,
     "BD X...|X...|X...|X..o;"
     "CH oxXx|oxXx|oxXx|oxXx;"
     "SD ....|x...|....|x...;"
     "RS ...x|....|..x.|....",
     "RS ...x|..x.|..x.|.x.."},
    {"Acid Line 6", 16, S16, 0,
     "BD X...|X..o|X...|X.o.;"
     "CH x..X|x..X|x..X|x..X;"
     "OH ..x.|..x.|..x.|..x.;"
     "SD ....|x...|....|x...;"
     "RS ..o.|....|..o.|....;"
     "AC ..x.|..x.|..x.|..x.",
     "BD X...|X..o|X...|X.xx"},
    {"Squelch Shuffle", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CH oxX.|oxX.|oxX.|oxX.;"
     "OH ...x|...x|...x|...x;"
     "CP ....|x...|....|x...;"
     "RS ..x.|....|.x..|..x.",
     "RS ..x.|..x.|.x..|..xx"},
    {"Rubber Acid", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CH ..X.|..X.|..X.|..X.;"
     "SD ....|x...|....|x...;"
     "MT ...x|....|...x|....",
     "MT ...x|..x.|...x|..x."},
    {"Acid Storm", 32, S16, 0,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "CH xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "RD ....|....|....|....|x.x.|x.x.|x.x.|x.x.;"
     "AC ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.",
     "SD ....|....|....|....|....|....|o.o.|x3x3X4X4"},
}),
    {"Acid Snare Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CH xxXx|xxXx|....|....;"
     "SD ....|....|xxxx|XXXX;"
     "AC ..x.|..x.|..x.|xxxx"});

// ---- Bank I, group 2: Electro ------------------------------------------------------
constexpr Group kElectro = group(std::to_array<PatternDef>({
    {"Body Pop", 16, S16, 0,
     "BD X..x|....|X.x.|..x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x..o;"
     "CH xoxo|xoxo|xoxo|xox.;"
     "OH ....|....|....|...x;"
     "RS ..o.|....|....|.o..;"
     "AC x...|....|x...|....",
     "BD X..x|....|X.x.|.xx.;"
     "HT ....|....|....|..xx"},
    {"Robot Electro", 16, S16, 0,
     "BD X..x|..x.|..x.|....;"
     "SD ....|X...|....|X...;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "HT ....|....|....|..x.;"
     "MT ....|....|....|...x;"
     "AC x...|....|....|....",
     "BD X..x|..x.|x.x.|.x.x;"
     "HT ....|....|..x.|..x."},
    {"Pop And Lock", 16, S16, 2,
     "BD X...|..x.|x...|..x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH x.xx|x.xx|x.xx|x.xx;"
     "RS ...x|....|...x|....",
     "RS ...x|..x.|...x|..xx"},
    {"Freestyle Glide", 16, S16, 0,
     "BD X..x|..x.|X...|x.x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x..o|....|x...;"
     "CH xxxx|xxxx|xxxx|xx.x;"
     "OH ....|....|....|..x.;"
     "HT ..x.|....|..x.|....;"
     "AC x...|....|x...|....",
     "BD X..x|..x.|X..x|..xx;"
     "HT ..x.|....|..x.|.x.."},
    {"Miami Drive", 16, S16, 0,
     "BD X..x|..x.|..x.|.x..;"
     "CP ....|X...|....|X...;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "AC x...|....|x...|....",
     "BD X..x|..x.|x.x.|.x.x;"
     "CH xoxo|xoxo|xoxo|xox3o"},
    {"Breakdance", 16, S16, 0,
     "BD X.x.|...x|..x.|....;"
     "SD ....|X..o|.o..|X...;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "AC ....|x...|....|x...",
     "BD X.x.|...x|..xx|..x.;"
     "SD ....|X..o|.o..|X.o.;"
     "CH x.x.|x.x.|x.x.|x...;"
     "OH ....|....|....|..x."},
}),
    {"Electro Tom Fill", 16, S16, 0,
     "BD X...|..x.|....|....;"
     "SD ....|X...|....|....;"
     "HT ....|....|x3.x3.|....;"
     "MT ....|....|....|x3...;"
     "LT ....|....|....|..x3X;"
     "CH x.x.|x.x.|....|....;"
     "AC ....|....|x...|x..."});

// ---- Bank I, group 3: Long-form acid ---------------------------------------------------
constexpr Group kAcidLong = group(std::to_array<PatternDef>({
    // Four bars: hats and kick alone, the clap comes in on bar 2, rims on bar 3.
    {"Acid Journey", 64, S16, 0,
     "BD X...|X...|X...|X...|X...|X...|X...|X...|X...|X...|X...|X...|X...|X...|X...|X.x.;"
     "CH xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx;"
     "CP ....|....|....|....|....|x...|....|x...|....|x...|....|x...|....|x...|....|x...;"
     "RS ....|....|....|....|....|....|....|....|...x|....|..x.|....|...x|..x.|..x.|.x..;"
     "AC ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x."},
    {"Acid Breakdown", 32, S16, 0,
     "BD X...|....|....|....|X...|....|....|....;"
     "CH xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx|xxXx;"
     "CP ....|....|....|....|....|....|....|x...;"
     "AC ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.",
     "BD X...|....|....|....|X...|....|X...|X..."},
    {"Squelch Ratchet", 16, S16, 0,
     "BD X..o|X...|X..o|X...;"
     "CH x2x.x|x2x.x|x2x.x|x3x.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS ....|..x.|....|.x2..;"
     "CP ....|x...|....|x...;"
     "AC ..x.|..x.|..x.|..x.",
     "CH x2x.x|x2x.x|x3x.x|x4x4.x4",
     24, 100, 50, 100},
    {"Acid Polymeter", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CH@ xxX;"
     "CP ....|x...|....|x...;"
     "RS@ ..x..x.x.."},
}),
    {"Acid Roll Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|....|o.o.|o2o2x3X4;"
     "CH xxXx|xxXx|xxXx|....;"
     "AC ..x.|..x.|..x.|....",
     {}, 24, 100, 30, 100});

constexpr BankDefs kDefs = memory(kAcid, kElectro, kAcidLong, kNoGroup, kNoGroup, kNoGroup);

constexpr MemoryDef kMemory{
    {"Acid & Electro", "Acid", 128.0f},
    false,
    &kDefs,
    {{
        "1.1*4 1.2*4 1.3*3 1.16 1.4*4 1.1*3 1.16",
        "2.1*4 2.2*3 2.16 2.6*4 2.4*3 2.16",
        // Acid Journey is four bars long: one entry, four bars.
        "3.2*2 3.1 3.3*3 3.16 3.1",
        "",
        "",
        "",
        "",
        "",
    }},
    "1.1*2 1.2 1.16",
};

static_assert(checkMemory(kMemory) == 0, "Acid & Electro: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& acidElectroMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
