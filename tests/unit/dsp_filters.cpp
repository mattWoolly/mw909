// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "mw909/dsp/Filters.h"
#include "mw909/dsp/Noise.h"

using namespace mw909::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Ring an impulse through a band-pass and measure the pitch from zero crossings
// and the decay from the peak envelope.
struct Ring {
    double freqHz;
    double tauSeconds;
};

Ring ringOf(double f0, double q, double fs) {
    Svf svf;
    svf.setFreqQ(f0, q, fs);
    const int n = static_cast<int>(fs * 1.0);
    std::vector<double> y(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        svf.tick(i == 0 ? 1.0 : 0.0);
        y[static_cast<std::size_t>(i)] = svf.bandpass();
    }
    // Pitch: average period over upward zero crossings in the first 200 ms.
    double first = -1.0, last = -1.0;
    int crossings = 0;
    const int limit = static_cast<int>(fs * 0.2);
    for (int i = 1; i < limit; ++i) {
        const double a = y[static_cast<std::size_t>(i - 1)], b = y[static_cast<std::size_t>(i)];
        if (a < 0.0 && b >= 0.0) {
            const double t = (i - 1) + a / (a - b);
            if (first < 0.0) first = t;
            last = t;
            ++crossings;
        }
    }
    const double freq = (crossings > 1) ? (crossings - 1) * fs / (last - first) : 0.0;
    // Decay: peak magnitude in two windows one tau-ish apart.
    auto peakIn = [&](double t0, double t1) {
        double p = 0.0;
        for (int i = static_cast<int>(t0 * fs); i < static_cast<int>(t1 * fs); ++i)
            p = std::max(p, std::abs(y[static_cast<std::size_t>(i)]));
        return p;
    };
    const double p0 = peakIn(0.02, 0.05), p1 = peakIn(0.12, 0.15);
    const double tau = 0.10 / std::log(p0 / p1);
    return {freq, tau};
}

}  // namespace

TEST_CASE("Svf resonator keeps pitch and decay across sample rates", "[dsp][svf]") {
    const double f0 = 238.0, q = 30.0;
    const double expectedTau = q / (kPi * f0);  // envelope tau of a 2-pole resonator
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const Ring r = ringOf(f0, q, fs);
        // The damped ring sits a hair below f0: sqrt(1 - 1/(4 Q^2)).
        REQUIRE_THAT(r.freqHz, WithinRel(f0, 0.002));
        REQUIRE_THAT(r.tauSeconds, WithinRel(expectedTau, 0.03));
    }
}

TEST_CASE("Svf responses sum to the input", "[dsp][svf]") {
    Svf svf;
    svf.setFreqQ(1000.0, 0.8, 48000.0);
    WhiteNoise noise;
    noise.seed(7);
    for (int i = 0; i < 2000; ++i) {
        const double x = noise.next();
        svf.tick(x);
        // hp + k*bp + lp == x for the TPT SVF (k = 1/Q).
        REQUIRE_THAT(svf.highpass() + svf.bandpassUnity() + svf.lowpass(), WithinAbs(x, 1e-9));
    }
}

TEST_CASE("OnePole low-pass is -3 dB at cutoff", "[dsp][onepole]") {
    for (double fs : {44100.0, 96000.0}) {
        OnePole lp;
        lp.setCutoff(1000.0, fs);
        double peak = 0.0;
        const int n = static_cast<int>(fs);
        for (int i = 0; i < n; ++i) {
            const double y = lp.lowpass(std::sin(kTwoPi * 1000.0 * i / fs));
            if (i > n / 2) peak = std::max(peak, std::abs(y));
        }
        REQUIRE_THAT(peak, WithinRel(std::sqrt(0.5), 0.01));
    }
}

TEST_CASE("WhiteNoise is unit RMS, zero mean and deterministic", "[dsp][noise]") {
    WhiteNoise a, b;
    a.seed(42);
    b.seed(42);
    double sum = 0.0, sumSq = 0.0;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        const float x = a.next();
        REQUIRE(x == b.next());
        sum += x;
        sumSq += static_cast<double>(x) * x;
    }
    REQUIRE_THAT(sum / n, WithinAbs(0.0, 0.01));
    REQUIRE_THAT(std::sqrt(sumSq / n), WithinRel(1.0, 0.02));
}

TEST_CASE("componentScale is nominal at zero variance and bounded by tolerance", "[dsp][math]") {
    for (std::uint32_t id = 0; id < 200; ++id) {
        REQUIRE(componentScale(1234U, id, 0.2f, 0.0f) == 1.0f);
        const float s = componentScale(1234U, id, 0.2f, 1.0f);
        REQUIRE(s >= 0.8f);
        REQUIRE(s <= 1.2f);
    }
    REQUIRE(componentScale(1U, 5U, 0.2f, 1.0f) != componentScale(2U, 5U, 0.2f, 1.0f));
}
