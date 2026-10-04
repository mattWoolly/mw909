// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Rim shot: the three resonators and the clipper's intermodulation, the
// envelope, accent, levels in volts, the service change (R417), extended
// knobs, robustness, and the comparison with recordings of the original
// machine (skipped when absent).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

#include "AnalogTestUtil.h"
#include "mw909/voices/RimShot.h"

using namespace analog_test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using mw909::voices::RimShot;

namespace {

const Prep<RimShot> prep = [](RimShot& v, const VoiceSetup& s) { v.prepare(s); };

VoiceParams knobs(double tune = 0.0, double decay = 0.5) {
    VoiceParams p = mw909::defaultParams(mw909::RS);
    p.k[mw909::knob::RsTune] = static_cast<float>(tune);
    p.k[mw909::knob::RsDecay] = static_cast<float>(decay);
    return p;
}

std::vector<float> hit(double fs, const VoiceParams& p = knobs(), const Hit& h = Hit::normal(), double seconds = 0.2,
                       mw909::Revision rev = mw909::Revision::Late) {
    return render<RimShot>(prep, setupAt(fs, 0, 0.0f, rev), p, {{0.0, h}}, seconds);
}

// r.m.s. in 3 ms windows from the onset, dB re the loudest of the first 3 ms.
std::vector<double> envelopeDb(const std::vector<float>& x0, double fs) {
    const auto x = fromOnset(x0, 0.05);
    double r0 = 0.0;
    for (int i = 0; i < static_cast<int>(0.003 * fs); ++i) r0 = std::max(r0, rms(x, fs, i / fs, i / fs + 0.002));
    std::vector<double> e;
    for (double t : {0.0, 0.003, 0.006, 0.01, 0.015, 0.02, 0.03}) e.push_back(toDb(rms(x, fs, t, t + 0.003) / r0));
    return e;
}

// The reference recording (accent 0), same metric.
constexpr double kRefEnv[7] = {-0.3, -1.3, -3.6, -8.9, -12.4, -17.9, -29.1};
constexpr double kNormalVpp = 0.976;

}  // namespace

TEST_CASE("RS: the three partials and the clipper's intermodulation, at every rate", "[rs][pitch]") {
    for (double fs : kRates) {
        const auto x = fromOnset(hit(fs), 0.05);
        const double p1 = peakFreq(x, fs, 180, 270, 0.0, 0.04), p2 = peakFreq(x, fs, 420, 560, 0.0, 0.04);
        const double p3 = peakFreq(x, fs, 900, 1150, 0.0, 0.03), im = peakFreq(x, fs, 650, 800, 0.0, 0.03);
        INFO("fs " << fs << ": " << p1 << " / " << p2 << " / " << p3 << " Hz, intermodulation " << im);
        CHECK_THAT(p1, WithinRel(222.0, 0.02));
        CHECK_THAT(p2, WithinRel(481.0, 0.02));
        CHECK_THAT(p3, WithinRel(1018.0, 0.02));
        CHECK_THAT(im, WithinRel(727.0, 0.04));
    }
}

TEST_CASE("RS: envelope and decay, at every rate", "[rs][decay]") {
    for (double fs : kRates) {
        const auto e = envelopeDb(hit(fs), fs);
        const auto x = fromOnset(hit(fs), 0.05);
        INFO("fs " << fs << " envelope " << e[0] << " " << e[1] << " " << e[2] << " " << e[3] << " " << e[4] << " " << e[5] << " " << e[6]);
        for (std::size_t i = 0; i < 7; ++i) CHECK_THAT(e[i], WithinAbs(kRefEnv[i], 3.5));
        CHECK_THAT(rmsDecayTime(x, fs, -10.0) * 1e3, WithinRel(14.1, 0.15));
        CHECK_THAT(rmsDecayTime(x, fs, -20.0) * 1e3, WithinRel(22.9, 0.15));
    }
}

TEST_CASE("RS: levels in volts and the TOTAL ACCENT span, at every rate", "[rs][level]") {
    for (double fs : kRates) {
        const double n = vpp(hit(fs)), a1 = vpp(hit(fs, knobs(), Hit::accented(false, true, 1.0f)));
        std::printf("[rs level] fs %.0f: normal %.3f Vpp, TOTAL ACCENT span %+.1f dB (measured +3.9)\n", fs, n, toDb(a1 / n));
        CHECK_THAT(n, WithinRel(kNormalVpp, 0.03));
        CHECK_THAT(toDb(a1 / n), WithinAbs(3.9, 1.0));
    }
    // Accent changes the level only: same partials and decay.
    const double fs = 48000.0;
    const auto n = fromOnset(hit(fs), 0.05), a = fromOnset(hit(fs, knobs(), Hit::accented(false, true, 1.0f)), 0.05);
    CHECK_THAT(rmsDecayTime(a, fs, -20.0), WithinRel(rmsDecayTime(n, fs, -20.0), 0.10));
    CHECK_THAT(peakFreq(a, fs, 900, 1150, 0.0, 0.03), WithinRel(peakFreq(n, fs, 900, 1150, 0.0, 0.03), 0.01));
    CHECK(vpp(hit(fs, knobs(), Hit::weak())) < vpp(hit(fs)));
}

TEST_CASE("RS: the service change R417 3.3k (12k before) weights F3", "[rs][params]") {
    const double fs = 48000.0;
    auto ratio = [&](mw909::Revision rev) {
        const auto x = fromOnset(hit(fs, knobs(), Hit::normal(), 0.2, rev), 0.05);
        const Spectrum s = spectrum(x, fs, 0.0, 0.03);
        return 10.0 * std::log10(s.band(900, 1150) / s.band(420, 560));
    };
    const double late = ratio(mw909::Revision::Late), early = ratio(mw909::Revision::Early);
    std::printf("[rs revision] 1 kHz band re 480 Hz band: late %.1f dB, early %.1f dB\n", late, early);
    CHECK(late > early + 2.0);
}

TEST_CASE("RS: Tune shifts the partials, Decay their ring", "[rs][params]") {
    const double fs = 48000.0;
    const auto up = fromOnset(hit(fs, knobs(7.0)), 0.05);
    CHECK_THAT(peakFreq(up, fs, 640, 840, 0.0, 0.04), WithinRel(481.0 * std::exp2(7.0 / 12.0), 0.03));
    const double stock = rmsDecayTime(fromOnset(hit(fs), 0.05), fs, -20.0);
    const double lng = rmsDecayTime(fromOnset(hit(fs, knobs(0.0, 1.0), Hit::normal(), 0.4), 0.05), fs, -20.0);
    const double shrt = rmsDecayTime(fromOnset(hit(fs, knobs(0.0, 0.0)), 0.05), fs, -20.0);
    std::printf("[rs knobs] -20 dB: Decay 0 %.1f, stock %.1f, 1 %.1f ms\n", shrt * 1e3, stock * 1e3, lng * 1e3);
    CHECK(lng > 1.5 * stock);
    CHECK(shrt < 0.8 * stock);
}

TEST_CASE("RS: retrigger restarts the kick into the ringing resonators, no clicks", "[rs][retrigger]") {
    const double fs = 48000.0;
    const auto fresh = render<RimShot>(prep, setupAt(fs), knobs(), {{0.008, Hit::normal()}}, 0.1);
    const auto re = render<RimShot>(prep, setupAt(fs), knobs(), {{0.0, Hit::normal()}, {0.008, Hit::normal()}}, 0.1);
    double diff = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.009 * fs); i < fresh.size(); ++i) diff = std::max(diff, std::abs(static_cast<double>(fresh[i]) - re[i]));
    CHECK(diff > 0.01);
    CHECK(allFinite(re));
}

TEST_CASE("RS: robustness fuzz, non-finite controls, corrupt variance", "[rs][fuzz]") {
    fuzzRobustness<RimShot>(prep, mw909::RS, 48000.0, 1U, 5.0);
    fuzzRobustness<RimShot>(prep, mw909::RS, 44100.0, 2U, 5.0, 1.0f);
    fuzzRobustness<RimShot>(prep, mw909::RS, 192000.0, 3U, 5.0);
    checkNonFiniteParams<RimShot>(prep, mw909::RS, 48000.0);
    checkBadVariance<RimShot>(prep, 48000.0);
}

TEST_CASE("RS: deterministic, partition-independent, seed ignored at zero variance", "[rs][determinism]") {
    for (double fs : {44100.0, 96000.0}) {
        checkDeterministic<RimShot>(prep, mw909::RS, fs);
        checkPartitionIndependent<RimShot>(prep, knobs(3.0, 0.7), fs);
        checkSeedIgnoredAtZeroVariance<RimShot>(prep, fs, knobs());
    }
}

TEST_CASE("RS: sleeps only once inaudible and wakes exactly", "[rs][sleep]") {
    for (double fs : kRates) {
        const double slept = checkQuietBeforeSleep<RimShot>(prep, fs, knobs(0.0, 1.0), Hit::accented(true, true, 1.0f), 0.01);
        CHECK(slept < 2.0);
        checkWakeIsFresh<RimShot>(prep, fs, knobs(), 0.1);
    }
}

TEST_CASE("RS: against recordings of the original machine", "[rs][reference]") {
    if (!haveRefs()) SKIP("reference recordings not present");
    const double fs = 48000.0;
    Wav w;
    REQUIRE(loadRef("RimShot/RimShot909-accent000.wav", w));
    const auto r = fromOnset(w.x, 0.05), m = fromOnset(hit(fs), 0.05);
    const double rp[4] = {peakFreq(r, w.fs, 180, 270, 0, 0.04), peakFreq(r, w.fs, 420, 560, 0, 0.04), peakFreq(r, w.fs, 900, 1150, 0, 0.03),
                          peakFreq(r, w.fs, 650, 800, 0, 0.03)};
    const double mp[4] = {peakFreq(m, fs, 180, 270, 0, 0.04), peakFreq(m, fs, 420, 560, 0, 0.04), peakFreq(m, fs, 900, 1150, 0, 0.03),
                          peakFreq(m, fs, 650, 800, 0, 0.03)};
    std::printf("[rs reference] partials %.0f / %.0f / %.0f Hz, intermodulation %.0f Hz; model %.0f / %.0f / %.0f, %.0f Hz\n", rp[0], rp[1], rp[2],
                rp[3], mp[0], mp[1], mp[2], mp[3]);
    for (int i = 0; i < 4; ++i) CHECK_THAT(mp[i], WithinRel(rp[i], 0.03));
    const auto er = envelopeDb(w.x, w.fs), em = envelopeDb(hit(fs), fs);
    std::printf("[rs reference] envelope (3 ms windows from 0, 3, 6, 10, 15, 20, 30 ms):");
    for (std::size_t i = 0; i < 7; ++i) std::printf(" %.1f/%.1f", er[i], em[i]);
    std::printf(" dB\n");
    for (std::size_t i = 0; i < 7; ++i) CHECK_THAT(em[i], WithinAbs(er[i], 3.5));
    double r0 = 0.0;
    for (int a = 0; a <= 100; a += 25) {
        Wav wa;
        REQUIRE(loadRef("RimShot/RimShot909-accent" + pct(a == 25 ? 20 : (a == 75 ? 80 : a)) + ".wav", wa));
        const float k = static_cast<float>(a == 25 ? 20 : (a == 75 ? 80 : a)) / 100.0f;
        const double rv = toDb(vpp(wa.x)), mv = toDb(vpp(hit(fs, knobs(), Hit::accented(false, true, k))));
        if (a == 0) r0 = rv - mv;
        std::printf("[rs reference] TOTAL ACCENT %3.0f %%: Vpp %+.1f / %+.1f dB\n", k * 100.0f, rv, mv + r0);
        CHECK_THAT(mv + r0, WithinAbs(rv, 1.2));
    }
}

TEST_CASE("RS: renders for listening", "[.render]") {
    const double fs = 48000.0;
    std::vector<float> all;
    for (const Hit& h : {Hit::normal(), Hit::accented(true, true, 1.0f), Hit::weak()}) {
        for (auto rev : {mw909::Revision::Late, mw909::Revision::Early}) {
            const auto x = hit(fs, knobs(), h, 0.25, rev);
            all.insert(all.end(), x.begin(), x.end());
        }
    }
    writeWav(renderDir() + "/rs.wav", all, fs, 0.1f);
    SUCCEED("wrote rs.wav");
}

TEST_CASE("RS: CPU cost per sample", "[rs][bench]") {
    const double ns = benchNsPerSample<RimShot>(prep, 48000.0, knobs(), 0.1, 3.0);
    std::printf("[rs bench] 48 kHz, re-hit every 100 ms: %.1f ns/sample\n", ns);
    checkCpu(ns, 400.0);
}
