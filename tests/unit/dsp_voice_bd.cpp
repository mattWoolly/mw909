// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Bass drum: pitch sweep, waveshaper, hold and thresholded decay, ATTACK,
// accent, levels in volts, the extended knobs, robustness, and the
// comparison with recordings of the original machine (skipped when absent).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

#include "AnalogTestUtil.h"
#include "mw909/voices/BassDrum.h"

using namespace analog_test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using mw909::voices::BassDrum;

namespace {

const Prep<BassDrum> prep = [](BassDrum& v, const VoiceSetup& s) { v.prepare(s); };

VoiceParams knobs(double tune, double attack, double decay) {
    VoiceParams p = mw909::defaultParams(mw909::BD);
    p.k[mw909::knob::BdTune] = static_cast<float>(tune);
    p.k[mw909::knob::BdAttack] = static_cast<float>(attack);
    p.k[mw909::knob::BdDecay] = static_cast<float>(decay);
    return p;
}

std::vector<float> hit(double fs, const VoiceParams& p, const Hit& h = Hit::normal(), double seconds = 0.6) {
    return renderHit<BassDrum>(prep, fs, p, h, seconds);
}

// The sweep as the research measured it: per-cycle frequencies of the
// 1.5 kHz-low-passed hit, from its onset, fitted with fb + df e^(-t/tau).
SweepFit sweep(const std::vector<float>& x, double fs) {
    return fitSweep(cycleTrack(fromOnset(x, 0.1), fs, 1500.0, 0.005, 0.35));
}

// Fact-checked t(-20 dB) at DECAY 0/25/50/75/100 (research/01 F1; the mean of
// TUNE 0/50/100, 400 Hz low-pass, 20 ms Hilbert envelope, 0 dB = 20-50 ms).
constexpr double kDecayMs[5] = {97.8, 142.7, 174.3, 208.5, 298.6};
// Sweep fits of the reference recordings with the same metric (TUNE 0..100).
constexpr double kRefTauMs[5] = {7.60, 9.66, 11.79, 14.42, 29.2};
constexpr double kRefDfHz[5] = {210.1, 218.9, 217.3, 218.4, 221.5};
constexpr double kNormalVpp = 1.02;  // CP trimmer target x the reference BD/CP ratio

}  // namespace

TEST_CASE("BD: settled pitch 49.5 Hz and the sweep, at every sample rate", "[bd][pitch]") {
    for (double fs : kRates) {
        for (int t = 0; t <= 4; ++t) {
            const SweepFit f = sweep(hit(fs, knobs(t * 0.25, 0.0, 1.0), Hit::normal(), 0.45), fs);
            INFO("fs " << fs << " TUNE " << t * 25 << " %: fb " << f.fb << " df " << f.df << " tau " << f.tau * 1e3 << " ms");
            CHECK_THAT(f.fb, WithinRel(49.5, 0.006));
            CHECK_THAT(f.tau * 1e3, WithinRel(kRefTauMs[t], 0.03));
            CHECK_THAT(f.df, WithinRel(kRefDfHz[t], 0.05));
            CHECK(f.rmsErr < 1.0);
        }
    }
}

TEST_CASE("BD: the sweep does not follow the accent; the early units' C9 shortens it", "[bd][pitch]") {
    const double fs = 48000.0;
    const SweepFit n = sweep(hit(fs, knobs(1, 0, 1)), fs);
    const SweepFit a = sweep(hit(fs, knobs(1, 0, 1), Hit::accented(true, true, 1.0f)), fs);
    CHECK_THAT(a.df, WithinRel(n.df, 0.03));
    CHECK_THAT(a.tau, WithinRel(n.tau, 0.03));
    const auto early = render<BassDrum>(prep, setupAt(fs, 0, 0.0f, mw909::Revision::Early), knobs(1, 0, 1), {{0.0, Hit::normal()}}, 0.45);
    const SweepFit e = sweep(early, fs);
    std::printf("[bd pitch] TUNE 100: tau %.1f ms (late, C9 0.33u), %.1f ms (early, C9 0.22u)\n", n.tau * 1e3, e.tau * 1e3);
    CHECK_THAT(e.tau / n.tau, WithinRel(0.22 / 0.33, 0.05));
}

TEST_CASE("BD: waveshaper harmonics are the measured, level-independent signature", "[bd][pitch]") {
    for (double fs : kRates) {
        const auto x = fromOnset(hit(fs, knobs(0.5, 0, 1), Hit::normal(), 0.45));
        const auto h = harmonicsDb(x, fs, 0.08, 0.16, 49.5, 7);
        const auto hq = harmonicsDb(x, fs, 0.20, 0.28, 49.5, 7);
        INFO("fs " << fs << " H2..H7 " << h[0] << " " << h[1] << " " << h[2] << " " << h[3] << " " << h[4] << " " << h[5]);
        // research/05 §0.5: H2 -28, H3 -25.7, H5 -41, H7 -41 dB re H1
        CHECK_THAT(h[0], WithinAbs(-28.1, 1.5));
        CHECK_THAT(h[1], WithinAbs(-25.7, 1.5));
        CHECK_THAT(h[3], WithinAbs(-41.0, 2.5));
        CHECK_THAT(h[5], WithinAbs(-40.9, 2.5));
        CHECK_THAT(hq[1], WithinAbs(h[1], 1.0));  // level-independent
    }
}

TEST_CASE("BD: hold then thresholded decay; t(-20 dB) follows DECAY at every rate", "[bd][decay]") {
    for (double fs : kRates) {
        for (int d = 0; d <= 4; ++d) {
            const auto x = fromOnset(hit(fs, knobs(0.5, 0, d * 0.25), Hit::normal(), 0.8));
            const double t = decayTime(x, fs, -20.0) * 1e3;
            INFO("fs " << fs << " DECAY " << d * 25 << " %: " << t << " ms (measured " << kDecayMs[d] << ")");
            CHECK_THAT(t, WithinRel(kDecayMs[d], 0.05));
        }
        // The hold: the body stays within 1 dB for ~60 ms whatever DECAY is.
        const auto x = fromOnset(hit(fs, knobs(0.5, 0, 0), Hit::normal(), 0.3));
        const double t1 = decayTime(x, fs, -1.0) * 1e3;
        INFO("fs " << fs << " -1 dB at " << t1 << " ms");
        CHECK(t1 > 55.0);
        CHECK(t1 < 75.0);
    }
}

TEST_CASE("BD: levels in volts at every rate; accent raises the body, not the pitch", "[bd][level]") {
    for (double fs : kRates) {
        const double n = vpp(hit(fs, knobs(0.5, 0.5, 0.5)));
        const double a = vpp(hit(fs, knobs(0.5, 0.5, 0.5), Hit::accented(true, false, 0.0f)));
        std::printf("[bd level] fs %.0f: normal %.3f Vpp, accented %.3f Vpp (%+.1f dB)\n", fs, n, a, toDb(a / n));
        CHECK_THAT(n, WithinRel(kNormalVpp, 0.03));
        CHECK(a > 1.4 * n);
        CHECK(a < 2.4 * n);
    }
}

TEST_CASE("BD: TOTAL ACCENT gain curve", "[bd][level]") {
    const double fs = 48000.0;
    double v0 = 0.0, b0 = 0.0, prev = -1e9;
    for (int k = 0; k <= 10; ++k) {
        const auto x = fromOnset(hit(fs, knobs(1, 1, 1), Hit::accented(false, true, static_cast<float>(k) * 0.1f), 0.4));
        const double v = toDb(vpp(x)), b = toDb(vppWindow(x, fs, 0.01, 0.03));
        if (k == 0) {
            v0 = v;
            b0 = b;
        }
        CHECK(v >= prev - 0.05);
        prev = v;
        if (k == 10) {
            std::printf("[bd accent] TOTAL ACCENT 0 -> 100 %%: Vpp %+.1f dB (measured +6.2), body %+.1f dB (+6.7)\n", v - v0, b - b0);
            CHECK_THAT(v - v0, WithinAbs(6.2, 1.0));
            CHECK_THAT(b - b0, WithinAbs(6.7, 1.0));
        }
    }
    // A weak hit is quieter, finite and click-free.
    const auto w = hit(fs, knobs(0.5, 0.5, 0.5), Hit::weak());
    CHECK(vpp(w) < vpp(hit(fs, knobs(0.5, 0.5, 0.5))));
    CHECK(allFinite(w));
}

TEST_CASE("BD: ATTACK adds the click at the tone's start", "[bd][params]") {
    const double fs = 96000.0;
    auto first3 = [&](double attack) {
        const auto x = fromOnset(hit(fs, knobs(0.5, attack, 0.5), Hit::normal(), 0.1), 0.1);
        return toDb(peakAbs(std::vector<float>(x.begin(), x.begin() + static_cast<long>(0.003 * fs))));
    };
    const double a0 = first3(0.0), a1 = first3(1.0);
    std::printf("[bd attack] first 3 ms peak: ATTACK 0 -> 100 %+.1f dB (measured +4.3)\n", a1 - a0);
    CHECK(a1 - a0 > 2.0);
    CHECK(a1 - a0 < 6.0);
    // The click is short: after 10 ms the two settings agree.
    const auto x0 = hit(fs, knobs(0.5, 0.0, 0.5), Hit::normal(), 0.1), x1 = hit(fs, knobs(0.5, 1.0, 0.5), Hit::normal(), 0.1);
    double d = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.010 * fs); i < x0.size(); ++i) d = std::max(d, std::abs(static_cast<double>(x1[i]) - x0[i]));
    CHECK(d < 0.01 * peakAbs(x0));
}

TEST_CASE("BD: Pitch, Tune Depth and Shaper", "[bd][params]") {
    const double fs = 48000.0;
    VoiceParams p = knobs(0.5, 0, 1);
    p.k[mw909::knob::BdPitch] = 12.0f;
    CHECK_THAT(sweep(hit(fs, p), fs).fb, WithinRel(99.0, 0.01));
    p.k[mw909::knob::BdPitch] = -12.0f;
    CHECK_THAT(sweep(hit(fs, p), fs).fb, WithinRel(24.75, 0.02));
    p = knobs(0.5, 0, 1);
    const double df = sweep(hit(fs, p), fs).df;
    p.k[mw909::knob::BdDepth] = 1.0f;
    CHECK_THAT(sweep(hit(fs, p), fs).df, WithinRel(2.0 * df, 0.08));
    p.k[mw909::knob::BdDepth] = 0.0f;
    const auto flat = hit(fs, p);
    CHECK_THAT(zeroCrossings(flat, fs, 1500.0).size() > 0 ? peakFreq(flat, fs, 30, 80, 0.01, 0.2) : 0.0, WithinRel(49.5, 0.01));
    // Shaper: more drive, a squarer wave (more H3); little drive leaves the
    // triangle (H3 -19.1 dB). The stock drive sits near H3's null.
    p = knobs(0.5, 0, 1);
    const double h3 = harmonicsDb(fromOnset(hit(fs, p)), fs, 0.08, 0.16, 49.5, 3)[1];
    p.k[mw909::knob::BdDrive] = 1.0f;
    const double h3hot = harmonicsDb(fromOnset(hit(fs, p)), fs, 0.08, 0.16, 49.5, 3)[1];
    p.k[mw909::knob::BdDrive] = 0.0f;
    const double h3soft = harmonicsDb(fromOnset(hit(fs, p)), fs, 0.08, 0.16, 49.5, 3)[1];
    std::printf("[bd shaper] H3: drive 0 %.1f, stock %.1f, full %.1f dB\n", h3soft, h3, h3hot);
    CHECK(h3hot > h3 + 3.0);
    CHECK_THAT(h3soft, WithinAbs(-19.1, 2.5));
    // Chromatic play scales the whole VCO.
    const auto up = renderHit<BassDrum>(prep, fs, knobs(0.5, 0, 1), Hit{Hit::normal().acc, 7.0f}, 0.6);
    CHECK_THAT(sweep(up, fs).fb, WithinRel(49.5 * std::exp2(7.0 / 12.0), 0.01));
}

TEST_CASE("BD: Shaper automation is smooth and cheap (tables built in prepare)", "[bd][params]") {
    for (double fs : kRates) {
        // A jump 0 -> 1 inside the hold: the curve glides (5 ms) instead of
        // stepping, so no sample step beyond the waveform's own.
        Rig<BassDrum> rig(prep, setupAt(fs));
        VoiceParams p = knobs(0.5, 0.0, 1.0);
        p.k[mw909::knob::BdDrive] = 0.0f;
        rig.voice.setParams(p);
        rig.voice.trigger(Hit::normal());
        std::vector<float> x;
        const int change = static_cast<int>(0.052 * fs);
        rig.run(x, change, 16);
        p.k[mw909::knob::BdDrive] = 1.0f;
        rig.voice.setParams(p);
        rig.run(x, static_cast<int>(0.03 * fs), 16);
        // A squarer curve has steeper edges: compare with a hit that had the
        // full drive all along.
        const auto hot = render<BassDrum>(prep, setupAt(fs), p, {{0.0, Hit::normal()}}, 0.082);
        const double before = maxStep(x, fs, 0.010, 0.051), at = maxStep(x, fs, 0.0515, 0.06);
        const double own = std::max(before, maxStep(hot, fs, 0.0515, 0.06));
        INFO("fs " << fs << ": largest sample step 10..51 ms " << before << " V, after the jump " << at << " V, full drive "
                   << own << " V");
        CHECK(at < 1.1 * own);
    }
    // setParams with the Shaper moving every block costs no more than any other knob.
    BassDrum v;
    v.prepare(setupAt(48000.0));
    VoiceParams p = mw909::defaultParams(mw909::BD);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i) {
        p.k[mw909::knob::BdDrive] = (i & 1) != 0 ? 0.2f : 0.8f;
        v.setParams(p);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0;
    std::printf("[bd shaper] setParams with the Shaper moving: %.3f us per call\n", us);
    if (kTimingChecks) CHECK(us < 2.0);
    // Rendering with the Shaper automated once per 64-sample block.
    Rig<BassDrum> rig(prep, setupAt(48000.0));
    std::vector<float> out(64), noise(64, 0.0f);
    VoiceParams q = knobs(0.5, 0.5, 1.0);
    const int blocks = 48000 * 4 / 64;
    const auto t1 = std::chrono::steady_clock::now();
    double sink = 0.0;
    for (int b = 0; b < blocks; ++b) {
        q.k[mw909::knob::BdDrive] = static_cast<float>(0.5 + 0.5 * std::sin(b * 0.05));
        rig.voice.setParams(q);
        if (b % 188 == 0) rig.voice.trigger(Hit::normal());
        rig.voice.render(out.data(), noise.data(), 64);
        sink += static_cast<double>(out[0]);
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t1).count() / (blocks * 64.0);
    std::printf("[bd shaper] Shaper automated per 64-sample block: %.1f ns/sample\n", ns);
    CHECK(std::isfinite(sink));
    checkCpu(ns, 400.0);
}

TEST_CASE("BD: retrigger resets the VCO (the circuit does) but not the envelopes; no clicks", "[bd][retrigger]") {
    const double fs = 48000.0;
    const VoiceParams p = knobs(0.5, 0.5, 1.0);
    const auto fresh = render<BassDrum>(prep, setupAt(fs), p, {{0.1, Hit::normal()}}, 0.4);
    const auto re = render<BassDrum>(prep, setupAt(fs), p, {{0.0, Hit::accented(true, true, 1.0f)}, {0.1, Hit::normal()}}, 0.4);
    double diff = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.1 * fs); i < fresh.size(); ++i) diff = std::max(diff, std::abs(static_cast<double>(fresh[i]) - re[i]));
    INFO("max difference after the retrigger " << diff);
    CHECK(diff > 0.01);  // the accented hit's C8 charge is still there
    // The VCO reset is an RC discharge, not a step: no sample jump beyond the
    // waveform's own slope during the 2 ms trigger.
    CHECK(maxStep(re, fs, 0.099, 0.104) < 0.25 * peakAbs(re));
}

TEST_CASE("BD: robustness fuzz, non-finite controls, corrupt variance", "[bd][fuzz]") {
    fuzzRobustness<BassDrum>(prep, mw909::BD, 48000.0, 1U, 10.0);
    fuzzRobustness<BassDrum>(prep, mw909::BD, 44100.0, 2U, 10.0, 1.0f);
    fuzzRobustness<BassDrum>(prep, mw909::BD, 192000.0, 3U, 10.0);
    checkNonFiniteParams<BassDrum>(prep, mw909::BD, 48000.0);
    checkBadVariance<BassDrum>(prep, 48000.0);
}

TEST_CASE("BD: deterministic, partition-independent, seed ignored at zero variance", "[bd][determinism]") {
    for (double fs : {44100.0, 96000.0}) {
        checkDeterministic<BassDrum>(prep, mw909::BD, fs);
        checkPartitionIndependent<BassDrum>(prep, knobs(0.3, 0.7, 0.6), fs);
        checkSeedIgnoredAtZeroVariance<BassDrum>(prep, fs, knobs(0.5, 0.5, 0.5));
    }
    // Unit variance moves the pitch a little (two units agree within 0.3 %).
    double lo = 1e9, hi = 0.0;
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        const auto x = render<BassDrum>(prep, setupAt(48000.0, seed, 1.0f), knobs(0.5, 0, 1), {{0.0, Hit::normal()}}, 0.45);
        const double f = sweep(x, 48000.0).fb;
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    std::printf("[bd variance] eight units: settled pitch %.2f .. %.2f Hz\n", lo, hi);
    CHECK(hi > lo);
    CHECK(hi / lo < 1.02);
}

TEST_CASE("BD: sleeps only once inaudible and wakes exactly", "[bd][sleep]") {
    for (double fs : kRates) {
        const double slept = checkQuietBeforeSleep<BassDrum>(prep, fs, knobs(0.5, 0.5, 1.0), Hit::accented(true, true, 1.0f), 0.05);
        CHECK(slept < 3.0);
        checkWakeIsFresh<BassDrum>(prep, fs, knobs(0.5, 0.5, 0.5), 0.3);
    }
}

TEST_CASE("BD: against recordings of the original machine", "[bd][reference]") {
    if (!haveRefs()) SKIP("reference recordings not present");
    const double fs = 48000.0;
    std::printf("[bd reference] sweep fits (TUNE 0..100): recording vs model\n");
    for (int t = 0; t <= 4; ++t) {
        Wav w;
        REQUIRE(loadRef("BassDrum/BassDrum909-tune" + pct(t * 25) + "-attack000-decay100.wav", w));
        const SweepFit r = sweep(w.x, w.fs), m = sweep(hit(fs, knobs(t * 0.25, 0, 1), Hit::normal(), 0.45), fs);
        std::printf("  TUNE %3d: fb %.2f / %.2f Hz, df %.0f / %.0f Hz, tau %.1f / %.1f ms\n", t * 25, r.fb, m.fb, r.df, m.df, r.tau * 1e3, m.tau * 1e3);
        CHECK_THAT(m.fb, WithinRel(r.fb, 0.006));
        CHECK_THAT(m.tau, WithinRel(r.tau, 0.05));
        CHECK_THAT(m.df, WithinRel(r.df, 0.08));
    }
    for (int d = 0; d <= 4; ++d) {
        double rt = 0.0;
        for (int t : {0, 50, 100}) {
            Wav w;
            REQUIRE(loadRef("BassDrum/BassDrum909-tune" + pct(t) + "-attack000-decay" + pct(d * 25) + ".wav", w));
            rt += decayTime(w.x, w.fs, -20.0) / 3.0;
        }
        const double mt = decayTime(fromOnset(hit(fs, knobs(0.5, 0, d * 0.25), Hit::normal(), 0.8)), fs, -20.0);
        std::printf("  DECAY %3d: t(-20 dB) %.0f / %.0f ms\n", d * 25, rt * 1e3, mt * 1e3);
        CHECK_THAT(mt, WithinRel(rt, 0.06));
    }
    Wav h;
    REQUIRE(loadRef("BassDrum/BassDrum909-tune050-attack000-decay100.wav", h));
    const auto hr = harmonicsDb(h.x, h.fs, 0.08, 0.16, 49.5, 7);
    const auto hm = harmonicsDb(fromOnset(hit(fs, knobs(0.5, 0, 1))), fs, 0.08, 0.16, 49.5, 7);
    std::printf("  H2..H7: recording %.1f %.1f %.1f %.1f %.1f %.1f, model %.1f %.1f %.1f %.1f %.1f %.1f dB\n", hr[0], hr[1], hr[2], hr[3], hr[4],
                hr[5], hm[0], hm[1], hm[2], hm[3], hm[4], hm[5]);
    CHECK_THAT(hm[0], WithinAbs(hr[0], 1.5));
    CHECK_THAT(hm[1], WithinAbs(hr[1], 1.5));
    CHECK_THAT(hm[3], WithinAbs(hr[3], 2.5));
    // TOTAL ACCENT curve (relative), body window.
    double r0 = 0.0, m0 = 0.0;
    for (int k = 0; k <= 10; k += 2) {
        Wav w;
        REQUIRE(loadRef("BassDrum/BassDrum909-accent" + pct(k * 10) + "-tune100-attack100-decay100.wav", w));
        const double r = toDb(vppWindow(w.x, w.fs, 0.01, 0.03));
        const double m = toDb(vppWindow(fromOnset(hit(fs, knobs(1, 1, 1), Hit::accented(false, true, static_cast<float>(k) * 0.1f), 0.4)), fs, 0.01, 0.03));
        if (k == 0) {
            r0 = r;
            m0 = m;
        }
        std::printf("  TOTAL ACCENT %3d %%: body %+.1f / %+.1f dB\n", k * 10, r - r0, m - m0);
        CHECK_THAT(m - m0, WithinAbs(r - r0, 1.8));
    }
}

TEST_CASE("BD: renders for listening", "[.render]") {
    const double fs = 48000.0;
    std::vector<float> all;
    for (const VoiceParams& p : {knobs(0.5, 0.5, 0.5), knobs(0, 0, 0), knobs(1, 1, 1), knobs(0.5, 0.5, 1)}) {
        for (const Hit& h : {Hit::normal(), Hit::accented(true, true, 1.0f), Hit::weak()}) {
            const auto x = hit(fs, p, h, 0.7);
            all.insert(all.end(), x.begin(), x.end());
        }
    }
    writeWav(renderDir() + "/bd.wav", all, fs, 0.1f);
    SUCCEED("wrote bd.wav");
}

TEST_CASE("BD: CPU cost per sample", "[bd][bench]") {
    const double ns = benchNsPerSample<BassDrum>(prep, 48000.0, knobs(0.5, 0.5, 1.0), 0.25, 4.0);
    std::printf("[bd bench] 48 kHz, re-hit every 250 ms: %.1f ns/sample\n", ns);
    checkCpu(ns, 400.0);  // ~70 ns on the development machine
}
