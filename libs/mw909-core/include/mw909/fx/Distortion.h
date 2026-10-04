// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Distortion effect: a diode clipper behind a 2-way crossover, so the low end
// can stay clean while the band above the split is driven.
//
//   x -> LR4 split at splitHz (below 20 Hz: no split, the full band is clipped)
//     low  -> lowOut gain                                   (unclipped)
//     high -> preamp (0..36 dB) -> diode clip (threshold, knee) -> clipOut gain
//   wet = low + clipped,  out = dry + mix (wet - dry)
//
// The clipper is linear up to threshold*(1 - knee) and approaches the
// threshold along a tanh curve above it (knee 0 = hard clip, 1 = a full tanh,
// the soft turn-on of a diode pair). It runs oversampled (4x below 88.2 kHz,
// 2x above) through polyphase IIR half-band filters; both bands and the dry
// signal pass the same interpolator and decimator, so they stay phase-aligned
// and the dry/wet mix has no comb. Pass-band to 0.2 fs of the oversampled rate
// (18 kHz at 44.1 kHz), group delay about 4 samples at low frequencies.

#pragma once

#include <array>

#include "mw909/dsp/Halfband.h"
#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class Distortion : public StereoGate<Distortion> {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) noexcept {
        fs_ = sampleRate;
        os_ = sampleRate < 88200.0 ? 4 : 2;
        gatePrepare(sampleRate);
        const double tick = sampleRate / static_cast<double>(ctrl_);
        logSplit_.setTime(0.030, tick);
        logSplit_.setEps(1.0e-5);
        logSplit_.snapTo(std::log2(200.0));
        splitOn_.setTime(0.010, sampleRate);
        for (auto* s : {&pre_, &lin_, &thr_, &lowOut_, &clipOut_, &mix_}) {
            s->setTime(kDefaultSmoothSeconds, sampleRate);
            s->setEps(1.0e-7);
        }
        for (int c = 0; c < 2; ++c) {
            up_[c][0].prepare(os_);
            up_[c][1].prepare(os_);
            down_[c].prepare(os_);
        }
        reset();
        setParams(DistortionParams{});
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        clearState();
        snapAll();
    }

    void setParams(const DistortionParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        const float pre = sanitize(p.preampDb, 0.0f, 0.0f, 36.0f);
        const float thrDb = sanitize(p.thresholdDb, 0.0f, -36.0f, 0.0f);
        const float knee = sanitize(p.knee, 0.5f, 0.0f, 1.0f);
        const float split = sanitize(p.splitHz, 200.0f, 0.0f, 2000.0f);
        const float lo = sanitize(p.lowOutDb, 0.0f, -60.0f, 12.0f);
        const float co = sanitize(p.clipOutDb, 0.0f, -60.0f, 12.0f);
        const float mix = sanitize(p.mix, 1.0f, 0.0f, 1.0f);
        const double thr = dsp::dbToGain(thrDb);
        pre_.setTarget(dsp::dbToGain(pre));
        thr_.setTarget(thr);
        lin_.setTarget(thr * (1.0 - knee));
        lowOut_.setTarget(lo <= -60.0f ? 0.0 : dsp::dbToGain(lo));
        clipOut_.setTarget(co <= -60.0f ? 0.0 : dsp::dbToGain(co));
        mix_.setTarget(mix);
        splitOn_.setTarget(split >= 20.0f ? 1.0 : 0.0);
        if (split >= 20.0f) logSplit_.setTarget(std::log2(static_cast<double>(split)));
        if (fresh_ || !awake()) snapAll();
        fresh_ = false;
    }

    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }
    double tailSeconds() const noexcept { return 0.01; }

    // --- StereoGate hooks ---------------------------------------------------
    void control() noexcept {
        if (!logSplit_.settled() || dirty_) {
            xover_.setFreq(std::exp2(logSplit_.tick()), fs_);
            dirty_ = false;
        }
    }
    void run(float* l, float* r, int n) noexcept {
        float* ch[2] = {l, r};
        for (int i = 0; i < n; ++i) {
            const double pre = pre_.tick(), thr = thr_.tick(), lin = lin_.tick();
            const double lo = lowOut_.tick(), co = clipOut_.tick(), mix = mix_.tick();
            const double w = splitOn_.tick();
            for (int c = 0; c < 2; ++c) {
                double low, high;
                const double x = ch[c][i];
                xover_.split(c, x, low, high);
                // Blend toward "no split": everything in the clipped band.
                low *= w;
                high = x + w * (high - x);
                double ul[4], uh[4], y[4];
                up_[c][0].process(low, ul);
                up_[c][1].process(high, uh);
                for (int k = 0; k < os_; ++k) {
                    const double clipped = kneeClip(static_cast<float>(uh[k] * pre), static_cast<float>(lin),
                                                    static_cast<float>(thr));
                    const double wet = ul[k] * lo + clipped * co;
                    const double dry = ul[k] + uh[k];
                    y[k] = dry + mix * (wet - dry);
                }
                ch[c][i] = static_cast<float>(down_[c].process(y));
            }
        }
    }
    bool quiet() const noexcept {
        double m = xover_.stateMagnitude();
        for (int c = 0; c < 2; ++c) m += up_[c][0].stateMagnitude() + up_[c][1].stateMagnitude();
        return m < kSleepFloor;
    }
    void sleepNow() noexcept {
        clearState();
        snapAll();
    }
    void onWake() noexcept {}

private:
    void clearState() noexcept {
        xover_.clear();
        for (int c = 0; c < 2; ++c) {
            up_[c][0].reset();
            up_[c][1].reset();
            down_[c].reset();
        }
    }
    void snapAll() noexcept {
        for (auto* s : {&pre_, &lin_, &thr_, &lowOut_, &clipOut_, &mix_}) s->snap();
        logSplit_.snap();
        splitOn_.snap();
        xover_.setFreq(std::exp2(logSplit_.value()), fs_);
        dirty_ = false;
    }

    double fs_ = 48000.0;
    int os_ = 4;
    bool fresh_ = true, dirty_ = true;
    Smoothed pre_, lin_, thr_, lowOut_, clipOut_, mix_, logSplit_;
    Fade splitOn_;
    Lr4Crossover<2> xover_;
    std::array<std::array<Interpolator, 2>, 2> up_{};
    std::array<dsp::MetalDecimator, 2> down_{};
    LastParams<DistortionParams> last_;
};

}  // namespace mw909::fx
