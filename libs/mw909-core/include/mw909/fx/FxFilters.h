// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Filter, modulation and delay-line building blocks for the mixer and FX:
//
//  * EqFilter<N>: one N-channel trapezoidal (zero-delay-feedback) state-variable
//    filter section in any of the usual EQ shapes (cut, band, bell, shelf,
//    all-pass), Simper's mixing form. Frequency, Q and gain are smoothed on the
//    control grid; switching the section in or out crossfades it against the
//    dry signal, so nothing clicks. A section that is out costs nothing.
//    The response is the analog prototype through the bilinear transform with
//    the centre frequency pre-warped (exact at the centre).
//  * Lr4Crossover<N>: Linkwitz-Riley 24 dB/oct split whose bands sum to a
//    2nd-order all-pass (flat magnitude), plus the matching all-pass.
//  * Upsampler2x: polyphase IIR half-band interpolator, the mirror of
//    dsp::HalfbandStage, for the oversampled clippers.
//  * Ahr: attack-hold-release envelope fired by voice triggers.
//  * Lfo: free-running low-frequency oscillator, five waves.
//  * DelayLine: fractional delay line whose storage is allocated once; a
//    logical clear is O(1) (samples older than the last clear read as zero).

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "mw909/dsp/Halfband.h"
#include "mw909/dsp/Math.h"
#include "mw909/fx/FxCommon.h"

namespace mw909::fx {

// ---------------------------------------------------------------------------
// Zero-delay-feedback SVF core with a mixed output y = m0 x + m1 v1 + m2 v2,
// where v1 is the band-pass (peak gain Q) and v2 the low-pass state output.

struct SvfCoefs {
    double a1 = 1.0, a2 = 0.0, a3 = 0.0;
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
};

struct SvfState {
    double ic1 = 0.0, ic2 = 0.0;

    double tick(const SvfCoefs& c, double x) noexcept {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return c.m0 * x + c.m1 * v1 + c.m2 * v2;
    }
    // Low-pass and high-pass of the same tick (for crossovers).
    void tickLpHp(const SvfCoefs& c, double x, double& lp, double& hp, double k) noexcept {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        lp = v2;
        hp = x - k * v1 - v2;
    }
    void clear() noexcept { ic1 = ic2 = 0.0; }
    void flushTiny() noexcept {
        ic1 = dsp::killDenormal(ic1);
        ic2 = dsp::killDenormal(ic2);
    }
    void scale(double k) noexcept {
        ic1 *= k;
        ic2 *= k;
    }
    double magnitude() const noexcept { return std::abs(ic1) + std::abs(ic2); }
};

// One-pole TPT section (low-pass and high-pass outputs) with its state exposed,
// so its owner can flush it before it turns denormal and test it for rest.
class OnePoleTpt {
public:
    void setCutoff(double freqHz, double sampleRate) noexcept {
        const double g = dsp::prewarp(freqHz, sampleRate);
        G_ = g / (1.0 + g);
    }
    double lowpass(double x) noexcept {
        const double v = (x - s_) * G_;
        const double y = v + s_;
        s_ = y + v;
        return y;
    }
    double highpass(double x) noexcept { return x - lowpass(x); }
    void reset() noexcept { s_ = 0.0; }
    void flushTiny() noexcept { s_ = dsp::killDenormal(s_); }
    double state() const noexcept { return s_; }

private:
    double G_ = 0.0, s_ = 0.0;
};

enum class EqKind : std::uint8_t { HighPass = 0, LowPass, BandPass, Bell, LowShelf, HighShelf, AllPass };

// Coefficients of one shape. `g0` = tan(pi f / fs) (pre-warped), q = Q,
// gainDb for bell and shelves.
inline SvfCoefs svfDesign(EqKind kind, double g0, double q, double gainDb) noexcept {
    SvfCoefs c;
    double g = g0;
    double k = 1.0 / (q > 1.0e-6 ? q : 1.0e-6);
    const double A = std::pow(10.0, gainDb / 40.0);
    switch (kind) {
        case EqKind::HighPass: c.m0 = 1.0; c.m1 = -k; c.m2 = -1.0; break;
        case EqKind::LowPass: c.m0 = 0.0; c.m1 = 0.0; c.m2 = 1.0; break;
        case EqKind::BandPass: c.m0 = 0.0; c.m1 = k; c.m2 = 0.0; break;
        case EqKind::Bell:
            k = 1.0 / (q * A);
            c.m0 = 1.0; c.m1 = k * (A * A - 1.0); c.m2 = 0.0;
            break;
        case EqKind::LowShelf:
            g = g0 / std::sqrt(A);
            c.m0 = 1.0; c.m1 = k * (A - 1.0); c.m2 = A * A - 1.0;
            break;
        case EqKind::HighShelf:
            g = g0 * std::sqrt(A);
            c.m0 = A * A; c.m1 = k * (1.0 - A) * A; c.m2 = 1.0 - A * A;
            break;
        case EqKind::AllPass: c.m0 = 1.0; c.m1 = -2.0 * k; c.m2 = 0.0; break;
    }
    c.a1 = 1.0 / (1.0 + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

// Peak gain of a resonant 2nd-order low/high-pass of quality q (>= 1 for q <= 0.707).
inline double resonantPeakGain(double q) noexcept {
    return q > 0.70710678118654752 ? q / std::sqrt(1.0 - 0.25 / (q * q)) : 1.0;
}

// ---------------------------------------------------------------------------
// One smoothed, crossfaded EQ section for N channels. Coefficients are
// recomputed on the control grid while a parameter glides and interpolated
// linearly across the interval, so a sweep has no coefficient steps.

template <int N>
class EqFilter {
public:
    void prepare(double sampleRate, int ctrlInterval) noexcept {
        fs_ = sampleRate;
        ctrl_ = ctrlInterval;
        const double tickRate = sampleRate / static_cast<double>(ctrlInterval);
        logF_.setTime(0.030, tickRate);
        logF_.setEps(1.0e-5);
        logQ_.setTime(0.030, tickRate);
        logQ_.setEps(1.0e-5);
        gain_.setTime(0.030, tickRate);
        gain_.setEps(1.0e-4);
        fade_.setTime(0.010, sampleRate);
        clear();
        snap();
    }

    void setKind(EqKind k) noexcept {
        if (k != kind_) {
            kind_ = k;
            dirty_ = true;
        }
    }
    EqKind kind() const noexcept { return kind_; }

    // engaged: section in the signal path (it fades in or out on change).
    void setTarget(bool engaged, double freqHz, double q, double gainDb) noexcept {
        fade_.setTarget(engaged ? 1.0 : 0.0);
        logF_.setTarget(std::log2(dsp::clamp(freqHz, 1.0, 0.49 * fs_)));
        logQ_.setTarget(std::log(dsp::clamp(q, 0.05, 50.0)));
        gain_.setTarget(gainDb);
    }

    // Jump every smoother to its target (prepare, wake, first parameters).
    void snap() noexcept {
        fade_.snap();
        snapParams();
    }
    // The same but the in/out crossfade keeps running (a section switched on
    // while its owner was bypassed fades in from the dry signal).
    void snapParams() noexcept {
        logF_.snap();
        logQ_.snap();
        gain_.snap();
        if (fade_.value() == 0.0) clear();
        c_ = design();
        ramp_ = 0;
        dirty_ = false;
    }

    // Once per control tick: advance the slow smoothers and start a ramp.
    void control() noexcept {
        if (ramp_ > 0) {   // a ramp left over from a short chunk: land it
            c_ = cEnd_;
            ramp_ = 0;
        }
        if (!dirty_ && logF_.settled() && logQ_.settled() && gain_.settled()) return;
        if (!running()) {   // out of the path: jump to the targets for free
            logF_.snap();
            logQ_.snap();
            gain_.snap();
            c_ = design();
            dirty_ = false;
            return;
        }
        logF_.tick();
        logQ_.tick();
        gain_.tick();
        cEnd_ = design();
        dirty_ = false;
        const double inv = 1.0 / static_cast<double>(ctrl_);
        d_.a1 = (cEnd_.a1 - c_.a1) * inv;
        d_.a2 = (cEnd_.a2 - c_.a2) * inv;
        d_.a3 = (cEnd_.a3 - c_.a3) * inv;
        d_.m0 = (cEnd_.m0 - c_.m0) * inv;
        d_.m1 = (cEnd_.m1 - c_.m1) * inv;
        d_.m2 = (cEnd_.m2 - c_.m2) * inv;
        ramp_ = ctrl_;
    }

    // In the path (or fading).
    bool running() const noexcept { return fade_.value() > 0.0 || fade_.target() > 0.0; }
    bool settled() const noexcept {
        return ramp_ == 0 && fade_.settled() && logF_.settled() && logQ_.settled() && gain_.settled();
    }

    // Process a block in place (n must not cross a control tick).
    void processMono(float* x, int n) noexcept requires(N == 1) {
        if (!running()) return;
        if (fade_.settled() && fade_.value() == 1.0 && ramp_ == 0) {
            for (int i = 0; i < n; ++i) x[i] = static_cast<float>(s_[0].tick(c_, x[i]));
            return;
        }
        for (int i = 0; i < n; ++i) {
            stepRamp();
            const double w = fade_.tick();
            if (justOut(w)) return;   // out of the path from this sample: the rest is dry
            const double y = s_[0].tick(c_, x[i]);
            // w == 1 gives exactly the fast path's result (block-partition independence).
            x[i] = static_cast<float>(w >= 1.0 ? y : x[i] + w * (y - x[i]));
        }
    }
    void processStereo(float* l, float* r, int n) noexcept requires(N == 2) {
        if (!running()) return;
        if (fade_.settled() && fade_.value() == 1.0 && ramp_ == 0) {
            for (int i = 0; i < n; ++i) {
                l[i] = static_cast<float>(s_[0].tick(c_, l[i]));
                r[i] = static_cast<float>(s_[1].tick(c_, r[i]));
            }
            return;
        }
        for (int i = 0; i < n; ++i) {
            stepRamp();
            const double w = fade_.tick();
            if (justOut(w)) return;   // out of the path from this sample: the rest is dry
            const double yl = s_[0].tick(c_, l[i]);
            const double yr = s_[1].tick(c_, r[i]);
            l[i] = static_cast<float>(w >= 1.0 ? yl : l[i] + w * (yl - l[i]));
            r[i] = static_cast<float>(w >= 1.0 ? yr : r[i] + w * (yr - r[i]));
        }
    }

    // Per-sample access (for filters inside feedback loops): call step() once
    // per sample, then tick() per channel, but only while it returns > 0 (at 0
    // the section is out of the path: its output must be the dry input).
    double step() noexcept {
        stepRamp();
        const double w = fade_.tick();
        justOut(w);
        return w;
    }
    double tick(int ch, double x) noexcept { return s_[static_cast<std::size_t>(ch)].tick(c_, x); }
    double fade() const noexcept { return fade_.value(); }

    void clear() noexcept {
        for (auto& s : s_) s.clear();
    }
    // Rescale the state as if the input had always been k times larger (a
    // linear section only: lets a gain stage move across it without a step).
    void scaleState(double k) noexcept {
        for (auto& s : s_) s.scale(k);
    }
    double stateMagnitude() const noexcept {
        double m = 0.0;
        for (const auto& s : s_) m += s.magnitude();
        return m;
    }
    const SvfCoefs& coefs() const noexcept { return c_; }
    double currentQ() const noexcept { return std::exp(logQ_.value()); }

private:
    // The fade-out just reached 0: the section leaves the path at this very
    // sample (ramp landed, state cleared), so where a host block ends never
    // changes the state it keeps.
    bool justOut(double w) noexcept {
        if (w != 0.0 || fade_.target() != 0.0) return false;
        if (ramp_ > 0) {
            c_ = cEnd_;
            ramp_ = 0;
        }
        clear();
        return true;
    }

    SvfCoefs design() const noexcept {
        return svfDesign(kind_, dsp::prewarp(std::exp2(logF_.value()), fs_), std::exp(logQ_.value()), gain_.value());
    }
    void stepRamp() noexcept {
        if (ramp_ > 0) {
            if (--ramp_ == 0) {
                c_ = cEnd_;
            } else {
                c_.a1 += d_.a1;
                c_.a2 += d_.a2;
                c_.a3 += d_.a3;
                c_.m0 += d_.m0;
                c_.m1 += d_.m1;
                c_.m2 += d_.m2;
            }
        }
    }

    double fs_ = 48000.0;
    int ctrl_ = 16, ramp_ = 0;
    EqKind kind_ = EqKind::Bell;
    bool dirty_ = true;
    Smoothed logF_, logQ_, gain_;
    Fade fade_;
    SvfCoefs c_{}, cEnd_{}, d_{};
    std::array<SvfState, N> s_{};
};

// ---------------------------------------------------------------------------
// Linkwitz-Riley 4th-order crossover: lp4 = BW2lp(BW2lp(x)), hp4 = BW2hp(BW2hp(x)).
// lp4 + hp4 = 2nd-order all-pass at the same frequency (Q = 1/sqrt 2).

template <int N>
class Lr4Crossover {
public:
    void setFreq(double freqHz, double sampleRate) noexcept {
        const double g = dsp::prewarp(freqHz, sampleRate);
        bw_ = svfDesign(EqKind::LowPass, g, 0.70710678118654752, 0.0);
        ap_ = svfDesign(EqKind::AllPass, g, 0.70710678118654752, 0.0);
    }
    void split(int ch, double x, double& low, double& high) noexcept {
        auto& st = st_[static_cast<std::size_t>(ch)];
        double lp1, hp1, dummy;
        st[0].tickLpHp(bw_, x, lp1, hp1, kSqrt2);
        st[1].tickLpHp(bw_, lp1, low, dummy, kSqrt2);
        st[2].tickLpHp(bw_, hp1, dummy, high, kSqrt2);
    }
    // The phase twin: what a band that skipped this crossover must pass through.
    double allpass(int ch, double x) noexcept { return ap_st_[static_cast<std::size_t>(ch)].tick(ap_, x); }

    void clear() noexcept {
        for (auto& st : st_)
            for (auto& s : st) s.clear();
        for (auto& s : ap_st_) s.clear();
    }
    double stateMagnitude() const noexcept {
        double m = 0.0;
        for (const auto& st : st_)
            for (const auto& s : st) m += s.magnitude();
        for (const auto& s : ap_st_) m += s.magnitude();
        return m;
    }
    void flushTiny() noexcept {
        for (auto& st : st_)
            for (auto& s : st) s.flushTiny();
        for (auto& s : ap_st_) s.flushTiny();
    }

private:
    static constexpr double kSqrt2 = 1.41421356237309505;
    SvfCoefs bw_{}, ap_{};
    std::array<std::array<SvfState, 3>, N> st_{};
    std::array<SvfState, N> ap_st_{};
};

// ---------------------------------------------------------------------------
// Polyphase half-band interpolator 1 -> 2 (same all-pass pair as the
// decimator in dsp/Halfband.h). Gain 1 in the pass-band.

template <int S>
class Upsampler2x {
public:
    constexpr explicit Upsampler2x(const std::array<double, S>& coefs) noexcept : c_(coefs) {}
    void reset() noexcept {
        x1_.fill(0.0);
        y1_.fill(0.0);
    }
    // One input sample -> two output samples (older first).
    void process(double x, double& out0, double& out1) noexcept {
        double a = x, b = x;
        for (std::size_t i = 0; i < static_cast<std::size_t>(S); i += 2) {
            const double y = c_[i] * (a - y1_[i]) + x1_[i];
            x1_[i] = a;
            y1_[i] = y;
            a = y;
        }
        for (std::size_t i = 1; i < static_cast<std::size_t>(S); i += 2) {
            const double y = c_[i] * (b - y1_[i]) + x1_[i];
            x1_[i] = b;
            y1_[i] = y;
            b = y;
        }
        out0 = a;   // H(z) = (A0(z^2) + z^-1 A1(z^2)) / 2: A0 lands on the even output
        out1 = b;
    }
    double stateMagnitude() const noexcept {
        double m = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(S); ++i) m += std::abs(x1_[i]) + std::abs(y1_[i]);
        return m;
    }

private:
    std::array<double, S> c_;
    std::array<double, S> x1_{};
    std::array<double, S> y1_{};
};

// 1 -> factor (1, 2 or 4) interpolator, mirror of dsp::MetalDecimator.
class Interpolator {
public:
    void prepare(int factor) noexcept {
        factor_ = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);
        reset();
    }
    void reset() noexcept {
        first_.reset();
        second_.reset();
    }
    int factor() const noexcept { return factor_; }
    // Writes factor() samples (oldest first).
    void process(double x, double* out) noexcept {
        switch (factor_) {
            case 2: first_.process(x, out[0], out[1]); break;
            case 4: {
                double a, b;
                first_.process(x, a, b);
                second_.process(a, out[0], out[1]);
                second_.process(b, out[2], out[3]);
                break;
            }
            default: out[0] = x; break;
        }
    }
    double stateMagnitude() const noexcept { return first_.stateMagnitude() + second_.stateMagnitude(); }

private:
    int factor_ = 1;
    Upsampler2x<5> first_{dsp::kHalfbandFinalCoefs};    // base -> 2x: the sharp stage
    Upsampler2x<3> second_{dsp::kHalfbandFirstCoefs};   // 2x -> 4x: the relaxed stage
};

// ---------------------------------------------------------------------------
// Attack-hold-release envelope, 0..1. Attack is a linear ramp from wherever
// the envelope is (a retrigger never resets it); release falls exponentially
// and reaches -60 dB at the release time.

class Ahr {
public:
    void prepare(double sampleRate) noexcept {
        fs_ = sampleRate;
        clear();
    }
    void setTimes(double attackMs, double holdMs, double releaseMs) noexcept {
        attackInc_ = 1.0 / std::max(1.0, attackMs * 0.001 * fs_);
        holdSamples_ = static_cast<int>(std::lround(holdMs * 0.001 * fs_));
        releaseCoef_ = std::exp(-6.907755278982137 / std::max(1.0, releaseMs * 0.001 * fs_));
    }
    void fire() noexcept {
        stage_ = Stage::Attack;
        holdLeft_ = holdSamples_;
    }
    double tick() noexcept {
        switch (stage_) {
            case Stage::Attack:
                v_ += attackInc_;
                if (v_ >= 1.0) {
                    v_ = 1.0;
                    stage_ = Stage::Hold;
                }
                break;
            case Stage::Hold:
                if (--holdLeft_ <= 0) stage_ = Stage::Release;
                break;
            case Stage::Release:
                v_ *= releaseCoef_;
                if (v_ < 1.0e-7) {
                    v_ = 0.0;
                    stage_ = Stage::Idle;
                }
                break;
            case Stage::Idle: break;
        }
        return v_;
    }
    bool active() const noexcept { return stage_ != Stage::Idle; }
    double value() const noexcept { return v_; }
    void clear() noexcept {
        v_ = 0.0;
        stage_ = Stage::Idle;
        holdLeft_ = 0;
    }

private:
    enum class Stage : std::uint8_t { Idle, Attack, Hold, Release };
    double fs_ = 48000.0;
    double attackInc_ = 1.0, releaseCoef_ = 0.0;
    int holdSamples_ = 0, holdLeft_ = 0;
    double v_ = 0.0;
    Stage stage_ = Stage::Idle;
};

// ---------------------------------------------------------------------------
// LFO with a phase in cycles. value(offset) evaluates the wave at phase +
// offset, so a stereo pair shares one phase accumulator.

class Lfo {
public:
    void reset() noexcept {
        phase_ = 0.0;
        held_.fill(0.0f);
        cycle_.fill(-1);
    }
    void advance(double cyclesPerSample, int n) noexcept {
        phase_ += cyclesPerSample * static_cast<double>(n);
        if (phase_ >= 1048576.0) phase_ -= 1048576.0;   // keep precision, integer shift
    }
    double phase() const noexcept { return phase_; }

    // ch selects the random-hold slot (0 or 1).
    float value(LfoWave wave, double offsetCycles, int ch) noexcept {
        const double p = phase_ + offsetCycles;
        const double f = p - fastFloor(p);
        switch (wave) {
            case LfoWave::Sine: return fastSinCycles(p);
            case LfoWave::Triangle: return static_cast<float>(f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f);
            case LfoWave::Saw: return static_cast<float>(2.0 * f - 1.0);
            case LfoWave::Square: return f < 0.5 ? 1.0f : -1.0f;
            case LfoWave::Random: {
                const auto c = static_cast<std::int64_t>(fastFloor(p));
                auto& slot = held_[static_cast<std::size_t>(ch & 1)];
                auto& last = cycle_[static_cast<std::size_t>(ch & 1)];
                if (c != last) {
                    last = c;
                    slot = hashToUnit(static_cast<std::uint32_t>(c) * 2u + static_cast<std::uint32_t>(ch & 1));
                }
                return slot;
            }
        }
        return 0.0f;
    }

private:
    static float hashToUnit(std::uint32_t i) noexcept {
        return dsp::hashToBipolar(dsp::hash32(0x51ED27u, i));
    }
    double phase_ = 0.0;
    std::array<float, 2> held_{};
    std::array<std::int64_t, 2> cycle_{{-1, -1}};
};

// ---------------------------------------------------------------------------
// Delay line. allocate() is the only allocating call (prepare path). The
// storage is a power of two, so a read is a subtract and a mask; a sample
// older than the last clear() reads as zero (one compare, no branch taken in
// steady state).

class DelayLine {
public:
    void allocate(int maxDelaySamples) {
        int size = 1;
        while (size < maxDelaySamples + 4) size <<= 1;
        mask_ = size - 1;
        maxDelay_ = maxDelaySamples;
        buf_.assign(static_cast<std::size_t>(size), 0.0f);
        clear();
    }
    int maxDelay() const noexcept { return maxDelay_; }
    // O(1): what was written before reads as zero from now on.
    void clear() noexcept {
        w_ = 0;
        written_ = 0;
    }
    void write(float x) noexcept {
        buf_[static_cast<std::size_t>(w_)] = x;
        w_ = (w_ + 1) & mask_;
        if (written_ <= mask_) ++written_;
    }
    // Integer read: d = 1 is the sample written last (d >= 1).
    float read(int d) const noexcept {
        const float v = buf_[static_cast<std::size_t>((w_ - d) & mask_)];
        return d <= written_ ? v : 0.0f;
    }
    float readLinear(double d) const noexcept {
        const int i = static_cast<int>(d);
        const float f = static_cast<float>(d - static_cast<double>(i));
        const float a = read(i), b = read(i + 1);
        return a + (b - a) * f;
    }
    // 4-point Hermite (d >= 2 for causality with the newest sample at 1).
    float readHermite(double d) const noexcept {
        const int i = static_cast<int>(d);
        const float f = static_cast<float>(d - static_cast<double>(i));
        const float xm1 = read(i - 1), x0 = read(i), x1 = read(i + 1), x2 = read(i + 2);
        const float c0 = x0;
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }

private:
    std::vector<float> buf_;
    int mask_ = 0;
    int maxDelay_ = 0;
    int w_ = 0;
    int written_ = 0;
};

}  // namespace mw909::fx
