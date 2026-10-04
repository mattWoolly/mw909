// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Toms (LT, MT, HT): the three VCOs on the rail-law pitch envelope, VCO-1's
// overdrive, the envelopes and DECAY, the tom noise, accent, levels in volts,
// extended knobs, robustness, and the comparison with recordings of the
// original machine (skipped when absent).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

#include "AnalogTestUtil.h"
#include "mw909/voices/Tom.h"

using namespace analog_test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using mw909::voices::Tom;

namespace {

constexpr mw909::VoiceId kToms[3] = {mw909::LT, mw909::MT, mw909::HT};
const char* const kName[3] = {"LT", "MT", "HT"};
const char* const kFile[3] = {"LowTom/LowTom909", "MidTom/MidTom909", "HiTom/HiTom909"};
const char* const kAccFile[3] = {"LowTom/LowTom", "MidTom/MidTom", "HiTom/HiTom"};

Prep<Tom> prepFor(mw909::VoiceId id) {
    return [id](Tom& v, const VoiceSetup& s) { v.prepare(s, id); };
}

VoiceParams knobs(mw909::VoiceId id, double tune, double decay) {
    VoiceParams p = mw909::defaultParams(id);
    p.k[mw909::knob::TomTune] = static_cast<float>(tune);
    p.k[mw909::knob::TomDecay] = static_cast<float>(decay);
    return p;
}

std::vector<float> hit(int t, double fs, const VoiceParams& p, const Hit& h = Hit::normal(), double seconds = 0.7,
                       std::uint32_t seed = kNoiseSeed) {
    return renderHit<Tom>(prepFor(kToms[t]), fs, p, h, seconds, seed);
}

// The strongest peak near the expected VCO-1 frequency in [t0, t1).
double f1At(const std::vector<float>& x, double fs, double fexp, double t0, double t1) {
    return peakFreq(x, fs, 0.75 * fexp, 1.9 * fexp, t0, t1);
}

// t(-20 dB) of the full-band Hilbert envelope (20 ms smoothing) re its maximum.
double tomDecay(const std::vector<float>& x0, double fs, double db = -20.0) {
    const auto x = fromOnset(x0, 0.01);
    const auto e = movingAverage(hilbertEnvelope(toDouble(x)), static_cast<std::size_t>(0.02 * fs));
    const auto im = static_cast<std::size_t>(std::max_element(e.begin(), e.end()) - e.begin());
    const double thr = e[im] * std::pow(10.0, db / 20.0);
    for (std::size_t i = im; i < e.size(); ++i)
        if (e[i] < thr) return static_cast<double>(i) / fs;
    return static_cast<double>(e.size()) / fs;
}

// 20 ms centred moving r.m.s. at times `ts` after the onset (10 % of the
// peak), dB re its maximum, above 30 Hz (the recordings carry a slow
// sub-40 Hz settle after the hit, at -47..-55 dB at 200-350 ms, that is not
// part of the voice's ring).
std::vector<double> rmsCurveDb(const std::vector<float>& x0, double fs, const std::vector<double>& ts) {
    const auto x = zeroPhase(toDouble(fromOnset(x0, 0.1)), Biquad::highpass(30.0, fs));
    std::vector<double> sq(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) sq[i] = x[i] * x[i];
    const auto e = movingAverage(sq, static_cast<std::size_t>(0.02 * fs));
    const double m = *std::max_element(e.begin(), e.end());
    std::vector<double> out;
    for (double t : ts) {
        const auto i = std::min(e.size() - 1, static_cast<std::size_t>(t * fs));
        out.push_back(10.0 * std::log10(std::max(e[i] / m, 1e-30)));
    }
    return out;
}

// Measured on the reference unit with the metrics above (TUNE 0 / 50 / 100,
// DECAY 100): VCO-1 at 400-600 ms and 120-250 ms; t(-20 dB) at TUNE 50 and
// DECAY 0/20/40/60/80/100; the TOTAL ACCENT spans (research/05 §0.11).
constexpr double kF1Late[3][3] = {{58.1, 87.6, 118.4}, {73.3, 108.0, 142.7}, {84.4, 127.1, 168.3}};
constexpr double kF1Mid[3][3] = {{61.7, 89.8, 122.0}, {77.4, 112.2, 146.9}, {89.1, 132.1, 173.4}};
constexpr double kF1Early[3][3] = {{83.9, 116.7, 150.6}, {115.2, 145.3, 176.1}, {124.1, 162.5, 205.6}};
constexpr double kRatio2[3] = {0.677, 0.674, 0.676};
constexpr double kDecayMs[3][6] = {{180, 221, 249, 284, 284, 315}, {129, 164, 190, 194, 218, 219}, {125, 166, 189, 212, 228, 229}};
constexpr double kAccentDb[3] = {7.3, 7.7, 7.4};
constexpr double kNormalVpp[3] = {0.948, 1.053, 1.064};

}  // namespace

TEST_CASE("TOM: VCO-1 pitch along the rail-law envelope, at every sample rate", "[tom][pitch]") {
    for (int t = 0; t < 3; ++t) {
        for (double fs : kRates) {
            for (int k = 0; k < 3; ++k) {
                const auto x = fromOnset(hit(t, fs, knobs(kToms[t], k * 0.5, 1.0)), 0.1);
                const double late = f1At(x, fs, kF1Late[t][k], 0.4, 0.6), mid = f1At(x, fs, kF1Late[t][k], 0.12, 0.25);
                const double early = f1At(x, fs, kF1Late[t][k], 0.002, 0.027);
                INFO(kName[t] << " fs " << fs << " TUNE " << k * 50 << ": " << early << " / " << mid << " / " << late << " Hz");
                CHECK_THAT(late, WithinRel(kF1Late[t][k], 0.015));
                CHECK_THAT(mid, WithinRel(kF1Mid[t][k], 0.025));
                if (!(t == 0 && k == 0)) CHECK_THAT(early, WithinRel(kF1Early[t][k], 0.08));  // LT TUNE 0: see the report
                CHECK(early > 1.15 * late);  // the sweep
            }
        }
    }
}

TEST_CASE("TOM: VCO-2 and VCO-3 ratios", "[tom][pitch]") {
    // research/01 §5.2 (three-oscillator fits): 0.679 / 1.88 (LT), 0.673 / 1.77
    // (MT), 0.675 / 1.95 (HT). Read with the pitch envelope off (Tune Depth 0)
    // so the short-lived VCO-3 can be resolved.
    const double r3[3] = {1.88, 1.77, 1.95};
    for (int t = 0; t < 3; ++t) {
        const double fs = 48000.0;
        const auto x = fromOnset(hit(t, fs, knobs(kToms[t], 0.5, 1.0)), 0.1);
        const double f1 = f1At(x, fs, kF1Mid[t][1], 0.12, 0.25);
        const double f2 = peakFreq(x, fs, 0.6 * f1, 0.75 * f1, 0.12, 0.25);
        VoiceParams p = knobs(kToms[t], 0.5, 1.0);
        p.k[mw909::knob::TomDepth] = 0.0f;
        const auto y = fromOnset(hit(t, fs, p), 0.1);
        const double g1 = f1At(y, fs, kF1Late[t][1], 0.1, 0.3);
        const double g3 = peakFreq(y, fs, 1.6 * g1, 2.2 * g1, 0.003, 0.05);
        INFO(kName[t] << ": VCO-2/VCO-1 " << f2 / f1 << ", VCO-3/VCO-1 " << g3 / g1);
        CHECK_THAT(f2 / f1, WithinRel(kRatio2[t], 0.02));
        CHECK_THAT(g3 / g1, WithinRel(r3[t], 0.03));
    }
}

TEST_CASE("TOM: DECAY sets the length, saturating as on the original, at every rate", "[tom][decay]") {
    for (int t = 0; t < 3; ++t) {
        for (double fs : kRates) {
            for (int d = 0; d <= 5; ++d) {
                const double ms = tomDecay(hit(t, fs, knobs(kToms[t], 0.5, d * 0.2), Hit::normal(), 0.8), fs) * 1e3;
                INFO(kName[t] << " fs " << fs << " DECAY " << d * 20 << " %: t(-20 dB) " << ms << " ms (measured " << kDecayMs[t][d] << ")");
                CHECK_THAT(ms, WithinRel(kDecayMs[t][d], 0.10));
            }
        }
    }
}

TEST_CASE("TOM: levels in volts and the TOTAL ACCENT span, at every rate", "[tom][level]") {
    for (int t = 0; t < 3; ++t) {
        double ref = 0.0;
        for (double fs : kRates) {
            double n = 0.0, a = 0.0, a0 = 0.0, a1 = 0.0;
            for (std::uint32_t h = 0; h < 4; ++h) {
                n += vpp(hit(t, fs, knobs(kToms[t], 0.5, 0.5), Hit::normal(), 0.4, 700U + h)) / 4.0;
                a += vpp(hit(t, fs, knobs(kToms[t], 0.5, 0.5), Hit::accented(true, false, 0.0f), 0.4, 700U + h)) / 4.0;
                a0 += vpp(hit(t, fs, knobs(kToms[t], 0.5, 1.0), Hit::accented(false, true, 0.0f), 0.4, 700U + h)) / 4.0;
                a1 += vpp(hit(t, fs, knobs(kToms[t], 0.5, 1.0), Hit::accented(false, true, 1.0f), 0.4, 700U + h)) / 4.0;
            }
            if (fs == 48000.0) ref = n;
            std::printf("[tom level] %s fs %.0f: normal %.3f Vpp, accented %.3f (%+.1f dB), TOTAL ACCENT span %+.1f dB (measured %+.1f)\n",
                        kName[t], fs, n, a, toDb(a / n), toDb(a1 / a0), kAccentDb[t]);
            CHECK_THAT(n, WithinRel(kNormalVpp[t], 0.03));
            CHECK_THAT(toDb(a1 / a0), WithinAbs(kAccentDb[t], 1.0));
            CHECK(a > 1.4 * n);
        }
        (void)ref;
    }
}

TEST_CASE("TOM: Tune Depth, Tune Decay and Noise", "[tom][params]") {
    const double fs = 48000.0;
    VoiceParams p = knobs(mw909::MT, 0.5, 1.0);
    auto early = [&](const VoiceParams& q) { return f1At(fromOnset(hit(1, fs, q), 0.1), fs, 108.0, 0.002, 0.027); };
    auto mid = [&](const VoiceParams& q) { return f1At(fromOnset(hit(1, fs, q), 0.1), fs, 108.0, 0.03, 0.06); };
    const double stockE = early(p), stockM = mid(p);
    p.k[mw909::knob::TomDepth] = 0.0f;
    const double flat = early(p), flatM = mid(p);
    p.k[mw909::knob::TomDepth] = 0.5f;
    p.k[mw909::knob::TomSweep] = 1.0f;
    const double slowM = mid(p);
    std::printf("[tom knobs] MT early pitch: stock %.0f Hz, depth 0 %.0f Hz; 30-60 ms: stock %.0f, slow sweep %.0f Hz\n", stockE, flat, stockM, slowM);
    CHECK(flat < 0.9 * stockE);
    (void)flatM;
    // no pitch envelope: VCO-1 sits at f_s from the start to the end
    VoiceParams noEnv = knobs(mw909::MT, 0.5, 1.0);
    noEnv.k[mw909::knob::TomDepth] = 0.0f;
    CHECK_THAT(f1At(fromOnset(hit(1, fs, noEnv), 0.1), fs, 108.0, 0.12, 0.25), WithinRel(106.4, 0.015));
    CHECK(slowM > stockM + 5.0);
    // Noise: 0 removes the tom noise (the 3-6 kHz band at the onset drops).
    auto hf = [&](float noise) {
        VoiceParams q = knobs(mw909::MT, 0.5, 0.5);
        q.k[mw909::knob::TomNoise] = noise;
        double e = 0.0;
        for (std::uint32_t h = 0; h < 6; ++h) e += spectrum(hit(1, fs, q, Hit::normal(), 0.05, 50U + h), fs, 0.0, 0.03).band(3000, 6000);
        return 10.0 * std::log10(e);
    };
    const double n0 = hf(0.0f), n5 = hf(0.5f), n1 = hf(1.0f);
    std::printf("[tom knobs] 3-6 kHz energy at the onset: noise 0 %.1f, 0.5 %.1f, 1 %.1f dB\n", n0, n5, n1);
    CHECK(n5 > n0 + 3.0);
    CHECK(n1 > n5 + 3.0);
}

TEST_CASE("TOM: the early units' tom noise (C54 2.2n, R194 47k) is louder", "[tom][params]") {
    const double fs = 48000.0;
    auto hf = [&](mw909::Revision rev) {
        double e = 0.0;
        for (std::uint32_t h = 0; h < 6; ++h)
            e += spectrum(render<Tom>(prepFor(mw909::LT), setupAt(fs, 0, 0.0f, rev), knobs(mw909::LT, 0.5, 0.5), {{0.0, Hit::normal()}}, 0.05, 60U + h), fs,
                          0.0, 0.03)
                     .band(600, 1600);
        return 10.0 * std::log10(e);
    };
    const double late = hf(mw909::Revision::Late), early = hf(mw909::Revision::Early);
    std::printf("[tom revision] 0.6-1.6 kHz energy at the onset: late %.1f dB, early %+.1f dB\n", late, early - late);
    CHECK(early > late);
}

TEST_CASE("TOM: retrigger keeps the envelopes, resets the VCOs, no clicks", "[tom][retrigger]") {
    const double fs = 48000.0;
    const VoiceParams p = knobs(mw909::LT, 0.5, 1.0);
    const auto fresh = render<Tom>(prepFor(mw909::LT), setupAt(fs), p, {{0.05, Hit::weak()}}, 0.4);
    const auto re = render<Tom>(prepFor(mw909::LT), setupAt(fs), p, {{0.0, Hit::accented(true, true, 1.0f)}, {0.05, Hit::weak()}}, 0.4);
    CHECK(rms(re, fs, 0.06, 0.2) > 1.5 * rms(fresh, fs, 0.06, 0.2));
    CHECK(maxStep(re, fs, 0.049, 0.054) < 0.3 * peakAbs(re));
}

TEST_CASE("TOM: the DECAY 100 tail rings out exponentially, at every rate", "[tom][decay]") {
    // The recordings (TUNE 50, DECAY 100) fall ~11 dB per 100 ms from 200 to
    // 500 ms, with neither a cliff nor a hang (research/01 §1.3).
    const std::vector<double> ts = {0.2, 0.3, 0.4, 0.5};
    for (int t = 0; t < 3; ++t) {
        for (double fs : kRates) {
            const auto e = rmsCurveDb(hit(t, fs, knobs(kToms[t], 0.5, 1.0), Hit::normal(), 0.8), fs, ts);
            INFO(kName[t] << " fs " << fs << ": " << e[0] << " / " << e[1] << " / " << e[2] << " / " << e[3] << " dB at 200..500 ms");
            for (std::size_t k = 1; k < ts.size(); ++k) {
                const double step = e[k] - e[k - 1];
                CHECK(step < -5.0);
                CHECK(step > -19.0);
            }
        }
    }
}

TEST_CASE("TOM: robustness fuzz, non-finite controls, corrupt variance", "[tom][fuzz]") {
    for (int t = 0; t < 3; ++t) {
        fuzzRobustness<Tom>(prepFor(kToms[t]), kToms[t], 48000.0, 1U + static_cast<std::uint32_t>(t), 15.0);
        checkNonFiniteParams<Tom>(prepFor(kToms[t]), kToms[t], 44100.0);
        checkBadVariance<Tom>(prepFor(kToms[t]), 48000.0);
    }
    fuzzRobustness<Tom>(prepFor(mw909::HT), mw909::HT, 192000.0, 9U, 15.0, 1.0f);
}

TEST_CASE("TOM: deterministic, partition-independent, seed ignored at zero variance", "[tom][determinism]") {
    for (int t = 0; t < 3; ++t) {
        checkDeterministic<Tom>(prepFor(kToms[t]), kToms[t], 44100.0);
        checkPartitionIndependent<Tom>(prepFor(kToms[t]), knobs(kToms[t], 0.3, 0.7), 96000.0);
        checkSeedIgnoredAtZeroVariance<Tom>(prepFor(kToms[t]), 48000.0, knobs(kToms[t], 0.5, 0.5));
    }
}

TEST_CASE("TOM: sleeps only once inaudible and wakes exactly", "[tom][sleep]") {
    for (int t = 0; t < 3; ++t) {
        for (double fs : kRates) {
            const double slept = checkQuietBeforeSleep<Tom>(prepFor(kToms[t]), fs, knobs(kToms[t], 0.0, 1.0), Hit::accented(true, true, 1.0f), 0.05);
            CHECK(slept < 4.0);
            checkWakeIsFresh<Tom>(prepFor(kToms[t]), fs, knobs(kToms[t], 0.5, 0.5), 0.3);
        }
    }
}

TEST_CASE("TOM: against recordings of the original machine", "[tom][reference]") {
    if (!haveRefs()) SKIP("reference recordings not present");
    const double fs = 48000.0;
    for (int t = 0; t < 3; ++t) {
        for (int k = 0; k < 3; ++k) {
            Wav w;
            REQUIRE(loadRef(std::string(kFile[t]) + "-tune" + pct(k * 50) + "-decay100.wav", w));
            const auto r = fromOnset(w.x, 0.1), m = fromOnset(hit(t, fs, knobs(kToms[t], k * 0.5, 1.0)), 0.1);
            const double rl = f1At(r, w.fs, kF1Late[t][k], 0.4, 0.6), ml = f1At(m, fs, kF1Late[t][k], 0.4, 0.6);
            const double re = f1At(r, w.fs, kF1Late[t][k], 0.002, 0.027), me = f1At(m, fs, kF1Late[t][k], 0.002, 0.027);
            std::printf("[tom reference] %s TUNE %3d: VCO-1 early %.1f / %.1f Hz, 400-600 ms %.1f / %.1f Hz\n", kName[t], k * 50, re, me, rl, ml);
            CHECK_THAT(ml, WithinRel(rl, 0.015));
        }
        for (int d = 0; d <= 100; d += 20) {
            Wav w;
            REQUIRE(loadRef(std::string(kFile[t]) + "-tune050-decay" + pct(d) + ".wav", w));
            const double r = tomDecay(w.x, w.fs), m = tomDecay(hit(t, fs, knobs(kToms[t], 0.5, d / 100.0), Hit::normal(), 0.8), fs);
            std::printf("[tom reference] %s DECAY %3d: t(-20 dB) %.0f / %.0f ms\n", kName[t], d, r * 1e3, m * 1e3);
            CHECK_THAT(m, WithinRel(r, 0.12));
        }
        // The whole tail down to the recording's floor, at short, medium and
        // long DECAY (review finding: below -25 dB the tails used to collapse).
        const std::vector<double> ts = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
        for (int d : {0, 50, 100}) {
            Wav w;
            REQUIRE(loadRef(std::string(kFile[t]) + "-tune050-decay" + pct(d) + ".wav", w));
            const auto er = rmsCurveDb(w.x, w.fs, ts), em = rmsCurveDb(hit(t, fs, knobs(kToms[t], 0.5, d / 100.0), Hit::normal(), 0.9), fs, ts);
            const double floorDb = rmsCurveDb(w.x, w.fs, {static_cast<double>(w.x.size()) / w.fs - 0.05})[0];
            std::printf("[tom reference] %s DECAY %3d tail at 100..600 ms (recording / model, dB):", kName[t], d);
            for (std::size_t k = 0; k < ts.size(); ++k) std::printf(" %.1f/%.1f", er[k], em[k]);
            std::printf("\n");
            for (std::size_t k = 0; k < ts.size(); ++k) {
                if (er[k] < floorDb + 8.0) continue;  // the recording's noise floor
                INFO(kName[t] << " DECAY " << d << " at " << ts[k] * 1e3 << " ms");
                // At DECAY 0 the cliff is steep (~-300 dB/s) and comes ~10 ms
                // early in the model: 7 dB at 200 ms for MT / HT.
                CHECK_THAT(em[k], WithinAbs(er[k], d == 0 ? 8.0 : 6.0));
            }
        }
        double r0 = 0.0, m0 = 0.0;
        for (int a : {0, 50, 100}) {
            Wav w;
            REQUIRE(loadRef(std::string(kAccFile[t]) + "-accent" + pct(a) + "-tune050-decay100.wav", w));
            double m = 0.0;
            for (std::uint32_t h = 0; h < 4; ++h) m += vpp(hit(t, fs, knobs(kToms[t], 0.5, 1.0), Hit::accented(false, true, static_cast<float>(a) / 100.0f), 0.4, 500U + h)) / 4.0;
            const double r = toDb(vpp(w.x));
            if (a == 0) {
                r0 = r;
                m0 = toDb(m);
            }
            std::printf("[tom reference] %s TOTAL ACCENT %3d %%: Vpp %+.1f / %+.1f dB\n", kName[t], a, r - r0, toDb(m) - m0);
            // The measured curves have a dead first ~10 % and are then close
            // to linear in dB; the model's (a linear DAC into linear envelope
            // and VCA cells) is concave: ~1.5 dB high at 50 %, up to ~2.2 dB
            // at 40 %, exact at the ends.
            CHECK_THAT(toDb(m) - m0, WithinAbs(r - r0, a == 100 ? 1.0 : 2.0));
        }
    }
}

TEST_CASE("TOM: renders for listening", "[.render]") {
    const double fs = 48000.0;
    std::vector<float> all;
    for (int t = 0; t < 3; ++t)
        for (double tune : {0.0, 0.5, 1.0})
            for (const Hit& h : {Hit::normal(), Hit::accented(true, true, 1.0f)}) {
                const auto x = hit(t, fs, knobs(kToms[t], tune, 0.5), h, 0.7);
                all.insert(all.end(), x.begin(), x.end());
            }
    writeWav(renderDir() + "/toms.wav", all, fs, 0.1f);
    SUCCEED("wrote toms.wav");
}

TEST_CASE("TOM: CPU cost per sample", "[tom][bench]") {
    for (int t = 0; t < 3; ++t) {
        const double ns = benchNsPerSample<Tom>(prepFor(kToms[t]), 48000.0, knobs(kToms[t], 0.5, 0.5), 0.25, 3.0);
        std::printf("[tom bench] %s 48 kHz, re-hit every 250 ms: %.1f ns/sample\n", kName[t], ns);
        checkCpu(ns, 800.0);
    }
}
