// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Master chain: output stage responses (Original's 130 Hz coupling high-pass
// and the Late revision's 6.6 kHz low-pass), level-compensated drive, the
// 3-band compressor's flat crossover sum and solo, limiter ceiling and timing,
// soft clip, NaN fuzz, determinism and block-partition independence; the
// chain at rest (isActive false) skips silence with byte-identical output.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/MasterChain.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

MasterParams neutral() {
    MasterParams p;
    p.stage = OutputStage::Off;
    p.compOn = false;
    p.limiterOn = false;
    p.softClip = false;
    p.volumeDb = 0.0f;
    return p;
}

std::vector<float> impulse(MasterChain& m, int n, float amp = 1.0e-3f, std::vector<float>* right = nullptr) {
    std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r = l;
    l[0] = r[0] = amp;
    m.process(l.data(), r.data(), n);
    for (auto& v : l) v /= amp;
    for (auto& v : r) v /= amp;
    if (right != nullptr) *right = r;
    return l;
}

// First-order bilinear prototypes.
cplx hp1(double f, double fc, double fs) {
    const cplx s = warpedS(f, fc, fs);
    return s / (s + 1.0);
}
cplx lp1(double f, double fc, double fs) {
    const cplx s = warpedS(f, fc, fs);
    return 1.0 / (s + 1.0);
}

}  // namespace

TEST_CASE("master with everything off is an exact pass-through", "[fx][master]") {
    MasterChain m;
    m.prepare(48000.0, 512);
    m.setParams(neutral(), Revision::Late);
    auto x = noise(4096, 0.9f);
    std::vector<float> l = x, r = x;
    m.process(l.data(), r.data(), 4096);
    REQUIRE(l == x);
    REQUIRE(r == x);
}

TEST_CASE("output stage: Original high-pass 130 Hz, Late low-pass 6.6 kHz; Modern 10 Hz", "[fx][master]") {
    for (double fs : kRates) {
        for (auto rev : {Revision::Late, Revision::Early}) {
            MasterChain m;
            m.prepare(fs, 512);
            auto p = neutral();
            p.stage = OutputStage::Original;
            m.setParams(p, rev);
            const auto h = impulse(m, static_cast<int>(fs * 0.5));
            for (double f : {30.0, 130.0, 500.0, 2000.0, 6570.0, 15000.0}) {
                cplx ref = hp1(f, 130.0, fs);
                if (rev == Revision::Late) ref *= lp1(f, 6570.0, fs);
                INFO("fs " << fs << " rev " << int(rev) << " f " << f);
                REQUIRE_THAT(magDb(dftAt(h, f, fs)), WithinAbs(magDb(ref), 0.02));
            }
        }
        MasterChain m;
        m.prepare(fs, 512);
        auto p = neutral();
        p.stage = OutputStage::Modern;
        m.setParams(p, Revision::Late);
        const auto h = impulse(m, static_cast<int>(fs * 1.0));
        for (double f : {10.0, 40.0, 1000.0, 15000.0})
            REQUIRE_THAT(magDb(dftAt(h, f, fs)), WithinAbs(magDb(hp1(f, 10.0, fs)), 0.02));
    }
}

TEST_CASE("output stage drive is level-compensated and saturates loud signals", "[fx][master]") {
    const double fs = 48000.0;
    for (float drive : {0.0f, 0.5f, 1.0f}) {
        MasterChain m;
        m.prepare(fs, 512);
        auto p = neutral();
        p.stage = OutputStage::Modern;
        p.drive = drive;
        m.setParams(p, Revision::Late);
        // Quiet: -40 dBFS sine keeps its level (the op-amp is linear there even at +24 dB).
        auto q = sine(1000.0, fs, 24000, 0.01);
        std::vector<float> l = q, r = q;
        m.process(l.data(), r.data(), 24000);
        REQUIRE_THAT(rms(l.data() + 12000, 12000), WithinRel(rms(q.data() + 12000, 12000), 2e-3));
        // Loud: 0 dBFS sine is squashed by the drive.
        MasterChain m2;
        m2.prepare(fs, 512);
        m2.setParams(p, Revision::Late);
        auto x = sine(1000.0, fs, 24000, 1.0);
        std::vector<float> a = x, b = x;
        m2.process(a.data(), b.data(), 24000);
        const double pk = peak(a.data() + 12000, 12000);
        INFO("drive " << drive << " peak " << pk);
        // Pushed +12/+24 dB into the +6 dB rail, then brought back down by the same amount.
        const double pre = std::pow(10.0, drive * 24.0 / 20.0);
        if (drive == 0.0f) REQUIRE_THAT(pk, WithinAbs(1.0, 2e-3));
        else REQUIRE_THAT(pk, WithinRel(MasterChain::kRail / pre, 0.02));
    }
}

TEST_CASE("3-band crossover sums flat when the bands are neutral", "[fx][master]") {
    for (double fs : kRates) {
        for (auto [lo, hi] : {std::pair{200.0f, 3000.0f}, std::pair{40.0f, 12000.0f}, std::pair{900.0f, 1100.0f}}) {
            MasterChain m;
            m.prepare(fs, 512);
            auto p = neutral();
            p.compOn = true;
            p.xoverLowHz = lo;
            p.xoverHighHz = hi;
            m.setParams(p, Revision::Late);
            const auto h = impulse(m, static_cast<int>(fs * 0.5));
            for (double f = 20.0; f < 20000.0; f *= 1.19) {
                INFO("fs " << fs << " xover " << lo << "/" << hi << " f " << f);
                REQUIRE_THAT(magDb(dftAt(h, f, fs)), WithinAbs(0.0, 0.01));
            }
        }
    }
}

TEST_CASE("3-band solo isolates a band", "[fx][master]") {
    const double fs = 48000.0;
    MasterChain m;
    m.prepare(fs, 512);
    auto p = neutral();
    p.compOn = true;
    p.xoverLowHz = 200.0f;
    p.xoverHighHz = 3000.0f;
    p.band[1].solo = true;
    m.setParams(p, Revision::Late);
    const auto h = impulse(m, 24000);
    REQUIRE(magDb(dftAt(h, 30.0, fs)) < -30.0);
    REQUIRE(magDb(dftAt(h, 20000.0, fs)) < -30.0);
    REQUIRE_THAT(magDb(dftAt(h, 775.0, fs)), WithinAbs(0.0, 1.0));
}

TEST_CASE("3-band compressor compresses only the loud band", "[fx][master]") {
    const double fs = 48000.0;
    MasterChain m;
    m.prepare(fs, 512);
    auto p = neutral();
    p.compOn = true;
    p.band[0].thresholdDb = -30.0f;
    p.band[0].ratio = 10.0f;
    p.band[0].attackMs = 1.0f;
    m.setParams(p, Revision::Late);
    const int n = 48000;
    auto lo = sine(60.0, fs, n, 0.5);
    auto hi = sine(8000.0, fs, n, 0.1);
    std::vector<float> l(static_cast<std::size_t>(n)), r;
    for (int i = 0; i < n; ++i) l[static_cast<std::size_t>(i)] = lo[static_cast<std::size_t>(i)] + hi[static_cast<std::size_t>(i)];
    r = l;
    m.process(l.data(), r.data(), n);
    std::vector<float> seg(l.begin() + 24000, l.end());
    const double aLo = 2.0 * std::abs(dftAt(seg, 60.0, fs)) / static_cast<double>(seg.size());
    const double aHi = 2.0 * std::abs(dftAt(seg, 8000.0, fs)) / static_cast<double>(seg.size());
    REQUIRE(aLo < 0.5 * 0.3);                   // squashed by > 10 dB
    REQUIRE_THAT(aHi, WithinRel(0.1, 0.02));     // untouched
}

TEST_CASE("limiter holds its ceiling; soft clip caps at 0 dBFS", "[fx][master]") {
    for (double fs : kRates) {
        for (float thr : {0.0f, -6.0f, -18.0f}) {
            for (float att : {0.1f, 1.0f, 10.0f}) {
                MasterChain m;
                m.prepare(fs, 512);
                auto p = neutral();
                p.limiterOn = true;
                p.limThresholdDb = thr;
                p.limAttackMs = att;
                m.setParams(p, Revision::Late);
                auto x = bursts(static_cast<int>(fs), static_cast<int>(fs * 0.1), fs, 4.0f);   // +12 dBFS hits
                std::vector<float> l = x, r = x;
                m.process(l.data(), r.data(), static_cast<int>(l.size()));
                const double ceiling = std::pow(10.0, thr / 20.0);
                INFO("fs " << fs << " thr " << thr << " att " << att);
                REQUIRE(peak(l.data(), l.size()) <= ceiling * 1.0001);
            }
        }
    }
    // Release: after a loud hit the gain recovers with the release time constant.
    const double fs = 48000.0;
    MasterChain m;
    m.prepare(fs, 512);
    auto p = neutral();
    p.limiterOn = true;
    p.limThresholdDb = -6.0f;
    p.limReleaseMs = 100.0f;
    m.setParams(p, Revision::Late);
    std::vector<float> l(48000, 0.01f), r = l;
    for (int i = 0; i < 480; ++i) l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = 2.0f;
    m.process(l.data(), r.data(), 48000);
    const double grAfter = -m.limiterReductionDb();
    REQUIRE(grAfter > -0.01);
    const double g100 = l[480 + 4800] / 0.01;   // one release time constant after the hit
    const double gEnd = 0.5012 / 2.0;           // gain at the end of the hit (thr/peak)
    REQUIRE_THAT(g100, WithinAbs(1.0 - (1.0 - gEnd) * std::exp(-1.0), 0.01));
    // Soft clip.
    MasterChain c;
    c.prepare(fs, 512);
    auto q = neutral();
    q.softClip = true;
    c.setParams(q, Revision::Late);
    auto big = sine(100.0, fs, 4800, 4.0);
    std::vector<float> a = big, b = big;
    c.process(a.data(), b.data(), 4800);
    REQUIRE(peak(a.data(), a.size()) <= 1.0);
    auto small = sine(100.0, fs, 4800, 0.8);
    std::vector<float> s1 = small, s2 = small;
    c.process(s1.data(), s2.data(), 4800);
    REQUIRE(s1 == small);   // linear below -1.5 dBFS
}

TEST_CASE("master volume and switching are smooth", "[fx][master]") {
    const double fs = 48000.0;
    MasterChain m;
    m.prepare(fs, 512);
    MasterParams p;
    m.setParams(p, Revision::Late);
    auto x = sine(150.0, fs, 48000, 0.3);
    std::vector<float> l = x, r = x;
    for (int i = 0, k = 0; i < 48000; i += 480, ++k) {
        p.stage = static_cast<OutputStage>(k % 3);
        p.compOn = (k % 2) == 0;
        p.limiterOn = (k % 4) < 2;
        p.volumeDb = (k % 2) ? -12.0f : 0.0f;
        m.setParams(p, (k % 5) ? Revision::Late : Revision::Early);
        m.process(l.data() + i, r.data() + i, 480);
    }
    double maxStep = 0.0;
    for (std::size_t i = 1; i < l.size(); ++i) maxStep = std::max(maxStep, std::abs(double(l[i]) - l[i - 1]));
    REQUIRE(maxStep < 2.0 * kPi * 150.0 / fs * 0.3 * 1.5);
}

TEST_CASE("master NaN fuzz, determinism, partition independence", "[fx][master]") {
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};
    {
        MasterChain m;
        m.prepare(44100.0, 512);
        auto x = noise(44100, 0.9f);
        std::vector<float> l = x, r = x;
        for (int k = 0; k < 100; ++k) {
            const float v = bad[k % 5];
            MasterParams p;
            p.stage = static_cast<OutputStage>(k % 5);
            p.drive = v;
            p.compOn = true;
            p.xoverLowHz = v;
            p.xoverHighHz = v;
            for (auto& b : p.band) b = CompBand{true, (k & 1) != 0, v, v, v, v, v};
            p.limThresholdDb = p.limAttackMs = p.limReleaseMs = v;
            p.volumeDb = v;
            m.setParams(p, static_cast<Revision>(k % 3));
            m.process(l.data() + k * 441, r.data() + k * 441, 441);
        }
        REQUIRE(allFinite(l));
        REQUIRE(allFinite(r));
    }
    MasterParams p;
    p.stage = OutputStage::Original;
    p.drive = 0.4f;
    p.compOn = true;
    p.band[0].thresholdDb = -20.0f;
    p.band[2].thresholdDb = -30.0f;
    p.limThresholdDb = -3.0f;
    const auto x = bursts(96000, 12000, 48000.0, 1.5f);
    auto render = [&](std::uint32_t seed) {
        MasterChain m;
        m.prepare(48000.0, 700);
        m.setParams(p, Revision::Late);
        std::vector<float> l = x, r = x;
        int off = 0;
        for (int b : partition(static_cast<int>(l.size()), seed)) {
            m.setParams(p, Revision::Late);
            m.process(l.data() + off, r.data() + off, b);
            off += b;
        }
        return l;
    };
    const auto a = render(1), b = render(2), c = render(1);
    REQUIRE(a == c);
    REQUIRE(a == b);
}

TEST_CASE("master at rest: silence is skipped, output byte-identical for every partition", "[fx][master][partition]") {
    std::vector<MasterParams> configs;
    configs.push_back(MasterParams{});   // the default: Modern stage, limiter, soft clip
    {
        MasterParams p;
        p.stage = OutputStage::Original;
        p.drive = 0.7f;
        p.compOn = true;
        for (auto& b : p.band) {
            b.on = true;
            b.thresholdDb = -30.0f;
            b.makeupDb = 4.0f;
        }
        p.limThresholdDb = -6.0f;
        configs.push_back(p);
    }
    {
        MasterParams p = neutral();
        p.volumeDb = -12.0f;
        configs.push_back(p);
    }
    {
        MasterParams p;
        p.compOn = true;
        p.band[1].on = true;
        p.band[1].solo = true;
        p.band[1].thresholdDb = -40.0f;
        p.band[2].on = true;
        configs.push_back(p);
    }
    for (const double fs : {44100.0, 48000.0, 96000.0}) {
        const int total = static_cast<int>(fs * 7.0);
        const int burst = static_cast<int>(fs * 0.1);
        const int restCheck = static_cast<int>(fs * 3.0);    // 2.9 s after the burst
        const int tinyAt = restCheck + 100;                   // a -400 dBFS sample, then a -0
        const int changeAt = static_cast<int>(fs * 3.5);      // parameters change at rest
        const int burst2 = static_cast<int>(fs * 4.0);
        std::vector<float> inL(static_cast<std::size_t>(total), 0.0f), inR = inL;
        const auto nz = noise(burst, 1.5f, 99u);
        for (int i = 0; i < burst; ++i) {
            const auto k = static_cast<std::size_t>(i);
            inL[k] = nz[k];
            inR[k] = -0.7f * nz[k];
            inL[static_cast<std::size_t>(burst2) + k] = 0.5f * nz[k];
            inR[static_cast<std::size_t>(burst2) + k] = 0.5f * nz[k];
        }
        inL[static_cast<std::size_t>(tinyAt)] = 1.0e-20f;
        inR[static_cast<std::size_t>(tinyAt) + 1] = -0.0f;
        for (int i = tinyAt + 2; i < tinyAt + 500; ++i) inL[static_cast<std::size_t>(i)] = -0.0f;

        for (std::size_t c = 0; c < configs.size(); ++c) {
            MasterParams changed = configs[c];
            changed.volumeDb -= 6.0f;
            changed.compOn = !changed.compOn;
            // mode 0: process() in 64s; 1: process() in random blocks; 2: the
            // engine's way (zero-fill + skipSilence() at rest) in random blocks.
            auto render = [&](int mode, std::uint32_t seed, bool* restedAtCheck, int* restedBlocks) {
                MasterChain m;
                m.prepare(fs, 4096);
                m.setParams(configs[c], Revision::Late);
                std::vector<float> l = inL, r = inR;
                std::vector<int> sizes = mode == 0 ? std::vector<int>{} : partition(total, seed, 4096);
                std::size_t next = 0;
                for (int pos = 0; pos < total;) {
                    int n = mode == 0 ? 64 : sizes[next++ % sizes.size()];
                    n = std::min(n, total - pos);
                    for (int cut : {restCheck, changeAt})
                        if (pos < cut && pos + n > cut) n = cut - pos;
                    if (pos == restCheck && restedAtCheck != nullptr) *restedAtCheck = !m.isActive();
                    if (pos == changeAt) m.setParams(changed, Revision::Late);
                    float* bl = l.data() + pos;
                    float* br = r.data() + pos;
                    bool silent = true;
                    for (int i = 0; i < n; ++i) silent = silent && bl[i] == 0.0f && br[i] == 0.0f;
                    if (silent && !m.isActive() && restedBlocks != nullptr) ++*restedBlocks;
                    if (mode == 2 && silent && !m.isActive()) {
                        std::fill_n(bl, n, 0.0f);
                        std::fill_n(br, n, 0.0f);
                        m.skipSilence(n);
                    } else {
                        m.process(bl, br, n);
                    }
                    pos += n;
                }
                l.insert(l.end(), r.begin(), r.end());
                return l;
            };
            bool rested = false;
            int restedBlocks = 0;
            const auto ref = render(0, 0, &rested, &restedBlocks);
            INFO("fs " << fs << " config " << c);
            CHECK(rested);   // the burst's tails (limiter, compressors, filters) have died away exactly
            CHECK(restedBlocks > 0);
            REQUIRE(allFinite(ref));
            REQUIRE(peak(ref.data() + burst2, static_cast<std::size_t>(burst)) > 0.01);   // woke up again
            for (std::uint32_t seed : {1u, 2u, 3u}) {
                for (int mode : {1, 2}) {
                    const auto out = render(mode, seed, nullptr, nullptr);
                    INFO("mode " << mode << " seed " << seed);
                    REQUIRE(out.size() == ref.size());
                    REQUIRE(std::memcmp(out.data(), ref.data(), sizeof(float) * ref.size()) == 0);
                }
            }
        }
    }
}

TEST_CASE("master at rest: a band switched out keeps releasing, so the chain can rest", "[fx][master]") {
    const double fs = 48000.0;
    MasterChain m;
    m.prepare(fs, 4096);
    MasterParams p;
    p.compOn = true;
    p.band[0].on = true;
    p.band[0].thresholdDb = -40.0f;
    p.band[0].releaseMs = 500.0f;
    m.setParams(p, Revision::Late);
    REQUIRE_FALSE(m.isActive());   // freshly prepared: at rest
    auto x = sine(60.0, fs, 4800, 0.9);
    std::vector<float> r = x;
    m.process(x.data(), r.data(), 4800);
    REQUIRE(m.isActive());
    REQUIRE(m.bandReductionDb(0) > 10.0f);
    // Switch the band out in the middle of its reduction: its detector keeps
    // tracking the (now silent) band and releases, so the chain rests.
    p.band[0].on = false;
    m.setParams(p, Revision::Late);
    std::vector<float> zl(static_cast<std::size_t>(fs * 12.0), 0.0f), zr = zl;
    m.process(zl.data(), zr.data(), static_cast<int>(zl.size()));
    CHECK(m.bandReductionDb(0) == 0.0f);
    CHECK_FALSE(m.isActive());
    // At rest, silence in is +0 out (a -0 included).
    std::vector<float> s(256, -0.0f), t(256, 0.0f);
    m.process(s.data(), t.data(), 256);
    for (float v : s) REQUIRE(!std::signbit(v));
    CHECK_FALSE(m.isActive());
}

TEST_CASE("master: a compressing band switched out and back in does not overshoot", "[fx][master]") {
    // Heavy reduction with a lot of make-up on a sustained low tone: if the
    // band came back with no reduction, the make-up would jump the level by
    // about 16 dB until the slow attack caught up.
    const double fs = 48000.0;
    for (const bool limiter : {false, true}) {
        MasterChain m;
        m.prepare(fs, 512);
        MasterParams p = neutral();
        p.compOn = true;
        p.limiterOn = limiter;
        p.softClip = limiter;
        for (auto& b : p.band) b.on = false;
        p.band[0].on = true;
        p.band[0].thresholdDb = -30.0f;
        p.band[0].ratio = 10.0f;
        p.band[0].attackMs = 100.0f;
        p.band[0].releaseMs = 300.0f;
        p.band[0].makeupDb = 18.0f;
        m.setParams(p, Revision::Late);
        const int total = static_cast<int>(fs * 2.0);
        auto l = sine(80.0, fs, total, 0.5);
        auto r = l;
        for (int pos = 0; pos < total; pos += 64) {
            if (pos == 48000) p.band[0].on = false;          // out at 1.0 s
            if (pos == 48000 + 4800) p.band[0].on = true;    // back in at 1.1 s
            m.setParams(p, Revision::Late);
            m.process(l.data() + pos, r.data() + pos, 64);
        }
        const double before = peak(l.data() + 38400, 9600);     // 0.8..1.0 s
        const double after = peak(l.data() + 52800, 24000);     // 1.1..1.6 s
        INFO("limiter " << limiter << ": before " << 20.0 * std::log10(before) << " dBFS, after "
                        << 20.0 * std::log10(after) << " dBFS");
        CHECK(after < before * 1.06);   // within 0.5 dB
    }
}

TEST_CASE("master: random switching across rest is byte-identical for every partition", "[fx][master][partition]") {
    // Every section switched on and off, in and out of rest, between bursts of
    // sound and long silences (including a deep, slow limiter release that
    // outlasts everything else). The chain is rendered in 64-sample blocks,
    // in random blocks, and the engine's way (zero-fill + skipSilence() at
    // rest); all three must be the same bytes.
    struct Event {
        int pos;
        MasterParams p;
        Revision rev;
    };
    for (const double fs : {44100.0, 96000.0}) {
        for (const std::uint32_t seed : {11u, 12u, 13u}) {
            TestRng g{seed * 2654435761u + static_cast<std::uint32_t>(fs)};
            auto u = [&g] { return 0.5f * g.next() + 0.5f; };   // 0..1
            auto pick = [&u](int k) { return std::min(k - 1, static_cast<int>(u() * static_cast<float>(k))); };
            const int total = static_cast<int>(fs * 24.0);
            std::vector<float> inL(static_cast<std::size_t>(total), 0.0f), inR = inL;
            std::vector<Event> events;
            MasterParams p;
            p.compOn = true;
            p.limThresholdDb = -12.0f;
            p.limReleaseMs = 1000.0f;
            Revision rev = Revision::Late;
            events.push_back({0, p, rev});
            int pos = 0;
            while (pos < total) {
                const int kind = pick(6);
                int len = static_cast<int>(fs * (kind < 2 ? 0.2 + 3.0 * u() : 0.01 + 0.3 * u()));
                len = std::min(len, total - pos);
                const float amp = std::pow(10.0f, -3.0f + 3.5f * u());
                for (int i = 0; i < len; ++i) {
                    const auto k = static_cast<std::size_t>(pos + i);
                    switch (kind) {
                        case 2: inL[k] = amp * g.next(); inR[k] = amp * g.next(); break;
                        case 3: inL[k] = inR[k] = amp * static_cast<float>(std::sin(2.0 * kPi * 60.0 * i / fs)); break;
                        case 4: inL[k] = -0.0f; inR[k] = (i % 977 == 0) ? 1.0e-40f : 0.0f; break;
                        case 5: inL[k] = i == len / 2 ? amp : 0.0f; break;
                        default: break;   // silence
                    }
                }
                pos += len;
                // A switch somewhere in this stretch (often during silence).
                switch (pick(10)) {
                    case 0: p.stage = static_cast<OutputStage>(pick(3)); break;
                    case 1: rev = rev == Revision::Late ? Revision::Early : Revision::Late; break;
                    case 2: p.compOn = !p.compOn; break;
                    case 3: p.band[static_cast<std::size_t>(pick(3))].on ^= true; break;
                    case 4: p.band[static_cast<std::size_t>(pick(3))].solo ^= true; break;
                    case 5: p.limiterOn = !p.limiterOn; break;
                    case 6: p.softClip = !p.softClip; break;
                    case 7: p.volumeDb = -20.0f * u(); break;
                    case 8:
                        p.xoverLowHz = 40.0f + 900.0f * u();
                        p.xoverHighHz = 1000.0f + 11000.0f * u();
                        break;
                    default: p.limReleaseMs = 10.0f + 990.0f * u(); break;
                }
                events.push_back({std::max(0, pos - static_cast<int>(u() * static_cast<float>(len))), p, rev});
            }
            std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.pos < b.pos; });

            auto render = [&](int mode, std::uint32_t pseed, int* skipped) {
                MasterChain m;
                m.prepare(fs, 4096);
                std::vector<float> l = inL, r = inR;
                const auto sizes = mode == 0 ? std::vector<int>{} : partition(total, pseed, 4096);
                std::size_t next = 0, ev = 0;
                for (int at = 0; at < total;) {
                    while (ev < events.size() && events[ev].pos <= at) {
                        m.setParams(events[ev].p, events[ev].rev);
                        ++ev;
                    }
                    int n = std::min(mode == 0 ? 64 : sizes[next++ % sizes.size()], total - at);
                    if (ev < events.size() && events[ev].pos < at + n) n = events[ev].pos - at;
                    float* bl = l.data() + at;
                    float* br = r.data() + at;
                    bool silent = true;
                    for (int i = 0; i < n; ++i) silent = silent && bl[i] == 0.0f && br[i] == 0.0f;
                    if (mode == 2 && silent && !m.isActive()) {
                        std::fill_n(bl, n, 0.0f);
                        std::fill_n(br, n, 0.0f);
                        m.skipSilence(n);
                        if (skipped != nullptr) ++*skipped;
                    } else {
                        m.process(bl, br, n);
                    }
                    at += n;
                }
                l.insert(l.end(), r.begin(), r.end());
                return l;
            };
            INFO("fs " << fs << " seed " << seed);
            const auto ref = render(0, 0, nullptr);
            REQUIRE(allFinite(ref));
            int skipped = 0;
            for (const std::uint32_t pseed : {1u, 2u}) {
                for (const int mode : {1, 2}) {
                    INFO("mode " << mode << " partition " << pseed);
                    const auto out = render(mode, pseed, mode == 2 ? &skipped : nullptr);
                    REQUIRE(std::memcmp(out.data(), ref.data(), sizeof(float) * ref.size()) == 0);
                }
            }
            CHECK(skipped > 0);   // the chain did come to rest
        }
    }
}
