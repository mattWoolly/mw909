// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The switched-integrator triangle VCO of the original machine's analog voices
// (bass drum, snare, toms; research/01 §1.2, research/05 §2.1):
//
//   comparator (hysteresis +-V_th) -> square -> current source -> integrator C
//                                                 ^ slope set by the control voltage
//   trigger pulse: a transistor shorts C through ~10 kOhm (the integrator
//   output falls to 0 V) and the comparator is forced to one state, so every
//   hit starts at 0 V, mid-slope, in a fixed direction.
//
// The state is the integrator voltage normalised to the comparator thresholds
// (+-1). The slopes are given per sample (in normalised units, a full swing is
// 2): a symmetric triangle of frequency f has slope 4 f / fs. The two slopes
// may differ (the toms' CMOS supply rails, research/01 §5.3), which skews the
// triangle exactly as the circuit does.
//
// Discretisation (research/05 §0.6, §2.1): the waveform is computed exactly at
// the sample instants, segment by segment within each sample, and every slope
// discontinuity (the two comparator corners, the start of the short and the
// release at the end of the trigger pulse) gets a 2-point polyBLAMP residual.
// The short itself is an RC discharge (time constant R_short C), which has no
// value step, so no polyBLEP is needed. The residual reaches one sample back,
// so the output is delayed by one sample; voices use every VCO with the same
// delay, so it is a constant 1-sample offset of the tone (20 us at 48 kHz).
// Header-only, allocation-free, deterministic.

#pragma once

#include <cmath>

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

class TriangleVco {
public:
    // resetTauSamples: R_short * C in samples (the integrator short).
    void prepare(double resetTauSamples) noexcept {
        resetTau_ = std::max(resetTauSamples, 1.0e-3);
        reset();
    }

    // Silence: integrator at 0 V, comparator in the release state.
    void reset() noexcept {
        v_ = 0.0;
        dir_ = 1;
        held_ = false;
        startHold_ = false;
        prev_ = 0.0;
        lastSlope_ = 0.0;
    }

    // The trigger pulse starts at the next sample: the integrator is shorted
    // from then on until release().
    void hold() noexcept {
        if (!held_) startHold_ = true;
        held_ = true;
    }

    // The pulse ends `fraction` (0..1] of the way into the next sample; the
    // comparator was forced so the ramp starts in direction `dir` (+1 or -1).
    void release(double fraction, int dir) noexcept {
        releaseAt_ = clamp(fraction, 0.0, 1.0);
        releaseDir_ = dir >= 0 ? 1 : -1;
        releasePending_ = true;
    }

    bool held() const noexcept { return held_; }
    double state() const noexcept { return v_; }

    // One sample. up / down: slopes of the rising and falling segments, in
    // normalised units per sample (must be < 1). Returns the band-limited
    // triangle (normalised, +-1) of the PREVIOUS sample instant.
    double tick(double up, double down) noexcept {
        up = clamp(up, 0.0, 0.9);
        down = clamp(down, 0.0, 0.9);
        double cur = prev_;     // sample n (to receive the residual reaching back)
        double next = 0.0;      // sample n+1 (accumulated residual)
        double t = 0.0;         // position inside the sample, 0..1

        if (startHold_) {
            // Slope jumps from the free-running ramp to the RC discharge at t = 0.
            const double slope = -v_ / resetTau_;
            blamp(slope - lastSlope_, 1.0, cur, next);
            lastSlope_ = slope;
            startHold_ = false;
        }
        if (held_) {
            const double hEnd = releasePending_ ? releaseAt_ : 1.0;
            v_ *= std::exp(-hEnd / resetTau_);
            t = hEnd;
            lastSlope_ = -v_ / resetTau_;
            if (releasePending_) {
                held_ = false;
                releasePending_ = false;
                dir_ = releaseDir_;
                const double slope = dir_ > 0 ? up : -down;
                blamp(slope - lastSlope_, 1.0 - t, cur, next);
                lastSlope_ = slope;
            }
        }
        // Free-running part: linear segments with corners at +-1.
        int guard = 0;
        while (!held_ && t < 1.0 && guard++ < 8) {
            const double slope = dir_ > 0 ? up : -down;
            if (slope == 0.0) break;
            const double remain = 1.0 - t;
            const double target = dir_ > 0 ? 1.0 : -1.0;
            if (clearOfCorner(target - v_, slope, remain)) {  // need >= remain, without the division
                v_ += slope * remain;
                t = 1.0;
                lastSlope_ = dir_ > 0 ? up : -down;
                continue;
            }
            const double need = (target - v_) / slope;  // time to the corner
            if (need >= remain) {
                v_ += slope * remain;
                t = 1.0;
            } else {
                v_ = target;
                t += std::max(need, 0.0);
                dir_ = -dir_;
                const double newSlope = dir_ > 0 ? up : -down;
                blamp(newSlope - slope, 1.0 - t, cur, next);
            }
            lastSlope_ = dir_ > 0 ? up : -down;
        }
        next += v_;
        prev_ = next;
        return cur;
    }

private:
    // True only where the time to the corner, (target - v) / slope, rounds to
    // at least `remain` (> 0): the gap is larger than slope x remain with a
    // margin (4 ulp) that covers both roundings of the test, so the rounded
    // quotient is >= remain as well. Most samples do not reach a corner; this
    // spares them the division and decides exactly as it would (a gap within
    // the margin falls back to the division).
    static bool clearOfCorner(double gap, double slope, double remain) noexcept {
        const double g = slope > 0.0 ? gap : -gap;    // exact
        const double s = slope > 0.0 ? slope : -slope;
        return s * remain * (1.0 + 0x1p-50) < g;
    }

    // 2-point polyBLAMP: a slope change `delta` (per sample) at a distance d
    // (0..1] before sample n+1 adds delta d^3/6 to sample n and
    // delta (1-d)^3/6 to sample n+1.
    static void blamp(double delta, double d, double& atN, double& atN1) noexcept {
        const double e = 1.0 - d;
        atN += delta * d * d * d * (1.0 / 6.0);
        atN1 += delta * e * e * e * (1.0 / 6.0);
    }

    double resetTau_ = 4.0;
    double v_ = 0.0;
    int dir_ = 1;
    bool held_ = false, startHold_ = false;
    bool releasePending_ = false;
    double releaseAt_ = 1.0;
    int releaseDir_ = 1;
    double prev_ = 0.0;
    double lastSlope_ = 0.0;
};

}  // namespace mw909::dsp
