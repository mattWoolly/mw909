// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The two send-effect buses and their ten effect instances (FxRack).
// Contract: docs/design/architecture.md §4.3, parameters in fx/FxParams.h.
//
// Each instance exists once (distortion, two parametric EQs, delay, reverb,
// chorus, bitcrusher, two dynamics, filter) and is placed by FxPlacement on
// bus 1 or 2 at an order key; a bus runs its instances in ascending key, ties
// in FxId order. Placement changes are parameter changes (nothing allocates
// after prepare()):
//  * switch on: the instance starts from silence and fades in (20 ms) against
//    the signal at its place in the chain; if that signal is silent (the bus
//    input below the floor for a control interval, and at the first sample
//    after the switch) there is nothing to crossfade and it is fully in at
//    once, so the first hit after switching a send reverb on is all wet;
//  * switch off: it fades out (crossfade to its own input, i.e. bypass), then
//    is reset and leaves the chain;
//  * a new order key that keeps the bus's chain order (e.g. the only effect
//    on the bus going from key 0 to 3) changes nothing audible: no fade, no
//    reset, the tail rings on;
//  * a reorder within the bus that changes the chain order: the moved
//    instance fades out (bypass), takes its new position at the sample its
//    fade ends and fades back in, keeping its state (a reverb keeps its tail);
//  * a move to the other bus: fade out, reset, fade in on the other bus.
// Every fade and every move happens at a sample position: the rack splits
// its work at the sample where a fade-out ends, so the output never depends
// on how the host splits blocks.
//
// Bus 1 can also feed bus 2 (`toBus2`, taken before or after bus 1's effects,
// the pre/post switch crossfades). A bus whose input is silent and whose
// instances are all asleep (and no fade running) costs one scan of its input.
// Non-finite input samples are replaced by 0 (one bad sample upstream must not
// poison a reverb or delay line for the rest of the session).

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mw909/Voices.h"
#include "mw909/fx/Bitcrusher.h"
#include "mw909/fx/Chorus.h"
#include "mw909/fx/Delay.h"
#include "mw909/fx/Distortion.h"
#include "mw909/fx/Dynamics.h"
#include "mw909/fx/FilterFx.h"
#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxParams.h"
#include "mw909/fx/ParamEq.h"
#include "mw909/fx/Reverb.h"

namespace mw909::fx {

class FxRack {
public:
    static constexpr double kSlotFadeSeconds = 0.020;

    // Allocates the delay lines and scratch buffers (non-realtime).
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParams(const FxParams& params, double bpm) noexcept;

    // A voice was triggered: VCA-mode dynamics and filter envelopes listening to it fire.
    void noteTrigger(VoiceId v) noexcept;

    // in[b][c]: bus b's summed input (L, R), processed in place into its wet
    // return (before the return level and routing, which the engine applies).
    // key[v]: post-strip mono signal of each voice for compressor sidechains
    // (may be null).
    void process(float* const (&bus)[kNumFxBuses][2], const std::array<const float*, kNumVoices>& key,
                 int n) noexcept;

    // Bus b has a running effect, a tail still ringing, or a fade in progress.
    bool busActive(int b) const noexcept;
    // Longest tail among the effects placed on bus b.
    double tailSeconds(int b) const noexcept;

    // Instance order of bus b as it currently runs; returns the count.
    int chain(int b, std::array<std::uint8_t, kNumFxInstances>& ids) const noexcept;

    Distortion& distortion() noexcept { return distortion_; }
    ParamEq& eq(int i) noexcept { return eq_[static_cast<std::size_t>(i & 1)]; }
    Delay& delay() noexcept { return delay_; }
    Reverb& reverb() noexcept { return reverb_; }
    Chorus& chorus() noexcept { return chorus_; }
    Bitcrusher& bitcrusher() noexcept { return bitcrusher_; }
    Dynamics& dynamics(int i) noexcept { return dynamics_[static_cast<std::size_t>(i & 1)]; }
    FilterFx& filter() noexcept { return filter_; }

private:
    struct Slot {
        FxPlacement want{};        // sanitised target placement
        std::uint8_t bus = 0, order = 0;   // where it runs now
        bool placed = false;       // in a chain (running or fading)
        bool armed = false;        // just switched on: may skip its fade-in (silent input)
        Fade fade;
    };

    void rebuild() noexcept;
    void settleSlots() noexcept;
    int samplesToNextSettle() const noexcept;
    long quietAfter(int b, const float* l, const float* r, int n) const noexcept;
    bool instancePassThrough(int id) const noexcept;
    void runInstance(int id, float* l, float* r, int n, const std::array<const float*, kNumVoices>& key,
                     int offset) noexcept;
    void resetInstance(int id) noexcept;
    bool instanceActive(int id) const noexcept;
    double instanceTail(int id) const noexcept;
    // silent: the bus input of these n samples is below the floor throughout.
    void processBus(int b, float* l, float* r, int n, const std::array<const float*, kNumVoices>& key,
                    int offset, bool silent) noexcept;

    double fs_ = 48000.0;
    int maxBlock_ = 0;
    static constexpr long kQuietLong = 1L << 30;
    long quietMin_ = 16;
    std::array<long, kNumFxBuses> busQuiet_{};   // samples since each bus input was above the floor
    std::array<Slot, kNumFxInstances> slot_{};
    std::array<std::array<std::uint8_t, kNumFxInstances>, kNumFxBuses> chain_{};
    std::array<int, kNumFxBuses> count_{};
    Smoothed toBus2_;
    Fade toBus2Pre_;

    Distortion distortion_;
    std::array<ParamEq, 2> eq_{};
    Delay delay_;
    Reverb reverb_;
    Chorus chorus_;
    Bitcrusher bitcrusher_;
    std::array<Dynamics, 2> dynamics_{};
    FilterFx filter_;

    std::vector<float> scratchL_, scratchR_, preL_, preR_;
    LastParams<FxParams> last_;
};

}  // namespace mw909::fx
