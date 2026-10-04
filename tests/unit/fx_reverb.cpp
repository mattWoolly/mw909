// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Reverb: RT60 against the decay setting at four sample rates, pre-delay,
// early/late balance, stability for 60 s at the maximum settings, tail and
// sleep, NaN fuzz, determinism and block-partition independence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/Reverb.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> impulseResponse(Reverb& rv, double fs, double seconds, std::vector<float>* right = nullptr) {
    const int n = static_cast<int>(fs * seconds);
    std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(static_cast<std::size_t>(n), 0.0f);
    l[0] = 1.0f;
    r[0] = 1.0f;
    rv.process(l.data(), r.data(), n);
    if (right != nullptr) *right = r;
    return l;
}

// Second-order band-pass (centre f, Q 1.4) to look at one band of an IR.
std::vector<float> bandOf(const std::vector<float>& x, double f, double fs) {
    const double w = 2.0 * kPi * f / fs, alpha = std::sin(w) / (2.0 * 1.4);
    const double b0 = alpha, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * std::cos(w), a2 = 1 - alpha;
    std::vector<float> y(x.size());
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double v = (b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2) / a0;
        x2 = x1;
        x1 = x[i];
        y2 = y1;
        y1 = v;
        y[i] = static_cast<float>(v);
    }
    return y;
}

}  // namespace

TEST_CASE("RT60 follows the decay setting at every rate", "[fx][reverb]") {
    for (double fs : kRates) {
        for (float decay : {0.0f, 0.3f, 0.6f, 0.85f}) {
            Reverb rv;
            rv.prepare(fs);
            ReverbParams p;
            p.decay = decay;
            p.earlyLate = 1.0f;   // late tail only
            p.predelayMs = 0.0f;
            p.lateColor = 1.0f;   // no high-frequency damping: broadband RT60 = setting
            p.modulation = 0.0f;
            rv.setParams(p);
            const double t60 = Reverb::rt60ForDecay(decay);
            const auto h = impulseResponse(rv, fs, std::min(3.0 * t60 + 0.3, 12.0));
            const double meas = rt60(h, fs, -5.0, -35.0);
            INFO("fs " << fs << " decay " << decay << " target " << t60 << " measured " << meas);
            REQUIRE_THAT(meas, WithinRel(t60, 0.08));
        }
    }
}

TEST_CASE("RT60 in the mid band holds with damping and modulation", "[fx][reverb]") {
    for (double fs : {44100.0, 96000.0}) {
        Reverb rv;
        rv.prepare(fs);
        ReverbParams p;
        p.decay = 0.5f;
        p.earlyLate = 1.0f;
        p.lateColor = 0.3f;
        p.modulation = 0.6f;
        rv.setParams(p);
        const double t60 = Reverb::rt60ForDecay(0.5);
        const auto h = impulseResponse(rv, fs, 5.0);
        const double meas = rt60(bandOf(h, 250.0, fs), fs, -5.0, -35.0);
        INFO("fs " << fs << " measured " << meas << " target " << t60);
        REQUIRE_THAT(meas, WithinRel(t60, 0.12));
        // High frequencies decay faster when the colour is dark.
        const double hf = rt60(bandOf(h, 8000.0, fs), fs, -5.0, -35.0);
        REQUIRE(hf < 0.7 * meas);
    }
}

TEST_CASE("pre-delay and early reflections", "[fx][reverb]") {
    const double fs = 48000.0;
    Reverb rv;
    rv.prepare(fs);
    ReverbParams p;
    p.predelayMs = 100.0f;
    p.earlyLate = 0.0f;   // early only
    p.mix = 1.0f;
    rv.setParams(p);
    const auto h = impulseResponse(rv, fs, 0.5);
    // Nothing before the pre-delay plus the first tap.
    REQUIRE(peak(h.data(), 4800) < 1e-9);
    // The early pattern lies inside predelay + span (5 + 95 * 0.5 ms) + diffusion.
    const std::size_t a = 4800, b = static_cast<std::size_t>(fs * (0.1 + 0.0525 + 0.02));
    const double inside = rms(h.data() + a, b - a);
    const double after = rms(h.data() + b + 2400, 4800);
    REQUIRE(inside > 1e-3);
    REQUIRE(after < inside * 0.03);
}

TEST_CASE("reverb is stable for 60 s at the maximum settings", "[fx][reverb]") {
    const double fs = 48000.0;
    Reverb rv;
    rv.prepare(fs);
    ReverbParams p;
    p.decay = 1.0f;
    p.lateSize = 1.0f;
    p.earlySize = 1.0f;
    p.lateDiffusion = 1.0f;
    p.earlyDiffusion = 1.0f;
    p.modulation = 1.0f;
    p.lateColor = 1.0f;
    p.earlyColor = 1.0f;
    p.predelayMs = 250.0f;
    rv.setParams(p);
    auto x = noise(static_cast<int>(fs), 0.5f, 99u);
    double first = 0.0, last = 0.0;
    for (int sec = 0; sec < 60; ++sec) {
        std::vector<float> l = x, r = x;
        rv.process(l.data(), r.data(), static_cast<int>(l.size()));
        REQUIRE(allFinite(l));
        const double e = rms(l.data(), l.size());
        if (sec == 20) first = e;
        if (sec == 59) last = e;
        REQUIRE(peak(l.data(), l.size()) < 50.0);
    }
    // Steady state: no slow build-up.
    REQUIRE(last < first * 1.2);
    // And it rings down to sleep.
    std::vector<float> z(static_cast<std::size_t>(fs), 0.0f), z2 = z;
    for (int sec = 0; sec < 30 && rv.isActive(); ++sec) {
        std::fill(z.begin(), z.end(), 0.0f);
        std::fill(z2.begin(), z2.end(), 0.0f);
        rv.process(z.data(), z2.data(), static_cast<int>(z.size()));
    }
    REQUIRE_FALSE(rv.isActive());
    REQUIRE(rv.tailSeconds() > 20.0);
}

TEST_CASE("reverb sleeps and wakes with zero state", "[fx][reverb]") {
    const double fs = 44100.0;
    ReverbParams p;
    p.decay = 0.2f;
    Reverb a, b;
    for (auto* r : {&a, &b}) {
        r->prepare(fs);
        r->setParams(p);
    }
    impulseResponse(a, fs, 0.1);
    std::vector<float> z(static_cast<std::size_t>(fs * 8), 0.0f), z2 = z;
    a.process(z.data(), z2.data(), static_cast<int>(z.size()));
    REQUIRE_FALSE(a.isActive());
    std::vector<float> ra, rb;
    const auto ha = impulseResponse(a, fs, 1.0, &ra);
    const auto hb = impulseResponse(b, fs, 1.0, &rb);
    REQUIRE(ha == hb);
    REQUIRE(ra == rb);
}

TEST_CASE("reverb NaN fuzz, determinism, partition independence", "[fx][reverb]") {
    const double fs = 48000.0;
    {
        Reverb rv;
        rv.prepare(fs);
        auto x = noise(48000, 0.5f);
        std::vector<float> l = x, r = x;
        const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};
        for (int k = 0; k < 100; ++k) {
            const float v = bad[k % 5];
            ReverbParams p{v, v, v, v, v, v, v, v, v, v, v};
            rv.setParams(p);
            rv.process(l.data() + k * 480, r.data() + k * 480, 480);
        }
        REQUIRE(allFinite(l));
        REQUIRE(allFinite(r));
    }
    ReverbParams p;
    p.modulation = 0.7f;
    p.decay = 0.6f;
    p.mix = 0.4f;
    const auto x = bursts(static_cast<int>(fs * 2), 24000, fs);
    auto render = [&](std::uint32_t seed) {
        Reverb rv;
        rv.prepare(fs);
        rv.setParams(p);
        std::vector<float> l = x, r = x;
        int off = 0;
        for (int blk : partition(static_cast<int>(l.size()), seed)) {
            rv.setParams(p);
            rv.process(l.data() + off, r.data() + off, blk);
            off += blk;
        }
        return l;
    };
    const auto a = render(5), b = render(6), c = render(5);
    REQUIRE(a == c);
    REQUIRE(a == b);
}
