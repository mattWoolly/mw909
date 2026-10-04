// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The analog voices' shared primitives (triangle VCO, diode pair, envelope
// nodes, accent buffer) and the balance between the analog voices: their
// relative levels and accent gains, and how the accent DAC's codes land in
// each circuit.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

#include "AnalogTestUtil.h"
#include "mw909/dsp/DiodeClipper.h"
#include "mw909/dsp/TransistorVca.h"
#include "mw909/dsp/TriangleVco.h"
#include "mw909/voices/BassDrum.h"
#include "mw909/voices/HandClap.h"
#include "mw909/voices/RimShot.h"
#include "mw909/voices/SnareDrum.h"
#include "mw909/voices/Tom.h"

using namespace analog_test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
// Mean frequency from all upward zero crossings.
double zeroCrossFreqOf(const std::vector<float>& x, double fs) {
    const auto z = zeroCrossings(x, fs, 0.0);
    return z.size() > 1 ? static_cast<double>(z.size() - 1) / (z.back() - z.front()) : 0.0;
}
}  // namespace

TEST_CASE("TriangleVco: exact frequency, skew, reset and release", "[analog][vco]") {
    for (double fs : kRates) {
        mw909::dsp::TriangleVco vco;
        vco.prepare(1.0e-4 * fs);
        vco.hold();
        vco.release(0.37, 1);
        std::vector<float> x;
        const double f = 123.4;
        for (int i = 0; i < static_cast<int>(fs); ++i) x.push_back(static_cast<float>(vco.tick(4.0 * f / fs, 4.0 * f / fs)));
        CHECK_THAT(zeroCrossFreqOf(x, fs), WithinRel(f, 1e-4));
        CHECK(peakAbs(x) <= 1.0 + 1e-6);
        // Skewed: up slope s, down slope 3s -> f = s fs / (2 (1 + 1/3)).
        mw909::dsp::TriangleVco skew;
        skew.prepare(4.0);
        std::vector<float> y;
        for (int i = 0; i < static_cast<int>(fs); ++i) y.push_back(static_cast<float>(skew.tick(4.0 * f / fs, 12.0 * f / fs)));
        CHECK_THAT(zeroCrossFreqOf(y, fs), WithinRel(f * 2.0 / (1.0 + 1.0 / 3.0), 1e-4));
    }
    // A reset is an RC discharge to 0 V (no step), and the ramp restarts in the
    // forced direction from 0 V.
    mw909::dsp::TriangleVco vco;
    const double fs = 48000.0;
    vco.prepare(1.0e-4 * fs);
    for (int i = 0; i < 100; ++i) vco.tick(0.01, 0.01);
    vco.hold();
    double prev = vco.tick(0.01, 0.01), maxStepSeen = 0.0;
    for (int i = 0; i < 96; ++i) {
        const double v = vco.tick(0.01, 0.01);
        maxStepSeen = std::max(maxStepSeen, std::abs(v - prev));
        prev = v;
    }
    CHECK(std::abs(prev) < 1e-6);
    CHECK(maxStepSeen < 0.25);
    vco.release(1.0, -1);
    vco.tick(0.01, 0.01);
    vco.tick(0.01, 0.01);
    CHECK(vco.tick(0.01, 0.01) < 0.0);
}

TEST_CASE("TriangleVco: polyBLAMP lowers the alias floor", "[analog][vco]") {
    // 500 Hz at 44.1 kHz, about the highest any voice's VCO runs (the high
    // tom's VCO-3 at the top of its sweep): non-harmonic energy re the
    // fundamental, against a naive (point-sampled) triangle.
    const double fs = 44100.0, f = 500.3;
    auto floorDb = [&](const std::vector<float>& x) {
        const Spectrum s = spectrum(x, fs, 0.1, 1.4);
        double total = 0.0;
        for (std::size_t i = 0; i < s.power.size(); ++i) {
            const double k = static_cast<double>(i) * s.binHz / f;
            if (!(std::abs(k - std::round(k)) * f < 30.0 && std::round(k) >= 1.0)) total += s.power[i];
        }
        return 10.0 * std::log10(total / s.band(f - 30, f + 30));
    };
    mw909::dsp::TriangleVco vco;
    vco.prepare(4.0);
    std::vector<float> x, naive;
    double ph = 0.0;
    for (int i = 0; i < 1 << 16; ++i) {
        x.push_back(static_cast<float>(vco.tick(4.0 * f / fs, 4.0 * f / fs)));
        naive.push_back(static_cast<float>(4.0 * std::abs(ph - std::floor(ph + 0.5)) - 1.0));
        ph += f / fs;
    }
    const double a = floorDb(x), b = floorDb(naive);
    std::printf("[analog vco] alias floor at 500 Hz / 44.1 kHz: %.1f dB (naive %.1f dB)\n", a, b);
    CHECK(a < -66.0);
    CHECK(a < b - 10.0);
}

TEST_CASE("DiodePairClipper: Newton and table agree; the curve is the diode pair's", "[analog][diode]") {
    mw909::dsp::DiodePairClipper a, b;
    a.set(1.0 / 10.0e3, 1.0 / 2247.0, 2.5e-9, 1.75 * 0.02585);
    b.set(1.0 / 10.0e3, 1.0 / 2247.0, 2.5e-9, 1.75 * 0.02585);
    b.buildTable(5.2);
    double worst = 0.0, peak = 0.0;
    for (int i = -5000; i <= 5000; ++i) {
        const double v = 5.0 * i / 5000.0;
        const double u = a.process(v), t = b.processTable(v);
        worst = std::max(worst, std::abs(u - t));
        peak = std::max(peak, std::abs(u));
        // KCL holds at the solution
        const double res = (v - u) / 10.0e3 - u / 2247.0 - 2.0 * 2.5e-9 * std::sinh(u / (1.75 * 0.02585));
        CHECK(std::abs(res) < 1e-9);
    }
    INFO("table error " << worst << " V, node peak " << peak << " V");
    CHECK(worst < 2e-5);
    CHECK_THAT(peak, WithinAbs(0.50, 0.05));  // research/01 §3.5: node peak ~0.50 V at 4.9 V
    CHECK(std::isfinite(a.process(std::numeric_limits<double>::quiet_NaN())));
}

TEST_CASE("DiodeClipperBank: any drive between the tables matches the exact solve", "[analog][clipper]") {
    // The BD Shaper's bank: R_in 40k .. 2.5k, load 2.247k, 1SS133 pair.
    constexpr int kN = 9;
    mw909::dsp::DiodeClipperBank<kN> bank;
    const double vMax = 5.2;
    bank.build(1.0 / 40.0e3, 1.0 / 2.5e3, 1.0 / 2247.0, 2.5e-9, 1.75 * 0.02585, vMax);
    double worst = 0.0;
    for (int k = 0; k <= 64; ++k) {
        const double x = k / 64.0;
        const double gIn = std::exp2(4.0 * x - 2.0) / 10.0e3;
        mw909::dsp::DiodePairClipper exact;
        exact.set(gIn, 1.0 / 2247.0, 2.5e-9, 1.75 * 0.02585);
        for (int i = -200; i <= 200; ++i) {
            const double v = vMax * 0.99 * i / 200.0;
            worst = std::max(worst, std::abs(bank.process(v, x * (kN - 1), gIn) - exact.process(v)));
        }
    }
    INFO("worst |bank - exact| " << worst << " V");
    CHECK(worst < 2.0e-4);  // on a ~0.5 V node
    CHECK(std::isfinite(bank.process(std::numeric_limits<double>::quiet_NaN(), 3.5, 1.0e-4)));
    CHECK(std::isfinite(bank.process(50.0, std::numeric_limits<double>::infinity(), 1.0e-4)));
}

TEST_CASE("EnvNode: exact RC steps at any rate; base current law", "[analog][env]") {
    for (double fs : kRates) {
        mw909::dsp::EnvNode n;
        n.setCap(0.1e-6);
        n.reset(5.0);
        const double dt = 1.0 / fs;
        int steps = 0;
        while (n.v() > 5.0 / std::exp(1.0)) {
            n.begin();
            n.toSource(220.0e3, 0.0);
            n.step(dt);
            ++steps;
        }
        // tau = 220k x 0.1u (no leak: dsp/TransistorVca.h)
        const double tau = 220.0e3 * 0.1e-6;
        CHECK_THAT(steps / fs, WithinRel(tau, 2.0 / (tau * fs)));
        // A capacitor discharging only into a base: V - V_BE decays with RC.
        n.reset(5.0);
        const double t = 0.02;
        for (int i = 0; i < static_cast<int>(t * fs); ++i) {
            n.begin();
            n.toBase(242.0e3);
            n.step(dt);
        }
        const double rc = 242.0e3 * 0.1e-6;
        CHECK_THAT(n.v() - 0.6, WithinRel(4.4 * std::exp(-t / rc), 0.02));
    }
}

TEST_CASE("EnvNode: a base-only tail is exponential to the floor, then terminates", "[analog][env]") {
    // research/01 §1.3: a capacitor draining only into a base gives an
    // exactly exponential gain decay. The review found the earlier 10 MOhm
    // leak collapsing it from about -30 dB; nothing may bend it above the
    // termination floor (-70 dB re the peak drive), and below it the node
    // must reach silence within about a second.
    for (double fs : {44100.0, 192000.0}) {
        mw909::dsp::EnvNode n;
        n.setCap(0.1e-6);
        n.reset(4.6);
        const double dt = 1.0 / fs, rc = 470.0e3 * 0.1e-6;
        double peak = 0.0, worst = 0.0, tFloor = -1.0, tSilent = -1.0;
        for (int i = 0; i < static_cast<int>(3.0 * fs); ++i) {
            n.begin();
            n.toBase(470.0e3);
            n.step(dt);
            const double s = n.drive(), t = (i + 1) * dt;
            peak = std::max(peak, s);
            // linear region (more than ~10 nVt above the knee): s = s0 e^(-t/RC)
            if (s > 0.3) worst = std::max(worst, std::abs(std::log(s / (4.0 * std::exp(-(t - dt) / rc)))));
            if (tFloor < 0.0 && s < mw909::dsp::analog::kTailFloor * peak) tFloor = t;
            if (tSilent < 0.0 && s < 1.0e-9) tSilent = t;
        }
        INFO("fs " << fs << ": worst log error above the knee " << worst << ", floor at " << tFloor << " s, silent at " << tSilent << " s");
        CHECK(worst < 0.01);       // < 0.1 dB from the exact exponential
        CHECK(tFloor > 0.0);
        CHECK(tSilent > tFloor);
        CHECK(tSilent < tFloor + 1.0);
        // A leak (setLeak) adds an ordinary conductance.
        mw909::dsp::EnvNode l;
        l.setCap(0.1e-6);
        l.setLeak(220.0e3);
        l.reset(5.0);
        for (int i = 0; i < static_cast<int>(0.022 * fs); ++i) {
            l.begin();
            l.step(dt);
        }
        CHECK_THAT(l.v(), WithinRel(5.0 * std::exp(-static_cast<int>(0.022 * fs) * dt / 0.022), 0.01));
    }
}

TEST_CASE("Accent buffer: x3.128, the rail, finite for any input", "[analog][accent]") {
    using mw909::dsp::accentBufferVolts;
    CHECK_THAT(accentBufferVolts(1.0), WithinRel(3.128, 0.002));
    CHECK(accentBufferVolts(5.0) < 13.6);
    CHECK(accentBufferVolts(5.0) > 13.2);
    CHECK(accentBufferVolts(0.0) == 0.0);
    CHECK(std::isfinite(accentBufferVolts(std::numeric_limits<double>::quiet_NaN())));
    CHECK(std::isfinite(accentBufferVolts(-std::numeric_limits<double>::infinity())));
    double prev = -1.0;
    for (int c = 0; c <= 63; ++c) {
        const double v = accentBufferVolts(mw909::AccentDac::volts(c));
        CHECK(v >= prev);
        prev = v;
    }
}

namespace {
template <class V>
double meanVpp(const Prep<V>& p, int voice, const Hit& h, int hits, double seconds = 0.5) {
    double s = 0.0;
    for (int i = 0; i < hits; ++i) s += vpp(renderHit<V>(p, 48000.0, mw909::defaultParams(voice), h, seconds, 900U + 7919U * static_cast<std::uint32_t>(i)));
    return s / hits;
}
}  // namespace

TEST_CASE("Analog voices: relative levels and the accent DAC across voices", "[analog][level]") {
    using namespace mw909::voices;
    const Prep<BassDrum> bd = [](BassDrum& v, const VoiceSetup& s) { v.prepare(s); };
    const Prep<SnareDrum> sd = [](SnareDrum& v, const VoiceSetup& s) { v.prepare(s); };
    const Prep<Tom> lt = [](Tom& v, const VoiceSetup& s) { v.prepare(s, mw909::LT); };
    const Prep<Tom> mt = [](Tom& v, const VoiceSetup& s) { v.prepare(s, mw909::MT); };
    const Prep<Tom> ht = [](Tom& v, const VoiceSetup& s) { v.prepare(s, mw909::HT); };
    const Prep<RimShot> rs = [](RimShot& v, const VoiceSetup& s) { v.prepare(s); };
    const Prep<HandClap> cp = [](HandClap& v, const VoiceSetup& s) { v.prepare(s); };
    const char* names[7] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP"};
    // The reference unit at equal LEVEL settings, knobs centred, dB re CP's
    // mean Vpp (BD/SD one file each, toms one each, RS 11 shots, CP 9 shots).
    const double refDb[7] = {-5.86, -2.28, -6.49, -5.58, -5.48, -6.24, 0.0};
    const Hit n = Hit::normal(), a = Hit::accented(true, false, 0.0f), w = Hit::weak();
    double lv[7][3];
    auto row = [&](int i, auto&& f) {
        lv[i][0] = f(n);
        lv[i][1] = f(a);
        lv[i][2] = f(w);
    };
    row(0, [&](const Hit& h) { return meanVpp<BassDrum>(bd, mw909::BD, h, 1); });
    row(1, [&](const Hit& h) { return meanVpp<SnareDrum>(sd, mw909::SD, h, 12); });
    row(2, [&](const Hit& h) { return meanVpp<Tom>(lt, mw909::LT, h, 4); });
    row(3, [&](const Hit& h) { return meanVpp<Tom>(mt, mw909::MT, h, 4); });
    row(4, [&](const Hit& h) { return meanVpp<Tom>(ht, mw909::HT, h, 4); });
    row(5, [&](const Hit& h) { return meanVpp<RimShot>(rs, mw909::RS, h, 1); });
    row(6, [&](const Hit& h) { return meanVpp<HandClap>(cp, mw909::CP, h, 24); });
    std::printf("[analog levels] voice: normal / accented / weak Vpp (dB re CP: model, reference)\n");
    for (int i = 0; i < 7; ++i) {
        const double re = toDb(lv[i][0] / lv[6][0]);
        std::printf("  %s: %.3f / %.3f / %.3f V  (%+.2f, %+.2f dB)\n", names[i], lv[i][0], lv[i][1], lv[i][2], re, refDb[i]);
        CHECK_THAT(re, WithinAbs(refDb[i], 0.5));
        CHECK(lv[i][2] < lv[i][0]);                           // a weak hit is quieter
        if (i < 5) CHECK(lv[i][1] > 1.3 * lv[i][0]);           // the per-voice accent (BD SD toms)
    }
    CHECK_THAT(lv[6][0], WithinRel(2.0, 0.05));  // the clap trimmer's 2 Vpp
}

