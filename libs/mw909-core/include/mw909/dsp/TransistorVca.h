// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The pieces every analog voice of the original machine builds its dynamics
// from (research/01 §1.1, §1.3; research/05 §2.7):
//
//  * The accent buffer. Each voice's 6-bit accent DAC (0..5 V) drives a
//    non-inverting op-amp of gain 1 + 10k/4.7k = 3.128 (BD IC11b, SD IC34b,
//    toms IC18b/IC23b/IC45b, RS IC48b, HC IC26a). Its output cannot pass the
//    op-amp's positive swing (~13.5 V on +-15 V rails), so the DAC codes above
//    ~54 of 63 clip (research/03 §3.2). The toms also have 220k from the buffer
//    input to -15 V, an offset of about -0.54 V at the output.
//
//  * The transistor VCA cell. A selected transistor whose base is fed from an
//    envelope capacitor through 220..470 kOhm; the signal (a few tens of mV) is
//    on the emitter, the collector feeds the voice's summing amp. The gain is
//    proportional to the base current, which is linear above the base-emitter
//    threshold and exponential below it (Shockley, slope n Vt):
//        I_B R_b = s(V_env - V_BE),   s(x) = k ln(1 + e^(x/k)),  k = n Vt
//    The cell's gain saturates once the transistor is driven hard (research/01
//    §4.5, a single-source report of "little change above about 8 V"):
//        gain = s / (1 + s / V_sat)
//    V_sat is FITTED per voice to the measured accent spans.
//
//  * Envelope capacitors. Every node is C dV/dt = sum G_i (E_i - V) - I_B(V).
//    rcStep() advances one sample exactly for a node that is linear over the
//    step; the base current is linearised about the current voltage
//    (exponential Euler with the Jacobian), which is exact in the linear
//    region and stable everywhere. No leak is modelled above audibility: a
//    capacitor that drains only into a base gives an exactly exponential gain
//    decay (tau = R_b C) down to the base-emitter knee, ~40 dB under the
//    peak, and the recordings follow that to their noise floor (-50 dB and
//    more, research/01 §1.3).
//
//  * Tail termination below the knee (NOT a circuit mechanism as such).
//    Below the knee the base current is exponential in V, so a node that
//    drains only into a base would hang: its drive falls as 1/t and would
//    take minutes to reach -120 dBFS. Where the recordings reach that far
//    they keep falling instead (the toms' VCO-3, ENV3 being base-only, at
//    ~-170 dB/s below its knee; Tom.h), and nowhere do they show a hang.
//    So once a node's drive is kTailFloor (-70 dB) under its peak since it
//    was last charged, a drain of C / kDrainSeconds is added to it: the
//    drive then falls at ~-400 dB/s and the voice can sleep. Above that
//    floor the node is exactly the circuit, including the slower 1/t fall
//    between the knee (~40-45 dB under a typical peak) and the floor; the
//    recordings' tails end in their noise floor at -50..-65 dB, so they do
//    not decide between the two there. A voice whose node is measured to
//    keep falling below its knee gets a fitted leak (setLeak: tom ENV3).
//    A plain resistive leak cannot do this without bending
//    the audible tail: one that drains the node in a second or so already
//    equals the base current around -30 dB (the earlier 10 MOhm leak did
//    exactly that, review finding). The physical cause of the measured
//    sub-knee decay (capacitor or diode leakage?) is not identified.
//
// Header-only, allocation-free.

#pragma once

#include <cmath>

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

namespace analog {
inline constexpr double kAccentBufferGain = 1.0 + 10.0 / 4.7;  // 3.128
inline constexpr double kRailVolts = 13.5;                     // op-amp swing on +-15 V
inline constexpr double kVbe = 0.6;                            // VCA base-emitter threshold
inline constexpr double kKneeVolts = 0.026;                    // n Vt of the base junction
inline constexpr double kDiodeVolts = 0.6;                     // a silicon diode drop
inline constexpr double kTailFloor = 3.1623e-4;                // -70 dB re the node's peak drive (see above)
inline constexpr double kDrainSeconds = 0.5;                   // the termination drain's time constant
inline constexpr double kTomBufferOffset = -0.54;              // R118/R178/R381 220k to -15 V
}  // namespace analog

// ln(1 + e^z), safe for any z.
inline double softplus(double z) noexcept {
    if (z > 34.0) return z;
    if (z < -40.0) return std::exp(z);
    return std::log1p(std::exp(z));
}

// The logistic function (derivative of softplus).
inline double logistic(double z) noexcept {
    if (z > 40.0) return 1.0;
    if (z < -40.0) return std::exp(z);
    return 1.0 / (1.0 + std::exp(-z));
}

// Smooth upper limit: ~x well below `limit`, approaches `limit` with a knee of
// width `knee`. Monotonic, C1.
inline double softLimit(double x, double limit, double knee) noexcept {
    return limit - knee * softplus((limit - x) / knee);
}

// Accent-buffer output for an accent-line voltage (the DAC node, 0..5 V).
// Non-finite input is a normal hit's line voltage of 0 V (silence) clamped
// away; the result is 0..~13.5 V.
inline double accentBufferVolts(double accLine, double offset = 0.0) noexcept {
    const double a = std::isfinite(accLine) ? clamp(accLine, 0.0, 5.0) : 0.0;
    const double v = analog::kAccentBufferGain * a + offset;
    return std::max(0.0, softLimit(v, analog::kRailVolts, 0.15));
}

// Voltage a capacitor is charged to through `drops` volts of junctions from a
// source at `volts`, with a soft knee (a weak source still charges a little).
inline double chargeThroughJunctions(double volts, double drops) noexcept {
    return 0.05 * softplus((volts - drops) / 0.05);
}

// The base junction law: s = k ln(1 + e^z) and ds/dz-ish = logistic(z), for
// z = (V - V_BE) / k, with one exponential (and a log only near the knee).
inline void junction(double z, double& s, double& ds) noexcept {
    constexpr double k = analog::kKneeVolts;
    if (z > 30.0) {
        s = k * z;
        ds = 1.0;
    } else if (z > 12.0) {
        const double e = std::exp(-z);  // ln(1 + e^z) = z + ln(1 + e^-z) ~ z + e^-z
        s = k * (z + e);
        ds = 1.0 - e;
    } else if (z < -40.0) {
        s = 0.0;
        ds = 0.0;
    } else if (z < -12.0) {
        const double e = std::exp(z);
        s = k * e;
        ds = e;
    } else {
        const double e = std::exp(z);
        s = k * std::log1p(e);
        ds = e / (1.0 + e);
    }
}

// The VCA cell's base drive s(V - V_BE), in volts across the base resistor.
inline double vcaDrive(double vEnv) noexcept {
    double s, ds;
    junction((vEnv - analog::kVbe) * (1.0 / analog::kKneeVolts), s, ds);
    return s;
}
// Its derivative with respect to vEnv.
inline double vcaDriveSlope(double vEnv) noexcept {
    return logistic((vEnv - analog::kVbe) / analog::kKneeVolts);
}
// Collector gain for a base drive s (volts), saturating at vSat.
inline double vcaGain(double drive, double vSat) noexcept { return drive / (1.0 + drive / vSat); }


// Cells whose bases are fed through different resistors compare by base
// current: the drive referred to a 100 kOhm base resistor.
inline constexpr double kBaseRefOhms = 100.0e3;
inline double baseDrive(double vEnv, double baseOhms) noexcept { return vcaDrive(vEnv) * (kBaseRefOhms / baseOhms); }

// A VCA base fed from two envelope capacitors through r1 and r2 (the toms'
// VCO-1 cell). The base sees their Thevenin source; its voltage is that
// source less the drop across the Thevenin resistance, so when one capacitor
// has drained below the base it SINKS current from the other: the cell cuts
// off while the other capacitor still holds a volt or more.
struct TwoSourceBase {
    double vBase;  // base voltage (for the capacitors' branch currents)
    double drive;  // base drive referred to kBaseRefOhms
};
inline TwoSourceBase twoSourceBase(double v1, double r1, double v2, double r2) noexcept {
    const double g1 = 1.0 / r1, g2 = 1.0 / r2;
    const double rth = 1.0 / (g1 + g2);
    const double vth = (v1 * g1 + v2 * g2) * rth;
    const double s = vcaDrive(vth);  // I_B R_th
    return {vth - s, s * (kBaseRefOhms / rth)};
}

// One sample of C dV/dt = I0 - G V (I0 and G constant over the step), given as
// a = G dt / C and b = I0 dt / C. Exact (a short series where a is small,
// which is every slow envelope; expm1 otherwise).
inline double rcStep(double v, double a, double b) noexcept {
    double f;
    if (a < 0.02) f = 1.0 - a * (0.5 - a * (1.0 / 6.0 - a * (1.0 / 24.0 - a * (1.0 / 120.0))));
    else f = -std::expm1(-a) / a;
    return v + (b - a * v) * f;
}

// An envelope capacitor of `farads` with linear paths to sources and one or
// more VCA bases. Use: begin(), add paths, then step(dt).
class EnvNode {
public:
    void setCap(double farads) noexcept {
        invC_ = 1.0 / std::max(farads, 1.0e-15);
        gDrain_ = 1.0 / (invC_ * analog::kDrainSeconds);
    }
    void reset(double v = 0.0) noexcept {
        v_ = v;
        drive_ = 0.0;
        peak_ = 0.0;
    }
    double v() const noexcept { return v_; }
    // A fitted leak to ground (0: none), for nodes whose measured tail shows
    // one (the voice says which and why).
    void setLeak(double ohms) noexcept { gLeak_ = ohms > 0.0 ? 1.0 / ohms : 0.0; }
    // The tail termination's floor re the node's peak drive (default
    // analog::kTailFloor; a voice may raise it where its recordings stop
    // far above it, and says so).
    void setTailFloor(double ratio) noexcept { floor_ = clamp(ratio, 0.0, 0.1); }

    void begin() noexcept {
        g_ = gLeak_;
        i0_ = 0.0;
    }
    // A resistor `ohms` to a source at `volts`.
    void toSource(double ohms, double volts) noexcept { toConductance(1.0 / ohms, volts); }
    // The same with the conductance 1 / ohms given (a voice keeps it for a
    // resistance that is set by a knob, so the sample loop does not divide).
    void toConductance(double g, double volts) noexcept {
        g_ += g;
        i0_ += g * volts;
    }
    // A diode-isolated charging path: conducts only while the source is above
    // the node (the source already includes the junction drops). A charge
    // starts a new tail (see tail()).
    void chargeFrom(double ohms, double volts) noexcept {
        if (volts > v_) {
            toSource(ohms, volts);
            peak_ = 0.0;
        }
    }
    // The same with the conductance 1 / ohms given.
    void chargeFromConductance(double g, double volts) noexcept {
        if (volts > v_) {
            toConductance(g, volts);
            peak_ = 0.0;
        }
    }
    // A conductance to an arbitrary node voltage (two-capacitor networks: the
    // other node is held at its value from the start of the step).
    void toNode(double ohms, double other) noexcept { toSource(ohms, other); }
    // A VCA base through `ohms`, linearised about the present voltage. One
    // exponential gives both the drive and its slope; the drive (at the
    // voltage the step starts from) is kept for the voice's gain. Applies the
    // tail termination (header comment).
    void toBase(double ohms) noexcept { toBaseConductance(1.0 / ohms); }
    // The same with the conductance 1 / ohms given.
    void toBaseConductance(double g) noexcept {
        double s, ds;
        junction((v_ - analog::kVbe) * (1.0 / analog::kKneeVolts), s, ds);
        drive_ = s;
        // I_B ~ g (s + ds (V - v0)) = g ds V + g (s - ds v0)
        g_ += g * ds;
        i0_ -= g * (s - ds * v_);
        tail(s);
    }
    // The tail termination (header comment) for a node whose cell's drive is
    // `drive`: toBase() applies it; a node that reaches its base through
    // another network (dsp::twoSourceBase) calls it with that cell's drive.
    void tail(double drive) noexcept {
        if (drive > peak_) peak_ = drive;
        else if (drive < floor_ * peak_) g_ += gDrain_;
    }
    // The base drive s(V - V_BE) computed by the last toBase().
    double drive() const noexcept { return drive_; }
    // A current source (amperes, positive charges the node).
    void inject(double amps) noexcept { i0_ += amps; }

    void step(double dt) noexcept { v_ = rcStep(v_, g_ * dt * invC_, i0_ * dt * invC_); }

    // step(dt) of two independent nodes at once: where both take rcStep()'s
    // series (every slow envelope), as the two lanes of one 2-wide vector,
    // lane by lane the same arithmetic (so exactly the two steps); otherwise
    // one after the other.
    static void step2(EnvNode& p, EnvNode& q, double dt) noexcept {
        typedef double V2 __attribute__((vector_size(16)));
        const V2 a = V2{p.g_, q.g_} * dt * V2{p.invC_, q.invC_};
        if (!(a[0] < 0.02 && a[1] < 0.02)) {
            p.step(dt);
            q.step(dt);
            return;
        }
        const V2 b = V2{p.i0_, q.i0_} * dt * V2{p.invC_, q.invC_};
        const V2 v{p.v_, q.v_};
        const V2 f = 1.0 - a * (0.5 - a * (1.0 / 6.0 - a * (1.0 / 24.0 - a * (1.0 / 120.0))));
        const V2 r = v + (b - a * v) * f;
        p.v_ = r[0];
        q.v_ = r[1];
    }

private:
    double invC_ = 1.0e6, gDrain_ = 1.0e-6, gLeak_ = 0.0, floor_ = analog::kTailFloor;
    double v_ = 0.0;
    double g_ = 0.0, i0_ = 0.0;
    double drive_ = 0.0, peak_ = 0.0;
};

}  // namespace mw909::dsp
