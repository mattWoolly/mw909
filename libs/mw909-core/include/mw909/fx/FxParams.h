// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Parameters of everything after the voices: the per-voice channel strip, the
// two send-effect buses, the master chain and the audible trigger outputs.
// Plain values only; the DSP lives in fx/ChannelStrip.h, fx/FxBus.h,
// fx/MasterChain.h and fx/TriggerOut.h. Every stage that is switched off must
// cost nothing (no per-sample work). Contract: docs/design/architecture.md §4.
//
// Every value is in its natural unit (Hz, ms, dB, ratio, 0..1). Setters in the
// DSP classes clamp to the ranges noted here and treat NaN/Inf as the default.

#pragma once

#include <array>
#include <cstdint>

#include "mw909/Voices.h"

namespace mw909 {

// Stereo output buses: 0 is the main mix, 1..15 are the individual outs ("Out 2".."Out 16").
inline constexpr int kNumBuses = 16;
inline constexpr int kNumFxBuses = 2;

// ---------------------------------------------------------------------------
// Channel strip (mono, per voice, before pan): Filter/EQ and Compressor.

struct EqBand {
    bool on = false;
    float freqHz = 1000.0f;     // 20..20000
    float widthOct = 1.0f;      // 0.5..4 octaves
    float gainDb = 0.0f;        // -24..+24
};

struct StripParams {
    bool eqOn = false;
    float lowCutHz = 20.0f;     // 20..2000; 20 = off. 12 dB/oct high-pass
    float highCutHz = 20000.0f; // 500..20000; 20000 = off. 12 dB/oct low-pass
    float resonance = 0.0f;     // 0..1, shared by both cut filters (0 = Butterworth)
    std::array<EqBand, 2> band{{{false, 200.0f, 1.0f, 0.0f}, {false, 3000.0f, 1.0f, 0.0f}}};

    bool compOn = false;
    bool compPreEq = false;     // true: compressor before the Filter/EQ section
    float compThresholdDb = 0.0f;   // -48..0
    float compRatio = 4.0f;         // 1..20
    float compAttackMs = 10.0f;     // 1..500
    float compReleaseMs = 100.0f;   // 1..500
    float compMakeupDb = 0.0f;      // 0..24
};

// Mixer channel of one voice: drive -> strip -> level -> pan/spread -> destinations.
enum class ChannelRoute : std::uint8_t {
    Output = 0,   // straight to `output` (main mix or an individual out)
    FxBus1,       // into FX bus 1 instead (it carries the voice on to its own output)
    FxBus2
};

struct ChannelParams {
    float level = 0.5f;         // LEVEL pot 0..1; 0.5 = the calibration point (voice volts as rendered)
    float pan = 0.0f;           // -1 (left) .. +1 (right)
    float spread = 0.0f;        // 0..1: stereo spread (Haas delay of one side, up to 20 ms)
    float drive = 0.0f;         // per-voice overdrive, 0 = clean
    std::uint8_t output = 0;    // bus index 0..kNumBuses-1; a voice on an individual out leaves the main mix
    ChannelRoute route = ChannelRoute::Output;
    std::uint8_t sendBus = 0;   // 0 = no send, 1 = FX bus 1, 2 = FX bus 2
    float send = 0.0f;          // send amount 0..1
    bool sendPre = false;       // send taken before LEVEL (post strip)
};

// ---------------------------------------------------------------------------
// FX buses. Ten effect instances exist once each; every instance is placed on
// bus 1 or 2 at an order key, and each bus runs its instances in ascending
// order key (ties: instance order below). Swapping an effect between buses or
// reordering is a parameter change, never an allocation.

enum class FxId : std::uint8_t {
    Distortion = 0, EqA, EqB, Delay, Reverb, Chorus, Bitcrusher, DynamicsA, DynamicsB, Filter,
    kCount
};
inline constexpr int kNumFxInstances = static_cast<int>(FxId::kCount);

struct FxPlacement {
    bool on = false;
    std::uint8_t bus = 0;       // 0 = bus 1, 1 = bus 2
    std::uint8_t order = 0;     // 0..9
};

struct DistortionParams {
    float preampDb = 0.0f;      // 0..36
    float thresholdDb = 0.0f;   // -36..0, clip threshold
    float knee = 0.5f;          // 0..1, hard .. soft diode knee
    float splitHz = 200.0f;     // 20..2000: crossover; only the high band is clipped (0 = full band below 20)
    float lowOutDb = 0.0f;      // -inf(-60)..+12, the unclipped low band
    float clipOutDb = 0.0f;     // -60..+12, the clipped band
    float mix = 1.0f;           // 0..1 dry/wet
};

struct ParamEqParams {
    bool lowShelf = false;      // false = low cut (12 dB/oct), true = low shelf
    float lowHz = 20.0f;        // 20..2000
    float lowGainDb = 0.0f;     // -24..+24 (shelf only)
    float lowRes = 0.0f;        // 0..1
    std::array<EqBand, 2> mid{{{false, 400.0f, 1.0f, 0.0f}, {false, 2500.0f, 1.0f, 0.0f}}};
    bool highShelf = false;
    float highHz = 20000.0f;    // 500..20000
    float highGainDb = 0.0f;
    float highRes = 0.0f;
};

enum class SyncDiv : std::uint8_t {   // tempo-synced times, in fractions of a whole note
    D1_32 = 0, D1_16, D1_8, D1_4, D1_2, D1_1, kCount
};
enum class SyncKind : std::uint8_t { Straight = 0, Dotted, Triplet };
enum class FilterType : std::uint8_t { Off = 0, LowPass, BandPass, HighPass };

struct DelayParams {
    bool sync = true;
    float timeMs = 250.0f;      // 1..1000 when !sync
    SyncDiv div = SyncDiv::D1_8;
    SyncKind kind = SyncKind::Dotted;
    float feedback = 0.35f;     // 0..0.98
    float spread = 0.0f;        // -1..1: ping-pong offset between L and R
    FilterType loopFilter = FilterType::Off;
    float cutoffHz = 3000.0f;   // 20..20000
    float reso = 0.0f;          // 0..1
    float mix = 1.0f;
};

struct ReverbParams {
    float predelayMs = 10.0f;   // 0..250
    float earlySize = 0.5f;     // 0..1
    float earlyDiffusion = 0.6f;
    float earlyColor = 0.5f;    // dark .. bright
    float lateSize = 0.5f;
    float lateDiffusion = 0.7f;
    float lateColor = 0.5f;
    float decay = 0.5f;         // 0..1 -> RT60 0.2 .. 10 s
    float modulation = 0.2f;    // 0..1
    float earlyLate = 0.5f;     // 0 = early only, 1 = late only
    float mix = 1.0f;
};

struct ChorusParams {
    float offsetMs = 8.0f;      // 1..30 base delay
    float depth = 0.5f;         // 0..1
    float rateHz = 0.5f;        // 0.02..10
    float feedback = 0.0f;      // -0.95..0.95
    float spatial = 0.5f;       // 0..1 stereo phase offset of the LFOs
    float lowCutHz = 20.0f;
    float highCutHz = 20000.0f;
    float fatness = 0.0f;       // 0..1: second voice pair
    float gainDb = 0.0f;
    float mix = 0.5f;
};

struct BitcrusherParams {
    float bits = 16.0f;         // 1..24
    bool dither = false;
    float rateHz = 44100.0f;    // 44..44100 sample-and-hold rate
    float jitter = 0.0f;        // 0..1 clock jitter
    float approxFilter = 1.0f;  // 0..1: anti-alias before the hold, as a fraction of the hold Nyquist (1 = off)
    float imagesFilter = 1.0f;  // 0..1: smoothing after the hold (1 = off)
    float mix = 1.0f;
};

enum class DynamicsMode : std::uint8_t { Compressor = 0, Vca };
inline constexpr int kMaxTriggerSources = 3;
inline constexpr std::uint8_t kNoSource = 0xFF;

struct DynamicsParams {
    DynamicsMode mode = DynamicsMode::Compressor;
    float thresholdDb = -12.0f; // -48..0
    float ratio = 4.0f;         // 1..20
    float attackMs = 5.0f;      // 1..500
    float holdMs = 0.0f;        // 0..500
    float releaseMs = 120.0f;   // 1..500
    float makeupDb = 0.0f;      // 0..24
    std::uint8_t sidechain = kNoSource;   // Compressor: key from this voice's channel signal (VoiceId) or own input
    std::array<std::uint8_t, kMaxTriggerSources> triggers{{kNoSource, kNoSource, kNoSource}};  // Vca: voices whose hits fire the AHR
    float vcaDepth = -1.0f;     // Vca: -1 (full duck) .. +1 (gate open on hit)
    float mix = 1.0f;
};

enum class LfoWave : std::uint8_t { Sine = 0, Triangle, Saw, Square, Random };
enum class ModSource : std::uint8_t { Lfo = 0, Envelope };

struct FilterFxParams {
    FilterType type = FilterType::LowPass;
    float cutoffHz = 2000.0f;
    float reso = 0.2f;
    ModSource source = ModSource::Lfo;
    bool lfoSync = false;
    float lfoRateHz = 1.0f;     // 0.01..20
    SyncDiv lfoDiv = SyncDiv::D1_4;
    LfoWave lfoWave = LfoWave::Sine;
    float lfoSpread = 0.0f;     // 0..1 L/R phase offset
    float envAttackMs = 1.0f, envHoldMs = 0.0f, envReleaseMs = 150.0f;   // 1..500
    std::array<std::uint8_t, kMaxTriggerSources> envTriggers{{kNoSource, kNoSource, kNoSource}};
    float modAmount = 0.0f;     // -1..1, +-4 octaves
    float mix = 1.0f;
};

struct FxBusParams {
    float levelDb = 0.0f;       // -60..+12 return level
    std::uint8_t output = 0;    // bus index the return goes to (0 = main)
    float toBus2 = 0.0f;        // bus 1 only: amount also sent into bus 2 (0..1)
    bool toBus2Pre = false;     //   taken before bus 1's effects
};

struct FxParams {
    std::array<FxPlacement, kNumFxInstances> place{};
    std::array<FxBusParams, kNumFxBuses> bus{};
    DistortionParams distortion{};
    std::array<ParamEqParams, 2> eq{};
    DelayParams delay{};
    ReverbParams reverb{};
    ChorusParams chorus{};
    BitcrusherParams bitcrusher{};
    std::array<DynamicsParams, 2> dynamics{};
    FilterFxParams filter{};
};

// ---------------------------------------------------------------------------
// Master chain (main bus only; individual outs bypass it):
// analog output stage -> drive -> 3-band compressor -> limiter -> volume.

struct CompBand {
    bool on = true;
    bool solo = false;
    float thresholdDb = 0.0f;   // -48..0
    float ratio = 2.0f;         // 1..20
    float attackMs = 10.0f;     // 1..500
    float releaseMs = 150.0f;   // 1..500
    float makeupDb = 0.0f;      // 0..24
};

enum class OutputStage : std::uint8_t {
    Off = 0,
    Modern,      // op-amp saturation only, DC blocked below ~10 Hz (default)
    Original     // the hardware main output: ~130 Hz coupling high-pass at the summing node,
                 // op-amp saturation and (Revision::Late) the ~6.6 kHz output low-pass (research/03 §6)
};

struct MasterParams {
    OutputStage stage = OutputStage::Modern;
    float drive = 0.0f;         // 0..1, pushes the output stage harder (level-compensated)
    bool compOn = false;        // 3-band compressor
    float xoverLowHz = 200.0f;  // 40..1000
    float xoverHighHz = 3000.0f;// 1000..12000 (kept above xoverLowHz)
    std::array<CompBand, 3> band{};
    bool limiterOn = true;
    float limThresholdDb = 0.0f;   // -24..0
    float limAttackMs = 1.0f;      // 0.1..10
    float limReleaseMs = 100.0f;   // 10..1000
    bool softClip = true;          // diode-like clipper above 0 dBFS after the limiter
    float volumeDb = 0.0f;         // -60..+12
};

// ---------------------------------------------------------------------------
// Audible trigger outputs: the click of the hardware TRIG OUT pulse (rim shot:
// +14 V, 15..20 ms through the jack's coupling, research/03 §5.3), fired by a
// chosen voice's hits.

inline constexpr int kNumTriggerOuts = 3;

struct TriggerOutParams {
    bool on = false;
    std::uint8_t source = RS;   // VoiceId whose hits fire it
    ChannelRoute route = ChannelRoute::Output;
    std::uint8_t output = 0;    // bus index when route == Output
    float pan = 0.0f;
    float level = 0.5f;         // 0..1
};

}  // namespace mw909
