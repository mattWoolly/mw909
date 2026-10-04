// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The smaller effects: distortion, parametric EQ, chorus, bitcrusher,
// dynamics (compressor with external key, VCA ducking by voice triggers) and
// the filter effect (LFO and envelope). Responses against analytic values,
// behaviour, NaN/Inf fuzz, determinism, partition independence, sleep.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/Bitcrusher.h"
#include "mw909/fx/Chorus.h"
#include "mw909/fx/Distortion.h"
#include "mw909/fx/Dynamics.h"
#include "mw909/fx/FilterFx.h"
#include "mw909/fx/ParamEq.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

struct Stereo {
    std::vector<float> l, r;
};

template <typename Fx>
Stereo run(Fx& fx, const std::vector<float>& x, int block = 512) {
    Stereo s{x, x};
    for (int i = 0; i < static_cast<int>(x.size()); i += block) {
        const int b = std::min(block, static_cast<int>(x.size()) - i);
        fx.process(s.l.data() + i, s.r.data() + i, b);
    }
    return s;
}

const float kBad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};

// Generic partition-independence check.
template <typename Fx, typename Setup>
void checkPartition(double fs, Setup setup, const std::vector<float>& x) {
    auto render = [&](std::uint32_t seed) {
        Fx fx;
        fx.prepare(fs);
        setup(fx);
        Stereo s{x, x};
        for (auto& v : s.r) v *= -0.7f;
        int off = 0;
        for (int b : partition(static_cast<int>(x.size()), seed)) {
            setup(fx);
            fx.process(s.l.data() + off, s.r.data() + off, b);
            off += b;
        }
        return s;
    };
    const auto a = render(7), b = render(8), c = render(7);
    REQUIRE(a.l == c.l);
    REQUIRE(a.l == b.l);
    REQUIRE(a.r == b.r);
}

}  // namespace

// ---------------------------------------------------------------------------
TEST_CASE("distortion: clean below threshold, clips above, low band untouched", "[fx][distortion]") {
    for (double fs : kRates) {
        Distortion d;
        d.prepare(fs);
        DistortionParams p;
        p.preampDb = 0.0f;
        p.thresholdDb = -6.0f;
        p.knee = 0.0f;
        p.splitHz = 0.0f;   // full band
        d.setParams(p);
        // Quiet sine: passes at unity (oversampling pass-band flat).
        auto s = run(d, sine(1000.0, fs, static_cast<int>(fs * 0.2), 0.1));
        const std::size_t tail = static_cast<std::size_t>(fs * 0.1);
        REQUIRE_THAT(rms(s.l.data() + tail, tail), WithinRel(0.1 / std::sqrt(2.0), 0.005));
        // Loud sine: the output peak is held at the threshold.
        Distortion d2;
        d2.prepare(fs);
        d2.setParams(p);
        auto s2 = run(d2, sine(200.0, fs, static_cast<int>(fs * 0.2), 0.9));
        INFO("fs " << fs);
        REQUIRE_THAT(peak(s2.l.data() + tail, tail), WithinAbs(std::pow(10.0, -6.0 / 20.0), 0.03));
    }
    // With a 1 kHz split, a 100 Hz tone keeps its level while 5 kHz is clipped.
    const double fs = 48000.0;
    Distortion d;
    d.prepare(fs);
    DistortionParams p;
    p.preampDb = 24.0f;
    p.thresholdDb = -20.0f;
    p.splitHz = 1000.0f;
    p.clipOutDb = 0.0f;
    d.setParams(p);
    auto lo = run(d, sine(100.0, fs, 24000, 0.5));
    REQUIRE_THAT(rms(lo.l.data() + 12000, 12000), WithinRel(0.5 / std::sqrt(2.0), 0.02));
    Distortion d3;
    d3.prepare(fs);
    d3.setParams(p);
    auto hi = run(d3, sine(5000.0, fs, 24000, 0.5));
    // Clipped at 0.1: at most a square wave's RMS (0.1), against 0.354 unclipped.
    REQUIRE(rms(hi.l.data() + 12000, 12000) < 0.105);
}

TEST_CASE("distortion aliasing stays low (oversampled)", "[fx][distortion]") {
    const double fs = 44100.0;
    Distortion d;
    d.prepare(fs);
    DistortionParams p;
    p.preampDb = 30.0f;
    p.thresholdDb = -12.0f;
    p.knee = 0.0f;
    p.splitHz = 0.0f;
    d.setParams(p);
    const double f0 = 4410.0 * 1.13;   // 4983 Hz: harmonics fold to non-harmonic bins
    auto s = run(d, sine(f0, fs, 44100, 0.5));
    std::vector<float> seg(s.l.begin() + 22050, s.l.end());
    // Hann window.
    for (std::size_t i = 0; i < seg.size(); ++i)
        seg[i] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(seg.size() - 1)));
    const double fund = std::abs(dftAt(seg, f0, fs));
    // The 5th harmonic (24.9 kHz) would alias to 19.2 kHz; the 7th (34.9 kHz) to 9.2 kHz.
    const double a5 = std::abs(dftAt(seg, fs - 5.0 * f0, fs));
    const double a7 = std::abs(dftAt(seg, fs - 7.0 * f0, fs));
    INFO("a5 " << 20 * std::log10(a5 / fund) << " a7 " << 20 * std::log10(a7 / fund));
    REQUIRE(20.0 * std::log10(a7 / fund) < -40.0);
    REQUIRE(20.0 * std::log10(a5 / fund) < -30.0);
}

TEST_CASE("distortion fuzz, determinism, partition", "[fx][distortion]") {
    Distortion d;
    d.prepare(48000.0);
    auto x = noise(48000, 0.8f);
    std::vector<float> l = x, r = x;
    for (int k = 0; k < 100; ++k) {
        const float v = kBad[k % 5];
        d.setParams(DistortionParams{v, v, v, v, v, v, v});
        d.process(l.data() + k * 480, r.data() + k * 480, 480);
    }
    REQUIRE(allFinite(l));
    DistortionParams p;
    p.preampDb = 18.0f;
    p.thresholdDb = -10.0f;
    p.mix = 0.6f;
    checkPartition<Distortion>(96000.0, [&](Distortion& f) { f.setParams(p); }, bursts(96000, 20000, 96000.0));
}

// ---------------------------------------------------------------------------
TEST_CASE("parametric EQ matches the prototypes (cut and shelf ends)", "[fx][eq]") {
    for (double fs : kRates) {
        ParamEq eq;
        eq.prepare(fs);
        ParamEqParams p;
        p.lowShelf = true;
        p.lowHz = 150.0f;
        p.lowGainDb = 8.0f;
        p.mid[0] = {true, 700.0f, 2.0f, -10.0f};
        p.mid[1] = {true, 3000.0f, 1.0f, 6.0f};
        p.highShelf = false;
        p.highHz = 9000.0f;
        p.highRes = 0.4f;
        eq.setParams(p);
        const int n = static_cast<int>(fs * 0.5);
        std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r = l;
        l[0] = r[0] = 1.0f;
        eq.process(l.data(), r.data(), n);
        const double qc = resonanceToQ(0.4);
        for (double f : {50.0, 150.0, 700.0, 3000.0, 9000.0, 14000.0}) {
            const cplx ref = protoLowShelf(warpedS(f, 150.0, fs), 0.70710678, 8.0) *
                             protoBell(warpedS(f, 700.0, fs), octavesToQ(2.0), -10.0) *
                             protoBell(warpedS(f, 3000.0, fs), octavesToQ(1.0), 6.0) *
                             protoLp(warpedS(f, 9000.0, fs), qc);
            INFO("fs " << fs << " f " << f);
            REQUIRE_THAT(magDb(dftAt(l, f, fs)), WithinAbs(magDb(ref), 0.05));
            REQUIRE_THAT(magDb(dftAt(r, f, fs)), WithinAbs(magDb(ref), 0.05));
        }
    }
}

TEST_CASE("parametric EQ: flat settings are an exact pass-through", "[fx][eq]") {
    ParamEq eq;
    eq.prepare(48000.0);
    eq.setParams(ParamEqParams{});
    auto x = noise(4096);
    std::vector<float> l = x, r = x;
    eq.process(l.data(), r.data(), 4096);
    REQUIRE(l == x);
    REQUIRE_FALSE(eq.isActive());
    ParamEq e2;
    e2.prepare(48000.0);
    auto ok = [&](ParamEq& f) {
        ParamEqParams p;
        p.lowHz = 300.0f;
        p.lowRes = 0.7f;
        p.mid[0] = {true, 1000.0f, 1.0f, 12.0f};
        f.setParams(p);
    };
    ok(e2);
    std::vector<float> a = x, b = x;
    for (int k = 0; k < 100; ++k) {
        const float v = kBad[k % 5];
        ParamEqParams p;
        p.lowShelf = (k & 1) != 0;
        p.lowHz = v;
        p.lowGainDb = v;
        p.lowRes = v;
        p.mid[0] = {true, v, v, v};
        p.highShelf = (k & 2) != 0;
        p.highHz = v;
        p.highGainDb = v;
        p.highRes = v;
        e2.setParams(p);
        e2.process(a.data() + k * 40, b.data() + k * 40, 40);
    }
    REQUIRE(allFinite(a));
    checkPartition<ParamEq>(44100.0, ok, bursts(44100, 9000, 44100.0));
}

// ---------------------------------------------------------------------------
TEST_CASE("chorus: modulated delay around the offset, stereo spread", "[fx][chorus]") {
    const double fs = 48000.0;
    Chorus c;
    c.prepare(fs);
    ChorusParams p;
    p.offsetMs = 10.0f;
    p.depth = 0.0f;   // no modulation: a plain 10 ms echo
    p.mix = 1.0f;
    c.setParams(p);
    std::vector<float> l(4800, 0.0f), r(4800, 0.0f);
    l[0] = r[0] = 1.0f;
    c.process(l.data(), r.data(), 4800);
    REQUIRE_THAT(l[480], WithinAbs(1.0, 1e-4));
    REQUIRE(std::abs(l[470]) < 1e-6);
    // With depth and spatial = 1 the two sides differ (LFOs 180 degrees apart).
    Chorus c2;
    c2.prepare(fs);
    p.depth = 1.0f;
    p.spatial = 1.0f;
    p.rateHz = 2.0f;
    c2.setParams(p);
    auto s = run(c2, sine(500.0, fs, 48000, 0.5));
    double diff = 0.0;
    for (std::size_t i = 24000; i < 48000; ++i) diff = std::max(diff, std::abs(double(s.l[i]) - s.r[i]));
    REQUIRE(diff > 0.2);
    REQUIRE(allFinite(s.l));
    // Fuzz.
    std::vector<float> a = sine(300.0, fs, 48000, 0.5), b = a;
    for (int k = 0; k < 100; ++k) {
        const float v = kBad[k % 5];
        c2.setParams(ChorusParams{v, v, v, v, v, v, v, v, v, v});
        c2.process(a.data() + k * 480, b.data() + k * 480, 480);
    }
    REQUIRE(allFinite(a));
    REQUIRE(allFinite(b));
    ChorusParams q;
    q.feedback = 0.6f;
    q.fatness = 0.7f;
    q.lowCutHz = 200.0f;
    q.highCutHz = 8000.0f;
    checkPartition<Chorus>(fs, [&](Chorus& f) { f.setParams(q); }, bursts(96000, 20000, fs));
}

// ---------------------------------------------------------------------------
TEST_CASE("bitcrusher: hold rate and bit depth", "[fx][bitcrusher]") {
    for (double fs : kRates) {
        for (float rate : {44.0f, 1000.0f, 8000.0f, 30000.0f}) {
            Bitcrusher b;
            b.prepare(fs);
            BitcrusherParams p;
            p.bits = 24.0f;
            p.rateHz = rate;
            b.setParams(p);
            // A ramp: every hold produces a new value; count the changes over 1 s.
            const int n = static_cast<int>(fs);
            std::vector<float> l(static_cast<std::size_t>(n)), r;
            for (int i = 0; i < n; ++i) l[static_cast<std::size_t>(i)] = 0.1f + 0.8f * static_cast<float>(i) / static_cast<float>(n);
            r = l;
            b.process(l.data(), r.data(), n);
            int changes = 0;
            for (int i = 1; i < n; ++i) changes += l[static_cast<std::size_t>(i)] != l[static_cast<std::size_t>(i - 1)];
            const double expect = std::min(static_cast<double>(rate), fs);
            INFO("fs " << fs << " rate " << rate << " changes " << changes);
            REQUIRE_THAT(static_cast<double>(changes), WithinAbs(expect, 2.0));
        }
    }
    // Bits: a slow sine at 3 bits has at most 2^3 + 1 distinct levels on the
    // mid-tread grid of step 2^-2.
    Bitcrusher b;
    b.prepare(48000.0);
    BitcrusherParams p;
    p.bits = 3.0f;
    b.setParams(p);
    auto s = run(b, sine(50.0, 48000.0, 48000, 0.99));
    std::set<float> levels(s.l.begin(), s.l.end());
    REQUIRE(levels.size() <= 9);
    for (float v : levels) REQUIRE_THAT(v / 0.25f, WithinAbs(std::round(v / 0.25f), 1e-6));
    // Images filter smooths the steps: high-frequency content drops.
    Bitcrusher b2, b3;
    b2.prepare(48000.0);
    b3.prepare(48000.0);
    BitcrusherParams q;
    q.rateHz = 4000.0f;
    b2.setParams(q);
    q.imagesFilter = 0.8f;
    b3.setParams(q);
    auto raw = run(b2, sine(300.0, 48000.0, 24000, 0.5));
    auto smooth = run(b3, sine(300.0, 48000.0, 24000, 0.5));
    const double imgRaw = std::abs(dftAt(raw.l, 4000.0 - 300.0, 48000.0));
    const double imgSmooth = std::abs(dftAt(smooth.l, 4000.0 - 300.0, 48000.0));
    REQUIRE(imgSmooth < imgRaw * 0.25);
}

TEST_CASE("bitcrusher jitter, dither, fuzz, determinism", "[fx][bitcrusher]") {
    BitcrusherParams p;
    p.bits = 8.0f;
    p.dither = true;
    p.rateHz = 11025.0f;
    p.jitter = 0.7f;
    p.approxFilter = 0.6f;
    p.imagesFilter = 0.5f;
    p.mix = 0.8f;
    checkPartition<Bitcrusher>(48000.0, [&](Bitcrusher& f) { f.setParams(p); }, bursts(96000, 20000, 48000.0));
    Bitcrusher b;
    b.prepare(48000.0);
    auto x = noise(48000, 0.5f);
    std::vector<float> l = x, r = x;
    for (int k = 0; k < 100; ++k) {
        const float v = kBad[k % 5];
        b.setParams(BitcrusherParams{v, (k & 1) != 0, v, v, v, v, v});
        b.process(l.data() + k * 480, r.data() + k * 480, 480);
    }
    REQUIRE(allFinite(l));
}

// ---------------------------------------------------------------------------
TEST_CASE("dynamics: external sidechain key ducks the bus", "[fx][dynamics]") {
    const double fs = 48000.0;
    Dynamics d;
    d.prepare(fs);
    DynamicsParams p;
    p.thresholdDb = -30.0f;
    p.ratio = 20.0f;
    p.attackMs = 1.0f;
    p.releaseMs = 50.0f;
    p.sidechain = BD;
    d.setParams(p);
    const int n = 48000;
    auto x = sine(200.0, fs, n, 0.1);   // bus content, -20 dBFS, below threshold? no: -20 > -30
    std::vector<float> key(static_cast<std::size_t>(n), 0.0f);
    for (int i = 12000; i < 24000; ++i) key[static_cast<std::size_t>(i)] = 0.5f;   // a loud key
    std::vector<float> l = x, r = x;
    // The bus itself is ignored as key: without the key, no gain change.
    d.process(l.data(), r.data(), 12000, key.data());
    REQUIRE_THAT(rms(l.data() + 6000, 6000), WithinRel(rms(x.data() + 6000, 6000), 1e-6));
    d.process(l.data() + 12000, r.data() + 12000, n - 12000, key.data() + 12000);
    // While the key is up: about -26 dB of gain reduction (0.5 -> -6 dB, 24 dB over at 20:1).
    const double ducked = rms(l.data() + 18000, 4000) / rms(x.data() + 18000, 4000);
    REQUIRE_THAT(20.0 * std::log10(ducked), WithinAbs(-24.0 * (1.0 - 1.0 / 20.0), 0.5));
    // After release it is back (25 dB * e^(-t/50 ms) of reduction left: 0.02 dB at 0.37 s).
    REQUIRE_THAT(rms(l.data() + 42000, 6000), WithinRel(rms(x.data() + 42000, 6000), 5e-3));
}

TEST_CASE("dynamics: VCA ducking fired by up to three voices", "[fx][dynamics]") {
    const double fs = 48000.0;
    for (float depth : {-1.0f, -0.5f, 1.0f}) {
        Dynamics d;
        d.prepare(fs);
        DynamicsParams p;
        p.mode = DynamicsMode::Vca;
        p.attackMs = 2.0f;
        p.holdMs = 20.0f;
        p.releaseMs = 100.0f;
        p.vcaDepth = depth;
        p.triggers = {BD, CP, kNoSource};
        d.setParams(p);
        std::vector<float> l(9600, 0.5f), r = l;
        d.process(l.data(), r.data(), 480);
        const float before = l[479];
        d.noteTrigger(SD);   // not a source
        d.process(l.data() + 480, r.data() + 480, 480);
        REQUIRE(l[959] == before);
        d.noteTrigger(CP);
        d.process(l.data() + 960, r.data() + 960, 9600 - 960);
        // After attack (2 ms = 96 samples) the gain sits at the hold value.
        const double held = l[960 + 96 + 480] / 0.5;
        const double expectHeld = depth < 0.0f ? 1.0 + depth : 1.0;
        const double expectRest = depth < 0.0f ? 1.0 : 1.0 - depth;
        INFO("depth " << depth);
        REQUIRE_THAT(held, WithinAbs(expectHeld, 1e-3));
        REQUIRE_THAT(before / 0.5, WithinAbs(expectRest, 1e-3));
        // Release reaches -60 dB of the envelope at 100 ms: back to rest.
        REQUIRE_THAT(l[960 + 96 + 960 + 4800 + 200] / 0.5, WithinAbs(expectRest, 2e-3));
    }
}

TEST_CASE("dynamics: VCA envelope runs while the bus is silent", "[fx][dynamics]") {
    // A trigger during silence must still have ducked by the time audio arrives.
    const double fs = 48000.0;
    Dynamics d;
    d.prepare(fs);
    DynamicsParams p;
    p.mode = DynamicsMode::Vca;
    p.attackMs = 1.0f;
    p.holdMs = 50.0f;
    p.vcaDepth = -1.0f;
    p.triggers = {BD, kNoSource, kNoSource};
    d.setParams(p);
    d.noteTrigger(BD);
    std::vector<float> z(480, 0.0f), z2 = z;
    d.process(z.data(), z2.data(), 480);   // 10 ms of silence
    std::vector<float> l(480, 0.5f), r = l;
    d.process(l.data(), r.data(), 480);
    REQUIRE(std::abs(l[100]) < 1e-3);   // fully ducked
    REQUIRE(d.isActive());
}

TEST_CASE("dynamics fuzz and partition independence", "[fx][dynamics]") {
    Dynamics d;
    d.prepare(44100.0);
    auto x = noise(44100, 0.5f);
    std::vector<float> l = x, r = x;
    for (int k = 0; k < 100; ++k) {
        const float v = kBad[k % 5];
        DynamicsParams p{static_cast<DynamicsMode>(k % 3), v, v, v, v, v, v, static_cast<std::uint8_t>(k * 37), {}, v, v};
        d.setParams(p);
        d.noteTrigger(k % 11);
        d.process(l.data() + k * 441, r.data() + k * 441, 441);
    }
    REQUIRE(allFinite(l));
    DynamicsParams p;
    p.thresholdDb = -24.0f;
    p.ratio = 6.0f;
    p.holdMs = 10.0f;
    p.makeupDb = 6.0f;
    p.mix = 0.8f;
    checkPartition<Dynamics>(44100.0, [&](Dynamics& f) { f.setParams(p); }, bursts(88200, 11025, 44100.0));
}

// ---------------------------------------------------------------------------
TEST_CASE("filter fx: static response matches the SVF prototype", "[fx][filterfx]") {
    for (double fs : kRates) {
        for (auto type : {FilterType::LowPass, FilterType::BandPass, FilterType::HighPass}) {
            FilterFx f;
            f.prepare(fs);
            FilterFxParams p;
            p.type = type;
            p.cutoffHz = 1200.0f;
            p.reso = 0.5f;
            p.modAmount = 0.0f;
            f.setParams(p, 120.0);
            const int n = static_cast<int>(fs * 0.3);
            std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r = l;
            l[0] = r[0] = 1.0f;
            f.process(l.data(), r.data(), n);
            const double q = resonanceToQ(0.5, 16.0);
            for (double fr : {100.0, 600.0, 1200.0, 2400.0, 8000.0}) {
                const cplx s = warpedS(fr, 1200.0, fs);
                cplx ref;
                if (type == FilterType::LowPass) ref = protoLp(s, q);
                else if (type == FilterType::HighPass) ref = protoHp(s, q);
                else ref = (s / q) / (s * s + s / q + 1.0);
                INFO("fs " << fs << " type " << int(type) << " f " << fr);
                REQUIRE_THAT(magDb(dftAt(l, fr, fs)), WithinAbs(magDb(ref), 0.05));
            }
        }
    }
}

TEST_CASE("filter fx: LFO sweeps the cutoff at its rate, envelope on triggers", "[fx][filterfx]") {
    const double fs = 48000.0;
    FilterFx f;
    f.prepare(fs);
    FilterFxParams p;
    p.type = FilterType::LowPass;
    p.cutoffHz = 1000.0f;
    p.lfoRateHz = 4.0f;
    p.lfoWave = LfoWave::Triangle;
    p.lfoSpread = 1.0f;
    p.modAmount = 0.25f;   // +-1 octave
    f.setParams(p, 120.0);
    auto x = noise(48000, 0.3f);
    std::vector<float> l = x, r = x;
    double lo = 1e9, hi = 0.0;
    for (int i = 0; i < 48000; i += 64) {
        f.process(l.data() + i, r.data() + i, 64);
        lo = std::min(lo, f.cutoffHz(0));
        hi = std::max(hi, f.cutoffHz(0));
        // Right is 180 degrees away: its cutoff mirrors the left in octaves.
        REQUIRE_THAT(std::log2(f.cutoffHz(0) / 1000.0) + std::log2(f.cutoffHz(1) / 1000.0), WithinAbs(0.0, 0.02));
    }
    REQUIRE_THAT(lo, WithinRel(500.0, 0.02));
    REQUIRE_THAT(hi, WithinRel(2000.0, 0.02));
    // Synced LFO: 1/4 at 120 BPM = 2 Hz.
    FilterFx g;
    g.prepare(fs);
    p.lfoSync = true;
    p.lfoDiv = SyncDiv::D1_4;
    p.lfoWave = LfoWave::Saw;
    p.lfoSpread = 0.0f;
    g.setParams(p, 120.0);
    auto x2 = noise(57600, 0.3f);
    std::vector<float> a = x2, b = x2;
    int wraps = 0;
    double prev = 0.0;
    bool falling = false;
    for (int i = 0; i < 57600; i += 32) {   // 1.2 s: wraps at 0.5 s and 1.0 s
        g.process(a.data() + i, b.data() + i, 32);
        const double c = g.cutoffHz(0);
        // A wrap: a big drop after a rise (the 1 ms edge rounding spans a few steps).
        if (i > 0 && c < prev * 0.8 && !falling) ++wraps;
        falling = i > 0 && c < prev;
        prev = c;
    }
    REQUIRE(wraps == 2);
    // Envelope: fired by RS only.
    FilterFx e;
    e.prepare(fs);
    FilterFxParams q;
    q.type = FilterType::LowPass;
    q.cutoffHz = 300.0f;
    q.source = ModSource::Envelope;
    q.envAttackMs = 1.0f;
    q.envHoldMs = 10.0f;
    q.envReleaseMs = 50.0f;
    q.envTriggers = {RS, kNoSource, kNoSource};
    q.modAmount = 1.0f;   // +4 octaves at the peak
    e.setParams(q, 120.0);
    std::vector<float> m = x, n2 = x;
    e.process(m.data(), n2.data(), 480);
    REQUIRE_THAT(e.cutoffHz(0), WithinRel(300.0, 1e-3));
    e.noteTrigger(SD);
    e.process(m.data() + 480, n2.data() + 480, 480);
    REQUIRE_THAT(e.cutoffHz(0), WithinRel(300.0, 1e-3));
    e.noteTrigger(RS);
    e.process(m.data() + 960, n2.data() + 960, 480);   // inside attack+hold
    REQUIRE_THAT(e.cutoffHz(0), WithinRel(4800.0, 0.01));
    e.process(m.data() + 1440, n2.data() + 1440, 9600);
    REQUIRE_THAT(e.cutoffHz(0), WithinRel(300.0, 0.01));
}

TEST_CASE("filter fx fuzz, type switching, partition independence", "[fx][filterfx]") {
    FilterFx f;
    f.prepare(96000.0);
    auto x = noise(96000, 0.5f);
    std::vector<float> l = x, r = x;
    for (int k = 0; k < 200; ++k) {
        const float v = kBad[k % 5];
        FilterFxParams p;
        p.type = static_cast<FilterType>(k % 6);
        p.cutoffHz = v;
        p.reso = v;
        p.source = static_cast<ModSource>(k % 3);
        p.lfoSync = (k & 4) != 0;
        p.lfoRateHz = v;
        p.lfoDiv = static_cast<SyncDiv>(k % 8);
        p.lfoWave = static_cast<LfoWave>(k % 7);
        p.lfoSpread = v;
        p.envAttackMs = p.envHoldMs = p.envReleaseMs = v;
        p.modAmount = v;
        p.mix = v;
        f.setParams(p, static_cast<double>(v));
        f.noteTrigger(k % 11);
        f.process(l.data() + k * 480, r.data() + k * 480, 480);
    }
    REQUIRE(allFinite(l));
    REQUIRE(allFinite(r));
    FilterFxParams p;
    p.type = FilterType::BandPass;
    p.reso = 0.8f;
    p.lfoWave = LfoWave::Random;
    p.lfoRateHz = 7.0f;
    p.lfoSpread = 0.3f;
    p.modAmount = -0.4f;
    p.mix = 0.9f;
    checkPartition<FilterFx>(48000.0, [&](FilterFx& g) { g.setParams(p, 120.0); }, noise(96000, 0.4f, 3u));
}
