// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The host-rate anti-alias step of the sample voices' band-limited ZOH
// (dsp/PcmBlep.h): the running integral of a Kaiser-windowed sinc with its
// -6 dB point at the host Nyquist, normalised to end at exactly 1. With time
// in host samples it does not depend on the sample rate or on the unit, only
// on the kernel length, so it is computed once per process (at static
// initialisation, before any audio thread exists) for the four lengths the
// voices use, and PcmStepKernel::build() only has to run the analog
// reconstruction filter over it. This keeps a voice's prepare() far below a
// block's deadline (it runs on the audio thread when the engine is
// reconfigured).

#include <array>
#include <cmath>
#include <cstddef>

#include "mw909/dsp/PcmBlep.h"

namespace mw909::dsp::detail {

namespace {

constexpr int kPhases = PcmStepKernel::kPhases;
constexpr double kBeta = PcmStepKernel::kKaiserBeta;

constexpr std::size_t tableSize(int taps) noexcept { return static_cast<std::size_t>(taps * kPhases + 1); }

double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1.0e-17 * sum) break;
    }
    return sum;
}

// out[m], m = 0 .. taps*kPhases: the normalised AA step at t = -taps/2 + m/kPhases.
template <std::size_t N>
void buildStep(int taps, std::array<float, N>& out) noexcept {
    const int d = taps / 2;
    const int n = taps * kPhases;
    const double i0Beta = besselI0(kBeta);
    const double pi = 3.14159265358979323846;
    // The kernel h(m) (symmetric about m = n/2), then its trapezoid integral.
    std::array<double, N> h{};
    for (int m = 0; m <= n / 2; ++m) {
        const double t = -static_cast<double>(d) + static_cast<double>(m) / kPhases;
        double v = 0.0;
        if (m > 0) {
            const double r = t / static_cast<double>(d);
            const double w = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta;
            const double x = pi * t;
            v = (std::abs(x) < 1.0e-12 ? 1.0 : std::sin(x) / x) * w;
        }
        h[static_cast<std::size_t>(m)] = v;
        h[static_cast<std::size_t>(n - m)] = v;
    }
    double area = 0.0;
    for (int m = 1; m <= n; ++m) area += 0.5 * (h[static_cast<std::size_t>(m - 1)] + h[static_cast<std::size_t>(m)]);
    double acc = 0.0;
    out[0] = 0.0f;
    for (int m = 1; m <= n; ++m) {
        acc += 0.5 * (h[static_cast<std::size_t>(m - 1)] + h[static_cast<std::size_t>(m)]);
        out[static_cast<std::size_t>(m)] = static_cast<float>(acc / area);
    }
    out[static_cast<std::size_t>(n)] = 1.0f;
}

struct Tables {
    std::array<float, tableSize(16)> t16{};
    std::array<float, tableSize(32)> t32{};
    std::array<float, tableSize(48)> t48{};
    std::array<float, tableSize(64)> t64{};
    Tables() noexcept {
        buildStep(16, t16);
        buildStep(32, t32);
        buildStep(48, t48);
        buildStep(64, t64);
    }
};

const Tables& tables() noexcept {
    static const Tables t;
    return t;
}

// Built during static initialisation, so the audio thread only ever takes the
// initialised fast path of the function-local static above.
[[maybe_unused]] const Tables& kBuiltAtLoad = tables();

}  // namespace

const float* pcmAaStep(int taps) noexcept {
    const Tables& t = tables();
    if (taps <= 16) return t.t16.data();
    if (taps <= 32) return t.t32.data();
    if (taps <= 48) return t.t48.data();
    return t.t64.data();
}

}  // namespace mw909::dsp::detail
