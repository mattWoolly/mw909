// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// 2:1 and 4:1 decimation for the oversampled metallic voices.
//
// Each stage is a polyphase IIR half-band: two branches of first-order
// all-pass sections running at the low rate, H(z) = (A0(z^2) + z^-1 A1(z^2))/2
// (the elliptic "all-pass pair" design of Valenzuela & Constantinides). It is
// chosen over a linear-phase FIR for its short delay: a 70 dB FIR with this
// transition band needs ~47 taps, 11.5 samples of latency at the output rate.
//
//   Final stage (2x -> 1x), 5 sections, transition 0.092 fs:
//     pass-band flat (ripple < 1e-6 dB) to 0.204 fs_hi = 18.0 kHz at 2 x 44.1 kHz,
//     stop-band below -83 dB from 0.296 fs_hi (26.1 kHz).
//     Group delay 2.75 high-rate samples at DC = 1.38 output samples
//     (1.74 at 10 kHz, 3.6 at 18 kHz, for 44.1 kHz output).
//   First stage of 4x (4x -> 2x), 3 sections, transition 0.2 fs:
//     only has to clear what would fold onto the final pass-band;
//     stop-band below -76 dB, group delay 2.04 samples at 4x = 0.51 output samples.
//
// Coefficients were designed offline and are checked by tests/unit/dsp_metal_halfband.cpp.

#pragma once

#include <array>

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

template <int N>
class HalfbandStage {
public:
    constexpr explicit HalfbandStage(const std::array<double, N>& coefs) noexcept : c_(coefs) {}

    void reset() noexcept {
        x1_.fill(0.0);
        y1_.fill(0.0);
    }

    // Two consecutive input samples (older, newer) -> one output sample.
    double process(double older, double newer) noexcept {
        double a = newer;  // branch 0: even-numbered sections
        double b = older;  // branch 1: odd-numbered sections, plus the z^-1
        for (std::size_t i = 0; i < static_cast<std::size_t>(N); i += 2) {
            const double y = c_[i] * (a - y1_[i]) + x1_[i];
            x1_[i] = a;
            y1_[i] = y;
            a = y;
        }
        for (std::size_t i = 1; i < static_cast<std::size_t>(N); i += 2) {
            const double y = c_[i] * (b - y1_[i]) + x1_[i];
            x1_[i] = b;
            y1_[i] = y;
            b = y;
        }
        return 0.5 * (a + b);
    }

    double stateMagnitude() const noexcept {
        double m = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(N); ++i) m += std::abs(x1_[i]) + std::abs(y1_[i]);
        return m;
    }

private:
    std::array<double, N> c_;
    std::array<double, N> x1_{};
    std::array<double, N> y1_{};
};

inline constexpr std::array<double, 5> kHalfbandFinalCoefs{
    0.057302914247913256, 0.20933578755379315, 0.41321920393737094,
    0.63484351877280443, 0.86930360804384899};
inline constexpr std::array<double, 3> kHalfbandFirstCoefs{
    0.081430023176615865, 0.31565984021665966, 0.70977080010247717};

// Decimator by 1, 2 or 4 with a fixed layout (no allocation).
class MetalDecimator {
public:
    static constexpr int kMaxFactor = 4;

    void prepare(int factor) noexcept {
        factor_ = factor >= 4 ? 4 : (factor >= 2 ? 2 : 1);
        reset();
    }
    void reset() noexcept {
        first_.reset();
        final_.reset();
    }
    int factor() const noexcept { return factor_; }

    // `in` holds factor() consecutive samples at the high rate, oldest first.
    double process(const double* in) noexcept {
        switch (factor_) {
            case 2: return final_.process(in[0], in[1]);
            case 4: {
                const double a = first_.process(in[0], in[1]);
                const double b = first_.process(in[2], in[3]);
                return final_.process(a, b);
            }
            default: return in[0];
        }
    }

    // Low-frequency group delay in output samples.
    double latencySamples() const noexcept {
        switch (factor_) {
            case 2: return 2.75 * 0.5;
            case 4: return 2.04 * 0.25 + 2.75 * 0.5;
            default: return 0.0;
        }
    }

private:
    int factor_ = 1;
    HalfbandStage<3> first_{kHalfbandFirstCoefs};
    HalfbandStage<5> final_{kHalfbandFinalCoefs};
};

}  // namespace mw909::dsp
