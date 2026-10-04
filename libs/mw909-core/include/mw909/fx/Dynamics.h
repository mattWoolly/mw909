// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Dynamics effect (stereo), two modes:
//
//  * Compressor: feed-forward, stereo-linked peak detector, 6 dB soft knee
//    (fx/Compressor.h), threshold -48..0 dB, ratio 1..20, attack / hold /
//    release, make-up. The key is the bus's own input, or, with `sidechain`
//    set to a voice, that voice's post-strip channel signal (the classic
//    kick-ducks-the-bass pump without routing the kick through the bus).
//  * VCA: an attack-hold-release envelope (fx/FxFilters.h Ahr) fired by the
//    hits of up to three voices; vcaDepth -1..+1 sets what it does:
//    negative ducks (gain 1 + depth * env), positive opens a gate on each hit
//    (gain (1 - depth) + depth * env). Make-up applies as the output gain.
// out = dry + mix (processed - dry). Mode changes crossfade over 10 ms.

#pragma once

#include <array>
#include <cmath>

#include "mw909/Voices.h"
#include "mw909/fx/Compressor.h"
#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class Dynamics : public StereoGate<Dynamics> {
public:
    static constexpr float kKneeDb = 6.0f;

    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) noexcept {
        fs_ = sampleRate;
        gatePrepare(sampleRate);
        comp_.prepare(sampleRate);
        env_.prepare(sampleRate);
        vcaFade_.setTime(0.010, sampleRate);
        for (auto* s : {&makeup_, &depth_, &mix_}) {
            s->setTime(kDefaultSmoothSeconds, sampleRate);
            s->setEps(1.0e-7);
        }
        reset();
        setParams(DynamicsParams{});
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        comp_.clear();
        env_.clear();
        snapAll();   // silence: no glides pending
    }

    void setParams(const DynamicsParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        mode_ = sanitizeEnum(p.mode, DynamicsMode::Compressor, 2);
        const float thr = sanitize(p.thresholdDb, -12.0f, -48.0f, 0.0f);
        const float ratio = sanitize(p.ratio, 4.0f, 1.0f, 20.0f);
        const float att = sanitize(p.attackMs, 5.0f, 1.0f, 500.0f);
        const float hold = sanitize(p.holdMs, 0.0f, 0.0f, 500.0f);
        const float rel = sanitize(p.releaseMs, 120.0f, 1.0f, 500.0f);
        comp_.setParams(thr, ratio, att, hold, rel, kKneeDb);
        env_.setTimes(att, hold, rel);
        makeup_.setTarget(dsp::dbToGain(sanitize(p.makeupDb, 0.0f, 0.0f, 24.0f)));
        depth_.setTarget(sanitize(p.vcaDepth, -1.0f, -1.0f, 1.0f));
        mix_.setTarget(sanitize(p.mix, 1.0f, 0.0f, 1.0f));
        sidechain_ = p.sidechain < kNumVoices ? p.sidechain : kNoSource;
        triggers_ = p.triggers;
        vcaFade_.setTarget(mode_ == DynamicsMode::Vca ? 1.0 : 0.0);
        if (fresh_ || !awake()) snapAll();
        fresh_ = false;
    }

    // A voice was triggered (the engine calls this for every hit).
    void noteTrigger(int voice) noexcept {
        if (mode_ != DynamicsMode::Vca && vcaFade_.value() == 0.0) return;
        for (auto t : triggers_)
            if (t != kNoSource && t == voice) {
                env_.fire();
                gateWake();
                return;
            }
    }

    // The voice whose channel keys the compressor, or kNoSource.
    int sidechainVoice() const noexcept { return sidechain_ == kNoSource ? -1 : sidechain_; }

    // key: the sidechain voice's signal for these n samples (null = silence).
    void process(float* l, float* r, int n, const float* key = nullptr) noexcept {
        keyed_ = sidechain_ != kNoSource;
        key_ = keyed_ ? key : nullptr;
        base_ = l;
        gateProcess(l, r, n);
        key_ = nullptr;
    }
    bool isActive() const noexcept { return awake(); }
    double tailSeconds() const noexcept { return 0.5; }
    float gainReductionDb() const noexcept { return comp_.reductionDb(); }
    double envelope() const noexcept { return env_.value(); }

    // --- StereoGate hooks ---------------------------------------------------
    // A loud key must move the detector even while the bus is silent.
    int firstWake(const float* l, const float* r, int i, int n) const noexcept {
        const int a = i + firstAbove(l + i, r + i, n - i, kSleepFloor);
        if (key_ == nullptr) return a;
        const int b = i + firstAbove(key_ + i, nullptr, n - i, kSleepFloor);
        return a < b ? a : b;
    }
    void control() noexcept {}
    void run(float* l, float* r, int n) noexcept {
        const float* key = key_ != nullptr ? key_ + (l - base_) : nullptr;
        for (int i = 0; i < n; ++i) {
            const double w = vcaFade_.tick();
            const double mk = makeup_.tick(), depth = depth_.tick(), mix = mix_.tick();
            double g = 1.0;
            if (w < 1.0) {
                float level;
                if (keyed_) level = key != nullptr ? std::abs(key[i]) : 0.0f;
                else level = std::max(std::abs(l[i]), std::abs(r[i]));
                g = comp_.tick(level) * mk;
            }
            if (w > 0.0) {
                const double e = env_.tick();
                const double gv = (depth < 0.0 ? 1.0 + depth * e : (1.0 - depth) + depth * e) * mk;
                g += w * (gv - g);
            }
            const double k = 1.0 + mix * (g - 1.0);
            l[i] = static_cast<float>(l[i] * k);
            r[i] = static_cast<float>(r[i] * k);
        }
    }
    bool quiet() const noexcept { return comp_.idle() && !env_.active(); }
    void sleepNow() noexcept {
        comp_.clear();
        env_.clear();
        snapAll();
    }
    void onWake() noexcept {}

private:
    void snapAll() noexcept {
        for (auto* s : {&makeup_, &depth_, &mix_}) s->snap();
        vcaFade_.snap();
    }

    double fs_ = 48000.0;
    bool fresh_ = true, keyed_ = false;
    DynamicsMode mode_ = DynamicsMode::Compressor;
    std::uint8_t sidechain_ = kNoSource;
    std::array<std::uint8_t, kMaxTriggerSources> triggers_{{kNoSource, kNoSource, kNoSource}};
    CompressorCore comp_;
    Ahr env_;
    Fade vcaFade_;
    Smoothed makeup_, depth_, mix_;
    const float* key_ = nullptr;
    const float* base_ = nullptr;
    LastParams<DynamicsParams> last_;
};

}  // namespace mw909::fx
