// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 4, "Shuffle & Breaks": the swung end of the machine.
// Shuffle is in 1/96-beat ticks of delay on the off-beat 16ths: 4 is a light
// push, 8 a lazy hip-hop swing, 10..12 the heavy two-step skip (12 is the
// hardware's strongest setting, a full triplet feel).
//
//   Bank I   group 1  UK garage and two-step (heavy shuffle, skipping kicks)
//            group 2  Speed garage and four-to-the-floor garage
//            group 3  Breaks and big beat
//   Bank II  group 1  Hip-hop and boom-bap

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: UK garage / two-step ------------------------------------------
constexpr Group kTwoStep = group(std::to_array<PatternDef>({
    {"Two-Step Shuffle", 16, S16, 10,
     "BD X...|...x|..x.|....;"
     "SD ....|X...|....|X...;"
     "RS ..o.|....|.o..|..o.;"
     "CH xo.o|xo.o|xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC ....|x...|....|x...",
     "BD X...|...x|..x.|.x..;"
     "SD ....|X..o|....|X..."},
    {"Garage Skip", 16, S16, 12,
     "BD X...|....|..x.|.x..;"
     "SD ....|X...|....|X...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|....|..x.|....;"
     "RS ....|..o.|...o|....",
     "BD X...|...x|..x.|.x..;"
     "CH .x.x|.x.x|.x.x|.x3.x"},
    {"Bumpy Two-Step", 16, S16, 10,
     "BD X..x|....|..x.|....;"
     "SD ....|X...|....|X...;"
     "RS ..x.|..x.|....|.x..;"
     "CH xoxo|xoxo|xoxo|xoxo",
     "BD X..x|....|..x.|..x."},
    // The hats swing harder than the rest: their own shuffle of 12 ticks
    // against the pattern's 8.
    {"Shuffled Rims", 16, S16, 8,
     "BD X...|...x|..x.|....;"
     "RS ..x.|x..x|..x.|x...;"
     "CP ....|x...|....|x...;"
     "CH+12 xoxo|xoxo|xoxo|xoxo",
     "RS ..x.|x..x|..xo|x..o"},
    {"Dark Garage", 16, S16, 10,
     "BD X...|....|..x.|....;"
     "SD ....|X...|....|X...;"
     "CP ....|X...|....|X...;"
     "CH ..x.|.xx.|..x.|.x..;"
     "OH ....|....|....|..x.",
     "BD X...|....|..x.|..xx"},
    {"Two-Step Roller", 32, S16, 10,
     "BD X...|...x|..x.|....|X...|....|..x.|.x..;"
     "SD ....|X...|....|X...|....|X...|....|X..o;"
     "CH xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "RS ....|..x.|....|....|....|..x.|...o|...."},
}),
    {"Garage Snare Fill", 16, S16, 10,
     "BD X...|...x|....|....;"
     "SD ....|X...|o.o.|x2xX2X;"
     "CH xo.o|xo.o|....|....;"
     "OH ..x.|..x.|....|....",
     {}, 24, 100, 60, 100});

// ---- Bank I, group 2: Speed garage ------------------------------------------------------
constexpr Group kSpeed = group(std::to_array<PatternDef>({
    {"Speed Garage", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CH xo.x|xo.x|xo.x|xo.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS ....|..x.|.x..|...x;"
     "AC ..x.|..x.|..x.|..x.",
     "SD ....|x...|....|x.oo;"
     "RS ....|..x.|.x.x|...x"},
    {"Bass Bin Bounce", 16, S16, 6,
     "BD X...|X...|X...|X..x;"
     "SD ....|x..o|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.",
     "SD ....|x..o|..o.|x.o."},
    {"Four-Four Skippy", 16, S16, 8,
     "BD X...|X...|X..o|X...;"
     "CP ....|x...|....|x...;"
     "CH ox.o|ox.o|ox.o|ox.o;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS ...x|..x.|...x|..x.;"
     "SD ....|....|..o.|...o",
     "CH ox.o|ox.o|ox.o|ox3.o"},
}),
    {"Speed Clap Fill", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|x.x.|xxxx;"
     "CH xo.o|xo.o|....|....;"
     "OH ..x.|..x.|....|...."});

// ---- Bank I, group 3: Breaks and big beat ---------------------------------------------------
constexpr Group kBreaks = group(std::to_array<PatternDef>({
    {"Big Beat Smash", 16, S16, 2,
     "BD X.x.|...x|..x.|.x..;"
     "SD ....|X..o|....|X...;"
     "CP ....|x...|....|x...;"
     "CH xxxx|xxxx|xxxx|xx..;"
     "OH ....|....|....|..x.;"
     "CR x...|....|....|....;"
     "AC x...|x...|....|x...",
     "BD X.x.|...x|..xx|.x.x;"
     "SD ....|X..o|...o|X.x3x3"},
    {"Breakbeat Funk", 16, S16, 4,
     "BD X.x.|..x.|..x.|....;"
     "SD ....|X.o.|.o..|X.oo;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "OH ....|...x|....|...x",
     "SD ....|X.o.|.o.o|X.oo;"
     "BD X.x.|..x.|..x.|.x.."},
    {"Stomp Break", 16, S16, 2,
     "BD X..x|..x.|..x.|....;"
     "SD ....|X...|.o..|X...;"
     "CP ....|x...|....|x...;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "AC x...|....|....|....",
     "BD X..x|..x.|..x.|..xx"},
    {"Nu Skool Break", 16, S16, 0,
     "BD X...|...x|..x.|....;"
     "SD ....|X...|....|X.o.;"
     "CH x.xx|x.xx|x.xx|x.xx",
     "CH x.xx|x.xx|x.xx|x2.x2x"},
    {"Rolling Break", 32, S16, 4,
     "BD X.x.|....|..x.|....|X.x.|....|..x.|.x..;"
     "SD ....|X..o|.o..|X...|....|X..o|.o.o|X..o;"
     "CH x.xo|x.xo|x.xo|x.xo|x.xo|x.xo|x.xo|x.x."},
}),
    {"Break Roll Fill", 16, S16, 4,
     "BD X...|..x.|X...|....;"
     "SD ....|X..o|o2.o2.|x3x3X4X4;"
     "HT ....|....|..x.|....;"
     "MT ....|....|...x|....;"
     "CH x.x.|x.x.|....|....;"
     "AC ....|....|....|x.x.",
     {}, 24, 100, 50, 100});

// ---- Bank II, group 1: Hip-hop -----------------------------------------------------------------
constexpr Group kHipHop = group(std::to_array<PatternDef>({
    {"Basement Bap", 16, S16, 8,
     "BD X.x.|....|..xX|....;"
     "SD ....|X...|....|X..o;"
     "RS ...o|....|.o..|....;"
     "CH x.o.|x.o.|x.o.|x.oo;"
     "AC ....|x...|....|x...",
     "BD X.x.|....|..xX|..x.;"
     "SD ....|X...|..o.|X..o"},
    {"Dusty Swing", 16, S16, 10,
     "BD X...|...x|..x.|.x..;"
     "SD ....|X...|....|X...;"
     "CH xoxo|xoxo|xoxo|xo.o;"
     "OH ....|....|....|..x.",
     "BD X...|...x|..xx|.x.."},
    {"Head Nod", 16, S16, 8,
     "BD X..x|..x.|..x.|....;"
     "SD ....|X...|....|X...;"
     "RS ....|....|....|...o;"
     "CH x.x.|x.x.|x.x.|x.x.",
     "CH x.x.|x.x.|x.x.|x.x3."},
    {"Lo-Fi Lean", 16, S16, 12,
     "BD X...|....|X.x.|....;"
     "SD ....|X...|....|X...;"
     "CH x.x?.|x.x?.|x.x?.|x.x?.;"
     "RS ...o|....|....|.o..",
     "SD ....|X..o|....|X..."},
    {"Dusty Crates", 16, S16, 8,
     "BD X..x|..x.|....|.x..;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH x.xo|x.xo|x.xo|x.xo",
     "BD X..x|..x.|..x.|.x.."},
}),
    {"Boom Bap Fill", 16, S16, 8,
     "BD X..x|....|..x.|....;"
     "SD ....|X...|..o.|XoXX;"
     "CH x.o.|x.o.|....|...."});

constexpr BankDefs kDefs = memory(kTwoStep, kSpeed, kBreaks, kHipHop, kNoGroup, kNoGroup);

constexpr MemoryDef kMemory{
    {"Shuffle & Breaks", "Garage", 134.0f},
    false,
    &kDefs,
    {{
        "1.1*4 1.2*3 1.16 1.3*4 1.6 1.4 1.16",
        "2.1*4 2.2*4 2.3*3 2.16",
        "3.1*4 3.2*3 3.16 3.3*4 3.4*3 3.16",
        "",
        "4.1*4 4.2*3 4.16 4.3*4 4.5*3 4.16",
        "",
        "",
        "",
    }},
    "",
};

static_assert(checkMemory(kMemory) == 0, "Shuffle & Breaks: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& shuffleBreaksMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
