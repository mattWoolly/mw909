// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Delay effect: echo time accuracy (free and synced), sync arithmetic,
// feedback decay, ping-pong spread, loop-filter stability at the extremes,
// tail/sleep, NaN fuzz, determinism and block-partition independence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/Delay.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

struct Stereo {
    std::vector<float> l, r;
};

Stereo impulseThrough(Delay& d, int n, float amp = 1.0f, bool left = true, bool right = true) {
    Stereo s{std::vector<float>(static_cast<std::size_t>(n), 0.0f), std::vector<float>(static_cast<std::size_t>(n), 0.0f)};
    if (left) s.l[0] = amp;
    if (right) s.r[0] = amp;
    d.process(s.l.data(), s.r.data(), n);
    return s;
}

// Delay of the echo from its low-frequency phase (exact for a fractional
// delay): tau = D0 - arg(H(f) e^{j w D0}) / w, with D0 the integer peak.
double echoDelay(const std::vector<float>& h, double fs) {
    std::size_t best = 0;
    for (std::size_t i = 0; i < h.size(); ++i)
        if (std::abs(h[i]) > std::abs(h[best])) best = i;
    const double f = 100.0, w = 2.0 * kPi * f / fs;
    const cplx H = dftAt(h, f, fs) * std::polar(1.0, w * static_cast<double>(best));
    return static_cast<double>(best) - std::arg(H) / w;
}

}  // namespace

TEST_CASE("sync arithmetic", "[fx][delay]") {
    // 120 BPM: a quarter note is 0.5 s.
    REQUIRE_THAT(syncSeconds(SyncDiv::D1_4, SyncKind::Straight, 120.0), WithinRel(0.5, 1e-12));
    REQUIRE_THAT(syncSeconds(SyncDiv::D1_8, SyncKind::Dotted, 120.0), WithinRel(0.375, 1e-12));
    REQUIRE_THAT(syncSeconds(SyncDiv::D1_16, SyncKind::Triplet, 120.0), WithinRel(0.125 * 2.0 / 3.0, 1e-12));
    REQUIRE_THAT(syncSeconds(SyncDiv::D1_32, SyncKind::Straight, 90.0), WithinRel(60.0 / 90.0 / 8.0, 1e-12));
    REQUIRE_THAT(syncSeconds(SyncDiv::D1_1, SyncKind::Straight, 60.0), WithinRel(4.0, 1e-12));
    REQUIRE_THAT(syncSeconds(SyncDiv::D1_2, SyncKind::Straight, std::numeric_limits<double>::quiet_NaN()),
                 WithinRel(1.0, 1e-12));   // NaN bpm -> 120
}

TEST_CASE("echo lands at exactly the delay time (free and synced)", "[fx][delay]") {
    for (double fs : kRates) {
        for (float ms : {1.0f, 37.3f, 250.0f, 1000.0f}) {
            Delay d;
            d.prepare(fs);
            DelayParams p;
            p.sync = false;
            p.timeMs = ms;
            p.feedback = 0.0f;
            p.mix = 1.0f;
            d.setParams(p, 120.0);
            const int n = static_cast<int>(fs * (ms * 0.001 + 0.05));
            const auto s = impulseThrough(d, n);
            const double expect = ms * 0.001 * fs;
            INFO("fs " << fs << " ms " << ms);
            REQUIRE_THAT(echoDelay(s.l, fs), WithinAbs(expect, 0.02));
            REQUIRE_THAT(echoDelay(s.r, fs), WithinAbs(expect, 0.02));
        }
        // Synced: 1/16 triplet at 133 BPM.
        Delay d;
        d.prepare(fs);
        DelayParams p;
        p.sync = true;
        p.div = SyncDiv::D1_16;
        p.kind = SyncKind::Triplet;
        p.feedback = 0.0f;
        d.setParams(p, 133.0);
        const double expect = syncSeconds(SyncDiv::D1_16, SyncKind::Triplet, 133.0) * fs;
        const auto s = impulseThrough(d, static_cast<int>(expect) + 1000);
        INFO("synced fs " << fs);
        REQUIRE_THAT(echoDelay(s.l, fs), WithinAbs(expect, 0.02));
    }
}

TEST_CASE("feedback repeats decay by the feedback factor", "[fx][delay]") {
    const double fs = 48000.0;
    Delay d;
    d.prepare(fs);
    DelayParams p;
    p.sync = false;
    p.timeMs = 100.0f;
    p.feedback = 0.5f;
    d.setParams(p, 120.0);
    const auto s = impulseThrough(d, 48000);
    for (int k = 1; k <= 4; ++k) {
        const double a = s.l[static_cast<std::size_t>(4800 * k)];
        REQUIRE_THAT(a, WithinAbs(std::pow(0.5, k - 1), 1e-4));
    }
}

TEST_CASE("ping-pong spread alternates the sides", "[fx][delay]") {
    const double fs = 48000.0;
    for (float spread : {1.0f, -1.0f}) {
        Delay d;
        d.prepare(fs);
        DelayParams p;
        p.sync = false;
        p.timeMs = 50.0f;
        p.feedback = 0.8f;
        p.spread = spread;
        d.setParams(p, 120.0);
        const auto s = impulseThrough(d, 48000);   // the impulse on both sides
        const std::size_t t = 2400;
        const bool leftFirst = spread > 0.0f;
        const auto& first = leftFirst ? s.l : s.r;
        const auto& second = leftFirst ? s.r : s.l;
        REQUIRE(std::abs(first[t]) > 0.9);
        REQUIRE(std::abs(second[t]) < 1e-6);
        REQUIRE(std::abs(second[2 * t]) > 0.7);
        REQUIRE(std::abs(first[2 * t]) < 1e-6);
        REQUIRE(std::abs(first[3 * t]) > 0.5);
    }
}

TEST_CASE("loop filter shapes the repeats and never runs away", "[fx][delay]") {
    const double fs = 48000.0;
    for (auto type : {FilterType::LowPass, FilterType::BandPass, FilterType::HighPass}) {
        Delay d;
        d.prepare(fs);
        DelayParams p;
        p.sync = false;
        p.timeMs = 20.0f;
        p.feedback = 0.98f;
        p.loopFilter = type;
        p.cutoffHz = 1500.0f;
        p.reso = 1.0f;
        d.setParams(p, 120.0);
        // 60 s of loud noise at the worst settings: bounded.
        auto x = noise(static_cast<int>(fs), 1.0f, 11u);
        double maxPeak = 0.0;
        for (int sec = 0; sec < 60; ++sec) {
            std::vector<float> l = x, r = x;
            d.process(l.data(), r.data(), static_cast<int>(l.size()));
            REQUIRE(allFinite(l));
            maxPeak = std::max(maxPeak, peak(l.data(), l.size()));
        }
        INFO("type " << int(type) << " peak " << maxPeak);
        REQUIRE(maxPeak < 40.0);
    }
}

TEST_CASE("delay tail, sleep and wake", "[fx][delay]") {
    const double fs = 48000.0;
    Delay d;
    d.prepare(fs);
    DelayParams p;
    p.sync = false;
    p.timeMs = 100.0f;
    p.feedback = 0.5f;
    d.setParams(p, 120.0);
    // Tail to -120 dB: 0.1 * (1 + ln 1e-6 / ln 0.5) = 2.09 s.
    REQUIRE_THAT(d.tailSeconds(), WithinRel(0.1 * (1.0 + std::log(1e-6) / std::log(0.5)), 1e-9));
    REQUIRE_FALSE(d.isActive());
    auto s = impulseThrough(d, static_cast<int>(fs * 0.2));
    REQUIRE(d.isActive());
    std::vector<float> l(static_cast<std::size_t>(fs * 3.0), 0.0f), r = l;
    d.process(l.data(), r.data(), static_cast<int>(l.size()));
    REQUIRE_FALSE(d.isActive());
    // After waking, the echo of a new impulse is exactly a fresh delay's.
    Delay fresh;
    fresh.prepare(fs);
    fresh.setParams(p, 120.0);
    const auto a = impulseThrough(d, 20000);
    const auto b = impulseThrough(fresh, 20000);
    REQUIRE(a.l == b.l);
    REQUIRE(a.r == b.r);
}

TEST_CASE("delay time changes glide without clicks", "[fx][delay]") {
    const double fs = 48000.0;
    Delay d;
    d.prepare(fs);
    DelayParams p;
    p.sync = false;
    p.timeMs = 100.0f;
    p.feedback = 0.3f;
    p.mix = 0.5f;
    d.setParams(p, 120.0);
    auto x = sine(300.0, fs, 96000, 0.3);
    std::vector<float> l = x, r = x;
    for (int i = 0; i < 96000; i += 512) {
        p.timeMs = (i / 512) % 2 ? 80.0f : 120.0f;
        d.setParams(p, 120.0);
        d.process(l.data() + i, r.data() + i, std::min(512, 96000 - i));
    }
    double maxStep = 0.0;
    for (std::size_t i = 1; i < l.size(); ++i) maxStep = std::max(maxStep, std::abs(double(l[i]) - l[i - 1]));
    // A 300 Hz sine at <= 0.3 * (1 + 0.43 feedback sum) with Doppler; a click would be ~0.3.
    REQUIRE(maxStep < 0.12);
}

TEST_CASE("delay NaN/Inf fuzz, determinism, partition independence", "[fx][delay]") {
    const double fs = 44100.0;
    {
        Delay d;
        d.prepare(fs);
        const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};
        auto x = noise(44100, 0.5f);
        std::vector<float> l = x, r = x;
        for (int k = 0; k < 100; ++k) {
            DelayParams p;
            const float v = bad[k % 5];
            p.sync = (k % 2) == 0;
            p.timeMs = v;
            p.feedback = v;
            p.spread = v;
            p.cutoffHz = v;
            p.reso = v;
            p.mix = v;
            p.loopFilter = static_cast<FilterType>(k % 7);
            p.div = static_cast<SyncDiv>(k % 9);
            p.kind = static_cast<SyncKind>(k % 5);
            d.setParams(p, static_cast<double>(v));
            d.process(l.data() + k * 441, r.data() + k * 441, 441);
        }
        REQUIRE(allFinite(l));
        REQUIRE(allFinite(r));
    }
    DelayParams p;
    p.sync = true;
    p.div = SyncDiv::D1_8;
    p.kind = SyncKind::Dotted;
    p.feedback = 0.6f;
    p.spread = 0.4f;
    p.loopFilter = FilterType::BandPass;
    p.cutoffHz = 900.0f;
    p.reso = 0.5f;
    p.mix = 0.7f;
    const auto x = bursts(static_cast<int>(fs * 3), 11025, fs);
    auto render = [&](std::uint32_t seed) {
        Delay d;
        d.prepare(fs);
        d.setParams(p, 127.0);
        std::vector<float> l = x, r = x;
        for (std::size_t i = 0; i < r.size(); ++i) r[i] *= -0.5f;
        int off = 0;
        for (int b : partition(static_cast<int>(l.size()), seed)) {
            d.setParams(p, 127.0);
            d.process(l.data() + off, r.data() + off, b);
            off += b;
        }
        return Stereo{l, r};
    };
    const auto a = render(3), b = render(4), c = render(3);
    REQUIRE(a.l == c.l);
    REQUIRE(a.l == b.l);
    REQUIRE(a.r == b.r);
}
