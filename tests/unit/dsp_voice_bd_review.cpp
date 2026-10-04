// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Independent review checks for the bass drum (BD). Self-contained: no shared
// measurement helpers. Checks tagged [!mayfail] document defects found in
// review; they report but do not fail the run until the voice is fixed.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <vector>

#include "mw909/dsp/Noise.h"
#include "mw909/voices/BassDrum.h"

namespace {
std::atomic<long> gAllocs{0};
std::atomic<bool> gCount{false};
}  // namespace

void* operator new(std::size_t n) {
    if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n != 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using mw909::Hit;
using mw909::VoiceParams;
using mw909::VoiceSetup;
using mw909::voices::BassDrum;

namespace {

float hostile(std::mt19937_64& r) {
    constexpr float c[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), 1.0e-40f, -1.0e-40f, 1.0e30f, -1.0e30f, 0.0f,
                           std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    return c[r() % (sizeof(c) / sizeof(c[0]))];
}

double uni(std::mt19937_64& r) { return std::uniform_real_distribution<double>(0.0, 1.0)(r); }

// Renders hits at fixed sample positions; blocks are random (or fixed) but are
// always cut at the hits and at every `paramEvery` samples, where the knob set
// changes deterministically.
std::vector<float> renderPartitioned(double fs, long n, const std::vector<std::pair<long, Hit>>& hits, std::uint64_t blockSeed,
                                     long paramEvery) {
    static BassDrum v;
    VoiceSetup s;
    s.sampleRate = fs;
    s.unitSeed = 99U;
    s.variance = 0.7f;
    v.prepare(s);
    mw909::dsp::WhiteNoise bus;
    bus.prepare(fs);
    bus.seed(4242U);
    std::vector<float> noise(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
    bus.fill(noise.data(), static_cast<int>(n));
    std::mt19937_64 br(blockSeed);
    long i = 0;
    std::size_t h = 0;
    while (i < n) {
        while (h < hits.size() && hits[h].first <= i) v.trigger(hits[h++].second);
        long len = blockSeed == 0 ? 8192 : 1 + static_cast<long>(br() % 8192U);
        if (h < hits.size()) len = std::min(len, hits[h].first - i);
        len = std::min({len, n - i, (i / paramEvery + 1) * paramEvery - i});
        VoiceParams p = mw909::defaultParams(mw909::BD);
        const long seg = i / paramEvery;
        p.k[mw909::knob::BdAttack] = static_cast<float>(seg % 5) * 0.25f;
        p.k[mw909::knob::BdDrive] = static_cast<float>(seg % 3) * 0.5f;
        p.k[mw909::knob::BdDecay] = static_cast<float>(seg % 4) * 0.33f;
        v.setParams(p);
        v.render(out.data() + i, noise.data() + i, static_cast<int>(len));
        i += len;
    }
    return out;
}

}  // namespace

TEST_CASE("BD review: prepare, setParams, trigger and render never allocate", "[bd][review]") {
    static BassDrum v;
    static float out[4096], noise[4096];
    std::mt19937_64 r(5);
    mw909::dsp::WhiteNoise bus;
    gAllocs = 0;
    for (double fs : {22050.0, 44100.0, 96000.0, 384000.0}) {
        VoiceSetup s;
        s.sampleRate = fs;
        s.variance = 1.0f;
        s.unitSeed = static_cast<std::uint32_t>(r());
        bus.prepare(fs);
        gCount = true;
        v.prepare(s);
        for (int b = 0; b < 200; ++b) {
            VoiceParams p = mw909::defaultParams(mw909::BD);
            for (auto& k : p.k) k = static_cast<float>(uni(r) * 2.0 - 0.5);
            v.setParams(p);
            if (b % 7 == 0) v.trigger(Hit::fromCode(static_cast<int>(r() % 64U)));
            gCount = false;
            bus.fill(noise, 4096);
            gCount = true;
            v.render(out, noise, 1 + static_cast<int>(r() % 4096U));
        }
        gCount = false;
    }
    CHECK(gAllocs.load() == 0);
}

TEST_CASE("BD review: hostile controls at 22.05..384 kHz with random blocks stay finite and bounded", "[bd][review][fuzz]") {
    static BassDrum v;
    static float out[8192], noise[8192];
    std::mt19937_64 r(77);
    mw909::dsp::WhiteNoise bus;
    double worst = 0.0;
    long bad = 0;
    for (double fs : {22050.0, 32000.0, 44100.0, 88200.0, 176400.0, 384000.0}) {
        VoiceSetup s;
        s.sampleRate = fs;
        s.variance = static_cast<float>(uni(r));
        s.unitSeed = static_cast<std::uint32_t>(r());
        s.revision = (r() & 1U) != 0U ? mw909::Revision::Early : mw909::Revision::Late;
        v.prepare(s);
        bus.prepare(fs);
        long done = 0;
        while (done < static_cast<long>(3.0 * fs)) {
            VoiceParams p{};
            for (auto& k : p.k) k = uni(r) < 0.15 ? hostile(r) : static_cast<float>(uni(r) * 30.0 - 15.0);
            v.setParams(p);
            for (int t = static_cast<int>(r() % 4U); t > 0; --t) {
                Hit h;
                h.acc = uni(r) < 0.2 ? hostile(r) : static_cast<float>(uni(r) * 7.0 - 1.0);
                h.pitchSemis = uni(r) < 0.2 ? hostile(r) : static_cast<float>(uni(r) * 120.0 - 60.0);
                v.trigger(h);
            }
            if (r() % 9U == 0U) v.setAccLine(hostile(r));
            const int n = 1 + static_cast<int>(r() % 8192U);
            bus.fill(noise, n);
            v.render(out, noise, n);
            for (int i = 0; i < n; ++i) {
                if (!std::isfinite(out[i])) ++bad;
                else worst = std::max(worst, static_cast<double>(std::abs(out[i])));
            }
            done += n;
        }
    }
    INFO("worst |y| " << worst << " V");
    CHECK(bad == 0);
    CHECK(worst < 15.0);
}

TEST_CASE("BD review: random partitions with knob changes at fixed samples are bit-identical", "[bd][review][determinism]") {
    for (double fs : {22050.0, 48000.0, 384000.0}) {
        const long n = static_cast<long>(3.0 * fs);
        std::vector<std::pair<long, Hit>> hits;
        std::mt19937_64 hr(7);
        for (int k = 0; k < 30; ++k) hits.push_back({static_cast<long>(hr() % static_cast<std::uint64_t>(n)), Hit::fromCode(static_cast<int>(hr() % 64U))});
        hits.push_back({1000, Hit::normal()});
        hits.push_back({1001, Hit::normal()});
        hits.push_back({1003, Hit::weak()});
        std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const auto ref = renderPartitioned(fs, n, hits, 0, 4800);
        for (std::uint64_t seed : {1U, 2U}) {
            const auto x = renderPartitioned(fs, n, hits, seed, 4800);
            INFO("fs " << fs << " block seed " << seed);
            CHECK(std::memcmp(ref.data(), x.data(), ref.size() * sizeof(float)) == 0);
        }
    }
}

// Review finding: moving the Shaper knob (an automatable extension) rebuilds
// the diode table in setParams (~1k Newton solves, ~100 us per call on the
// review machine) and switches the static curve in one sample.
TEST_CASE("BD review: Shaper automation is click-free", "[bd][review][!mayfail]") {
    static BassDrum v;
    VoiceSetup s;
    s.sampleRate = 48000.0;
    v.prepare(s);
    VoiceParams p = mw909::defaultParams(mw909::BD);
    p.k[mw909::knob::BdDecay] = 1.0f;
    p.k[mw909::knob::BdDrive] = 0.0f;
    v.setParams(p);
    std::vector<float> out(9600), noise(9600, 0.0f);
    v.trigger(Hit::normal());
    const int change = 2512;  // ~52 ms, inside the hold
    double before = 0.0, at = 0.0;
    float prev = 0.0f;
    for (int i = 0; i < 9600; i += 16) {
        if (i == change) {
            p.k[mw909::knob::BdDrive] = 1.0f;
            v.setParams(p);
        }
        v.render(out.data() + i, noise.data() + i, 16);
        for (int q = i; q < i + 16; ++q) {
            const double st = std::abs(static_cast<double>(out[static_cast<std::size_t>(q)]) - prev);
            prev = out[static_cast<std::size_t>(q)];
            if (q > 480 && q < change) before = std::max(before, st);
            if (q == change) at = st;
        }
    }
    INFO("step at the Shaper change " << at << " V; largest step 10..52 ms before " << before << " V");
    CHECK(at < 4.0 * before);
}

TEST_CASE("BD review: setParams with a moving Shaper stays cheap", "[bd][review][bench][!mayfail]") {
    static BassDrum v;
    VoiceSetup s;
    v.prepare(s);
    VoiceParams p = mw909::defaultParams(mw909::BD);
    constexpr int kCalls = 400;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; ++i) {
        p.k[mw909::knob::BdDrive] = (i & 1) != 0 ? 0.5f : 0.51f;
        v.setParams(p);
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kCalls;
    INFO("setParams with the Shaper moving: " << us << " us per call (64 samples at 48 kHz = 1333 us)");
    CHECK(us < 10.0);
}
