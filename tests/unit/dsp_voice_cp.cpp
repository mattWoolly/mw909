// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Hand clap: burst timing and depth, the reverb tail, the band-pass and the
// spectra, the trimmer level in volts, accent, extended knobs, robustness,
// wake, and the comparison with recordings of the original machine (skipped
// when absent).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>

#include "AnalogTestUtil.h"
#include "mw909/voices/HandClap.h"

using namespace analog_test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using mw909::voices::HandClap;

namespace {

const Prep<HandClap> prep = [](HandClap& v, const VoiceSetup& s) { v.prepare(s); };

VoiceParams knobs(double tune = 0.0, double decay = 0.5, double reverb = 0.5) {
    VoiceParams p = mw909::defaultParams(mw909::CP);
    p.k[mw909::knob::CpTune] = static_cast<float>(tune);
    p.k[mw909::knob::CpDecay] = static_cast<float>(decay);
    p.k[mw909::knob::CpReverb] = static_cast<float>(reverb);
    return p;
}

std::vector<std::vector<float>> hits(double fs, const VoiceParams& p = knobs(), const Hit& h = Hit::normal(), int n = 16,
                                     double seconds = 0.4) {
    std::vector<std::vector<float>> xs;
    for (int i = 0; i < n; ++i) xs.push_back(fromOnset(renderHit<HandClap>(prep, fs, p, h, seconds, 1000U + 7919U * static_cast<std::uint32_t>(i)), 0.05));
    return xs;
}

// 1 ms r.m.s. envelope of the 0.5-5 kHz band, power-averaged over the hits,
// dB re its maximum.
std::vector<double> envDb(const std::vector<std::vector<float>>& xs, double fs) {
    const auto hop = static_cast<std::size_t>(fs / 1000.0);
    std::vector<double> acc;
    for (const auto& x0 : xs) {
        auto v = toDouble(x0);
        v = zeroPhase(v, Biquad::highpass(500, fs));
        v = zeroPhase(v, Biquad::lowpass(5000, fs));
        if (acc.empty()) acc.assign(std::min<std::size_t>(v.size() / hop, 400), 0.0);
        for (std::size_t i = 0; i < acc.size(); ++i)
            for (std::size_t k = 0; k < hop; ++k) acc[i] += v[i * hop + k] * v[i * hop + k];
    }
    const double m = *std::max_element(acc.begin(), acc.end());
    for (auto& a : acc) a = 10.0 * std::log10(std::max(a / m, 1e-12));
    return acc;
}

// Times (ms) of the re-attacks: the envelope rising by > 9 dB within 2 ms.
std::vector<double> reattacks(const std::vector<double>& e) {
    std::vector<double> t;
    for (std::size_t i = 4; i < 45 && i + 2 < e.size(); ++i) {
        if (e[i + 2] - e[i] > 9.0 && (t.empty() || static_cast<double>(i + 1) - t.back() > 5.0)) t.push_back(static_cast<double>(i + 1));
    }
    return t;
}

Spectrum meanSpectrum(const std::vector<std::vector<float>>& xs, double fs, double t0, double t1, std::size_t size) {
    Spectrum s;
    for (const auto& x : xs) {
        const Spectrum a = spectrum(x, fs, t0, t1, size);
        if (s.power.empty()) s = a;
        else
            for (std::size_t i = 0; i < a.power.size(); ++i) s.power[i] += a.power[i];
    }
    return s;
}

double meanRms(const std::vector<std::vector<float>>& xs, double fs, double t0, double t1) {
    double s = 0.0;
    for (const auto& x : xs) s += rms(x, fs, t0, t1) / static_cast<double>(xs.size());
    return s;
}

// The same below 16 kHz (the audible band, comparable across rates).
double meanBandRms(const std::vector<std::vector<float>>& xs, double fs, double t0, double t1) {
    double s = 0.0;
    for (const auto& x : xs) {
        const auto v = zeroPhase(toDouble(x), Biquad::lowpass(16000.0, fs), 2);
        s += rms(std::vector<float>(v.begin(), v.end()), fs, t0, t1) / static_cast<double>(xs.size());
    }
    return s;
}

// The reference unit (accent 0 and eight single shots), same metrics.
constexpr double kRefBursts[3] = {9.0, 20.0, 30.0};
constexpr double kRefTail[4] = {-15.0, -24.6, -29.0, -35.1};  // at 60, 100, 140, 200 ms
constexpr double kRefBurstBands[6] = {-3, 0, -3, -6, -10, -16};  // 0.5..16 kHz octaves, 0-34 ms
constexpr double kTargetVpp = 2.0;                               // the TM trimmer target

}  // namespace

TEST_CASE("CP: four bursts at the measured times, at every rate", "[cp][decay]") {
    for (double fs : kRates) {
        const auto e = envDb(hits(fs), fs);
        const auto t = reattacks(e);
        REQUIRE(t.size() == 3);
        double dip = 0.0;
        for (std::size_t i = 3; i < static_cast<std::size_t>(t[0]); ++i) dip = std::min(dip, e[i]);
        INFO("fs " << fs << ": re-attacks at " << t[0] << " / " << t[1] << " / " << t[2] << " ms, first dip " << dip << " dB");
        for (int i = 0; i < 3; ++i) CHECK_THAT(t[static_cast<std::size_t>(i)], WithinAbs(kRefBursts[i], 1.5));
        CHECK(dip < -20.0);  // each burst falls ~25 dB before the next (measured -25)
        CHECK(dip > -36.0);
    }
}

TEST_CASE("CP: the tail after the bursts, at every rate", "[cp][decay]") {
    for (double fs : kRates) {
        const auto e = envDb(hits(fs), fs);
        const double tail[4] = {e[60], e[100], e[140], e[200]};
        INFO("fs " << fs << " tail at 60/100/140/200 ms: " << tail[0] << " " << tail[1] << " " << tail[2] << " " << tail[3]);
        for (int i = 0; i < 4; ++i) CHECK_THAT(tail[i], WithinAbs(kRefTail[i], 4.0));
    }
}

TEST_CASE("CP: band-pass and spectra; the tail is darker than the bursts", "[cp][pitch]") {
    for (double fs : kRates) {
        const auto xs = hits(fs);
        const Spectrum b = meanSpectrum(xs, fs, 0.0, 0.034, 1 << 14), t = meanSpectrum(xs, fs, 0.06, 0.25, 1 << 16);
        const auto bands = octaveBandsDb(b, 125, 8);
        // The band-pass x 1.94 kHz high-pass makes a broad plateau, so its
        // peak is read at 48 kHz only; the octave bands up to 8 kHz and the
        // centroid at every rate. (Above ~10 kHz the reference carries the
        // multi-out RC and the host-rate filters their warping.)
        INFO("fs " << fs << " burst peak " << b.peakHz(400, 4000, 60) << " Hz, centroids " << b.centroid(100, 8000) << " / " << t.centroid(100, 8000));
        if (fs == 48000.0) CHECK_THAT(b.peakHz(400, 4000, 60), WithinRel(1084.0, 0.12));  // the reference, same metric
        for (int i = 0; i < 5; ++i) CHECK_THAT(bands[static_cast<std::size_t>(i + 2)], WithinAbs(kRefBurstBands[i], 3.0));
        CHECK_THAT(b.centroid(100, 8000), WithinRel(2350.0, 0.10));
        CHECK(t.centroid(100, 20000) < 0.6 * b.centroid(100, 20000));  // measured 1.0 vs 2.6 kHz
    }
}

TEST_CASE("CP: the trimmer level in volts; the in-band level holds at every rate", "[cp][level]") {
    // The 2 Vpp target is calibrated as the mean in-band Vpp (below 16 kHz,
    // the late units' multi-out RC is a 15.9 kHz pole) of normal hits; the
    // hits vary over 6 dB, so the mean is over 64 of them.
    const double ref = meanBandRms(hits(48000.0), 48000.0, 0.0, 0.15);
    for (double fs : kRates) {
        const auto xs = hits(fs, knobs(), Hit::normal(), 64, 0.12);
        double v = 0.0, raw = 0.0;
        for (const auto& x : xs) {
            v += vppInBand(x, fs) / static_cast<double>(xs.size());
            raw += vpp(x) / static_cast<double>(xs.size());
        }
        const double r = meanBandRms(hits(fs), fs, 0.0, 0.15);
        std::printf("[cp level] fs %.0f: mean %.3f Vpp below 16 kHz (trimmer target 2.0), %.3f Vpp raw; r.m.s. below 16 kHz %+.2f dB re 48 kHz\n", fs, v, raw, toDb(r / ref));
        CHECK_THAT(v, WithinRel(kTargetVpp, 0.03));
        CHECK_THAT(r, WithinRel(ref, 0.03));
    }
}

TEST_CASE("CP: accent raises the bursts and the tail", "[cp][level]") {
    const double fs = 48000.0;
    const auto n = hits(fs), a = hits(fs, knobs(), Hit::accented(false, true, 1.0f));
    const double db = toDb(meanRms(a, fs, 0.0, 0.04) / meanRms(n, fs, 0.0, 0.04));
    const double dt = toDb(meanRms(a, fs, 0.06, 0.15) / meanRms(n, fs, 0.06, 0.15));
    std::printf("[cp accent] TOTAL ACCENT 100 %%: bursts %+.1f dB, tail %+.1f dB (measured +5.6 / +6.4)\n", db, dt);
    CHECK_THAT(db, WithinAbs(5.3, 1.5));
    CHECK_THAT(dt, WithinAbs(6.4, 1.5));
    const auto w = hits(fs, knobs(), Hit::weak());
    CHECK(meanRms(w, fs, 0.0, 0.04) < meanRms(n, fs, 0.0, 0.04));
}

TEST_CASE("CP: Tune moves the band-pass; Decay and Reverb shape the tail", "[cp][params]") {
    const double fs = 48000.0;
    const double c0 = meanSpectrum(hits(fs), fs, 0.0, 0.034, 1 << 14).peakHz(400, 4000, 60);
    const double up = meanSpectrum(hits(fs, knobs(12.0)), fs, 0.0, 0.034, 1 << 14).peakHz(800, 8000, 120);
    std::printf("[cp knobs] burst peak %.0f Hz, +12 semitones %.0f Hz\n", c0, up);
    CHECK_THAT(up / c0, WithinRel(2.0, 0.15));
    auto tail = [&](const VoiceParams& p) { return toDb(meanRms(hits(fs, p, Hit::normal(), 8, 0.6), fs, 0.15, 0.3)); };
    const double stock = tail(knobs()), none = tail(knobs(0, 0.5, 0.0)), full = tail(knobs(0, 0.5, 1.0)), lng = tail(knobs(0, 1.0, 0.5));
    std::printf("[cp knobs] tail 150-300 ms: reverb 0 %.1f, stock %.1f, reverb 1 %.1f, decay 1 %.1f dB\n", none, stock, full, lng);
    CHECK(none < stock - 6.0);
    CHECK(full > stock + 6.0);
    CHECK(lng > stock + 6.0);
}

TEST_CASE("CP: the first burst after sleep matches a fresh voice's", "[cp][sleep]") {
    const double fs = 48000.0;
    auto burstDb = [&](bool sleepFirst) {
        double e = 0.0;
        for (int h = 0; h < 64; ++h) {
            Rig<HandClap> rig(prep, setupAt(fs), 77U + 7919U * static_cast<std::uint32_t>(h));
            std::vector<float> out;
            if (sleepFirst) {
                rig.voice.trigger(Hit::normal());
                while (rig.voice.isActive()) rig.run(out, 4800, 4800);
                rig.run(out, 4800, 4800);
            }
            out.clear();
            rig.voice.trigger(Hit::normal());
            rig.run(out, static_cast<int>(0.003 * fs));
            for (float v : out) e += static_cast<double>(v) * v;
        }
        return 10.0 * std::log10(e);
    };
    const double fresh = burstDb(false), slept = burstDb(true);
    std::printf("[cp wake] first 3 ms: fresh %.2f dB, after sleeping %+.2f dB\n", fresh, slept - fresh);
    CHECK_THAT(slept, WithinAbs(fresh, 0.5));
    for (double r : kRates) {
        // Decay at x4 and the strongest accent: the reverb capacitors drain
        // only into Q37's base, whose drive falls as 1/t below the knee until
        // the tail termination (-70 dB re its peak, dsp/TransistorVca.h).
        const double s = checkQuietBeforeSleep<HandClap>(prep, r, knobs(0, 1.0, 1.0), Hit::accented(true, true, 1.0f), 0.02);
        CHECK(s < 7.0);
        const double s0 = checkQuietBeforeSleep<HandClap>(prep, r, knobs(), Hit::normal(), 0.02);
        std::printf("[cp sleep] fs %.0f: asleep after %.2f s (stock, normal hit), %.2f s (Decay x4, full accent)\n", r, s0, s);
        CHECK(s0 < 4.0);
    }
}

TEST_CASE("CP: retrigger restarts the burst train without resetting the filters", "[cp][retrigger]") {
    const double fs = 48000.0;
    const auto e = envDb({render<HandClap>(prep, setupAt(fs), knobs(), {{0.0, Hit::normal()}, {0.05, Hit::normal()}}, 0.3)}, fs);
    double before = 0.0;
    for (std::size_t i = 45; i < 50; ++i) before += e[i] / 5.0;
    CHECK(e[51] > before + 6.0);  // a new first burst at 50 ms
}

TEST_CASE("CP: robustness fuzz, non-finite controls, corrupt variance", "[cp][fuzz]") {
    fuzzRobustness<HandClap>(prep, mw909::CP, 48000.0, 1U, 10.0);
    fuzzRobustness<HandClap>(prep, mw909::CP, 44100.0, 2U, 10.0, 1.0f);
    fuzzRobustness<HandClap>(prep, mw909::CP, 192000.0, 3U, 10.0);
    checkNonFiniteParams<HandClap>(prep, mw909::CP, 48000.0);
    checkBadVariance<HandClap>(prep, 48000.0);
}

TEST_CASE("CP: deterministic, partition-independent, seed ignored at zero variance", "[cp][determinism]") {
    for (double fs : {44100.0, 96000.0}) {
        checkDeterministic<HandClap>(prep, mw909::CP, fs);
        checkPartitionIndependent<HandClap>(prep, knobs(2.0, 0.7, 0.6), fs);
        checkSeedIgnoredAtZeroVariance<HandClap>(prep, fs, knobs());
    }
}

TEST_CASE("CP: against recordings of the original machine", "[cp][reference]") {
    if (!haveRefs()) SKIP("reference recordings not present");
    std::vector<std::vector<float>> xs;
    double rfs = 96000.0;
    for (int i = 0; i <= 8; ++i) {
        Wav w;
        REQUIRE(loadRef(i == 0 ? std::string("HandClap/HandClap909-accent000.wav") : "HandClap/HandClap909-shot" + std::to_string(i) + ".wav", w));
        xs.push_back(w.x);
        rfs = w.fs;
    }
    const double fs = 48000.0;
    const auto er = envDb(xs, rfs), em = envDb(hits(fs), fs);
    const auto tr = reattacks(er), tm = reattacks(em);
    REQUIRE(tr.size() == 3);
    REQUIRE(tm.size() == 3);
    std::printf("[cp reference] re-attacks %.0f / %.0f / %.0f ms, model %.0f / %.0f / %.0f ms\n", tr[0], tr[1], tr[2], tm[0], tm[1], tm[2]);
    for (std::size_t i = 0; i < 3; ++i) CHECK_THAT(tm[i], WithinAbs(tr[i], 1.5));
    std::printf("[cp reference] tail at 45/60/100/140/200/300 ms:");
    for (int ms : {45, 60, 100, 140, 200, 300}) std::printf(" %.1f/%.1f", er[static_cast<std::size_t>(ms)], em[static_cast<std::size_t>(ms)]);
    std::printf(" dB\n");
    for (int ms : {60, 100, 140, 200}) CHECK_THAT(em[static_cast<std::size_t>(ms)], WithinAbs(er[static_cast<std::size_t>(ms)], 4.0));
    const Spectrum br = meanSpectrum(xs, rfs, 0.0, 0.034, 1 << 14), bm = meanSpectrum(hits(fs), fs, 0.0, 0.034, 1 << 14);
    const auto obr = octaveBandsDb(br, 125, 8), obm = octaveBandsDb(bm, 125, 8);
    std::printf("[cp reference] burst octave bands 125 Hz..16 kHz:");
    for (std::size_t i = 0; i < 8; ++i) std::printf(" %.0f/%.0f", obr[i], obm[i]);
    std::printf(" dB; peak %.0f / %.0f Hz\n", br.peakHz(400, 4000, 60), bm.peakHz(400, 4000, 60));
    for (std::size_t i = 1; i < 7; ++i) CHECK_THAT(obm[i], WithinAbs(obr[i], 3.0));
}

TEST_CASE("CP: renders for listening", "[.render]") {
    const double fs = 48000.0;
    std::vector<float> all;
    for (const Hit& h : {Hit::normal(), Hit::accented(true, true, 1.0f), Hit::weak()}) {
        const auto x = renderHit<HandClap>(prep, fs, knobs(), h, 0.6);
        all.insert(all.end(), x.begin(), x.end());
    }
    writeWav(renderDir() + "/cp.wav", all, fs, 0.1f);
    SUCCEED("wrote cp.wav");
}

TEST_CASE("CP: CPU cost per sample", "[cp][bench]") {
    const double ns = benchNsPerSample<HandClap>(prep, 48000.0, knobs(), 0.25, 3.0);
    std::printf("[cp bench] 48 kHz, re-hit every 250 ms: %.1f ns/sample\n", ns);
    checkCpu(ns, 400.0);
}
