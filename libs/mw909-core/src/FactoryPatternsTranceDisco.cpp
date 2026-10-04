// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 6, "Trance & Disco": the four-to-the-floor family with
// open-hat off-beats, at both ends of the tempo range.
//
//   Bank I   group 1  Trance (138..145 BPM), including a four-bar breakdown riser
//            group 2  Hard trance and progressive
//   Bank II  group 1  Italo, space disco and hi-NRG (118..130 BPM)
//            group 2  Disco and boogie (110..122 BPM)

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: Trance --------------------------------------------------------------
constexpr Group kTrance = group(std::to_array<PatternDef>({
    {"Trance Gate", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS x.xx|.x.x|x.xx|.xx.;"
     "AC ..x.|..x.|..x.|..x.",
     "RS x.xx|.x.x|x.xx|xxxx;"
     "CP ....|x...|....|x..x"},
    {"Euphoria Drive", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CP ....|X...|....|X...;"
     "OH ..x.|..x.|..x.|..x.;"
     "CH x..x|x..x|x..x|x..x;"
     "RD xoxo|xoxo|xoxo|xoxo",
     "SD ....|....|....|.ooo"},
    // Four bars of snare build for the breakdown: quarters, 8ths, 16ths, rolls.
    {"Breakdown Riser", 64, S16, 0,
     "BD X...|....|....|....|....|....|....|....|....|....|....|....|....|....|....|....;"
     "SD o...|o...|o...|o...|o.o.|o.o.|o.o.|o.o.|xxxx|xxxx|xxxx|xxxx|x2x2x2x2|x2x2x2x2|X3X3X3X3|X4X4X4X4;"
     "CR x...|....|....|....|....|....|....|....|....|....|....|....|....|....|....|....;"
     "AC ....|....|....|....|....|....|....|....|....|....|....|....|....|....|x...|x...",
     {}, 24, 100, 60, 100},
    {"Psy Gallop", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "LT .ooo|.ooo|.ooo|.ooo;"
     "OH ..x.|..x.|..x.|..x.;"
     "CH x..x|x..x|x..x|x..x;"
     "CP ....|x...|....|x...;"
     "AC ..x.|..x.|..x.|..x.",
     "LT .ooo|.ooo|.ooo|.o2o2o2;"
     "CP ....|x...|....|x..o"},
    {"Uplifter", 32, S16, 0,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "CP ....|x...|....|x...|....|x...|....|x...;"
     "OH ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "CH x..x|x..x|x..x|x..x|x..x|x..x|x..x|x..x;"
     "CR x...|....|....|....|....|....|....|....;"
     "RD ....|....|....|....|x.x.|x.x.|x.x.|x.x."},
}),
    {"Trance Snare Fill", 16, S16, 0,
     "BD X...|X...|X...|....;"
     "SD ....|o.o.|xxxx|x2x2X3X4;"
     "CP ....|x...|....|....;"
     "OH ..x.|..x.|....|....;"
     "CH x..x|x..x|....|....",
     {}, 24, 100, 50, 100});

// ---- Bank I, group 2: Hard trance and progressive -----------------------------------------------
constexpr Group kHardTrance = group(std::to_array<PatternDef>({
    {"Hard Trance", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CP ....|X...|....|X...;"
     "CH xxxx|xxxx|xxxx|xxxx;"
     "RD ..x.|..x.|..x.|..x.;"
     "AC x...|x...|x...|x...",
     "CP ....|X...|....|X.x."},
    {"Progressive Glide", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CH o.xo|o.xo|o.xo|o.xo;"
     "RS ....|..x.|....|.x..;"
     "CP ....|x...|....|x...",
     "RS ....|..x.|..x.|.x.."},
    {"Tech Trance", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CH xo.o|xo.o|xo.o|xo.o;"
     "OH ..x.|..x.|..x.|..x.;"
     "LT ...x|....|...x|....;"
     "MT ....|..x.|....|..x.;"
     "CP ....|x...|....|x...",
     "LT ...x|....|...x|.x.x"},
}),
    {"Hard Trance Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|....|x.x.|xxXX;"
     "CP ....|X...|....|....;"
     "CH xxxx|xxxx|....|....;"
     "AC ....|....|....|xxxx"});

// ---- Bank II, group 1: Italo, space disco, hi-NRG --------------------------------------------------
constexpr Group kItalo = group(std::to_array<PatternDef>({
    {"Neon Italo", 16, S16, 2,
     "BD X...|X...|X...|X..o;"
     "SD ....|X...|....|X...;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "HT ....|....|..x.|....;"
     "MT ....|....|....|x.x.;"
     "AC ..x.|..x.|..x.|..x.",
     "MT ....|....|....|x.xx;"
     "HT ....|....|..x.|.x.."},
    {"Space Disco", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "OH ...x|...x|...x|...x;"
     "LT ...x|....|...x|....;"
     "AC x...|....|x...|....",
     "SD ....|x...|....|x.xx;"
     "LT ...x|....|...x|..x."},
    {"Cosmic Toms", 16, S16, 2,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "MT ..x.|....|..x.|..x.;"
     "HT ....|...x|....|....;"
     "OH ..x.|..x.|..x.|..x.",
     "MT ..x.|....|..x.|..x3."},
    {"Platform Strut", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "SD ....|x..o|..o.|x...;"
     "CP ....|x...|....|x...;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x.xx|x.xx|x.xx|x.xx;"
     "AC ....|x...|....|x...",
     "SD ....|x..o|..o.|x.xo"},
    {"Hi-NRG Rush", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|x~...|....|x~...;"
     "CP ....|x...|....|x...;"
     "CH Xxxx|Xxxx|Xxxx|Xxxx;"
     "LT ..x.|..x.|..x.|..x.;"
     "AC x...|x...|x...|x...",
     "SD ....|x~...|....|x~.x~x~"},
}),
    {"Disco Roll Fill", 16, S16, 2,
     "BD X...|X...|X...|....;"
     "SD ....|x~...|....|x~x~x~x~;"
     "HT ....|....|x.x.|....;"
     "MT ....|....|.x.x|....;"
     "LT ....|....|....|x.x.;"
     "CH xx.x|xx.x|....|....;"
     "OH ..x.|..x.|....|....;"
     "AC ....|....|....|x..x",
     {}, 20, 70});

// ---- Bank II, group 2: Disco and boogie -------------------------------------------------------------
constexpr Group kBoogie = group(std::to_array<PatternDef>({
    {"Four-Four Disco", 16, S16, 4,
     "BD X...|X...|X...|X...;"
     "SD ....|x..o|....|x...;"
     "CP ....|x...|....|x...;"
     "CH xxx.|xxx.|xxx.|xxx.;"
     "OH ...x|...x|...x|...x;"
     "AC ....|x...|....|x...",
     "CP ....|x...|....|x..x"},
    {"Nu Disco Roll", 16, S16, 6,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH xo2xo|xo2xo|xo2xo|xo2xo;"
     "RS ..x.|.x..|..x.|.x.x;"
     "SD ....|....|...o|....",
     "CH xo2xo|xo2xo|xo2xo|xo3x3o3",
     24, 100, 50, 100},
    {"Glitter Floor", 32, S16, 4,
     "BD X...|X...|X...|X...|X...|X...|X...|X...;"
     "SD ....|x...|....|x...|....|x...|....|x...;"
     "CH xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo.o|xo..;"
     "OH ..x.|..x.|..x.|..x.|..x.|..x.|..x.|..x.;"
     "CR x...|....|....|....|....|....|....|....;"
     "HT ....|....|....|....|....|....|....|...x"},
    {"Boogie Bounce", 16, S16, 6,
     "BD X...|..x.|X...|..x.;"
     "SD ....|X...|....|X...;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "CP ....|x...|....|x...",
     "BD X...|..x.|X..x|..x."},
}),
    {"Boogie Tom Fill", 16, S16, 6,
     "BD X...|..x.|X...|....;"
     "SD ....|X...|....|....;"
     "HT ....|....|x.x.|....;"
     "MT ....|....|.x.x|x...;"
     "LT ....|....|....|.xxX;"
     "CH xoxo|xoxo|....|...."});

constexpr BankDefs kDefs = memory(kTrance, kHardTrance, kNoGroup, kItalo, kBoogie, kNoGroup);

constexpr MemoryDef kMemory{
    {"Trance & Disco", "Trance", 138.0f},
    false,
    &kDefs,
    {{
        // Breakdown Riser is one four-bar entry, Uplifter one two-bar entry.
        "1.1*4 1.2*4 1.3 1.5 1.2 1.16 1.1*4",
        "2.2*4 2.1*7 2.16 2.3*3 2.16",
        "",
        "",
        "4.1*4 4.2*3 4.16 4.4*3 4.16",
        "5.1*4 5.3*2 5.2*3 5.16",
        "",
        "",
    }},
    "",
};

static_assert(checkMemory(kMemory) == 0, "Trance & Disco: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& tranceDiscoMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
