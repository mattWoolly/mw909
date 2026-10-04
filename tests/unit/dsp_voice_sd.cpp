// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Snare drum: the two VCOs and their 3:2 lock, the bend, the drum VCAs, the
// snappy (TONE cut-off, SNAPPY level, spectrum), accent, levels in volts,
// extended knobs, robustness, and the comparison with recordings of the
// original machine (skipped when absent).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

#include "AnalogTestUtil.h"
#include "mw909/voices/SnareDrum.h"

using namespace analog_test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using mw909::voices::SnareDrum;

namespace {

const Prep<SnareDrum> prep = [](SnareDrum& v, const VoiceSetup& s) { v.prepare(s); };

VoiceParams knobs(double tune, double tone, double snappy) {
    VoiceParams p = mw909::defaultParams(mw909::SD);
    p.k[mw909::knob::SdTune] = static_cast<float>(tune);
    p.k[mw909::knob::SdTone] = static_cast<float>(tone);
    p.k[mw909::knob::SdSnappy] = static_cast<float>(snappy);
    return p;
}

std::vector<float> hit(double fs, const VoiceParams& p, const Hit& h = Hit::normal(), double seconds = 0.4,
                       std::uint32_t seed = kNoiseSeed) {
    return renderHit<SnareDrum>(prep, fs, p, h, seconds, seed);
}

std::vector<double> bandEnv(const std::vector<float>& x, double fs, double lo, double hi, double smooth) {
    auto v = toDouble(x);
    v = zeroPhase(v, Biquad::highpass(lo, fs), 2);
    v = zeroPhase(v, Biquad::lowpass(hi, fs), 2);
    return movingAverage(hilbertEnvelope(v), static_cast<std::size_t>(smooth * fs));
}

double firstBelow(const std::vector<double>& e, double fs, double db) {
    const auto im = static_cast<std::size_t>(std::max_element(e.begin(), e.end()) - e.begin());
    const double thr = e[im] * std::pow(10.0, db / 20.0);
    for (std::size_t i = im; i < e.size(); ++i)
        if (e[i] < thr) return static_cast<double>(i) / fs;
    return static_cast<double>(e.size()) / fs;
}

double envTau(const std::vector<double>& e, double fs, double t0, double t1) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (std::size_t i = static_cast<std::size_t>(t0 * fs); i < static_cast<std::size_t>(t1 * fs) && i < e.size(); i += 8) {
        const double t = static_cast<double>(i) / fs, y = std::log(std::max(e[i], 1e-12));
        sx += t;
        sy += y;
        sxx += t * t;
        sxy += t * y;
        ++n;
    }
    return -1.0 / ((n * sxy - sx * sy) / (n * sxx - sx * sx));
}

// Mean frequency of the zero-crossing cycles in [t0, t1) (1 kHz low-pass).
double windowFreq(const std::vector<float>& x, double fs, double t0, double t1) {
    const Track tr = cycleTrack(x, fs, 1000.0, t0, t1);
    double s = 0.0;
    for (double f : tr.f) s += f;
    return tr.f.empty() ? 0.0 : s / static_cast<double>(tr.f.size());
}

// Snappy (3-12 kHz) against drum (100-300 Hz) energy over 5-60 ms, dB.
double noiseOverDrum(const std::vector<float>& x, double fs) {
    const auto n = bandEnv(x, fs, 3000, 12000, 0.005), d = bandEnv(x, fs, 100, 300, 0.005);
    double nn = 0, dd = 0;
    for (std::size_t i = static_cast<std::size_t>(0.005 * fs); i < static_cast<std::size_t>(0.06 * fs); ++i) {
        nn += n[i] * n[i];
        dd += d[i] * d[i];
    }
    return 10.0 * std::log10(nn / dd);
}

constexpr double kF1[3] = {117.4, 173.5, 230.7};          // settled VCO-1, TUNE 0/50/100 (fact-checked)
constexpr double kToneMs[5] = {99, 178, 225, 254, 289};    // snappy cut-off, TONE 0..100
constexpr double kNormalVpp = 1.537;

}  // namespace

TEST_CASE("SD: both VCOs, settled and locked at 3:2, at every sample rate", "[sd][pitch]") {
    for (double fs : kRates) {
        for (int t = 0; t < 3; ++t) {
            const auto x = fromOnset(hit(fs, knobs(t * 0.5, 0, 0)), 0.1);
            const double f1 = peakFreq(x, fs, 90, 260, 0.05, 0.25);
            const double f2 = peakFreq(x, fs, 1.4 * f1, 1.6 * f1, 0.05, 0.25);
            INFO("fs " << fs << " TUNE " << t * 50 << " %: f1 " << f1 << " f2 " << f2);
            CHECK_THAT(f1, WithinRel(kF1[t], 0.01));
            CHECK_THAT(f2 / f1, WithinAbs(1.500, 0.004));
        }
    }
}

TEST_CASE("SD: the bend: VCO-1 starts ~216 Hz sharp and relaxes in ~5 ms", "[sd][pitch]") {
    for (double fs : kRates) {
        const auto x = fromOnset(hit(fs, knobs(1, 0, 0)), 0.1);
        const double early = windowFreq(x, fs, 0.0, 0.006), late = windowFreq(x, fs, 0.02, 0.035);
        INFO("fs " << fs << " early " << early << " late " << late);
        // reference recording, same metric: 337.9 / 227.2 Hz (TUNE 100)
        CHECK_THAT(early, WithinRel(337.9, 0.04));
        CHECK_THAT(late, WithinRel(227.2, 0.04));
    }
    // Tune Depth scales the bend; 0 removes it.
    VoiceParams p = knobs(1, 0, 0);
    p.k[mw909::knob::SdDepth] = 0.0f;
    const auto flat = fromOnset(hit(48000.0, p), 0.1);
    const auto stock = fromOnset(hit(48000.0, knobs(1, 0, 0)), 0.1);
    CHECK(windowFreq(flat, 48000.0, 0.0, 0.006) < windowFreq(stock, 48000.0, 0.0, 0.006) - 80.0);
}

TEST_CASE("SD: drum envelopes and the VCO-2 level", "[sd][decay]") {
    for (double fs : kRates) {
        const auto x = fromOnset(hit(fs, knobs(0.5, 0, 0)), 0.1);
        const double f1 = 173.5, f2 = 260.2;
        const auto e1 = bandEnv(x, fs, 0.8 * f1, 1.2 * f1, 0.003), e2 = bandEnv(x, fs, 0.85 * f2, 1.15 * f2, 0.003);
        const double t1 = envTau(e1, fs, 0.015, 0.07) * 1e3, t2 = envTau(e2, fs, 0.015, 0.07) * 1e3;
        INFO("fs " << fs << " tau VCO-1 " << t1 << " ms, VCO-2 " << t2 << " ms");
        CHECK_THAT(t1, WithinRel(25.4, 0.08));  // measured 25.0..25.9 (same metric)
        CHECK_THAT(t2, WithinRel(23.0, 0.10));  // measured 21.5..24.8
    }
}

TEST_CASE("SD: TONE sets the snappy cut-off, at every sample rate", "[sd][decay]") {
    for (double fs : kRates) {
        for (int o = 0; o <= 4; ++o) {
            double m = 0.0;
            for (std::uint32_t h = 0; h < 4; ++h)
                m += firstBelow(bandEnv(hit(fs, knobs(0.5, o * 0.25, 1), Hit::normal(), 0.5, 100U + h), fs, 3000, 12000, 0.002), fs, -40.0) / 4.0;
            INFO("fs " << fs << " TONE " << o * 25 << " %: cut-off " << m * 1e3 << " ms (measured " << kToneMs[o] << ")");
            CHECK_THAT(m * 1e3, WithinRel(kToneMs[o], 0.08));
        }
    }
}

TEST_CASE("SD: SNAPPY sets the noise against the drum", "[sd][level]") {
    const double fs = 48000.0;
    double r[3] = {0, 0, 0};
    for (int s = 0; s < 3; ++s)
        for (std::uint32_t h = 0; h < 6; ++h) r[s] += noiseOverDrum(hit(fs, knobs(0.5, 0.5, s * 0.5), Hit::normal(), 0.3, 300U + h), fs) / 6.0;
    std::printf("[sd snappy] noise/drum: SNAPPY 0 %.1f, 50 %.1f, 100 %.1f dB (measured -56.1 / -9.4 / -2.7)\n", r[0], r[1], r[2]);
    CHECK(r[0] < -45.0);
    CHECK_THAT(r[1], WithinAbs(-9.4, 1.5));
    CHECK_THAT(r[2], WithinAbs(-2.7, 1.5));
}

TEST_CASE("SD: levels in volts; the in-band level holds at every rate", "[sd][level]") {
    // The snappy noise reaches the host Nyquist, so the raw Vpp grows with
    // the rate; the level is calibrated and checked below 16 kHz (the late
    // units' multi-out RC is a 15.9 kHz pole), over 24 hits.
    auto meanVpp = [](double fs, const Hit& h, bool inBand) {
        double m = 0.0;
        for (std::uint32_t k = 0; k < 24; ++k) {
            const auto x = hit(fs, knobs(0.5, 0.5, 0.5), h, 0.3, 700U + k);
            m += (inBand ? vppInBand(x, fs) : vpp(x)) / 24.0;
        }
        return m;
    };
    const double ref = meanVpp(48000.0, Hit::normal(), true);
    for (double fs : kRates) {
        const double n = meanVpp(fs, Hit::normal(), true), a = meanVpp(fs, Hit::accented(true, false, 0.0f), true);
        const double raw = meanVpp(fs, Hit::normal(), false);
        std::printf("[sd level] fs %.0f: normal %.3f Vpp below 16 kHz (%.3f raw), accented %.3f Vpp (%+.1f dB), %+.1f %% re 48 kHz\n", fs, n, raw, a,
                    toDb(a / n), 100.0 * (n / ref - 1.0));
        CHECK_THAT(n, WithinRel(kNormalVpp, 0.03));
        CHECK_THAT(a, WithinRel(meanVpp(48000.0, Hit::accented(true, false, 0.0f), true), 0.03));
        CHECK(a > 1.3 * n);
    }
}

TEST_CASE("SD: TOTAL ACCENT gain curve", "[sd][level]") {
    const double fs = 48000.0;
    auto meanVpp = [&](float k) {
        double m = 0.0;
        for (std::uint32_t h = 0; h < 6; ++h) m += vpp(hit(fs, knobs(0.5, 1, 1), Hit::accented(false, true, k), 0.4, 500U + h)) / 6.0;
        return toDb(m);
    };
    const double v0 = meanVpp(0.0f), v5 = meanVpp(0.5f), v1 = meanVpp(1.0f);
    std::printf("[sd accent] TOTAL ACCENT 0 -> 50 -> 100 %%: %+.1f / %+.1f dB (measured +3.4 / +4.8)\n", v5 - v0, v1 - v0);
    CHECK_THAT(v1 - v0, WithinAbs(4.8, 1.0));
    CHECK(v5 > v0);
    CHECK(v1 >= v5 - 0.1);
    CHECK(vpp(hit(fs, knobs(0.5, 0.5, 0.5), Hit::weak())) < vpp(hit(fs, knobs(0.5, 0.5, 0.5))));
}

TEST_CASE("SD: the snappy spectrum follows the loaded filters", "[sd][params]") {
    const double fs = 96000.0;
    Spectrum s;
    for (std::uint32_t h = 0; h < 8; ++h) {
        const auto x = hit(fs, knobs(0.5, 0.5, 1), Hit::normal(), 0.2, 40U + h);
        const Spectrum a = spectrum(x, fs, 0.05, 0.12);
        if (s.power.empty()) s = a;
        else
            for (std::size_t i = 0; i < a.power.size(); ++i) s.power[i] += a.power[i];
    }
    const auto b = octaveBandsDb(s, 250, 8);
    std::printf("[sd spectrum] octave bands 250 Hz..32 kHz:");
    for (double v : b) std::printf(" %.1f", v);
    std::printf(" dB (measured -19.0 -18.3 -11.2 -6.0 0.0 -3.0 -18.3 -34.3)\n");
    const double measured[6] = {-11.2, -6.0, 0.0, -3.0, -18.3, -34.3};  // 1 kHz .. 32 kHz
    for (int i = 0; i < 5; ++i) CHECK_THAT(b[static_cast<std::size_t>(i + 2)], WithinAbs(measured[i], 3.0));
}

TEST_CASE("SD: Decay scales the drum and the snappy", "[sd][params]") {
    const double fs = 48000.0;
    VoiceParams p = knobs(0.5, 0.5, 1);
    auto t20 = [&](const VoiceParams& q) { return rmsDecayTime(fromOnset(hit(fs, q, Hit::normal(), 1.5)), fs, -30.0); };
    const double stock = t20(p);
    p.k[mw909::knob::SdDecay] = 1.0f;
    const double lng = t20(p);
    p.k[mw909::knob::SdDecay] = 0.0f;
    const double shrt = t20(p);
    std::printf("[sd decay knob] -30 dB at %.0f / %.0f / %.0f ms (Decay 0 / stock / 1)\n", shrt * 1e3, stock * 1e3, lng * 1e3);
    CHECK(lng > 1.8 * stock);
    CHECK(shrt < 0.7 * stock);
}

TEST_CASE("SD: retrigger keeps the envelopes, resets the VCOs, no clicks", "[sd][retrigger]") {
    const double fs = 48000.0;
    const VoiceParams p = knobs(0.5, 1.0, 0.0);
    // A weak hit onto an accented ring: the envelope capacitors, charged
    // through diodes, keep the higher voltage (a fresh weak hit is quieter).
    const auto fresh = render<SnareDrum>(prep, setupAt(fs), p, {{0.03, Hit::weak()}}, 0.3);
    const auto re = render<SnareDrum>(prep, setupAt(fs), p, {{0.0, Hit::accented(true, true, 1.0f)}, {0.03, Hit::weak()}}, 0.3);
    const double a = rms(fresh, fs, 0.035, 0.08), b = rms(re, fs, 0.035, 0.08);
    INFO("r.m.s. after the retrigger: fresh weak hit " << a << ", onto the ring " << b);
    CHECK(b > 1.5 * a);
    CHECK(maxStep(re, fs, 0.029, 0.034) < 0.3 * peakAbs(re));
}

TEST_CASE("SD: robustness fuzz, non-finite controls, corrupt variance", "[sd][fuzz]") {
    fuzzRobustness<SnareDrum>(prep, mw909::SD, 48000.0, 1U, 10.0);
    fuzzRobustness<SnareDrum>(prep, mw909::SD, 44100.0, 2U, 10.0, 1.0f);
    fuzzRobustness<SnareDrum>(prep, mw909::SD, 192000.0, 3U, 10.0);
    checkNonFiniteParams<SnareDrum>(prep, mw909::SD, 48000.0);
    checkBadVariance<SnareDrum>(prep, 48000.0);
}

TEST_CASE("SD: deterministic, partition-independent, seed ignored at zero variance", "[sd][determinism]") {
    for (double fs : {44100.0, 96000.0}) {
        checkDeterministic<SnareDrum>(prep, mw909::SD, fs);
        checkPartitionIndependent<SnareDrum>(prep, knobs(0.3, 0.7, 0.6), fs);
        checkSeedIgnoredAtZeroVariance<SnareDrum>(prep, fs, knobs(0.5, 0.5, 0.5));
    }
}

TEST_CASE("SD: sleeps only once inaudible and wakes exactly", "[sd][sleep]") {
    for (double fs : kRates) {
        const double slept = checkQuietBeforeSleep<SnareDrum>(prep, fs, knobs(0.5, 1.0, 1.0), Hit::accented(true, true, 1.0f), 0.02);
        CHECK(slept < 3.0);
        checkWakeIsFresh<SnareDrum>(prep, fs, knobs(0.5, 0.5, 0.5), 0.3);
    }
}

TEST_CASE("SD: against recordings of the original machine", "[sd][reference]") {
    if (!haveRefs()) SKIP("reference recordings not present");
    const double fs = 48000.0;
    auto file = [](int t, int o, int s) { return "SnareDrum/SnareDrum909-tune" + pct(t) + "-tone" + pct(o) + "-snappy" + pct(s) + ".wav"; };
    for (int t : {0, 50, 100}) {
        Wav w;
        REQUIRE(loadRef(file(t, 0, 0), w));
        const auto r = fromOnset(w.x, 0.1), m = fromOnset(hit(fs, knobs(t / 100.0, 0, 0)), 0.1);
        const double rf1 = peakFreq(r, w.fs, 90, 260, 0.05, 0.25), mf1 = peakFreq(m, fs, 90, 260, 0.05, 0.25);
        const double rf2 = peakFreq(r, w.fs, 1.4 * rf1, 1.6 * rf1, 0.05, 0.25), mf2 = peakFreq(m, fs, 1.4 * mf1, 1.6 * mf1, 0.05, 0.25);
        std::printf("[sd reference] TUNE %3d: f1 %.1f / %.1f, f2 %.1f / %.1f Hz; cycles 0-6 ms %.0f / %.0f, 6-12 %.0f / %.0f, 12-20 %.0f / %.0f Hz\n", t,
                    rf1, mf1, rf2, mf2, windowFreq(r, w.fs, 0, 0.006), windowFreq(m, fs, 0, 0.006), windowFreq(r, w.fs, 0.006, 0.012),
                    windowFreq(m, fs, 0.006, 0.012), windowFreq(r, w.fs, 0.012, 0.02), windowFreq(m, fs, 0.012, 0.02));
        CHECK_THAT(mf1, WithinRel(rf1, 0.01));
        CHECK_THAT(mf2, WithinRel(rf2, 0.01));
        CHECK_THAT(windowFreq(m, fs, 0, 0.006), WithinRel(windowFreq(r, w.fs, 0, 0.006), 0.05));
        CHECK_THAT(windowFreq(m, fs, 0.012, 0.02), WithinRel(windowFreq(r, w.fs, 0.012, 0.02), 0.08));
    }
    for (int o = 0; o <= 100; o += 25) {
        Wav w;
        REQUIRE(loadRef(file(50, o, 100), w));
        const double r = firstBelow(bandEnv(w.x, w.fs, 3000, 12000, 0.002), w.fs, -40.0);
        double m = 0.0;
        for (std::uint32_t h = 0; h < 4; ++h) m += firstBelow(bandEnv(hit(fs, knobs(0.5, o / 100.0, 1), Hit::normal(), 0.5, 100U + h), fs, 3000, 12000, 0.002), fs, -40.0) / 4.0;
        std::printf("[sd reference] TONE %3d: snappy cut-off %.0f / %.0f ms\n", o, r * 1e3, m * 1e3);
        CHECK_THAT(m, WithinRel(r, 0.10));
    }
    for (int s : {50, 100}) {
        Wav w;
        REQUIRE(loadRef(file(50, 50, s), w));
        double m = 0.0;
        for (std::uint32_t h = 0; h < 6; ++h) m += noiseOverDrum(hit(fs, knobs(0.5, 0.5, s / 100.0), Hit::normal(), 0.3, 300U + h), fs) / 6.0;
        const double r = noiseOverDrum(w.x, w.fs);
        std::printf("[sd reference] SNAPPY %3d: noise/drum %.1f / %.1f dB\n", s, r, m);
        CHECK_THAT(m, WithinAbs(r, 2.0));
    }
    double r0 = 0.0, m0 = 0.0;
    for (int a : {0, 50, 100}) {
        Wav w;
        REQUIRE(loadRef("SnareDrum/Snare909-accent" + pct(a) + "-tune050-tone100-snappy100.wav", w));
        double m = 0.0;
        for (std::uint32_t h = 0; h < 6; ++h) m += vpp(hit(fs, knobs(0.5, 1, 1), Hit::accented(false, true, static_cast<float>(a) / 100.0f), 0.4, 500U + h)) / 6.0;
        const double r = toDb(vpp(w.x));
        if (a == 0) {
            r0 = r;
            m0 = toDb(m);
        }
        std::printf("[sd reference] TOTAL ACCENT %3d %%: Vpp %+.1f / %+.1f dB\n", a, r - r0, toDb(m) - m0);
        CHECK_THAT(toDb(m) - m0, WithinAbs(r - r0, 1.2));
    }
}

TEST_CASE("SD: renders for listening", "[.render]") {
    const double fs = 48000.0;
    std::vector<float> all;
    for (const VoiceParams& p : {knobs(0.5, 0.5, 0.5), knobs(0, 0, 0), knobs(1, 1, 1), knobs(0.5, 1, 1)}) {
        for (const Hit& h : {Hit::normal(), Hit::accented(true, true, 1.0f), Hit::weak()}) {
            const auto x = hit(fs, p, h, 0.5);
            all.insert(all.end(), x.begin(), x.end());
        }
    }
    writeWav(renderDir() + "/sd.wav", all, fs, 0.1f);
    SUCCEED("wrote sd.wav");
}

TEST_CASE("SD: CPU cost per sample", "[sd][bench]") {
    const double ns = benchNsPerSample<SnareDrum>(prep, 48000.0, knobs(0.5, 0.5, 0.5), 0.25, 4.0);
    std::printf("[sd bench] 48 kHz, re-hit every 250 ms: %.1f ns/sample\n", ns);
    checkCpu(ns, 800.0);
}
