// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Feed-forward compressor core shared by the channel strip, the Dynamics
// effect and the master's 3-band compressor.
//
//   level  = |x| (stereo: max of |L|, |R|)            instantaneous peak
//   x_G    = 20 log10(level)
//   y_G    = soft-knee static curve (threshold T, ratio R, knee W dB):
//              x_G                                   2(x_G - T) < -W
//              x_G + (1/R - 1)(x_G - T + W/2)^2/(2W) |2(x_G - T)| <= W
//              T + (x_G - T)/R                       otherwise
//   x_L    = x_G - y_G  (gain reduction wanted, dB >= 0)
//   y_L    = branching one-pole in the dB domain: attack time constant while
//            x_L > y_L, then an optional hold, then release (the "smooth
//            branching peak detector" of Giannoulis, Massberg & Reiss, JAES 2012)
//   gain   = 10^(-y_L/20) * makeup
//
// Attack and release are the time constants (63 %) of the gain-reduction
// moves. No look-ahead (it would add latency). Below the knee and with no
// gain reduction left the core costs a compare per sample and is exactly unity.

#pragma once

#include <cmath>

#include "mw909/dsp/Math.h"
#include "mw909/fx/FxCommon.h"

namespace mw909::fx {

class CompressorCore {
public:
    void prepare(double sampleRate) noexcept {
        fs_ = sampleRate;
        clear();
        setParams(thrDb_, ratio_, 10.0, 0.0, 100.0, kneeDb_);
    }
    void clear() noexcept {
        yL_ = 0.0f;
        holdLeft_ = 0;
    }

    // Values must already be sanitised by the caller.
    void setParams(float thresholdDb, float ratio, double attackMs, double holdMs, double releaseMs,
                   float kneeDb) noexcept {
        thrDb_ = thresholdDb;
        ratio_ = ratio < 1.0f ? 1.0f : ratio;
        kneeDb_ = kneeDb < 0.0f ? 0.0f : kneeDb;
        slope_ = 1.0f / ratio_ - 1.0f;
        aA_ = static_cast<float>(dsp::expDecayCoeff(attackMs * 0.001, fs_));
        aR_ = static_cast<float>(dsp::expDecayCoeff(releaseMs * 0.001, fs_));
        hold_ = static_cast<int>(std::lround(holdMs * 0.001 * fs_));
        // Linear level below which the curve is exactly unity.
        kneeLoLin_ = dsp::dbToGain(thrDb_ - 0.5f * kneeDb_);
        if (ratio_ <= 1.0f) kneeLoLin_ = 1.0e30f;
    }

    // Static curve: gain reduction (dB, >= 0) for a level in dB.
    float staticReductionDb(float xg) const noexcept {
        const float d = xg - thrDb_;
        if (2.0f * d < -kneeDb_) return 0.0f;
        float yg;
        if (2.0f * std::abs(d) <= kneeDb_ && kneeDb_ > 0.0f) {
            const float t = d + 0.5f * kneeDb_;
            yg = xg + slope_ * t * t / (2.0f * kneeDb_);
        } else {
            yg = thrDb_ + d / ratio_;
        }
        return xg - yg;
    }

    // One sample: detector level (linear, >= 0) -> gain multiplier (without makeup).
    float tick(float level) noexcept {
        float xL = 0.0f;
        if (level > kneeLoLin_) xL = staticReductionDb(fastGainToDb(level));
        if (xL > yL_) {
            yL_ = aA_ * yL_ + (1.0f - aA_) * xL;
            holdLeft_ = hold_;
        } else if (holdLeft_ > 0) {
            --holdLeft_;
        } else if (yL_ > 0.0f) {
            yL_ = aR_ * yL_ + (1.0f - aR_) * xL;
            if (yL_ < 1.0e-5f) yL_ = 0.0f;
        }
        return yL_ > 0.0f ? fastDbToGain(-yL_) : 1.0f;
    }

    float reductionDb() const noexcept { return yL_; }
    bool idle() const noexcept { return yL_ == 0.0f && holdLeft_ == 0; }

private:
    double fs_ = 48000.0;
    float thrDb_ = 0.0f, ratio_ = 4.0f, kneeDb_ = 6.0f, slope_ = -0.75f;
    float aA_ = 0.0f, aR_ = 0.0f;
    int hold_ = 0, holdLeft_ = 0;
    float kneeLoLin_ = 1.0f;
    float yL_ = 0.0f;
};

}  // namespace mw909::fx
