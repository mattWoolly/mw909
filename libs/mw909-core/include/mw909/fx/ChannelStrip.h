// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The per-voice channel strip (mono, before pan): a Filter/EQ section and a
// compressor, in either order (StripParams::compPreEq). Contract:
// docs/design/architecture.md §4.3.
//
// Filter/EQ: a 12 dB/oct low cut (high-pass) and high cut (low-pass) sharing one
// resonance (0 = Butterworth, 1 = Q 12), and two parametric bells, 0.5..4
// octaves wide, +-24 dB. Each filter is a zero-delay-feedback SVF tuned by the
// bilinear transform with pre-warping, so its response matches the analog
// prototype at its centre frequency at every sample rate. The low cut is out
// at 20 Hz and the high cut at 20 kHz, as the panel marks them.
//
// Compressor: feed-forward, instantaneous peak detector, 6 dB soft knee
// (fx/Compressor.h), threshold -48..0 dB, ratio 1..20, attack/release 1..500 ms
// (time constants), make-up 0..24 dB.
//
// Every section that is switched off costs nothing; switching crossfades over
// 10 ms, frequency/Q/gain glide over ~30 ms, make-up per sample. The strip goes
// to sleep (isActive() false, process() writes zeros for silent input without
// doing any filtering) once input, filter state and gain reduction are all below
// the -120 dBFS floor. Once every section has faded out (checked on the
// control grid, so at a sample position counted from the wake-up) the strip is
// bypassed: process() returns at once and the signal passes untouched; a
// section switched on again while signal passes fades in from dry.

#pragma once

#include <array>

#include "mw909/fx/Compressor.h"
#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class ChannelStrip {
public:
    static constexpr float kCompKneeDb = 6.0f;

    void prepare(double sampleRate) noexcept {
        fs_ = sampleRate;
        ctrl_ = controlInterval(sampleRate);
        lowCut_.setKind(EqKind::HighPass);
        highCut_.setKind(EqKind::LowPass);
        for (auto& b : bell_) b.setKind(EqKind::Bell);
        lowCut_.prepare(sampleRate, ctrl_);
        highCut_.prepare(sampleRate, ctrl_);
        for (auto& b : bell_) b.prepare(sampleRate, ctrl_);
        comp_.prepare(sampleRate);
        compFade_.setTime(0.010, sampleRate);
        makeup_.setTime(kDefaultSmoothSeconds, sampleRate);
        makeup_.setEps(1.0e-7);
        reset();
    }

    void reset() noexcept {
        lowCut_.clear();
        highCut_.clear();
        for (auto& b : bell_) b.clear();
        comp_.clear();
        lastGain_ = 1.0;
        last_.valid = false;
        awake_ = false;
        ctrlLeft_ = 0;
        passQuiet_ = kQuietLong;
        applyTargets();
        snapAll();
    }

    void setParams(const StripParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        const bool wasBypassed = !awake_ && isBypassed() && passQuiet_ < static_cast<long>(ctrl_);
        const bool eqOn = p.eqOn;
        const float lowHz = sanitize(p.lowCutHz, 20.0f, 20.0f, 2000.0f);
        const float highHz = sanitize(p.highCutHz, 20000.0f, 500.0f, 20000.0f);
        const float res = sanitize(p.resonance, 0.0f, 0.0f, 1.0f);
        const double q = resonanceToQ(res);
        p_.lowOn = eqOn && lowHz > 20.0f;
        p_.highOn = eqOn && highHz < 20000.0f;
        p_.lowHz = lowHz;
        p_.highHz = highHz;
        p_.q = q;
        for (std::size_t b = 0; b < 2; ++b) {
            const auto& eb = p.band[b];
            p_.bandOn[b] = eqOn && eb.on;
            p_.bandHz[b] = sanitize(eb.freqHz, b == 0 ? 200.0f : 3000.0f, 20.0f, 20000.0f);
            p_.bandQ[b] = octavesToQ(sanitize(eb.widthOct, 1.0f, 0.5f, 4.0f));
            p_.bandDb[b] = sanitize(eb.gainDb, 0.0f, -24.0f, 24.0f);
        }
        p_.compOn = p.compOn;
        p_.compPre = p.compPreEq;
        const float thr = sanitize(p.compThresholdDb, 0.0f, -48.0f, 0.0f);
        const float ratio = sanitize(p.compRatio, 4.0f, 1.0f, 20.0f);
        const float att = sanitize(p.compAttackMs, 10.0f, 1.0f, 500.0f);
        const float rel = sanitize(p.compReleaseMs, 100.0f, 1.0f, 500.0f);
        const float mk = sanitize(p.compMakeupDb, 0.0f, 0.0f, 24.0f);
        comp_.setParams(thr, ratio, att, 0.0, rel, kCompKneeDb);
        p_.makeup = dsp::dbToGain(mk);
        applyTargets();
        if (fresh_ || !awake_) {
            if (!fresh_ && wasBypassed) {   // sections switched on fade in from dry
                lowCut_.snapParams();
                highCut_.snapParams();
                for (auto& b : bell_) b.snapParams();
            } else {
                snapAll();
            }
        }
        fresh_ = false;
    }

    // In place.
    void process(float* x, int n) noexcept {
        int i = 0;
        while (i < n) {
            if (!awake_) {
                if (isBypassed()) {   // every section out: the rest passes dry
                    notePassed(x + i, n - i);
                    return;
                }
                const int j = i + firstAbove(x + i, nullptr, n - i, kSleepFloor);
                zeroBuf(x + i, j - i);
                i = j;
                if (i >= n) break;
                wake();
            }
            if (ctrlLeft_ == 0) {
                control();
                ctrlLeft_ = ctrl_;
                if (!awake_) continue;
            }
            const int m = std::min(n - i, ctrlLeft_);
            float* b = x + i;
            const float pin = peakAbs(b, m);
            inPeak_ = pin > inPeak_ ? pin : inPeak_;
            if (compPre_) {
                runComp(b, m);
                runEq(b, m);
            } else {
                runEq(b, m);
                runComp(b, m);
            }
            const float pout = peakAbs(b, m);
            outPeak_ = pout > outPeak_ ? pout : outPeak_;
            i += m;
            ctrlLeft_ -= m;
        }
    }

    // Any internal state above the sleep floor (or a section still fading).
    bool isActive() const noexcept { return awake_ && !isBypassed(); }

    // Both sections off and fully faded out: process() is a no-op.
    bool isBypassed() const noexcept {
        return !p_.compOn && !p_.lowOn && !p_.highOn && !p_.bandOn[0] && !p_.bandOn[1] &&
               !lowCut_.running() && !highCut_.running() && !bell_[0].running() && !bell_[1].running() &&
               compFade_.value() == 0.0 && compFade_.settled();
    }

    float gainReductionDb() const noexcept { return comp_.reductionDb(); }

private:
    struct Targets {
        bool lowOn = false, highOn = false;
        double lowHz = 20.0, highHz = 20000.0, q = 0.70710678118654752;
        std::array<bool, 2> bandOn{};
        std::array<double, 2> bandHz{{200.0, 3000.0}};
        std::array<double, 2> bandQ{{1.4142135623730951, 1.4142135623730951}};
        std::array<double, 2> bandDb{};
        bool compOn = false, compPre = false;
        float makeup = 1.0f;
    };

    void applyTargets() noexcept {
        lowCut_.setTarget(p_.lowOn, p_.lowHz, p_.q, 0.0);
        highCut_.setTarget(p_.highOn, p_.highHz, p_.q, 0.0);
        for (std::size_t b = 0; b < 2; ++b) bell_[b].setTarget(p_.bandOn[b], p_.bandHz[b], p_.bandQ[b], p_.bandDb[b]);
        compFade_.setTarget(p_.compOn ? 1.0 : 0.0);
        makeup_.setTarget(p_.makeup);
        if (p_.compPre != compPre_) {
            // The compressor's gain moves across the (linear) EQ: rescale the EQ
            // state to what it would hold had it always seen the new input, so
            // the swap is seamless while the gain is steady.
            const double g = std::max(lastGain_, 1.0e-6);
            const double k = p_.compPre ? g : 1.0 / g;
            lowCut_.scaleState(k);
            highCut_.scaleState(k);
            bell_[0].scaleState(k);
            bell_[1].scaleState(k);
            compPre_ = p_.compPre;
        }
    }

    void snapAll() noexcept {
        compFade_.snap();
        lowCut_.snap();
        highCut_.snap();
        for (auto& b : bell_) b.snap();
        snapParams();
    }
    // Everything but the section in/out fades.
    void snapParams() noexcept {
        lowCut_.snapParams();
        highCut_.snapParams();
        for (auto& b : bell_) b.snapParams();
        makeup_.snap();
        if (compFade_.value() == 0.0) comp_.clear();
    }

    // Samples since the bypassed input was last above the floor (backward scan).
    void notePassed(const float* x, int m) noexcept {
        for (int k = m - 1; k >= 0; --k)
            if (std::abs(x[k]) > kSleepFloor) {
                passQuiet_ = m - 1 - k;
                return;
            }
        passQuiet_ = passQuiet_ + m < kQuietLong ? passQuiet_ + m : kQuietLong;
    }

    void wake() noexcept {
        awake_ = true;
        ctrlLeft_ = 0;
        ticksAwake_ = 0;
        inPeak_ = outPeak_ = 0.0f;
    }

    void control() noexcept {
        lowCut_.control();
        highCut_.control();
        bell_[0].control();
        bell_[1].control();
        if (isBypassed()) {   // the last section finished fading out: bypass from this tick
            lowCut_.clear();
            highCut_.clear();
            bell_[0].clear();
            bell_[1].clear();
            comp_.clear();
            makeup_.snap();
            awake_ = false;
            passQuiet_ = 0;   // it was running: treat the bypassed signal as live
            return;
        }
        if (ticksAwake_++ > 0 && inPeak_ < kSleepFloor && outPeak_ < kSleepFloor) {
            const double mag = lowCut_.stateMagnitude() + highCut_.stateMagnitude() +
                               bell_[0].stateMagnitude() + bell_[1].stateMagnitude();
            const bool settled = lowCut_.settled() && highCut_.settled() && bell_[0].settled() &&
                                 bell_[1].settled() && compFade_.settled() && makeup_.settled();
            if (mag < kSleepFloor && comp_.idle() && settled) {
                lowCut_.clear();
                highCut_.clear();
                bell_[0].clear();
                bell_[1].clear();
                awake_ = false;
                passQuiet_ = kQuietLong;
            }
        }
        inPeak_ = outPeak_ = 0.0f;
    }

    void runEq(float* b, int m) noexcept {
        lowCut_.processMono(b, m);
        highCut_.processMono(b, m);
        bell_[0].processMono(b, m);
        bell_[1].processMono(b, m);
    }

    void runComp(float* b, int m) noexcept {
        if (compFade_.value() == 0.0 && compFade_.settled()) {
            lastGain_ = 1.0;
            makeup_.snap();   // out: the make-up waits at its target (never at a block-dependent value)
            return;
        }
        // One formula for every path: with the fade at 1 and the make-up settled
        // it is the same arithmetic sample for sample (block-partition independence).
        double gw = 1.0;
        if (compFade_.settled() && makeup_.settled()) {
            const double mk = makeup_.value();
            for (int i = 0; i < m; ++i) {
                gw = static_cast<double>(comp_.tick(std::abs(b[i]))) * mk;
                b[i] = static_cast<float>(b[i] * gw);
            }
        } else {
            for (int i = 0; i < m; ++i) {
                const double w = compFade_.tick();
                const double g = static_cast<double>(comp_.tick(std::abs(b[i]))) * makeup_.tick();
                gw = w >= 1.0 ? g : 1.0 + w * (g - 1.0);
                b[i] = static_cast<float>(b[i] * gw);
            }
            if (compFade_.value() == 0.0) {
                comp_.clear();
                makeup_.snap();
            }
        }
        lastGain_ = gw;
    }

    double fs_ = 48000.0;
    int ctrl_ = 16, ctrlLeft_ = 0, ticksAwake_ = 0;
    bool awake_ = false, fresh_ = true, compPre_ = false;
    float inPeak_ = 0.0f, outPeak_ = 0.0f;
    static constexpr long kQuietLong = 1L << 30;
    long passQuiet_ = kQuietLong;   // see notePassed()
    double lastGain_ = 1.0;   // the compressor stage's last gain (for order swaps)
    Targets p_{};
    EqFilter<1> lowCut_, highCut_;
    std::array<EqFilter<1>, 2> bell_{};
    CompressorCore comp_;
    LastParams<StripParams> last_;
    Fade compFade_;
    Smoothed makeup_;
};

}  // namespace mw909::fx
