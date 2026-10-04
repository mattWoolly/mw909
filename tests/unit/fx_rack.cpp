// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// FxRack: placement and ordering, moves and reorders without allocation
// (operator new is counted), bus 1 -> bus 2 send pre/post, voice triggers
// and sidechain keys reaching the dynamics and filter, busActive/tail,
// NaN fuzz over the whole parameter block, determinism and block-partition
// independence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/FxBus.h"

// ---------------------------------------------------------------------------
// Allocation counter: every operator new in this executable is counted.
namespace {
std::atomic<long> gAllocs{0};
}
void* operator new(std::size_t n) {
    ++gAllocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    ++gAllocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr int kMaxBlock = 512;

struct Buses {
    std::vector<float> b[2][2];
    explicit Buses(int n) {
        for (auto& bus : b)
            for (auto& c : bus) c.assign(static_cast<std::size_t>(n), 0.0f);
    }
};

void runRack(FxRack& rack, Buses& io, int off, int n, const std::array<const float*, kNumVoices>& key = {}) {
    float* const ptr[kNumFxBuses][2] = {{io.b[0][0].data() + off, io.b[0][1].data() + off},
                                        {io.b[1][0].data() + off, io.b[1][1].data() + off}};
    std::array<const float*, kNumVoices> k = key;
    for (auto& p : k)
        if (p != nullptr) p += off;
    rack.process(ptr, k, n);
}

FxPlacement at(int bus, int order) { return FxPlacement{true, static_cast<std::uint8_t>(bus), static_cast<std::uint8_t>(order)}; }

}  // namespace

TEST_CASE("rack orders instances by key, ties by instance order", "[fx][rack]") {
    FxRack rack;
    rack.prepare(48000.0, kMaxBlock);
    FxParams p;
    p.place[static_cast<int>(FxId::Delay)] = at(0, 3);
    p.place[static_cast<int>(FxId::Reverb)] = at(0, 1);
    p.place[static_cast<int>(FxId::EqA)] = at(0, 1);
    p.place[static_cast<int>(FxId::Chorus)] = at(1, 0);
    p.place[static_cast<int>(FxId::Distortion)] = at(1, 5);
    rack.setParams(p, 120.0);
    std::array<std::uint8_t, kNumFxInstances> ids{};
    REQUIRE(rack.chain(0, ids) == 3);
    REQUIRE(ids[0] == static_cast<int>(FxId::EqA));
    REQUIRE(ids[1] == static_cast<int>(FxId::Reverb));
    REQUIRE(ids[2] == static_cast<int>(FxId::Delay));
    REQUIRE(rack.chain(1, ids) == 2);
    REQUIRE(ids[0] == static_cast<int>(FxId::Chorus));
    REQUIRE(ids[1] == static_cast<int>(FxId::Distortion));
}

TEST_CASE("rack processes the chain in order (matches standalone instances)", "[fx][rack]") {
    const double fs = 48000.0;
    FxParams p;
    p.place[static_cast<int>(FxId::Distortion)] = at(0, 2);
    p.place[static_cast<int>(FxId::EqA)] = at(0, 1);
    p.distortion.preampDb = 20.0f;
    p.distortion.thresholdDb = -12.0f;
    p.eq[0].mid[0] = {true, 1000.0f, 1.0f, 12.0f};
    FxRack rack;
    rack.prepare(fs, kMaxBlock);
    rack.setParams(p, 120.0);
    rack.reset();   // placements apply at once (no fade-in)
    const int n = 9600;
    Buses io(n);
    const auto x = bursts(n, 4800, fs);
    io.b[0][0] = x;
    io.b[0][1] = x;
    for (int off = 0; off < n; off += kMaxBlock) runRack(rack, io, off, std::min(kMaxBlock, n - off));
    // Reference: EQ then distortion, standalone.
    ParamEq eq;
    Distortion d;
    eq.prepare(fs);
    d.prepare(fs);
    eq.setParams(p.eq[0]);
    d.setParams(p.distortion);
    std::vector<float> l = x, r = x;
    for (int off = 0; off < n; off += kMaxBlock) {
        const int b = std::min(kMaxBlock, n - off);
        eq.process(l.data() + off, r.data() + off, b);
        d.process(l.data() + off, r.data() + off, b);
    }
    REQUIRE(io.b[0][0] == l);
    REQUIRE(io.b[0][1] == r);
}

TEST_CASE("placement, reorder and moves never allocate and never click", "[fx][rack]") {
    const double fs = 48000.0;
    FxRack rack;
    rack.prepare(fs, kMaxBlock);
    FxParams p;
    p.place[static_cast<int>(FxId::Reverb)] = at(0, 0);
    p.place[static_cast<int>(FxId::Delay)] = at(0, 1);
    p.delay.mix = 0.5f;
    p.reverb.mix = 0.3f;
    rack.setParams(p, 120.0);
    const int n = static_cast<int>(fs * 4);
    Buses io(n);
    const auto x = sine(220.0, fs, n, 0.3);
    io.b[0][0] = x;
    io.b[0][1] = x;
    io.b[1][0] = x;
    io.b[1][1] = x;
    std::array<float, kNumVoices> keyBuf{};
    std::array<const float*, kNumVoices> key{};
    (void)keyBuf;
    const long before = gAllocs.load();
    int k = 0;
    for (int off = 0; off < n; off += 256, ++k) {
        // Every 40 ms: move, reorder, toggle, swap buses.
        if (k % 8 == 0) {
            const int step = k / 8;
            for (int id = 0; id < kNumFxInstances; ++id) {
                auto& pl = p.place[static_cast<std::size_t>(id)];
                pl.on = ((id + step) % 3) != 0;
                pl.bus = static_cast<std::uint8_t>((id + step) % 2);
                pl.order = static_cast<std::uint8_t>((id * 7 + step * 3) % 10);
            }
            p.bus[0].toBus2 = (step % 2) ? 0.5f : 0.0f;
            p.bus[0].toBus2Pre = (step % 4) < 2;
        }
        rack.setParams(p, 120.0 + k % 7);
        if (k % 5 == 0) rack.noteTrigger(static_cast<VoiceId>(k % kNumVoices));
        runRack(rack, io, off, std::min(256, n - off), key);
    }
    const long after = gAllocs.load();
    REQUIRE(after == before);
    REQUIRE(allFinite(io.b[0][0]));
    REQUIRE(allFinite(io.b[1][1]));
    REQUIRE(peak(io.b[0][0].data(), io.b[0][0].size()) < 20.0);
}

TEST_CASE("switching an effect on fades it in", "[fx][rack]") {
    const double fs = 48000.0;
    FxRack rack;
    rack.prepare(fs, kMaxBlock);
    FxParams p;
    rack.setParams(p, 120.0);
    const int n = 4800;
    Buses io(n);
    const auto x = sine(300.0, fs, n, 0.5);
    io.b[0][0] = x;
    io.b[0][1] = x;
    runRack(rack, io, 0, 480);
    REQUIRE(std::equal(x.begin(), x.begin() + 480, io.b[0][0].begin()));   // no effects: unchanged
    p.place[static_cast<int>(FxId::Bitcrusher)] = at(0, 0);
    p.bitcrusher.bits = 2.0f;   // brutal: an instant switch would step
    rack.setParams(p, 120.0);
    runRack(rack, io, 480, n - 480);
    double maxStep = 0.0;
    for (int i = 481; i < 480 + 48; ++i)
        maxStep = std::max(maxStep, std::abs(double(io.b[0][0][static_cast<std::size_t>(i)]) - io.b[0][0][static_cast<std::size_t>(i - 1)]));
    REQUIRE(maxStep < 0.1);   // 1 ms into a 20 ms fade
}

TEST_CASE("bus 1 feeds bus 2 pre or post its effects", "[fx][rack]") {
    const double fs = 48000.0;
    for (bool pre : {true, false}) {
        FxRack rack;
        rack.prepare(fs, kMaxBlock);
        FxParams p;
        p.place[static_cast<int>(FxId::Delay)] = at(0, 0);
        p.delay.sync = false;
        p.delay.timeMs = 10.0f;
        p.delay.feedback = 0.0f;
        p.bus[0].toBus2 = 0.5f;
        p.bus[0].toBus2Pre = pre;
        rack.setParams(p, 120.0);
        rack.reset();
        Buses io(2400);
        io.b[0][0][0] = io.b[0][1][0] = 1.0f;
        runRack(rack, io, 0, 2400);
        // Bus 2 has no effects: its return is what it received.
        if (pre) {
            REQUIRE_THAT(io.b[1][0][0], WithinAbs(0.5, 1e-6));
            REQUIRE(std::abs(io.b[1][0][480]) < 1e-6);
        } else {
            REQUIRE(std::abs(io.b[1][0][0]) < 1e-6);
            REQUIRE_THAT(io.b[1][0][480], WithinAbs(0.5, 1e-4));
        }
        REQUIRE_THAT(io.b[0][0][480], WithinAbs(1.0, 1e-4));   // bus 1's own return: the echo
    }
}

TEST_CASE("voice triggers and sidechain keys reach the rack's dynamics", "[fx][rack]") {
    const double fs = 48000.0;
    FxRack rack;
    rack.prepare(fs, kMaxBlock);
    FxParams p;
    p.place[static_cast<int>(FxId::DynamicsA)] = at(1, 0);
    p.dynamics[0].mode = DynamicsMode::Vca;
    p.dynamics[0].vcaDepth = -1.0f;
    p.dynamics[0].attackMs = 1.0f;
    p.dynamics[0].holdMs = 30.0f;
    p.dynamics[0].triggers = {BD, kNoSource, kNoSource};
    p.place[static_cast<int>(FxId::DynamicsB)] = at(0, 0);
    p.dynamics[1].mode = DynamicsMode::Compressor;
    p.dynamics[1].sidechain = SD;
    p.dynamics[1].thresholdDb = -40.0f;
    p.dynamics[1].ratio = 20.0f;
    p.dynamics[1].attackMs = 1.0f;
    rack.setParams(p, 120.0);
    rack.reset();
    const int n = 4800;
    Buses io(n);
    for (int b = 0; b < 2; ++b)
        for (int c = 0; c < 2; ++c) std::fill(io.b[b][c].begin(), io.b[b][c].end(), 0.25f);
    std::vector<float> sdKey(static_cast<std::size_t>(n), 0.0f);
    for (int i = 2400; i < n; ++i) sdKey[static_cast<std::size_t>(i)] = 0.8f;
    std::array<const float*, kNumVoices> key{};
    key[SD] = sdKey.data();
    runRack(rack, io, 0, 1200, key);
    REQUIRE_THAT(io.b[1][0][1199], WithinAbs(0.25, 1e-6));   // bus 2 untouched before the hit
    rack.noteTrigger(BD);
    runRack(rack, io, 1200, n - 1200, key);
    REQUIRE(std::abs(io.b[1][0][1200 + 480]) < 1e-3);         // ducked by the BD trigger
    REQUIRE_THAT(io.b[0][0][2000], WithinAbs(0.25, 1e-6));   // key silent: no reduction
    REQUIRE(io.b[0][0][4700] < 0.25 * 0.1);                   // SD key up: heavy reduction
}

TEST_CASE("busActive and tails follow the effects", "[fx][rack]") {
    const double fs = 48000.0;
    FxRack rack;
    rack.prepare(fs, kMaxBlock);
    FxParams p;
    p.place[static_cast<int>(FxId::Reverb)] = at(1, 0);
    p.reverb.decay = 0.0f;   // 0.2 s
    rack.setParams(p, 120.0);
    rack.reset();
    REQUIRE_FALSE(rack.busActive(0));
    REQUIRE_FALSE(rack.busActive(1));
    REQUIRE(rack.tailSeconds(1) > 0.4);
    Buses io(static_cast<int>(fs * 2));
    io.b[1][0][0] = io.b[1][1][0] = 1.0f;
    runRack(rack, io, 0, 4800);
    REQUIRE(rack.busActive(1));
    REQUIRE_FALSE(rack.busActive(0));
    for (int off = 4800; off < static_cast<int>(fs * 2); off += kMaxBlock)
        runRack(rack, io, off, std::min(kMaxBlock, static_cast<int>(fs * 2) - off));
    REQUIRE_FALSE(rack.busActive(1));
}

TEST_CASE("rack NaN/garbage fuzz, determinism and partition independence", "[fx][rack]") {
    const double fs = 44100.0;
    {
        FxRack rack;
        rack.prepare(fs, kMaxBlock);
        Buses io(44100);
        const auto x = noise(44100, 0.5f);
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c) io.b[b][c] = x;
        const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};
        TestRng rng{17};
        for (int k = 0; k < 80; ++k) {
            FxParams p;
            const float v = bad[k % 5];
            for (auto& pl : p.place) {
                pl.on = rng.next() > 0.0f;
                pl.bus = static_cast<std::uint8_t>((rng.next() * 0.5f + 0.5f) * 255.0f);
                pl.order = static_cast<std::uint8_t>((rng.next() * 0.5f + 0.5f) * 255.0f);
            }
            p.bus[0].toBus2 = v;
            p.distortion = DistortionParams{v, v, v, v, v, v, v};
            p.reverb = ReverbParams{v, v, v, v, v, v, v, v, v, v, v};
            p.delay.timeMs = p.delay.feedback = p.delay.mix = v;
            p.chorus = ChorusParams{v, v, v, v, v, v, v, v, v, v};
            p.bitcrusher.bits = p.bitcrusher.rateHz = v;
            p.dynamics[0].thresholdDb = p.dynamics[0].ratio = v;
            p.dynamics[0].sidechain = static_cast<std::uint8_t>(k * 13);
            p.filter.cutoffHz = p.filter.modAmount = v;
            rack.setParams(p, static_cast<double>(v));
            rack.noteTrigger(static_cast<VoiceId>(k % kNumVoices));
            runRack(rack, io, k * 551, 551);
        }
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 2; ++c) REQUIRE(allFinite(io.b[b][c]));
    }
    FxParams p;
    p.place[static_cast<int>(FxId::Reverb)] = at(1, 2);
    p.place[static_cast<int>(FxId::Delay)] = at(0, 1);
    p.place[static_cast<int>(FxId::Chorus)] = at(0, 0);
    p.place[static_cast<int>(FxId::Filter)] = at(1, 0);
    p.place[static_cast<int>(FxId::DynamicsA)] = at(1, 1);
    p.dynamics[0].sidechain = BD;
    p.filter.lfoWave = LfoWave::Triangle;
    p.filter.modAmount = 0.3f;
    p.bus[0].toBus2 = 0.3f;
    const int n = static_cast<int>(fs * 2);
    const auto x = bursts(n, 11025, fs);
    const auto kick = bursts(n, 22050, fs, 0.8f, 5u);
    auto render = [&](std::uint32_t seed) {
        FxRack rack;
        rack.prepare(fs, 1024);
        rack.setParams(p, 128.0);
        Buses io(n);
        io.b[0][0] = x;
        io.b[0][1] = x;
        io.b[1][0] = kick;
        io.b[1][1] = x;
        std::array<const float*, kNumVoices> key{};
        key[BD] = kick.data();
        int off = 0;
        for (int b : partition(n, seed, 1024)) {
            rack.setParams(p, 128.0);
            runRack(rack, io, off, b, key);
            off += b;
        }
        return io.b[1][0];
    };
    const auto a = render(1), b = render(2), c = render(1);
    REQUIRE(a == c);
    REQUIRE(a == b);
}
