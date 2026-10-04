// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Bitcrusher effect (stereo):
//
//   x -> approximation filter -> sample-and-hold at rateHz (with clock
//        jitter) -> quantiser (bits, optional TPDF dither) -> images filter
//   out = dry + mix (crushed - dry)
//
//  * Bits 1..24 (continuous): step = 2^(1 - bits) of full scale, mid-tread.
//    Dither adds triangular noise of +-1 step before rounding.
//  * Rate 44 Hz .. 44.1 kHz: a phase accumulator clocks the hold. The top of
//    the range (44.1 kHz, the default) means "no resampling" at every host
//    rate: every sample is taken, so a bits-only crush sounds the same at
//    44.1, 48 or 96 kHz. Below it, or at or above the host rate, the hold runs
//    at that rate. Jitter 0..1 randomises each hold period by up to +-50 %
//    (deterministic sequence).
//  * Approximation filter 0..1: 24 dB/oct Butterworth low-pass before the
//    hold at that fraction of the hold rate's Nyquist (1 = off).
//  * Images filter 0..1: the same filter after the hold, smoothing the steps
//    (1 = off).

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class Bitcrusher : public StereoGate<Bitcrusher> {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) noexcept {
        fs_ = sampleRate;
        gatePrepare(sampleRate);
        for (auto* f : {&approx_[0], &approx_[1], &images_[0], &images_[1]}) {
            f->setKind(EqKind::LowPass);
            f->prepare(sampleRate, ctrl_);
        }
        const double tick = sampleRate / static_cast<double>(ctrl_);
        bits_.setTime(0.030, tick);
        bits_.setEps(1.0e-4);
        mix_.setTime(kDefaultSmoothSeconds, sampleRate);
        mix_.setEps(1.0e-7);
        reset();
        setParams(BitcrusherParams{});
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        for (auto* f : {&approx_[0], &approx_[1], &images_[0], &images_[1]}) f->clear();
        onWake();
        snapAll();   // silence: no glides pending
    }

    void setParams(const BitcrusherParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        bits_.setTarget(sanitize(p.bits, 16.0f, 1.0f, 24.0f));
        dither_ = p.dither;
        rate_ = sanitize(p.rateHz, 44100.0f, 44.0f, static_cast<float>(kTopRate));
        jitter_ = sanitize(p.jitter, 0.0f, 0.0f, 1.0f);
        const double ap = sanitize(p.approxFilter, 1.0f, 0.0f, 1.0f);
        const double im = sanitize(p.imagesFilter, 1.0f, 0.0f, 1.0f);
        const double nyq = 0.5 * rate_;
        const double fa = std::max(20.0, ap * nyq), fi = std::max(20.0, im * nyq);
        approx_[0].setTarget(ap < 1.0, fa, kQ1, 0.0);
        approx_[1].setTarget(ap < 1.0, fa, kQ2, 0.0);
        images_[0].setTarget(im < 1.0, fi, kQ1, 0.0);
        images_[1].setTarget(im < 1.0, fi, kQ2, 0.0);
        mix_.setTarget(sanitize(p.mix, 1.0f, 0.0f, 1.0f));
        if (fresh_ || !awake()) snapAll();
        fresh_ = false;
    }

    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }
    double tailSeconds() const noexcept { return 0.05; }

    // --- StereoGate hooks ---------------------------------------------------
    void control() noexcept {
        for (auto* f : {&approx_[0], &approx_[1], &images_[0], &images_[1]}) f->control();
        step_ = std::exp2(1.0 - bits_.tick());
        inv_ = 1.0 / step_;
    }
    void run(float* l, float* r, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            dryL_[i] = l[i];
            dryR_[i] = r[i];
        }
        approx_[0].processStereo(l, r, n);
        approx_[1].processStereo(l, r, n);
        const double inc = rate_ >= kTopRate ? 1.0 : rate_ / fs_;
        for (int i = 0; i < n; ++i) {
            phase_ += inc;
            if (phase_ >= period_ || inc >= 1.0) {
                phase_ = inc >= 1.0 ? 0.0 : phase_ - period_;
                heldL_ = quantise(l[i]);
                heldR_ = quantise(r[i]);
                period_ = 1.0;
                if (jitter_ > 0.0f) period_ += 0.5 * static_cast<double>(jitter_) * noise();
            }
            l[i] = heldL_;
            r[i] = heldR_;
        }
        images_[0].processStereo(l, r, n);
        images_[1].processStereo(l, r, n);
        for (int i = 0; i < n; ++i) {
            const double mix = mix_.tick();
            l[i] = static_cast<float>(dryL_[i] + mix * (l[i] - dryL_[i]));
            r[i] = static_cast<float>(dryR_[i] + mix * (r[i] - dryR_[i]));
        }
    }
    bool quiet() const noexcept {
        double m = std::abs(heldL_) + std::abs(heldR_);
        for (const auto* f : {&approx_[0], &approx_[1], &images_[0], &images_[1]}) m += f->stateMagnitude();
        return m < kSleepFloor;
    }
    void sleepNow() noexcept {
        for (auto* f : {&approx_[0], &approx_[1], &images_[0], &images_[1]}) f->clear();
        snapAll();
        onWake();
    }
    void onWake() noexcept {
        phase_ = 1.0;   // the first sample after waking is taken at once
        period_ = 1.0;
        heldL_ = heldR_ = 0.0f;
        rng_ = 0x6C8E9CF5u;
    }

private:
    static constexpr double kTopRate = 44100.0;   // top of the knob: hold every sample
    static constexpr double kQ1 = 0.54119610014619698;   // 4th-order Butterworth pair
    static constexpr double kQ2 = 1.3065629648763766;
    static constexpr int kMaxChunk = 128;

    float quantise(float x) noexcept {
        double v = x;
        if (dither_) v += step_ * (noise() + noise()) * 0.5;   // TPDF, +-1 step
        return static_cast<float>(step_ * fastFloor(v * inv_ + 0.5));
    }
    // Uniform in [-1, 1).
    double noise() noexcept {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<double>(static_cast<std::int32_t>(rng_)) * (1.0 / 2147483648.0);
    }
    void snapAll() noexcept {
        for (auto* f : {&approx_[0], &approx_[1], &images_[0], &images_[1]}) f->snap();
        bits_.snap();
        mix_.snap();
        step_ = std::exp2(1.0 - bits_.value());
        inv_ = 1.0 / step_;
    }

    double fs_ = 48000.0;
    bool fresh_ = true, dither_ = false;
    double rate_ = 44100.0;
    float jitter_ = 0.0f;
    double step_ = 1.0 / 32768.0, inv_ = 32768.0;
    double phase_ = 1.0, period_ = 1.0;
    float heldL_ = 0.0f, heldR_ = 0.0f;
    std::uint32_t rng_ = 0x6C8E9CF5u;
    Smoothed bits_, mix_;
    std::array<EqFilter<2>, 2> approx_{}, images_{};
    std::array<float, kMaxChunk> dryL_{}, dryR_{};
    LastParams<BitcrusherParams> last_;
};

}  // namespace mw909::fx
