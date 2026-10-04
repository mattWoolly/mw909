// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// 6-bit ROM images for the sample voices (research/02 §1). Three 32768-word
// images exist on the original: the hi-hat (open hat at 0x0000..0x5FFF, closed
// hat at 0x6000..0x7FFF), the crash and the ride, of which only data bits
// D0..D5 are wired. mw909 never contains the original data: the factory
// images are built from CC0 recordings of real cymbals by tools/rombuild
// (provenance in assets/rom/PROVENANCE.md), and users may import their own
// (romFromAudio). Values are unsigned 0..63, mid-scale 32 = silence.

#pragma once

#include <array>
#include <cstdint>

namespace mw909 {

inline constexpr int kRomWords = 32768;
inline constexpr int kHatOpenStart = 0x0000;
inline constexpr int kHatOpenEnd = 0x6000;      // exclusive: 24576 words
inline constexpr int kHatClosedStart = 0x6000;
inline constexpr int kHatClosedEnd = 0x8000;    // exclusive: 8192 words
inline constexpr std::uint8_t kRomMidScale = 32;

struct RomImage {
    std::array<std::uint8_t, kRomWords> data{};   // each 0..63
};

enum class RomSlot : std::uint8_t { Hat = 0, Crash, Ride, kCount };
inline constexpr int kNumRomSlots = static_cast<int>(RomSlot::kCount);

// The compiled-in factory images (src/RomData.cpp, generated; never edited by hand).
const RomImage& factoryRom(RomSlot slot) noexcept;

// Which part of a ROM a user import fills.
enum class RomRegion : std::uint8_t { HatOpen = 0, HatClosed, Crash, Ride };

// Build 6-bit ROM content from mono audio, as the original's sample data was
// prepared (research/02 §3.3, §9.3): flatten the envelope (20..50 ms RMS
// tracker divided out), normalise, low-pass and resample to the nominal ROM
// clock, quantise to 6 bits, pad with mid-scale. Writes the region of `image`
// (other regions are left untouched). Message thread only: allocates.
// Returns false (and leaves `image` unchanged) on empty/invalid input.
bool romFromAudio(const float* samples, int numSamples, double sampleRate, RomRegion region,
                  RomImage& image);

}  // namespace mw909
