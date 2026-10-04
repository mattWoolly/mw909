// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 2, "Techno Floor": techno in the extended format.
//
//   Bank I   group 1  Detroit: syncopated hats, claps, melodic toms
//            group 2  Minimal: sparse kits, polymeter rims and hats, per-track swing
//            group 3  Peak time: rides, rumble, two-bar builds
//   Bank II  group 1  Hard techno
//            group 2  EBM / body music (115..125 BPM)
//            group 3  Industrial (125..135 BPM)

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: Detroit --------------------------------------------------
constexpr Group kDetroit = group(std::to_array<PatternDef>({
    {"Assembly Line", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH xx.x|xo.x|xx.x|xo.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS ...x|....|..x.|....;"
     "AC x...|....|x...|....",
     "RS ...x|..x.|..x.|.x..;"
     "HT ....|....|....|..x."},
    {"Soul Machine", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "RD x.x.|x.x.|x.x.|x.x.;"
     "OH ..x.|..x.|..x.|..x.;"
     "MT ...x|....|...x|....;"
     "LT ....|..x.|....|.x..",
     "LT ....|..x.|....|.xx.;"
     "MT ...x|....|...x|...x"},
    {"Late Shift", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "HT ..x.|....|....|.x..;"
     "RS ....|.x..|x...|..x.",
     "CP ....|x...|....|x.x."},
    {"Chrome Groove", 32, S16, 2,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "CH xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "LT ....|....|...x|....|....|..x.|....|.x..;"
     "MT ....|..x.|....|....|...x|....|....|..x.;"
     "AC x...|....|....|....|x...|....|....|....",
     "CR x...|....|....|....|....|....|....|...."},
    {"Endless Ride", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "RD Xoxo|xoxo|Xoxo|xoxo;"
     "CP ....|x...|....|x...;"
     "RS ..x.|....|....|..x.",
     "CP ....|x...|..x.|x..."},
}),
    {"Detroit Tom Fill", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|..x.|x.xx;"
     "HT ....|....|x...|....;"
     "MT ....|....|..x.|x...;"
     "LT ....|....|....|..xX;"
     "CH xo.o|xo.o|....|....;"
     "OH ..x.|..x.|....|...."});

// ---- Bank I, group 2: Minimal ----------------------------------------------------
constexpr Group kMinimal = group(std::to_array<PatternDef>({
    {"Minimal Tick", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "CH ..x.|..x.|..x.|..x.;"
     "RS@ ...x|..x.|..x;"
     "CP ....|....|....|x...",
     "CP ....|....|....|x..o"},
    {"Micro Groove", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CH o?.x.|o?.x.|o?.x.|o?.x.;"
     "RS@ ..x..x.x..x..;"
     "CP ....|x...|....|...."},
    {"Five Against Four", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "MT@ x..x.;"
     "LT@ ..x...x;"
     "CH ..x.|..x.|..x.|..x."},
    {"Rubber Loop", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "RS@+10 ..x..x.x..;"
     "CH@ ..x;"
     "CP ....|....|....|x..."},
    {"Skeleton Key", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "RS x..x|..x.|....|..x.;"
     "RD ..x?.|..x?.|..x?.|..x?.",
     "RS x..x|..x.|..o.|..x."},
}),
    {"Minimal Rim Fill", 16, S16, 4,
     "BD X...|X...|X...|....;"
     "RS ..x.|..x.|x2.x2.|x3x3x3x4;"
     "CH ..x.|..x.|..x.|....",
     {}, 24, 100, 40, 100});

// ---- Bank I, group 3: Peak time ------------------------------------------------------
constexpr Group kPeak = group(std::to_array<PatternDef>({
    {"Peak Time", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "SD ....|...o|....|..o.;"
     "CH xo.o|xo.o|xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x..x|..x.|x..x|..x.;"
     "AC ..x.|..x.|..x.|..x.",
     "CP ....|x...|....|x..x;"
     "CR x...|....|....|...."},
    {"Main Room", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CP ....|X...|....|X...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x.xo|x.xo|x.xo|x.xo",
     "SD ....|....|....|.oxx"},
    {"Rolling Rumble", 16, S16, 0,
     "BD X...|X...|X...|X..o;"
     "LT ..o.|..o.|..o.|..o.;"
     "CP ....|x...|....|x...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.",
     "BD X...|X...|X...|X.oo"},
    {"Hypnotic Loop", 32, S16, 2,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "CH ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "RS@ ..x..x..x.;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "AC x...|....|....|....|....|....|....|....",
     "CP ....|x...|....|x...|....|x...|....|x.x."},
    {"Warehouse Ride", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "RD Xxxx|Xxxx|Xxxx|Xxxx;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...",
     "RD Xxxx|Xxxx|Xxxx|Xx2xx"},
}),
    {"Peak Snare Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|o.o.|oooo|x2x2X3X4;"
     "CP ....|x...|....|....;"
     "CH xo.o|xo.o|....|....;"
     "OH ..x.|..x.|....|....",
     {}, 24, 100, 40, 100});

// ---- Bank II, group 1: Hard techno ------------------------------------------
constexpr Group kHard = group(std::to_array<PatternDef>({
    {"Hard Groove", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "RS ..x.|.x..|x..x|..x.;"
     "LT ...x|..x.|...x|....;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...",
     "LT ...x|..x.|...x|.xx."},
    {"Rolling Mill", 16, S16, 0,
     "BD X..o|X...|X..o|X...;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH xxxx|xxxx|xxxx|xxxx;"
     "LT ..x.|....|..x.|..x.",
     "SD ....|X...|....|X.x3x3"},
    {"Distorted Stomp", 16, S16, 0,
     "BD X...|X..x|X...|X.x.;"
     "SD ....|x...|....|x...;"
     "CP ....|x...|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC x...|x...|x...|x...",
     "BD X...|X..x|X..x|X.xx"},
    {"Pressure Plant", 32, S16, 0,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "LT ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "CH xoxo|xoxo|xoxo|xoxo|xoxo|xoxo|xoxo|xoxo;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "AC x...|....|....|....|x...|....|....|....",
     "LT ..x.|..x.|..x.|..x.|..x.|..x.|..x.|xxxX"},
    {"Concrete Loop", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "RS@ x..x..x..x.x;"
     "CP ....|x...|....|x...;"
     "CH ..x.|..x.|..x.|..x.;"
     "HT@ ..x..x.",
     "CP ....|x...|..o.|x..."},
}),
    {"Hard Kick Fill", 16, S16, 0,
     "BD X...|X...|X.X.|XXXX;"
     "SD ....|x...|x.x.|xxxx;"
     "CH xxxx|xxxx|....|....;"
     "AC ....|....|....|x.x."});

// ---- Bank II, group 2: EBM ----------------------------------------------------------------------
constexpr Group kEbm = group(std::to_array<PatternDef>({
    {"Body Drill", 16, S16, 0,
     "BD X..x|X...|X..x|X...;"
     "SD ....|X...|....|X...;"
     "CH x.xx|x.xx|x.xx|x.xx;"
     "AC x...|x...|x...|x...",
     "BD X..x|X...|X..x|X.xx"},
    {"Hammer Step", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|X...|..o.|X.x~.;"
     "CH ..x.|..x.|..x.|..x.;"
     "LT ...x|....|...x|....;"
     "AC x...|....|x...|....",
     "SD ....|X...|..o.|X.x~x~;"
     "LT ...x|....|...x|..xx",
     32, 70},
    {"Steel Body", 16, S16, 0,
     "BD X..x|..x.|X..x|..x.;"
     "SD ....|X...|....|X...;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "RS ....|..x.|....|..x.",
     "BD X..x|..x.|X..x|.xx."},
    {"Military Snare", 16, S16, 0,
     "BD X...|....|X...|....;"
     "SD x~.x.|X...|x~.x.|X...;"
     "CH x.x.|x.x.|x.x.|x.x.",
     "SD x~.x.|X...|x~.x.|X.x3x3",
     28, 60},
}),
    {"EBM Snare Fill", 16, S16, 0,
     "BD X...|..x.|X...|....;"
     "SD ....|X...|x.x.|xxxx;"
     "LT ....|....|....|..xX;"
     "CH x.x.|x.x.|....|...."});

// ---- Bank II, group 3: Industrial --------------------------------------------------------------
constexpr Group kIndustrial = group(std::to_array<PatternDef>({
    {"Lathe Works", 16, S16, 0,
     "BD X..x|X..x|X..x|X..x;"
     "SD ....|X...|....|X...;"
     "RS x.x.|x.x.|x.x.|x.x.;"
     "AC x...|....|x...|....",
     "RS x.x.|x.x.|x.x.|x3x3x3x3"},
    {"Pistons", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "LT ..x.|..x.|..x.|..x.;"
     "MT ...x|...x|...x|...x;"
     "CP ....|x...|....|x...",
     "MT ...x|...x|...x|.x.x"},
    {"Rust Belt", 16, S16, 2,
     "BD X...|X..o|X...|X..o;"
     "SD ....|x...|....|x...;"
     "CH x2.x2.|x2.x2.|x2.x2.|x2.x2.",
     "CH x2.x2.|x2.x2.|x2.x2.|x3x3x4x4"},
    {"Noise Ritual", 16, S16, 0,
     "BD X...|....|X...|....;"
     "SD ....|X...|....|X...;"
     "HT ...x|..x.|...x|..x.;"
     "CR x...|....|....|....",
     "CR ....|....|....|....;"
     "RD x.x.|x.x.|x.x.|x.x."},
}),
    {"Industrial Fill", 16, S16, 0,
     "BD X..x|X..x|X.xx|XXXX;"
     "SD ....|X...|x.x.|x4x4X4X4;"
     "AC ....|....|....|x...",
     {}, 24, 100, 50, 100});

constexpr BankDefs kDefs = memory(kDetroit, kMinimal, kPeak, kHard, kEbm, kIndustrial);

constexpr MemoryDef kMemory{
    {"Techno Floor", "Techno", 132.0f},
    false,
    &kDefs,
    {{
        "1.1*4 1.2*3 1.16 1.4*2 1.3*3 1.16 1.1*4",
        "2.1*8 2.3*3 2.16 2.4*4 2.2*3 2.16",
        "3.1*4 3.2*4 3.3*3 3.16 3.5*4 3.1*3 3.16",
        "",
        "4.1*4 4.3*3 4.16 4.2*4 4.4 4.5 4.16",
        // From minimal to the main room.
        "2.1*4 2.5*4 1.1*4 3.1*3 3.16 3.3*3 3.16",
        // EBM.
        "5.1*4 5.2*3 5.16 5.3*4 5.4*3 5.16",
        // Industrial.
        "6.1*4 6.2*3 6.16 6.3*4 6.4*3 6.16",
    }},
    "",
};

static_assert(checkMemory(kMemory) == 0, "Techno Floor: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& technoMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
