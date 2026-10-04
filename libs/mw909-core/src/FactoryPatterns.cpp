// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The table of factory pattern memories and the code that turns one into a
// seq::PatternBank. The memories themselves live in FactoryPatterns<Name>.cpp,
// one per file, written in the step notation of FactoryNotation.h and checked
// at compile time there.
//
//   0  Classic Memory    hardware-legal: what the original could store (the default memory)
//   1  House Sessions    Chicago jack, deep, piano-era, tribal, garage house
//   2  Techno Floor      Detroit, minimal, peak-time, hard techno, EBM and industrial
//   3  Acid & Electro    acid lines with accented off-beat hats, electro and freestyle
//   4  Shuffle & Breaks  UK garage and two-step, speed garage, breaks, big beat, boom-bap
//   5  Hard & Fast       gabber, hardcore, rave and hardstyle
//   6  Trance & Disco    trance, hard trance, Italo, disco and hi-NRG
//   7  Rhythm Lab        polymeter, 32nd ratchets, triplet and odd grids, probability
//   8  Empty Memory      blank, for starting from scratch

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "FactoryNotation.h"

namespace mw909::factory::detail {

namespace {

constexpr BankDefs kEmptyDefs{};

const MemoryDef& emptyMemory() noexcept {
    static constexpr MemoryDef m{{"Empty Memory", "Utility", 120.0f}, false, &kEmptyDefs, {}, {}};
    return m;
}

using MemoryFn = const MemoryDef& (*)() noexcept;

constexpr std::array<MemoryFn, 9> kMemories{{
    &classicMemory,
    &houseMemory,
    &technoMemory,
    &acidElectroMemory,
    &shuffleBreaksMemory,
    &hardMemory,
    &tranceDiscoMemory,
    &labMemory,
    &emptyMemory,
}};

int clampBank(int index) noexcept {
    return index < 0 ? 0 : (index >= static_cast<int>(kMemories.size()) ? static_cast<int>(kMemories.size()) - 1 : index);
}

const MemoryDef& mem(int index) noexcept { return kMemories[static_cast<std::size_t>(clampBank(index))](); }

}  // namespace

int bankCount() noexcept { return static_cast<int>(kMemories.size()); }

const BankInfo& bankInfoAt(int index) noexcept { return mem(index).info; }

void buildBankAt(int index, seq::PatternBank& out) noexcept {
    const MemoryDef& m = mem(index);
    for (std::size_t i = 0; i < out.patterns.size(); ++i) {
        // Every memory is checked at compile time, so this cannot fail; on a
        // failure the slot would be left as parsed so far.
        static_cast<void>(parsePattern((*m.patterns)[i], m.hardware, out.patterns[i]));
    }
    out.chain = seq::Chain{};
    if (!m.chain.empty()) {
        const int n = parseMeasures(m.chain, *m.patterns, -1, out.chain.entries);
        out.chain.length = static_cast<std::uint8_t>(n < 0 ? 0 : n);
    }
    for (std::size_t t = 0; t < out.tracks.size(); ++t) {
        out.tracks[t] = seq::SongTrack{};
        const std::string_view text = m.songs[t];
        if (text.empty()) continue;
        const int bankOnly = m.hardware ? static_cast<int>(t) / seq::kSongTracksPerBank : -1;
        const int n = parseMeasures(text, *m.patterns, bankOnly, out.tracks[t].entries);
        out.tracks[t].length = static_cast<std::uint16_t>(n < 0 ? 0 : n);
        if (n < 0) out.tracks[t].entries.fill(0);
    }
    if (out.chain.length == 0) out.chain.entries.fill(0);
}

std::string_view patternNameAt(int bankIndex, int patternIndex) noexcept {
    if (patternIndex < 0 || patternIndex >= seq::kNumPatterns) return {};
    return (*mem(bankIndex).patterns)[static_cast<std::size_t>(patternIndex)].name;
}

}  // namespace mw909::factory::detail
