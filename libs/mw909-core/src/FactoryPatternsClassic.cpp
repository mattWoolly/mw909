// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory memory 0, "Classic Memory": the default memory, written only with
// what the original machine could store (the checker enforces it): 1..16
// steps in the four hardware scales, shuffle keys 1..7, flam keys, accent on
// BD SD toms and the closed hat only, flam on BD SD toms only and never with
// an accent, the TOTAL ACCENT row for everything else, no variations, song
// tracks that stay inside their own bank, and a chain entered the original's
// way (one run of adjoining patterns). Every group keeps a fill in slot 16,
// so auto fill always plays one that suits the groove, and the song tracks
// put each fill on the last bar of a 4-bar phrase.
//
//   Bank I   group 1  House: four to the floor, Chicago jack, deep, piano era, tribal, garage
//            group 2  Techno: Detroit, minimal, hard, industrial, peak time
//            group 3  Acid and electro
//   Bank II  group 1  Disco, Italo, hi-NRG and body music
//            group 2  Breaks, boom-bap and two-step (hardware shuffle)
//            group 3  The other scales: 32nds, both triplet scales, odd lengths, flam

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

// ---- Bank I, group 1: House -------------------------------------------------
constexpr Group kHouse = group(std::to_array<PatternDef>({
    {"Four On The Floor", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC x...|....|x...|...."},
    {"Chicago Jack", 16, S16, sh(3),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "RS ..x.|...x|..x.|.x.x;"
     "CH Xx.x|Xx.x|Xx.x|Xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC ....|x...|....|x..."},
    {"Warehouse Jack", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "SD ....|x~...|....|x~...;"
     "CP ....|x...|....|x...;"
     "CH xxXx|xxXx|xxXx|xxXx;"
     "RS ...x|....|..x.|....;"
     "AC x...|x...|x...|x..."},
    {"Deep Night", 16, S16, sh(3),
     "BD X...|X...|X...|X...;"
     "RS ....|x..x|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "CP ....|....|....|x..."},
    {"Piano Era", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CP ....|x...|....|x...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x...|x...|x...|x...;"
     "AC x...|....|....|...."},
    {"Tribal Toms", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "HT ...x|..x.|...x|..x.;"
     "MT ..x.|.x..|..x.|.x..;"
     "LT ....|...x|....|...X;"
     "CH ..x.|..x.|..x.|..x.;"
     "CP ....|x...|....|x...;"
     "AC x...|....|x...|...."},
    {"Garage House", 16, S16, sh(4),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "SD ....|....|...x|....;"
     "RS ...x|....|..x.|....;"
     "CH x..x|x..x|x..x|x..x;"
     "OH ..x.|..x.|..x.|..x."},
    {"Ride The Groove", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "RD x.xx|x.xx|x.xx|x.xx;"
     "CH ..x.|..x.|..x.|..x.;"
     "AC x...|....|x...|...."},
}),
    {"House Snare Fill", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|....;"
     "SD ....|....|x.xx|XxXX;"
     "CH xx.x|xx.x|....|....;"
     "OH ..x.|..x.|....|....;"
     "AC ....|....|....|x.x."});

// ---- Bank I, group 2: Techno ------------------------------------------------
constexpr Group kTechno = group(std::to_array<PatternDef>({
    {"Detroit Drive", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH Xx.x|xx.x|Xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RS ....|..x.|.x..|..x.;"
     "AC x...|....|x...|...."},
    {"Motor Toms", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x..x;"
     "CH Xxxx|Xxxx|Xxxx|Xxxx;"
     "RD ..x.|..x.|..x.|..x.;"
     "LT ...x|....|....|.x..;"
     "MT ....|....|..x.|...."},
    {"Minimal Pulse", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "RS ...x|..x.|....|.x..;"
     "CH ..x.|..x.|..x.|..x.;"
     "AC ....|....|....|..x."},
    {"Minimal Click", 16, S16, sh(3),
     "BD X...|X...|X...|X...;"
     "CH ..X.|..x.|..X.|..x.;"
     "RS .x..|....|..x.|...x;"
     "CP ....|....|....|x..."},
    {"Hard Ride", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x.x.|x.x.|x.x.|x.x.;"
     "AC x...|x...|x...|x..."},
    {"Industrial Stomp", 16, S16, sh(1),
     "BD X..x|X...|X..x|X.x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH xxxx|xxxx|xxxx|xxxx;"
     "LT ....|..x.|....|...x;"
     "AC x...|x...|x...|x..."},
    {"Floor Filler", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "SD ....|....|....|..x.;"
     "CH x..x|x..x|x..x|x..x;"
     "OH ..x.|..x.|..x.|..x.;"
     "RD x.xx|x.xx|x.xx|x.xx;"
     "CR x...|....|....|....;"
     "AC ..x.|..x.|..x.|..x."},
    {"Tom Machine", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "HT ...x|....|..x.|....;"
     "MT .x..|..x.|....|.x..;"
     "LT ....|...x|....|...X;"
     "CH ..x.|..x.|..x.|..x."},
}),
    {"Techno Roll", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "SD ....|..x.|x.xx|XXXX;"
     "CH xx.x|xx.x|xx.x|....;"
     "OH ..x.|..x.|..x.|....;"
     "AC ....|....|....|x.x."});

// ---- Bank I, group 3: Acid and electro ----------------------------------------
constexpr Group kAcidElectro = group(std::to_array<PatternDef>({
    {"Acid Line 1", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH xxXx|xxXx|xxXx|xxXx"},
    {"Acid Line 2", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CH xxXx|xxXx|xxXx|xXXx;"
     "RS ...x|..x.|....|.x..;"
     "AC x...|....|x...|...."},
    {"Acid Line 3", 16, S16, sh(1),
     "BD X...|X...|X...|X..x;"
     "CP ....|x...|....|x...;"
     "CH x..X|x..X|x..X|x..X;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC ..x.|..x.|..x.|..x."},
    {"Acid Shuffle", 16, S16, sh(3),
     "BD X...|X...|X...|X...;"
     "CP ....|x...|....|x...;"
     "CH xxXx|xxXx|xxXx|xxXx;"
     "RS ..x.|....|..x.|...x;"
     "AC ..x.|..x.|..x.|..x."},
    {"Electro Funk", 16, S16, sh(1),
     "BD X...|...x|..x.|....;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH x.x.|x.x.|x.x.|x.xx;"
     "AC x...|....|x...|...."},
    {"Circuit Funk", 16, S16, sh(1),
     "BD X..x|..x.|..x.|....;"
     "SD ....|X...|....|X...;"
     "CH x.x.|x.xx|x.x.|x.x.;"
     "RS ..x.|....|.x..|....;"
     "HT ....|....|....|..x."},
    {"Freestyle Heat", 16, S16, sh(1),
     "BD X..x|..x.|x..x|..x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH x.xx|x.xx|x.xx|x.xx;"
     "MT ...x|....|...x|....;"
     "AC x...|....|....|...."},
    {"Breaker Electro", 16, S16, sh(1),
     "BD X.x.|...x|..x.|....;"
     "SD ....|X...|....|X..x;"
     "CP ....|x...|....|x...;"
     "CH x.x.|x.x.|x.x.|x.x."},
}),
    {"Electro Tom Run", 16, S16, sh(1),
     "BD X...|..x.|....|....;"
     "SD ....|X...|....|....;"
     "HT ....|....|xx..|....;"
     "MT ....|....|..xx|....;"
     "LT ....|....|....|xxX.;"
     "CH x.x.|x.x.|....|....;"
     "AC ....|....|x...|x..."});

// ---- Bank II, group 1: Disco, Italo, hi-NRG, body music -------------------------
constexpr Group kDisco = group(std::to_array<PatternDef>({
    {"Disco Strut", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "CP ....|x...|....|x...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC ....|x...|....|x..."},
    {"Italo Drive", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH x..x|x..x|x..x|x..x;"
     "OH ..x.|..x.|..x.|..x.;"
     "HT ....|....|....|..x.;"
     "MT ....|....|....|...x"},
    {"Hi-NRG Pump", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "SD ....|x~...|....|x~...;"
     "CH Xx.x|Xx.x|Xx.x|Xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC x...|x...|x...|x..."},
    {"Body Pulse", 16, S16, sh(1),
     "BD X...|..x.|X...|..x.;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "AC x...|....|x...|...."},
    {"Foundry Stomp", 16, S16, sh(1),
     "BD X...|X...|X...|X...;"
     "SD ....|X..x|....|X.x~.;"
     "CH ..x.|..x.|..x.|..x.;"
     "LT ....|....|....|..x.;"
     "AC x...|....|x...|...."},
    {"Disco Toms", 16, S16, sh(2),
     "BD X...|X...|X...|X...;"
     "SD ....|x...|....|x...;"
     "HT ....|..x.|....|....;"
     "MT ....|...x|....|..x.;"
     "LT ....|....|....|...x;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x."},
}),
    {"Disco Flam Fill", 16, S16, sh(2),
     "BD X...|....|X...|....;"
     "SD ....|x~...|x~.x~.|xXxX;"
     "CH xx.x|xx.x|....|....;"
     "OH ..x.|..x.|....|....;"
     "AC ....|....|....|.x.x",
     {}, fl(3)});

// ---- Bank II, group 2: Breaks, boom-bap, two-step ----------------------------------
constexpr Group kBreaks = group(std::to_array<PatternDef>({
    {"Boom Bap", 16, S16, sh(5),
     "BD X..x|....|..x.|....;"
     "SD ....|X...|....|X...;"
     "CH x.x.|x.x.|x.x.|x.x.;"
     "AC ....|x...|....|x..."},
    {"Dusty Pocket", 16, S16, sh(6),
     "BD X...|...x|..x.|.x..;"
     "SD ....|X...|....|X...;"
     "CH xxxx|xxxx|xxxx|xxx.;"
     "OH ....|....|....|...x;"
     "RS ....|....|...x|...."},
    {"Big Beat Basic", 16, S16, sh(2),
     "BD X.x.|....|..x.|.x..;"
     "SD ....|X...|....|X...;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC ....|x...|....|x..."},
    {"Funky Break", 16, S16, sh(2),
     "BD X...|..x.|..x.|.x..;"
     "SD ....|X..x|.x..|X..x;"
     "CH xxxx|xxxx|xxxx|xxx.;"
     "OH ....|....|....|...x"},
    {"Skippy Two-Step", 16, S16, sh(6),
     "BD X...|...x|..x.|....;"
     "SD ....|X...|....|X...;"
     "RS ....|..x.|....|.x..;"
     "CH xx.x|xx.x|xx.x|xx.x;"
     "OH ..x.|..x.|..x.|..x.;"
     "AC ....|x...|....|x..."},
    {"Skip Step", 16, S16, sh(7),
     "BD X...|....|..x.|.x..;"
     "SD ....|X...|....|X...;"
     "CP ....|x...|....|x...;"
     "CH .x.x|.x.x|.x.x|.x.x;"
     "OH ..x.|....|..x.|...."},
}),
    {"Break Snare Fill", 16, S16, sh(5),
     "BD X...|..x.|....|....;"
     "SD ....|X..x|.x.x|XxXx;"
     "CH x.x.|x.x.|....|....;"
     "AC ....|....|....|x.x."});

// ---- Bank II, group 3: the other scales ----------------------------------------
// 8th triplets: 12 steps make one 4/4 bar. 16th triplets and 32nds: 12 and 16
// steps make half a bar. 12 and 14 steps of 16ths are 3/4 and 7/8.
constexpr Group kScales = group(std::to_array<PatternDef>({
    {"Triplet Gallop", 12, T8, 0,
     "BD X..|X..|X..|X..;"
     "SD ...|x..|...|x..;"
     "CH x.x|x.x|x.x|x.x;"
     "AC x..|...|x..|..."},
    {"Triplet Half Bar", 12, T16, 0,
     "BD X.....|...x..;"
     "SD ......|X.....;"
     "CH x.xx.x|x.xx.x"},
    {"Double Time Hats", 16, S32, 0,
     "BD X.......|.....x..;"
     "SD ........|X.......;"
     "CH x.x.x.x.|x.x.x.xx"},
    {"Waltz Time", 12, S16, sh(2),
     "BD X...|....|....;"
     "SD ....|x...|x...;"
     "CH x.x.|x.x.|x.x."},
    {"Seven Eight", 14, S16, 0,
     "BD X.....|x...|x...;"
     "SD ...x..|..x.|..x.;"
     "CH x.x.x.|x.x.|x.x."},
    {"Full Shuffle", 16, S16, sh(7),
     "BD X...|..x.|X...|..x.;"
     "SD ....|x...|....|x...;"
     "CH xxxx|xxxx|xxxx|xxxx;"
     "AC ....|x...|....|x..."},
    {"Wide Flams", 16, S16, sh(1),
     "BD x~...|X...|x~...|X...;"
     "SD ....|x~...|....|x~.x~.;"
     "LT ....|....|..x~.|....;"
     "CH ..x.|..x.|..x.|..x.",
     {}, fl(8)},
    {"Voice Check", 16, S16, 0,
     "BD x...|....|....|....;"
     "SD .x..|....|....|....;"
     "LT ..x.|....|....|....;"
     "MT ...x|....|....|....;"
     "HT ....|x...|....|....;"
     "RS ....|.x..|....|....;"
     "CP ....|..x.|....|....;"
     "CH ....|...x|....|....;"
     "OH ....|....|x...|....;"
     "CR ....|....|..x.|....;"
     "RD ....|....|....|x..."},
}),
    {"Triplet Tom Fill", 12, T8, 0,
     "BD X..|...|...|...;"
     "HT ...|xxx|...|...;"
     "MT ...|...|xxx|...;"
     "LT ...|...|...|xxX;"
     "AC ...|...|...|..x"});

constexpr BankDefs kDefs = memory(kHouse, kTechno, kAcidElectro, kDisco, kBreaks, kScales);

// Song tracks (each inside its own bank, as on the hardware).
constexpr MemoryDef kMemory{
    {"Classic Memory", "Classic", 125.0f},
    true,
    &kDefs,
    {{
        // TRACK 1: a house tune, intro to break and back.
        "1.1*4 1.2*4 1.4*4 1.2*3 1.16 1.5*4 1.1*3 1.16",
        // TRACK 2: techno, minimal into peak time.
        "2.3*4 2.1*3 2.16 2.7*4 2.5*3 2.16 2.8*4 2.7*3 2.16",
        // TRACK 3: acid.
        "3.1*4 3.2*4 3.4*4 3.3*3 3.16 3.1*4",
        "",
        // TRACK 5 (bank II): disco.
        "4.1*4 4.2*4 4.6*3 4.16 4.3*4 4.1*3 4.16",
        // TRACK 6: breaks and boom-bap.
        "5.1*4 5.2*3 5.16 5.3*4 5.4*3 5.16",
        "",
        "",
    }},
    // The live chain, entered as the original does it: one run of adjoining
    // patterns (first and last key), here four house grooves in a row.
    "1.1 1.2 1.3 1.4",
};

static_assert(checkMemory(kMemory) == 0, "Classic Memory: see FactoryNotation.h checkMemory for the code");

}  // namespace

const MemoryDef& classicMemory() noexcept { return kMemory; }

}  // namespace mw909::factory::detail
