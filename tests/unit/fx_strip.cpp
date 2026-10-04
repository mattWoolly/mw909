// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Channel strip: filter/EQ responses against the analog prototypes at four
// sample rates, compressor static curve and timing, bypass, sleep/wake,
// NaN/Inf fuzz, determinism and block-partition independence. Also the shared
// building blocks (fast log/exp, EqFilter shapes, the 2x interpolator).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/ChannelStrip.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;

namespace {

std::vector<float> stripImpulse(ChannelStrip& s, int n) {
    std::vector<float> h(static_cast<std::size_t>(n), 0.0f);
    h[0] = 1.0f;
    s.process(h.data(), n);
    return h;
}

// max over n of |y[n+1] - 2y[n] + y[n-1]| / (w^2 * local peak), w = 2 pi f/fs.
double maxCurvatureRatio(const std::vector<float>& y, double f, double fs) {
    const double w2 = std::pow(2.0 * kPi * f / fs, 2.0);
    const int win = static_cast<int>(fs / f);
    double worst = 0.0;
    for (std::size_t i = static_cast<std::size_t>(win); i + 1 < y.size(); ++i) {   // skip the onset
        const std::size_t a = i > static_cast<std::size_t>(win) ? i - static_cast<std::size_t>(win) : 0;
        const std::size_t b = std::min(y.size(), i + static_cast<std::size_t>(win));
        const double local = peak(y.data() + a, b - a) + 1.0e-3;
        const double d2 = std::abs(double(y[i + 1]) - 2.0 * y[i] + y[i - 1]);
        worst = std::max(worst, d2 / (w2 * local));
    }
    return worst;
}

}  // namespace

TEST_CASE("fast log2/exp2 accuracy", "[fx][math]") {
    for (float x = 1.0e-6f; x < 1.0e4f; x *= 1.37f) {
        REQUIRE_THAT(fastLog2(x), Catch::Matchers::WithinAbs(std::log2(x), 2.0e-5));
    }
    for (float x = -40.0f; x < 40.0f; x += 0.173f) {
        REQUIRE_THAT(fastExp2(x), Catch::Matchers::WithinRel(std::exp2(x), 2.0e-6f));
    }
    for (double p = -2.0; p < 2.0; p += 0.0137) {
        REQUIRE_THAT(fastSinCycles(p), Catch::Matchers::WithinAbs(std::sin(2.0 * kPi * p), 1.2e-3));
    }
}

TEST_CASE("EqFilter shapes match the warped analog prototypes", "[fx][eq]") {
    struct Case {
        EqKind kind;
        double f0, q, db;
    };
    const Case cases[] = {
        {EqKind::HighPass, 120.0, 0.7071, 0.0},  {EqKind::LowPass, 5000.0, 3.0, 0.0},
        {EqKind::Bell, 1000.0, 2.0, 12.0},       {EqKind::Bell, 300.0, 0.4, -18.0},
        {EqKind::LowShelf, 200.0, 0.7071, 9.0},  {EqKind::HighShelf, 6000.0, 0.7071, -12.0},
    };
    for (double fs : kRates) {
        for (const auto& c : cases) {
            EqFilter<1> f;
            f.setKind(c.kind);
            f.prepare(fs, controlInterval(fs));
            f.setTarget(true, c.f0, c.q, c.db);
            f.snap();
            const int n = static_cast<int>(fs * 0.5);
            std::vector<float> h(static_cast<std::size_t>(n), 0.0f);
            h[0] = 1.0f;
            f.processMono(h.data(), n);
            for (double fr : {30.0, 100.0, 450.0, 1000.0, 3000.0, 9000.0, 16000.0}) {
                const cplx s = warpedS(fr, c.f0, fs);
                cplx ref;
                switch (c.kind) {
                    case EqKind::HighPass: ref = protoHp(s, c.q); break;
                    case EqKind::LowPass: ref = protoLp(s, c.q); break;
                    case EqKind::Bell: ref = protoBell(s, c.q, c.db); break;
                    case EqKind::LowShelf: ref = protoLowShelf(s, c.q, c.db); break;
                    case EqKind::HighShelf: ref = protoHighShelf(s, c.q, c.db); break;
                    default: break;
                }
                const double got = magDb(dftAt(h, fr, fs));
                INFO("fs " << fs << " kind " << int(c.kind) << " f " << fr);
                REQUIRE_THAT(got, Catch::Matchers::WithinAbs(magDb(ref), 0.02));
            }
        }
    }
}

TEST_CASE("Interpolator + decimator pass audio at unity and reject images", "[fx][os]") {
    for (int factor : {2, 4}) {
        Interpolator up;
        dsp::MetalDecimator down;
        up.prepare(factor);
        down.prepare(factor);
        const double fs = 48000.0;
        const auto x = sine(1000.0, fs, 9600, 0.5);
        std::vector<float> y(x.size());
        std::vector<double> hi(static_cast<std::size_t>(x.size()) * static_cast<std::size_t>(factor));
        for (std::size_t i = 0; i < x.size(); ++i) {
            double buf[4];
            up.process(x[i], buf);
            for (int k = 0; k < factor; ++k) hi[i * static_cast<std::size_t>(factor) + static_cast<std::size_t>(k)] = buf[k];
            y[i] = static_cast<float>(down.process(buf));
        }
        REQUIRE_THAT(rms(y.data() + 4800, 4800), Catch::Matchers::WithinRel(rms(x.data() + 4800, 4800), 1.0e-3));
        // Image of 1 kHz at fs - 1 kHz in the high-rate signal must be tiny.
        std::vector<float> hif(hi.begin() + 4800 * factor, hi.end());
        const double sig = std::abs(dftAt(hif, 1000.0, fs * factor));
        const double img = std::abs(dftAt(hif, fs - 1000.0, fs * factor));
        REQUIRE(20.0 * std::log10(img / sig) < -70.0);
    }
}

TEST_CASE("strip cut filters and bells match the prototypes at every rate", "[fx][strip]") {
    for (double fs : kRates) {
        ChannelStrip s;
        s.prepare(fs);
        StripParams p;
        p.eqOn = true;
        p.lowCutHz = 80.0f;
        p.highCutHz = 8000.0f;
        p.resonance = 0.3f;
        p.band[0] = {true, 400.0f, 1.0f, 9.0f};
        p.band[1] = {true, 2500.0f, 0.5f, -12.0f};
        s.setParams(p);
        const auto h = stripImpulse(s, static_cast<int>(fs * 0.5));
        const double q = resonanceToQ(0.3);
        for (double fr : {40.0, 80.0, 200.0, 400.0, 1000.0, 2500.0, 5000.0, 8000.0, 12000.0}) {
            const cplx ref = protoHp(warpedS(fr, 80.0, fs), q) * protoLp(warpedS(fr, 8000.0, fs), q) *
                             protoBell(warpedS(fr, 400.0, fs), octavesToQ(1.0), 9.0) *
                             protoBell(warpedS(fr, 2500.0, fs), octavesToQ(0.5), -12.0);
            INFO("fs " << fs << " f " << fr);
            REQUIRE_THAT(magDb(dftAt(h, fr, fs)), Catch::Matchers::WithinAbs(magDb(ref), 0.05));
        }
        // Rate independence of the audible band: within 0.5 dB of the pure analog
        // prototype up to 5 kHz (bilinear cramping only near Nyquist).
        for (double fr : {40.0, 200.0, 1000.0, 2500.0}) {
            const cplx s0{0.0, 1.0};
            const cplx ref = protoHp(s0 * (fr / 80.0), q) * protoLp(s0 * (fr / 8000.0), q) *
                             protoBell(s0 * (fr / 400.0), octavesToQ(1.0), 9.0) *
                             protoBell(s0 * (fr / 2500.0), octavesToQ(0.5), -12.0);
            REQUIRE_THAT(magDb(dftAt(h, fr, fs)), Catch::Matchers::WithinAbs(magDb(ref), 0.5));
        }
    }
}

TEST_CASE("bell width: 0.5..4 octaves and +-24 dB reach their nominal gains", "[fx][strip]") {
    const double fs = 48000.0;
    for (float w : {0.5f, 1.0f, 2.0f, 4.0f}) {
        for (float g : {-24.0f, 24.0f}) {
            ChannelStrip s;
            s.prepare(fs);
            StripParams p;
            p.eqOn = true;
            p.band[0] = {true, 1000.0f, w, g};
            s.setParams(p);
            const auto h = stripImpulse(s, 48000);
            REQUIRE_THAT(magDb(dftAt(h, 1000.0, fs)), Catch::Matchers::WithinAbs(g, 0.01));
            // Half-gain (in dB) points sit w/2 octaves either side.
            const double lo = 1000.0 * std::exp2(-w * 0.5), hi = 1000.0 * std::exp2(w * 0.5);
            const double glo = magDb(dftAt(h, lo, fs)), ghi = magDb(dftAt(h, hi, fs));
            INFO("w " << w << " g " << g << " lo " << glo << " hi " << ghi);
            REQUIRE_THAT(std::sqrt(std::abs(glo * ghi)), Catch::Matchers::WithinAbs(std::abs(g) * 0.5, std::abs(g) * 0.12));
        }
    }
}

TEST_CASE("strip with everything off is an exact no-op", "[fx][strip]") {
    ChannelStrip s;
    s.prepare(48000.0);
    StripParams p;   // defaults: both sections off
    p.band[0].on = true;    // band on but EQ section off: still off
    s.setParams(p);
    REQUIRE(s.isBypassed());
    auto x = noise(4096, 0.5f);
    const auto ref = x;
    s.process(x.data(), 4096);
    REQUIRE(x == ref);
    // EQ on but neutral (cut filters at their off positions, bands off): also exact.
    p.eqOn = true;
    p.band[0].on = false;
    s.setParams(p);
    REQUIRE(s.isBypassed());
}

TEST_CASE("compressor static curve and timing", "[fx][strip][comp]") {
    const double fs = 48000.0;
    // Static curve: steady sine at several levels, measure output level after settling.
    for (float thr : {-30.0f, -12.0f}) {
        for (float ratio : {2.0f, 4.0f, 20.0f}) {
            for (float inDb : {-40.0f, -20.0f, -6.0f, 0.0f}) {
                ChannelStrip s;
                s.prepare(fs);
                StripParams p;
                p.compOn = true;
                p.compThresholdDb = thr;
                p.compRatio = ratio;
                p.compAttackMs = 1.0f;
                p.compReleaseMs = 500.0f;   // slow release: ripple-free on a sine's peaks
                s.setParams(p);
                // A square wave has a constant |x|: the peak detector sees a steady level.
                const int n = 48000;
                std::vector<float> x(static_cast<std::size_t>(n));
                const float a = std::pow(10.0f, inDb / 20.0f);
                for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = ((i / 240) % 2) ? a : -a;
                s.process(x.data(), n);
                const double outDb = 20.0 * std::log10(peak(x.data() + n - 4800, 4800));
                const float d = inDb - thr;
                double expect;
                if (2.0f * d < -ChannelStrip::kCompKneeDb) expect = inDb;
                else if (2.0f * std::abs(d) <= ChannelStrip::kCompKneeDb) {
                    const float t = d + 0.5f * ChannelStrip::kCompKneeDb;
                    expect = inDb + (1.0f / ratio - 1.0f) * t * t / (2.0f * ChannelStrip::kCompKneeDb);
                } else expect = thr + d / ratio;
                INFO("thr " << thr << " ratio " << ratio << " in " << inDb);
                REQUIRE_THAT(outDb, Catch::Matchers::WithinAbs(expect, 0.05));
            }
        }
    }
    // Timing: step from silence-ish to +20 dB over threshold; gain reduction
    // reaches 63 % of its final value after the attack time constant.
    for (double rate : kRates) {
        for (float attackMs : {5.0f, 50.0f}) {
            ChannelStrip s;
            s.prepare(rate);
            StripParams p;
            p.compOn = true;
            p.compThresholdDb = -20.0f;
            p.compRatio = 20.0f;
            p.compAttackMs = attackMs;
            p.compReleaseMs = 100.0f;
            s.setParams(p);
            const int n = static_cast<int>(rate * 0.5);
            std::vector<float> x(static_cast<std::size_t>(n), 1.0f);   // DC step at 0 dBFS
            s.process(x.data(), n);
            const double finalGr = -20.0 * std::log10(x[static_cast<std::size_t>(n - 1)]);
            const int at = static_cast<int>(rate * attackMs * 0.001);
            const double grAt = -20.0 * std::log10(x[static_cast<std::size_t>(at)]);
            INFO("fs " << rate << " attack " << attackMs);
            REQUIRE_THAT(grAt / finalGr, Catch::Matchers::WithinAbs(1.0 - std::exp(-1.0), 0.01));
            // Release: drop the input to -60 dB; GR falls to 1/e after the release time.
            std::vector<float> y(static_cast<std::size_t>(n), 1.0e-3f);
            s.process(y.data(), n);
            const int rel = static_cast<int>(rate * 0.1);
            const double grRel = -20.0 * std::log10(y[static_cast<std::size_t>(rel)] / 1.0e-3);
            REQUIRE_THAT(grRel / finalGr, Catch::Matchers::WithinAbs(std::exp(-1.0), 0.01));
        }
    }
}

TEST_CASE("compressor pre/post EQ order", "[fx][strip][comp]") {
    // A bell boost before the compressor gets compressed; after it, it does not.
    const double fs = 48000.0;
    auto run = [&](bool pre) {
        ChannelStrip s;
        s.prepare(fs);
        StripParams p;
        p.eqOn = true;
        p.band[0] = {true, 1000.0f, 1.0f, 12.0f};
        p.compOn = true;
        p.compPreEq = pre;
        p.compThresholdDb = -30.0f;
        p.compRatio = 20.0f;
        p.compAttackMs = 1.0f;
        p.compReleaseMs = 200.0f;
        s.setParams(p);
        auto x = sine(1000.0, fs, 48000, 0.1);
        s.process(x.data(), 48000);
        return 20.0 * std::log10(peak(x.data() + 40000, 8000));
    };
    const double post = run(false), pre = run(true);
    REQUIRE(pre - post > 8.0);   // the boost after the compressor survives
}

TEST_CASE("strip parameter changes are smooth (no steps)", "[fx][strip]") {
    const double fs = 48000.0;
    ChannelStrip s;
    s.prepare(fs);
    StripParams p;
    s.setParams(p);
    auto x = sine(200.0, fs, 48000, 0.5);
    std::vector<float> y = x;
    int i = 0;
    int k = 0;
    while (i < 48000) {
        const int b = std::min(256, 48000 - i);
        // Toggle sections and jump parameters every few blocks.
        if (k % 8 == 0) {
            p.eqOn = !p.eqOn;
            p.lowCutHz = p.eqOn ? 600.0f : 20.0f;
            p.band[0] = {true, 200.0f, 1.0f, p.eqOn ? 18.0f : -18.0f};
            p.compOn = (k % 16) == 0;
            p.compThresholdDb = -30.0f;
            p.compMakeupDb = p.compOn ? 12.0f : 0.0f;
            p.compPreEq = !p.compPreEq;
        }
        s.setParams(p);
        s.process(y.data() + i, b);
        i += b;
        ++k;
    }
    // A click shows up as a second difference far above what a 200 Hz sine of the
    // local amplitude can produce (|d2 y| <= w^2 A for a pure sine). The
    // one-pole glides (make-up, bell gain) start with a slope step, which
    // scores 3..6 here when a +-18 dB bell and 12 dB of make-up turn around at
    // once; a click scores in the hundreds.
    REQUIRE(maxCurvatureRatio(y, 200.0, fs) < 8.0);
}

TEST_CASE("strip sleeps, costs nothing asleep and wakes with zero state", "[fx][strip]") {
    const double fs = 48000.0;
    ChannelStrip a, b;
    StripParams p;
    p.eqOn = true;
    p.lowCutHz = 100.0f;
    p.band[0] = {true, 300.0f, 1.0f, 6.0f};
    p.compOn = true;
    p.compThresholdDb = -24.0f;
    for (auto* s : {&a, &b}) {
        s->prepare(fs);
        s->setParams(p);
    }
    auto x = bursts(48000 * 3, 48000 * 3, fs);   // one hit, then a long silence
    for (std::size_t i = 24000; i < x.size(); ++i) x[i] = 0.0f;
    std::vector<float> y = x;
    a.process(y.data(), static_cast<int>(y.size()));
    REQUIRE_FALSE(a.isActive());
    // b: render only the hit and the start of the tail, then skip (as the engine
    // does while it is inactive), then a second hit: must equal a's second hit.
    std::vector<float> hit = bursts(4800, 4800, fs, 0.5f, 99u);
    std::vector<float> ya = hit, yb = hit;
    std::vector<float> yb0 = x;
    b.process(yb0.data(), 48000 * 3);
    REQUIRE_FALSE(b.isActive());
    a.process(ya.data(), 4800);
    // Fresh strip for reference: same as a sleeping one.
    ChannelStrip c;
    c.prepare(fs);
    c.setParams(p);
    std::vector<float> yc = hit;
    c.process(yc.data(), 4800);
    REQUIRE(ya == yc);
}

TEST_CASE("strip NaN/Inf parameter fuzz keeps output finite", "[fx][strip]") {
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, 0.0f};
    ChannelStrip s;
    s.prepare(44100.0);
    TestRng r{5};
    auto x = noise(44100, 0.8f);
    for (int blk = 0; blk < 200; ++blk) {
        StripParams p;
        auto pick = [&]() { return bad[static_cast<std::size_t>((r.next() * 0.5f + 0.5f) * 5.99f)]; };
        p.eqOn = true;
        p.compOn = true;
        p.lowCutHz = pick();
        p.highCutHz = pick();
        p.resonance = pick();
        p.band[0] = {true, pick(), pick(), pick()};
        p.band[1] = {true, pick(), pick(), pick()};
        p.compThresholdDb = pick();
        p.compRatio = pick();
        p.compAttackMs = pick();
        p.compReleaseMs = pick();
        p.compMakeupDb = pick();
        s.setParams(p);
        std::vector<float> y(x.begin() + blk * 200, x.begin() + blk * 200 + 200);
        s.process(y.data(), 200);
        REQUIRE(allFinite(y));
        REQUIRE(peak(y.data(), 200) < 1.0e4);
    }
}

TEST_CASE("strip is deterministic and block-partition independent", "[fx][strip]") {
    const double fs = 96000.0;
    StripParams p;
    p.eqOn = true;
    p.lowCutHz = 60.0f;
    p.highCutHz = 9000.0f;
    p.resonance = 0.5f;
    p.band[0] = {true, 250.0f, 2.0f, 5.0f};
    p.compOn = true;
    p.compThresholdDb = -20.0f;
    p.compMakeupDb = 4.0f;
    const auto x = bursts(96000 * 2, 20000, fs);
    auto render = [&](std::uint32_t seed) {
        ChannelStrip s;
        s.prepare(fs);
        s.setParams(p);
        std::vector<float> y = x;
        int off = 0;
        for (int b : partition(static_cast<int>(y.size()), seed)) {
            s.setParams(p);
            s.process(y.data() + off, b);
            off += b;
        }
        return y;
    };
    const auto a = render(1), b = render(2), c = render(1);
    REQUIRE(a == c);
    REQUIRE(a == b);
}
