// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The machine's single noise generator. One instance feeds every noise-using
// voice (BD click, SD snappy, toms, clap), as on the board, so their noise is
// correlated.
//
// The original is a 32-stage shift register clocked at a few hundred kHz
// (research/05 §0.7): across the audio band it is white. The CONTRACT is its
// spectral density, not its per-sample level: a sample at rate fs has
// variance  kDensity^2 * fs / 2,  so the noise in any audio band has the same
// RMS at every sample rate and a voice's filters see the same input level at
// 44.1 kHz and 192 kHz without rate compensation. (Unit RMS at 48 kHz.)
// The level is dimensionless; each voice scales it to the volts its own
// circuit sees.

#pragma once

#include <cmath>
#include <cstdint>

namespace mw909::dsp {

class WhiteNoise {
public:
    // One-sided density, 1/sqrt(Hz): sigma = kDensity * sqrt(fs / 2).
    static constexpr double kReferenceRate = 48000.0;
    static constexpr double kDensity = 1.0 / 154.919333848;   // 1/sqrt(24000): unit RMS at 48 kHz

    void prepare(double sampleRate) noexcept {
        sigma_ = static_cast<float>(kDensity * std::sqrt(sampleRate * 0.5));
    }
    void seed(std::uint32_t s) noexcept { state_ = s != 0U ? s : 0x1234abcdU; }

    // Approximately Gaussian (sum of four uniforms), standard deviation sigma().
    float next() noexcept {
        const float sum = uniform() + uniform() + uniform() + uniform();   // variance 4/3
        return sum * 0.8660254f * sigma_;
    }

    void fill(float* out, int n) noexcept {
        for (int i = 0; i < n; ++i) out[i] = next();
    }

    float sigma() const noexcept { return sigma_; }

private:
    float uniform() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(static_cast<std::int32_t>(state_)) * (1.0f / 2147483648.0f);
    }

    std::uint32_t state_ = 0x1234abcdU;
    float sigma_ = 1.0f;
};

}  // namespace mw909::dsp
