// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The bridged-T resonator (carried over from the sibling 808 emulation, where
// it is proven): an op-amp with C1 (inverting input -> junction M) and C2
// (M -> output) in its feedback path, bridged by R2 (output -> inverting
// input), with a conductance G_M from M to ground. The excitation x drives the
// non-inverting input. The original machine's rim shot uses three of them
// (research/01 §6.1). With
//
//   w = x - V_M        (voltage across C1; V_M is the junction voltage)
//   u = Vout - x       (the ringing part of the output)
//
// nodal analysis with an ideal op-amp gives
//
//   dw/dt = a u                         a = 1 / (R2 C1)
//   du/dt = -d u - b w + drive          b = G_M / C2
//                                       d = (C1 + C2)/(R2 C1 C2)
//                                       drive = x G_M / C2
//
// so w0^2 = a b and the amplitude decays as exp(-d t / 2).
//
// Discretisation: trapezoidal (TPT) integrators with the zero-delay loop solved
// in closed form, per-sample coefficients, frequency pre-warped, and the damping
// corrected so the decay TIME (not Q) is exact at any sample rate.

#pragma once

#include <algorithm>
#include <cmath>

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

class BridgedT {
public:
    void prepare(double sampleRate) noexcept {
        T_ = 1.0 / sampleRate;
        reset();
    }
    void reset() noexcept { sw_ = su_ = w_ = u_ = 0.0; }

    // Integrator step and damping for one sample, pre-warped for the angular
    // frequency sqrt(w0sq).
    struct Step {
        double h = 0.0;   // trapezoidal half-step, pre-warped
        double dd = 0.0;  // damping, corrected for the decay time
    };
    Step step(double w0sq, double d) const noexcept {
        // y = (w0 T)^2, limited so the warp series below stays accurate.
        const double y = std::min(w0sq * T_ * T_, 2.5);
        const double x2 = 0.25 * y;
        // tan(x)/x for x = w0 T / 2: pre-warps the resonance.
        const double warp =
            1.0 + x2 * (1.0 / 3.0 + x2 * (2.0 / 15.0 + x2 * (17.0 / 315.0 + x2 * (62.0 / 2835.0))));
        // (w0 T)/sin(w0 T): the bilinear map shrinks the real part of a pole
        // pair by sin(w0 T)/(w0 T); undo it so the decay time is preserved.
        const double keep =
            1.0 + y * (1.0 / 6.0 + y * (7.0 / 360.0 + y * (31.0 / 15120.0 + y * (127.0 / 604800.0))));
        return {0.5 * T_ * warp, d * keep};
    }

    // One sample. a, b, d in 1/s and drive in V/s as defined above; all may
    // change every sample. Requires a > 0, b > 0, d >= 0.
    void tick(double a, double b, double d, double drive) noexcept { tick(step(a * b, d), a, b, drive); }

    // One sample with a precomputed step. A voice whose b depends on the state
    // (a diode or transistor load on the junction) warps for its small-signal
    // frequency, so that h does not swing with the state within a cycle.
    void tick(const Step& s, double a, double b, double drive) noexcept {
        const double ha = s.h * a;
        u_ = (su_ + s.h * (drive - b * sw_)) / (1.0 + s.h * s.dd + ha * s.h * b);
        w_ = sw_ + ha * u_;
        sw_ = w_ + ha * u_;
        su_ = u_ + s.h * (drive - s.dd * u_ - b * w_);
    }

    // The w that tick(s, a, b, drive) would produce, without committing it:
    // lets a voice evaluate a state-dependent load at the end of the step
    // (one fixed-point iteration of the implicit solve).
    double trialW(const Step& s, double a, double b, double drive) const noexcept {
        const double ha = s.h * a;
        const double u = (su_ + s.h * (drive - b * sw_)) / (1.0 + s.h * s.dd + ha * s.h * b);
        return sw_ + ha * u;
    }

    // The op-amp cannot swing past its supply rails. Call after tick(), with
    // the same excitation x: the op-amp's output is Vout = x + u, so it is
    // Vout, not the ringing part u alone, that the rails limit.
    void clampOutput(double rail, double x) noexcept {
        const double hi = rail - x, lo = -rail - x;
        if (u_ > hi) {
            u_ = hi;
            su_ = std::min(su_, hi);
        } else if (u_ < lo) {
            u_ = lo;
            su_ = std::max(su_, lo);
        }
    }

    double u() const noexcept { return u_; }  // Vout - x
    double w() const noexcept { return w_; }  // x - V_M
    // w one sample ahead (explicit Euler), for evaluating nonlinear junction
    // loads without a one-sample lag.
    double predictW(double a) const noexcept { return w_ + T_ * a * u_; }

    // True when the ringing amplitude (in volts at the op-amp output) is
    // certainly below `volts`: a u^2 + b w^2 is the oscillation energy.
    bool quieterThan(double a, double b, double volts) const noexcept {
        return a * u_ * u_ + b * w_ * w_ < a * volts * volts;
    }

private:
    double T_ = 1.0 / 48000.0;
    double sw_ = 0.0, su_ = 0.0;
    double w_ = 0.0, u_ = 0.0;
};

}  // namespace mw909::dsp
