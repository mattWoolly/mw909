// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The sound engine: 11 voices, the shared noise generator, the ROM images,
// the channel strips, the two FX buses, the audible trigger outs and the
// master chain, mixed onto 16 stereo buses. No sequencing, no MIDI — the
// caller triggers voices and asks for audio (see Machine).
// Contract: docs/design/architecture.md §4.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mw909/Voices.h"
#include "mw909/dsp/Rom.h"
#include "mw909/fx/FxParams.h"

namespace mw909 {

enum class Quality : std::uint8_t {
    Eco = 0,   // no oversampling anywhere
    Standard,  // voices may oversample 2x below 88.2 kHz (the default; the CPU budget is measured here)
    High       // 4x below 88.2 kHz, 2x above
};

struct EngineParams {
    std::array<VoiceParams, kNumVoices> voice{};
    std::array<ChannelParams, kNumVoices> channel{};
    std::array<StripParams, kNumVoices> strip{};
    FxParams fx{};
    MasterParams master{};
    std::array<TriggerOutParams, kNumTriggerOuts> trig{};
    bool hatChoke = true;        // HiHat::setChoke (one hat circuit, as the hardware)
    double bpm = 120.0;          // for tempo-synced FX (delay, filter LFO)
};

// Destination pointers, [bus][left/right]. A null pair means the bus is not
// connected; anything routed there falls back to the main mix. Bus 0 must be valid.
struct BusBuffers {
    std::array<std::array<float*, 2>, kNumBuses> ch{};
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Allocates every buffer and delay line for the largest configuration.
    // Call from the non-realtime prepare path.
    void prepare(double sampleRate, int maxBlockSize);

    // Realtime-safe. Re-prepares the voices when anything changes (which
    // silences them); a no-op otherwise.
    void configure(Quality quality, std::uint32_t unitSeed, float variance, Revision revision) noexcept;

    // Silence everything (voices, strips, FX tails) and clear all state.
    void reset() noexcept;

    // Realtime-safe. Values are sanitised (NaN/Inf -> default, clamped).
    void setParams(const EngineParams& params) noexcept;

    // Trigger a voice at the next rendered sample. Also fires the trigger outs,
    // FX envelopes and VCA-ducking sources listening to this voice, and
    // rewrites the shared accent line of its DAC partner (RS/CP, CR/RD).
    void trigger(VoiceId voice, const Hit& hit) noexcept;

    // The hardware's user ROM slot for the sample voices; nullptr = none.
    // The pointer must stay valid until replaced (Machine owns the lifetime).
    void setUserRom(RomSlot slot, const RomImage* image) noexcept;

    // Render numSamples and OVERWRITE every connected bus in [offset, offset + numSamples).
    // numSamples must not exceed the prepared maxBlockSize.
    void render(const BusBuffers& out, int offset, int numSamples) noexcept;

    // Constant for a prepared sample rate, whatever the quality: every voice is
    // delayed to this common latency, and the plugin reports it to the host.
    int latencySamples() const noexcept { return latency_; }

    bool anyVoiceActive() const noexcept;
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlockSize() const noexcept { return maxBlock_; }
    int oversampleFactor() const noexcept { return oversample_; }

    // Post-fader peak of each channel over the last render() call (UI meters).
    float channelPeak(int voice) const noexcept { return channelPeak_[static_cast<std::size_t>(voice)]; }

private:
    double sampleRate_ = 48000.0;
    int maxBlock_ = 0;
    int oversample_ = 1;
    int latency_ = 0;
    std::array<float, kNumVoices> channelPeak_{};

    // The implementation owns everything else (voices, noise, strips, buses,
    // master, scratch). Members may be added freely below this line.
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace mw909
