// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Filter effect (stereo): a resonant 12 dB/oct state-variable low-, band- or
// high-pass whose cutoff is swept by an LFO or by an envelope.
//
//  * cutoff_eff = cutoff * 2^(4 * modAmount * m), m in -1..1 (LFO) or 0..1
//    (envelope): modAmount -1..1 spans +-4 octaves.
//  * LFO: 0.01..20 Hz or tempo-synced (1/32..1/1), sine / triangle / saw /
//    square / random (held per cycle); spread 0..1 offsets the right channel's
//    phase by up to 180 degrees. The LFO restarts at phase 0 when the effect
//    wakes from silence (so a render repeats exactly).
//  * Envelope: attack-hold-release (1..500 ms) fired by the hits of up to three
//    voices; a retrigger restarts the attack from the current level.
//  * Resonance 0..1 -> Q 0.707..16. The cutoff is evaluated on the control
//    grid (~0.33 ms) and the SVF's g = tan(pi f / fs) is interpolated linearly
//    in between, so sweeps are smooth and tuned at every sample rate.
//  * Type changes fade the old type out and the new one in; Off is dry.
//    Once a fade to Off ends (on the control grid) the effect is bypassed:
//    asleep, the input passing untouched at no cost; switched on again it
//    restarts from rest (LFO at phase 0) and, if signal is passing, fades in
//    from dry.
//    out = dry + mix (filtered - dry).

#pragma once

#include <array>
#include <cmath>

#include "mw909/Voices.h"
#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class FilterFx : public StereoGate<FilterFx> {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) noexcept {
        fs_ = sampleRate;
        gatePrepare(sampleRate);
        const double tick = sampleRate / static_cast<double>(ctrl_);
        for (auto* s : {&logCut_, &logQ_, &amount_}) {
            s->setTime(0.030, tick);
            s->setEps(1.0e-5);
        }
        for (auto& s : lfoS_) {
            s.setTime(0.001, tick);   // rounds the square/saw/random steps (no clicks)
            s.setEps(1.0e-6);
        }
        srcFade_.setTime(0.010, tick);
        typeFade_.setTime(0.010, sampleRate);
        mix_.setTime(kDefaultSmoothSeconds, sampleRate);
        mix_.setEps(1.0e-7);
        env_.prepare(sampleRate);
        reset();
        setParams(FilterFxParams{}, 120.0);
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        clearState();
        env_.clear();
        lfo_.reset();
        snapAll();   // silence: no glides pending
    }

    void setParams(const FilterFxParams& p, double bpm) noexcept {
        if (same(last_, p, bpm)) return;   // unchanged: nothing to do (cheap per-block calls)
        const bool wasBypassed = passThrough() && bypassLive();
        wantType_ = sanitizeEnum(p.type, FilterType::LowPass, 4);
        logCut_.setTarget(std::log2(static_cast<double>(sanitize(p.cutoffHz, 2000.0f, 20.0f, 20000.0f))));
        logQ_.setTarget(std::log(resonanceToQ(sanitize(p.reso, 0.2f, 0.0f, 1.0f), 16.0)));
        source_ = sanitizeEnum(p.source, ModSource::Lfo, 2);
        srcFade_.setTarget(source_ == ModSource::Envelope ? 1.0 : 0.0);
        wave_ = sanitizeEnum(p.lfoWave, LfoWave::Sine, 5);
        if (p.lfoSync) {
            const auto div = sanitizeEnum(p.lfoDiv, SyncDiv::D1_4, static_cast<int>(SyncDiv::kCount));
            lfoHz_ = 1.0 / syncSeconds(div, SyncKind::Straight, bpm);
        } else {
            lfoHz_ = sanitize(p.lfoRateHz, 1.0f, 0.01f, 20.0f);
        }
        spread_ = 0.5 * sanitize(p.lfoSpread, 0.0f, 0.0f, 1.0f);
        env_.setTimes(sanitize(p.envAttackMs, 1.0f, 1.0f, 500.0f), sanitize(p.envHoldMs, 0.0f, 0.0f, 500.0f),
                      sanitize(p.envReleaseMs, 150.0f, 1.0f, 500.0f));
        triggers_ = p.envTriggers;
        amount_.setTarget(sanitize(p.modAmount, 0.0f, -1.0f, 1.0f));
        mix_.setTarget(sanitize(p.mix, 1.0f, 0.0f, 1.0f));
        typeFade_.setTarget(type_ == wantType_ && type_ != FilterType::Off ? 1.0 : 0.0);
        if (fresh_ || !awake()) {
            snapAll();
            if (!fresh_ && wasBypassed && wantType_ != FilterType::Off) {   // out of bypass: fade in from dry
                typeFade_.snapTo(0.0);
                typeFade_.setTarget(1.0);
            }
        }
        fresh_ = false;
    }

    void noteTrigger(int voice) noexcept {
        if (source_ != ModSource::Envelope && srcFade_.value() == 0.0) return;
        if (!awake() && passThrough()) return;   // bypassed: nothing to sweep
        for (auto t : triggers_)
            if (t != kNoSource && t == voice) {
                env_.fire();
                gateWake();
                return;
            }
    }

    // Type Off: once bypassed, the input passes untouched (no cost).
    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }
    double tailSeconds() const noexcept { return 0.1; }
    double cutoffHz(int ch) const noexcept { return fEff_[static_cast<std::size_t>(ch & 1)]; }

    // --- StereoGate hooks ---------------------------------------------------
    bool passThrough() const noexcept {
        return type_ == FilterType::Off && wantType_ == FilterType::Off && typeFade_.settled();
    }
    void control() noexcept {
        if (type_ != wantType_ && typeFade_.value() == 0.0) {
            type_ = wantType_;
            clearState();
            typeFade_.setTarget(type_ != FilterType::Off ? 1.0 : 0.0);
        }
        if (passThrough()) {   // faded out to Off: bypass from this tick
            sleepNow();
            gateSleep();
            return;
        }
        const double lc = logCut_.tick();
        k_ = 1.0 / std::exp(logQ_.tick());
        const double amt = amount_.tick();
        const double ahead = lfoHz_ / fs_ * static_cast<double>(ctrl_);
        const double inv = 1.0 / static_cast<double>(ctrl_);
        const double ws = srcFade_.tick();   // LFO <-> envelope crossfade
        for (int c = 0; c < 2; ++c) {
            auto& ls = lfoS_[static_cast<std::size_t>(c)];
            if (ws < 1.0) {
                ls.setTarget(lfoAt(ahead + (c == 1 ? spread_ : 0.0), c));   // value at the end of the interval
                if (snapG_) ls.snap();
                ls.tick();
            }
            const double m = (1.0 - ws) * ls.value() + ws * env_.value();
            const double f = std::exp2(lc + 4.0 * amt * m);
            fEff_[static_cast<std::size_t>(c)] = dsp::clamp(f, 10.0, 0.45 * fs_);
            const double gEnd = dsp::prewarp(fEff_[static_cast<std::size_t>(c)], fs_);
            if (snapG_) g_[static_cast<std::size_t>(c)] = gEnd;
            gInc_[static_cast<std::size_t>(c)] = (gEnd - g_[static_cast<std::size_t>(c)]) * inv;
        }
        snapG_ = false;
        // The LFO moves a whole interval per tick (the grid is counted from the
        // wake-up, so the phase never depends on how the host splits blocks).
        lfo_.advance(lfoHz_ / fs_, ctrl_);
    }
    void run(float* l, float* r, int n) noexcept {
        float* io[2] = {l, r};
        const bool env = env_.active();
        for (int i = 0; i < n; ++i) {
            if (env) env_.tick();
            const double w = typeFade_.tick();
            const double mix = mix_.tick();
            for (std::size_t c = 0; c < 2; ++c) {
                g_[c] += gInc_[c];
                const double g = g_[c];
                const double a1 = 1.0 / (1.0 + g * (g + k_));
                const double a2 = g * a1, a3 = g * a2;
                const double x = io[c][i];
                SvfState& s = st_[c];
                const double v3 = x - s.ic2;
                const double v1 = a1 * s.ic1 + a2 * v3;
                const double v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
                s.ic1 = 2.0 * v1 - s.ic1;
                s.ic2 = 2.0 * v2 - s.ic2;
                double y;
                switch (type_) {
                    case FilterType::LowPass: y = v2; break;
                    case FilterType::BandPass: y = k_ * v1; break;
                    case FilterType::HighPass: y = x - k_ * v1 - v2; break;
                    default: y = x; break;
                }
                const double filtered = x + w * (y - x);
                io[c][i] = static_cast<float>(x + mix * (filtered - x));
            }
        }
    }
    bool quiet() const noexcept {
        return st_[0].magnitude() + st_[1].magnitude() < kSleepFloor && !env_.active();
    }
    void sleepNow() noexcept {
        clearState();
        env_.clear();
        snapAll();
    }
    void onWake() noexcept {
        lfo_.reset();
        snapG_ = true;
    }

private:
    double lfoAt(double offset, int ch) noexcept { return lfo_.value(wave_, offset, ch); }
    void clearState() noexcept {
        st_[0].clear();
        st_[1].clear();
    }
    void snapAll() noexcept {
        logCut_.snap();
        logQ_.snap();
        amount_.snap();
        mix_.snap();
        type_ = wantType_;
        typeFade_.setTarget(type_ != FilterType::Off ? 1.0 : 0.0);
        typeFade_.snap();
        srcFade_.snap();
        snapG_ = true;
    }

    double fs_ = 48000.0;
    bool fresh_ = true, snapG_ = true;
    FilterType type_ = FilterType::LowPass, wantType_ = FilterType::LowPass;
    ModSource source_ = ModSource::Lfo;
    LfoWave wave_ = LfoWave::Sine;
    double lfoHz_ = 1.0, spread_ = 0.0, k_ = 1.4142135623730951;
    std::array<std::uint8_t, kMaxTriggerSources> triggers_{{kNoSource, kNoSource, kNoSource}};
    Smoothed logCut_, logQ_, amount_, mix_;
    Fade typeFade_, srcFade_;
    std::array<Smoothed, 2> lfoS_{};
    Ahr env_;
    Lfo lfo_;
    std::array<SvfState, 2> st_{};
    std::array<double, 2> g_{}, gInc_{}, fEff_{{2000.0, 2000.0}};
    LastParams<FilterFxParams> last_;
};

}  // namespace mw909::fx
