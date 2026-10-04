// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory content, compiled in as plain data: kits (sound + mixer settings),
// pattern banks, and presets that pair a kit with a bank. Message-thread /
// offline use only (building a bank copies ~140 KB); nothing here is called
// from the audio path.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "mw909/Engine.h"
#include "mw909/Voices.h"
#include "mw909/seq/Pattern.h"

namespace mw909::factory {

// A kit: every per-voice sound, channel and strip setting, the FX buses, the
// trigger outs, total accent and the master section (EngineParams minus bpm).
struct Kit {
    std::string_view name;
    std::string_view category;  // e.g. "Classic", "House", "Driven"
    std::array<VoiceParams, kNumVoices> voice{};
    std::array<ChannelParams, kNumVoices> channel{};
    std::array<StripParams, kNumVoices> strip{};
    FxParams fx{};
    std::array<TriggerOutParams, kNumTriggerOuts> trig{};
    float totalAccent = 0.7f;
    MasterParams master{};
    Revision revision = Revision::Late;   // the unit revision the kit was voiced for
};

int numKits() noexcept;
const Kit& kit(int index) noexcept;  // index clamped; kit(0) is the stock machine

// A pattern memory: 96 patterns (2 banks x 3 groups x 16), the live chain and
// the 8 song tracks. (A factory "bank" here is a whole memory image.)
struct BankInfo {
    std::string_view name;
    std::string_view category;
    float tempo = 120.0f;  // suggested tempo
};

int numBanks() noexcept;
const BankInfo& bankInfo(int index) noexcept;                 // bank 0 is the default memory
void buildBank(int index, seq::PatternBank& out) noexcept;    // overwrites `out`
// Display name of one pattern (0..95) in a factory memory ("" if the slot is empty).
std::string_view patternName(int bankIndex, int patternIndex) noexcept;

// A full preset: a kit, a bank and the sequencer feel that goes with them.
struct Preset {
    std::string_view name;
    std::string_view category;
    int kit = 0;
    int bank = 0;
    int pattern = 0;       // pattern selected on load
    float tempo = 120.0f;
    float swingScale = 1.0f;
    seq::VariationMode variation = seq::VariationMode::A;
    seq::PlayMode play = seq::PlayMode::Pattern;
    std::uint8_t songTrack = 0;
    seq::AutoFill autoFill = seq::AutoFill::Manual;
};

int numPresets() noexcept;
const Preset& preset(int index) noexcept;  // preset(0) is the default state of a new instance

}  // namespace mw909::factory
