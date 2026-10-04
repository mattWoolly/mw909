// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 5, "Hard & Fast": gabber and hardcore, written for
// 150..200 BPM and a kick driven into distortion (the "Gabber Drive" kit).
// (EBM and industrial, at 115..135 BPM, live in "Techno Floor", so this
// memory's own tempo suits everything in it.)
//
//   Bank I   group 1  Gabber (170..200 BPM)
//            group 2  Hardcore, rave and hardstyle (150..175 BPM)

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: Gabber ------------------------------------------------------------
constexpr Group kGabber = group(std::to_array<PatternDef>({
    {"Gabber Stomp", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC x...|x...|x...|x...",
     "BD X...|X...|X...|X.x."},
    {"Port City Kick", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CH xxxx|xxxx|xxxx|xxxx;"
     "AC x...|x...|x...|x...",
     "SD ....|x...|....|x.x3x3"},
    {"Hakken Floor", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|X...|....|X...;"
     "CH x..x|x..x|x..x|x..x;"
     "RD ..o.|..o.|..o.|..o.;"
     "AC ..x.|..x.|..x.|..x.",
     "BD X...|X...|X...|X.xx"},
    // Written for 200 BPM: 16th-note kicks with the total accent on the beat.
    {"Terror Pulse", 16, S16, 0,
     "BD XxXx|XxXx|XxXx|XxXx;"
     "SD ....|X...|....|X...;"
     "AC x...|x...|x...|x...",
     "BD XxXx|XxXx|XxXx|X4X4X4X4"},
    {"Gabber Break", 16, S16, 0,
     "BD X...|X..x|X...|X.x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "OH ..x.|..x.|..x.|..x.",
     "BD X...|X..x|X..x|X.xx"},
}),
    {"Kick Roll Fill", 16, S16, 0,
     "BD X...|X.X.|XXXX|X2X2X3X4;"
     "OH ..x.|....|....|....;"
     "CP ....|x...|....|....;"
     "AC ....|....|x.x.|xxxx",
     {}, 24, 100, 60, 100});

// ---- Bank I, group 2: Hardcore -------------------------------------------------------------
constexpr Group kHardcore = group(std::to_array<PatternDef>({
    {"Happy Hardcore", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...;"
     "CH x..x|x..x|x..x|x..x;"
     "SD ....|....|....|..x.",
     "SD ....|....|..x.|x.xx"},
    {"Rave Stab Beat", 16, S16, 2,
     "BD X.x.|....|..x.|....;"
     "SD ....|X..o|.o..|X...;"
     "CH xxxx|xxxx|xxxx|xxx.;"
     "OH ....|....|....|...x",
     "SD ....|X..o|.o.o|X.xx"},
    {"Reverse Bass Kick", 16, S16, 0,
     "BD X.oo|X.oo|X.oo|X.oo;"
     "CP ....|X...|....|X...;"
     "CH x...|x...|x...|x...;"
     "OH ..x.|..x.|..x.|..x.",
     "BD X.oo|X.oo|X.oo|X.XX;"
     "CP ....|X...|....|X..x"},
    {"Hard Pump", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CP ....|x...|....|x...;"
     "CH xoxo|xoxo|xoxo|xoxo;"
     "RD ..x.|..x.|..x.|..x.",
     "SD ....|x...|....|xxxx"},
}),
    {"Hardcore Snare Fill", 16, S16, 0,
     "BD X...|X...|X...|X...;"
     "SD ....|....|xxxx|x2x2x4x4;"
     "OH ..x.|..x.|....|....;"
     "CP ....|x...|....|....;"
     "AC ....|....|....|x.x."});

constexpr BankDefs kDefs = memory(kGabber, kHardcore, kNoGroup, kNoGroup, kNoGroup, kNoGroup);

constexpr MemoryDef kMemory{
    {"Hard & Fast", "Hardcore", 180.0f},
    false,
    &kDefs,
    {{
        "1.1*4 1.2*3 1.16 1.4*4 1.3*3 1.16",
        "2.1*7 2.16 2.2*4 2.3*3 2.16",
        "",
        "",
        "",
        "",
        "",
        "",
    }},
    "",
};

static_assert(checkMemory(kMemory) == 0, "Hard & Fast: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& hardMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
