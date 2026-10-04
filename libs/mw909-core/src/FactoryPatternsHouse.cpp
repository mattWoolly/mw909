// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 1, "House Sessions": house in the extended format (weak
// ghost notes, A/B variations, rolls, two-bar patterns, probability).
//
//   Bank I   group 1  Chicago jack and warehouse
//            group 2  Deep and late-night
//            group 3  Piano era and garage house
//   Bank II  group 1  Tribal and percussive

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: Chicago jack --------------------------------------------
constexpr Group kJack = group(std::to_array<PatternDef>({
    {"Cellar Jack", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH Xo.o|xo.o|Xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS ...x|..x.|....|.x..;"
     "AC x...|....|x...|....",
     "RS ...x|..x.|..x.|.x.x;"
     "CP ....|x...|....|x..o"},
    {"Jackin' Sixteens", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CP ....|X...|....|X..o;"
     "CH oxXx|oxX.|oxXx|oxX.;"
     "OH ....|...x|....|...x;"
     "SD ....|....|.o..|..o.;"
     "AC ..x.|..x.|..x.|..x.",
     "CH oxXx|oxX.|oxXx|ox..;"
     "OH ....|...x|....|..xx;"
     "SD ....|....|.o..|..oo"},
    {"Rimshot Bounce", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "RS ..x.|.x..|x..x|..x.;"
     "CP ....|x...|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.",
     "RS ..x.|.x..|x..x|.xx.;"
     "LT ....|....|....|...x"},
    {"Two-Bar Jam", 32, S16, 4,
     "BD X...|X...|X...|X...|X...|X...|X...|X..x;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "CH xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "RS ....|....|....|....|...x|..x.|...x|....;"
     "HT ....|....|....|....|....|....|....|..x.;"
     "AC x...|....|....|....|x...|....|....|....",
     "CP ....|x...|....|x...|....|x...|....|x.x3."},
    {"Acid Jack", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CH xxXx|xxXx|xxXx|xxXx;"
     "CP ....|x...|....|x...;"
     "RS ...x|....|...x|....;"
     "AC ..x.|..x.|..x.|..x.",
     "BD X...|X...|X...|X.x.;"
     "CH xxXx|xxXx|xxXx|xX2Xx"},
    {"Clap Stack", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CP ..o.|X...|..o.|X..o;"
     "SD ....|x...|....|x...;"
     "CH x..x|x..x|x..x|x..x;"
     "OH ..x.|..x.|..x.|..x.",
     "CP ..o.|X...|..o.|X.x2x2"},
}),
    {"Jack Roll Fill", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|x.x.|x2x3x4X;"
     "SD ....|....|o.o.|xxxx;"
     "CH xo.o|xo.o|....|....;"
     "OH ..x.|..x.|....|....;"
     "AC ....|....|....|...x",
     {}, 24, 100, 50, 100});

// ---- Bank I, group 2: Deep -----------------------------------------------------
constexpr Group kDeep = group(std::to_array<PatternDef>({
    {"Deep Pocket", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "RS ..o.|x..o|..o.|x...;"
     "CH .o.x|.o.x|.o.x|.o.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...;"
     "SD ....|....|...o|.o..",
     "RS ..o.|x..o|..o.|x.oo"},
    {"Late Hours", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "SD ....|..o.|....|.o..;"
     "CH x.o?.|x.o?.|x.o?.|x.o?.;"
     "RD ..x.|..x.|..x.|..x.",
     "SD ....|..o.|..o.|.o.o"},
    {"Soulful Shuffle", 16, S16, 8,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "RS ...x|....|..x.|....;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "AC ....|x...|....|x...",
     "CH xoxo|xoxo|xoxo|xo.o;"
     "OH ....|....|....|..x."},
    {"Dub Chord Pulse", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CH o.x.|o.x.|o.x.|o.x.;"
     "RS ....|..x.|....|x..x;"
     "CP ....|....|....|x...",
     "RS ....|..x.|...x|x..x"},
    {"Sunrise Ride", 32, S16, 4,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "RD x.xo|x.xo|x.xo|x.xo|x.xo|x.xo|x.xo|x.xo;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "RS ....|....|...x|....|....|....|..x.|.x..",
     "CR x...|....|....|....|....|....|....|...."},
}),
    {"Deep Rim Fill", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "RS ....|x..o|.o.o|x2x2x3x4;"
     "CH .x.o|.x.o|....|....;"
     "OH ..x.|..x.|....|....",
     {}, 24, 100, 40, 100});

// ---- Bank I, group 3: Piano era and garage house ----------------------------------
constexpr Group kPiano = group(std::to_array<PatternDef>({
    {"Piano Anthem", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|X...|....|X...;"
     "SD ....|x...|..o.|x..o;"
     "CH x..x|x..x|x..x|x..x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x...|x...|x...|x...;"
     "AC x...|....|x...|....",
     "SD ....|x...|..o.|x.oo;"
     "CR x...|....|....|...."},
    {"Euphoric Piano", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "RD x.x.|x.x.|x.x.|x.x.;"
     "CH x...|x...|x...|x...;"
     "OH ..x.|..x.|..x.|..x.",
     "RD x.x.|x.x.|x.x.|xxxx"},
    {"Garage Organ", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "SD ....|...o|....|..o.;"
     "RS ..x.|....|..x.|..x.;"
     "CH x..x|x..x|x..x|x..x;"
     "OH ..x.|..x.|..x.|..x.",
     "SD ....|...o|.o..|..oo;"
     "RS ..x.|...x|..x.|..x."},
    {"Diva Shuffle", 16, S16, 8,
     "BD X...|X...|X...|X...;"
     "CP ....|X...|....|X...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "SD ....|....|....|...o;"
     "RD x...|x...|x...|x...",
     "SD ....|..o.|....|.o.o"},
    {"Handclap Heaven", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CP ....|X..x|....|X.x.;"
     "SD ....|x...|....|x...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.",
     "CP ....|X..x|..x.|X.x2x2"},
}),
    {"Piano Roll Fill", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "SD ....|x...|o.o.|x2x3X4X;"
     "CP ....|x...|....|....;"
     "CH xx.x|xx.x|....|....;"
     "OH ..x.|..x.|....|....",
     {}, 24, 100, 50, 100});

// ---- Bank II, group 1: Tribal ----------------------------------------------------
constexpr Group kTribal = group(std::to_array<PatternDef>({
    {"Tribal Drums", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "HT ...x|..x.|...x|....;"
     "MT .x..|....|.x..|..x.;"
     "LT ..x.|.x..|..x.|.x.x;"
     "CH ..x.|..x.|..x.|..x.;"
     "AC x...|....|x...|....",
     "LT ..x.|.x..|..x.|.xX4."},
    {"Tom Ritual", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "HT x..x|..x.|x..x|..x.;"
     "MT ..o.|.o..|..o.|.o.o;"
     "LT ....|...x|....|...x;"
     "CH ..x.|..x.|..x.|..x.",
     "HT x..x|..x.|x..x|.xxX"},
    {"Hand Drums", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "HT ..x.|.oxo|..x.|.ox.;"
     "MT o...|o...|o...|o..x;"
     "RS x..x|..x.|..x.|x...;"
     "OH ..x.|..x.|..x.|..x.",
     "RS ..x.|x...|x..x|..x."},
    {"Tribal Rolls", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "LT .x..|.x..|.x..|.x.x;"
     "MT ..x2.|....|..x2.|..x.;"
     "HT ....|..x.|....|x...;"
     "CP ....|x...|....|x...;"
     "RD ..x.|..x.|..x.|..x.",
     "HT ....|..x.|..x.|x3x3x3x4"},
    {"Drum Circle", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "LT ..x?.|.x..|..x?.|.x..;"
     "MT .x..|x?...|.x..|x?...;"
     "HT ...x?|..x.|...x?|..x?.;"
     "RS x..x|..x.|..x.|x...;"
     "OH ..x.|..x.|..x.|..x."},
}),
    {"Tribal Tom Fill", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "HT x.x.|....|xxxx|....;"
     "MT ....|....|....|xx..;"
     "LT ....|....|....|..xX;"
     "AC ....|....|....|...x"});

constexpr BankDefs kDefs = memory(kJack, kDeep, kPiano, kTribal, kNoGroup, kNoGroup);

constexpr MemoryDef kMemory{
    {"House Sessions", "House", 124.0f},
    false,
    &kDefs,
    {{
        "1.1*4 1.2*3 1.16 1.4*2 1.5*3 1.16 1.1*3 1.16",
        "2.1*4 2.2*3 2.16 2.5*2 2.3*3 2.16 2.1*4",
        "3.1*4 3.2*3 3.16 3.5*4 3.1*3 3.16",
        "",
        "4.1*4 4.2*3 4.16 4.3*4 4.4*3 4.16",
        // A mix across the memory: jack into piano, out through tribal.
        "1.1*4 1.6*4 3.1*3 3.16 3.5*4 4.4*3 4.16",
        "",
        "",
    }},
    "",
};

static_assert(checkMemory(kMemory) == 0, "House Sessions: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& houseMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
