// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Stereo chorus effect.
//
//  * One modulated delay per side around `offset` (1..30 ms); the sine LFO
//    (0.02..10 Hz) swings it by +-depth * offset/2 (at most +-10 ms).
//  * Spatial 0..1: phase offset of the right LFO, 0..180 degrees.
//  * Feedback -0.95..0.95 from the main tap back into the line.
//  * Fatness 0..1 adds a second voice pair: a tap at 1.3x the offset swung by
//    a second LFO at 0.87x the rate, 90 degrees apart.
//  * Low/high cut (12 dB/oct, out at 20 Hz / 20 kHz) and gain on the wet
//    signal only; out = dry + mix (wet - dry).
// Reads use 4-point Hermite interpolation.

#pragma once

#include <array>
#include <cmath>

#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class Chorus : public StereoGate<Chorus> {
public:
    static constexpr double kMaxOffsetMs = 30.0;
    static constexpr double kMaxSwingMs = 10.0;
    static constexpr double kFatRatio = 1.3;

    // Allocates the delay lines (non-realtime).
    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) {
        fs_ = sampleRate;
        gatePrepare(sampleRate);
        const int maxD = static_cast<int>(std::ceil((kMaxOffsetMs * kFatRatio + kMaxSwingMs + 2.0) * 0.001 * sampleRate)) + 8;
        for (auto& line : line_) line.allocate(maxD);
        for (auto* s : {&offset_, &swing_}) {
            s->setTime(0.050, sampleRate);
            s->setEps(1.0e-5);
        }
        for (auto* s : {&fb_, &fat_, &gain_, &mix_, &spatial_}) {
            s->setTime(kDefaultSmoothSeconds, sampleRate);
            s->setEps(1.0e-7);
        }
        lowCut_.setKind(EqKind::HighPass);
        highCut_.setKind(EqKind::LowPass);
        lowCut_.prepare(sampleRate, ctrl_);
        highCut_.prepare(sampleRate, ctrl_);
        reset();
        setParams(ChorusParams{});
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        for (auto& line : line_) line.clear();
        lowCut_.clear();
        highCut_.clear();
        lfoA_.reset();
        lfoB_.reset();
        sinceLoud_ = 0;
        snapAll();   // silence: no glides pending
    }

    void setParams(const ChorusParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        const double off = sanitize(p.offsetMs, 8.0f, 1.0f, 30.0f);
        const double depth = sanitize(p.depth, 0.5f, 0.0f, 1.0f);
        offset_.setTarget(off * 0.001 * fs_);
        swing_.setTarget(std::min(depth * 0.5 * off, kMaxSwingMs) * 0.001 * fs_);
        rate_ = sanitize(p.rateHz, 0.5f, 0.02f, 10.0f);
        fb_.setTarget(sanitize(p.feedback, 0.0f, -0.95f, 0.95f));
        spatial_.setTarget(0.5 * sanitize(p.spatial, 0.5f, 0.0f, 1.0f));
        const double lc = sanitize(p.lowCutHz, 20.0f, 20.0f, 20000.0f);
        const double hc = sanitize(p.highCutHz, 20000.0f, 20.0f, 20000.0f);
        lowCut_.setTarget(lc > 20.0, lc, 0.70710678118654752, 0.0);
        highCut_.setTarget(hc < 20000.0, hc, 0.70710678118654752, 0.0);
        fat_.setTarget(sanitize(p.fatness, 0.0f, 0.0f, 1.0f));
        gain_.setTarget(dsp::dbToGain(sanitize(p.gainDb, 0.0f, -24.0f, 24.0f)));
        mix_.setTarget(sanitize(p.mix, 0.5f, 0.0f, 1.0f));
        if (fresh_ || !awake()) snapAll();
        fresh_ = false;
    }

    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }
    double tailSeconds() const noexcept {
        const double t = (offset_.target() * kFatRatio + swing_.target()) / fs_;
        const double fb = std::abs(fb_.target());
        return fb < 1.0e-6 ? t : t * (1.0 + std::log(1.0e-6) / std::log(fb));
    }

    // --- StereoGate hooks ---------------------------------------------------
    void control() noexcept {
        lowCut_.control();
        highCut_.control();
    }
    void run(float* l, float* r, int n) noexcept {
        int lastLoud = -1;   // index of the last sample written above the floor
        const double incA = rate_ / fs_, incB = rate_ * 0.87 / fs_;
        float* io[2] = {l, r};
        for (int i = 0; i < n; ++i) {
            const double off = offset_.tick(), sw = swing_.tick();
            const double fb = fb_.tick(), fat = fat_.tick(), sp = spatial_.tick();
            double wet[2];
            for (int c = 0; c < 2; ++c) {
                const double ph = c == 0 ? 0.0 : sp;
                const double dA = off + sw * fastSinCycles(lfoA_.phase() + ph);
                const double dB = off * kFatRatio + sw * fastSinCycles(lfoB_.phase() + ph + 0.25);
                const double a = line_[c].readHermite(std::max(2.0, dA));
                double b = 0.0;
                if (fat > 0.0) b = line_[c].readHermite(std::max(2.0, dB));
                const float w = kneeClip(static_cast<float>(io[c][i] + fb * a), 1.5f, 3.0f);
                line_[c].write(w);
                if (std::abs(w) > kSleepFloor) lastLoud = i;
                wet[c] = (a + fat * b) / (1.0 + 0.5 * fat);
            }
            wetL_[i] = static_cast<float>(wet[0]);
            wetR_[i] = static_cast<float>(wet[1]);
            lfoA_.advance(incA, 1);
            lfoB_.advance(incB, 1);
        }
        lowCut_.processStereo(wetL_.data(), wetR_.data(), n);
        highCut_.processStereo(wetL_.data(), wetR_.data(), n);
        for (int i = 0; i < n; ++i) {
            const double g = gain_.tick(), mix = mix_.tick();
            l[i] = static_cast<float>(l[i] + mix * (g * wetL_[i] - l[i]));
            r[i] = static_cast<float>(r[i] + mix * (g * wetR_[i] - r[i]));
        }
        // Samples since the last loud write, exact whatever the chunking.
        sinceLoud_ = lastLoud >= 0 ? n - 1 - lastLoud : sinceLoud_ + n;
    }
    bool quiet() const noexcept {
        return sinceLoud_ > static_cast<long>(offset_.value() * kFatRatio + swing_.value()) + 8 &&
               lowCut_.stateMagnitude() + highCut_.stateMagnitude() < kSleepFloor;
    }
    void sleepNow() noexcept {
        for (auto& line : line_) line.clear();
        lowCut_.clear();
        highCut_.clear();
        snapAll();
    }
    void onWake() noexcept {
        lfoA_.reset();
        lfoB_.reset();
        sinceLoud_ = 0;
    }

private:
    void snapAll() noexcept {
        for (auto* s : {&offset_, &swing_, &fb_, &fat_, &gain_, &mix_, &spatial_}) s->snap();
        lowCut_.snap();
        highCut_.snap();
    }

    static constexpr int kMaxChunk = 128;   // >= controlInterval() at every rate
    double fs_ = 48000.0;
    bool fresh_ = true;
    double rate_ = 0.5;
    std::array<DelayLine, 2> line_{};
    Smoothed offset_, swing_, fb_, fat_, gain_, mix_, spatial_;
    EqFilter<2> lowCut_, highCut_;
    Lfo lfoA_, lfoB_;
    std::array<float, kMaxChunk> wetL_{}, wetR_{};
    long sinceLoud_ = 0;
    LastParams<ChorusParams> last_;
};

}  // namespace mw909::fx
