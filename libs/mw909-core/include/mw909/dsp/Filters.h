// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Topology-preserving (zero-delay-feedback) filters. They stay stable and keep
// their tuning under per-sample coefficient changes, which the voices rely on
// for pitch sweeps. Header-only, allocation-free, double-precision state.

#pragma once

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

// One-pole TPT section with simultaneous low-pass and high-pass outputs.
class OnePole {
public:
    void setCutoff(double freqHz, double sampleRate) noexcept {
        const double g = prewarp(freqHz, sampleRate);
        G_ = g / (1.0 + g);
    }
    void reset(double state = 0.0) noexcept { s_ = state; }

    double lowpass(double x) noexcept {
        const double v = (x - s_) * G_;
        const double y = v + s_;
        s_ = y + v;
        return y;
    }
    double highpass(double x) noexcept { return x - lowpass(x); }

private:
    double G_ = 0.0;
    double s_ = 0.0;
};

// Two-pole TPT state-variable filter (Zavalishin / Simper form).
//
//   H_bp(s) = (s/w0) / (s^2/w0^2 + k s/w0 + 1),   k = 1/Q
//
// tick() computes all three responses; read the one(s) you need. `bandpass()`
// has peak gain Q; `bandpassUnity()` is normalised to unity peak gain.
// k may be driven to zero or slightly negative by a caller modelling
// regenerative feedback; the caller is then responsible for bounding it.
class Svf {
public:
    void setFreqQ(double freqHz, double q, double sampleRate) noexcept {
        setG(prewarp(freqHz, sampleRate), 1.0 / std::max(q, 1.0e-6));
    }
    // Direct coefficient form for time-varying use: g = tan(pi f / fs), k = 1/Q.
    void setG(double g, double k) noexcept {
        g_ = g;
        k_ = k;
        a1_ = 1.0 / (1.0 + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }
    void reset() noexcept { ic1_ = ic2_ = 0.0; lp_ = bp_ = hp_ = 0.0; }

    void tick(double x) noexcept {
        const double v3 = x - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        bp_ = v1;
        lp_ = v2;
        hp_ = x - k_ * v1 - v2;
    }

    double lowpass() const noexcept { return lp_; }
    double bandpass() const noexcept { return bp_; }
    double bandpassUnity() const noexcept { return k_ * bp_; }
    double highpass() const noexcept { return hp_; }

    // Internal energy proxy, for voice sleep decisions.
    double stateMagnitude() const noexcept { return std::abs(ic1_) + std::abs(ic2_); }

private:
    double g_ = 0.0, k_ = 1.0;
    double a1_ = 0.0, a2_ = 0.0, a3_ = 0.0;
    double ic1_ = 0.0, ic2_ = 0.0;
    double lp_ = 0.0, bp_ = 0.0, hp_ = 0.0;
};

// One-pole parameter smoother (per-sample or per-block ticking).
class Smoother {
public:
    void setTime(double seconds, double tickRate) noexcept {
        coeff_ = static_cast<float>(1.0 - expDecayCoeff(seconds, tickRate));
    }
    void snap(float value) noexcept { z_ = target_ = value; }
    void setTarget(float value) noexcept { target_ = value; }
    float tick() noexcept {
        z_ += (target_ - z_) * coeff_;
        return z_;
    }
    float value() const noexcept { return z_; }
    bool settled() const noexcept { return std::abs(target_ - z_) < 1.0e-5f; }

private:
    float coeff_ = 1.0f;
    float z_ = 0.0f;
    float target_ = 0.0f;
};

}  // namespace mw909::dsp
