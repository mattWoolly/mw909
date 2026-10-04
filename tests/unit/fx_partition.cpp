// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Block-partition independence WITH parameter changes: every FX block is fed
// the same input, the same parameter schedule (random parameters, sections and
// placements switched on and off, reorders, triggers) at the same sample
// positions, once in fixed 64-sample blocks and once in random blocks of 1 to
// 4096 samples cut at the events. The outputs must be bit-identical.
// Also the behaviour behind the review fixes: a reorder keeps the instance's
// state, a switch-on into silence is fully wet at once, synced delay times
// beyond the line halve, non-finite audio cannot poison the rack or the
// master, the default master chain is linear to -0.5 dBFS, the trigger click
// of the early revision.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/Bitcrusher.h"
#include "mw909/fx/ChannelStrip.h"
#include "mw909/fx/Chorus.h"
#include "mw909/fx/Delay.h"
#include "mw909/fx/Distortion.h"
#include "mw909/fx/Dynamics.h"
#include "mw909/fx/FilterFx.h"
#include "mw909/fx/FxBus.h"
#include "mw909/fx/MasterChain.h"
#include "mw909/fx/ParamEq.h"
#include "mw909/fx/Reverb.h"
#include "mw909/fx/TriggerOut.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;

namespace {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t x) : s(x * 0x9E3779B97F4A7C15ull + 7u) {}
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uni() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    int range(int lo, int hi) { return lo + static_cast<int>(uni() * (hi - lo + 1)); }
    bool coin(double p = 0.5) { return uni() < p; }
    // Mostly in range, sometimes an end, rarely garbage.
    float f(float lo, float hi) {
        const double u = uni();
        if (u < 0.85) return static_cast<float>(lo + (hi - lo) * uni());
        if (u < 0.97) return coin() ? lo : hi;
        return coin() ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    }
    template <typename E>
    E e(int count) {
        return static_cast<E>(range(0, count - 1));
    }
    std::uint8_t voice() { return coin(0.8) ? static_cast<std::uint8_t>(range(0, kNumVoices - 1)) : kNoSource; }
};

StripParams randStrip(Rng& g) {
    StripParams p;
    p.eqOn = g.coin(0.6);
    p.lowCutHz = g.coin() ? 20.0f : g.f(20.0f, 2000.0f);
    p.highCutHz = g.coin() ? 20000.0f : g.f(500.0f, 20000.0f);
    p.resonance = g.f(0.0f, 1.0f);
    for (auto& b : p.band) b = {g.coin(), g.f(20.0f, 20000.0f), g.f(0.5f, 4.0f), g.f(-24.0f, 24.0f)};
    p.compOn = g.coin(0.4);
    p.compPreEq = g.coin();
    p.compThresholdDb = g.f(-48.0f, 0.0f);
    p.compRatio = g.f(1.0f, 20.0f);
    p.compAttackMs = g.f(1.0f, 500.0f);
    p.compReleaseMs = g.f(1.0f, 500.0f);
    p.compMakeupDb = g.f(0.0f, 24.0f);
    return p;
}

FxParams randFx(Rng& g) {
    FxParams p;
    for (auto& pl : p.place)
        pl = {g.coin(0.5), static_cast<std::uint8_t>(g.range(0, 1)), static_cast<std::uint8_t>(g.range(0, 9))};
    p.bus[0].toBus2 = g.coin() ? 0.0f : g.f(0.0f, 1.0f);
    p.bus[0].toBus2Pre = g.coin();
    auto& d = p.distortion;
    d = {g.f(0.0f, 36.0f), g.f(-36.0f, 0.0f), g.f(0.0f, 1.0f), g.f(0.0f, 2000.0f), g.f(-60.0f, 12.0f), g.f(-60.0f, 12.0f), g.f(0.0f, 1.0f)};
    for (auto& q : p.eq) {
        q.lowShelf = g.coin();
        q.lowHz = g.f(20.0f, 2000.0f);
        q.lowGainDb = g.f(-24.0f, 24.0f);
        q.lowRes = g.f(0.0f, 1.0f);
        for (auto& m : q.mid) m = {g.coin(0.3), g.f(20.0f, 20000.0f), g.f(0.5f, 4.0f), g.f(-24.0f, 24.0f)};
        q.highShelf = g.coin();
        q.highHz = g.coin() ? 20000.0f : g.f(500.0f, 20000.0f);
        q.highGainDb = g.f(-24.0f, 24.0f);
        q.highRes = g.f(0.0f, 1.0f);
    }
    auto& y = p.delay;
    y.sync = g.coin();
    y.timeMs = g.f(1.0f, 1000.0f);
    y.div = g.e<SyncDiv>(6);
    y.kind = g.e<SyncKind>(3);
    y.feedback = g.f(0.0f, 0.98f);
    y.spread = g.f(-1.0f, 1.0f);
    y.loopFilter = g.e<FilterType>(4);
    y.cutoffHz = g.f(20.0f, 20000.0f);
    y.reso = g.f(0.0f, 1.0f);
    y.mix = g.f(0.0f, 1.0f);
    auto& v = p.reverb;
    v = {g.f(0.0f, 250.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f),
         g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f)};
    auto& c = p.chorus;
    c = {g.f(1.0f, 30.0f), g.f(0.0f, 1.0f), g.f(0.02f, 10.0f), g.f(-0.95f, 0.95f), g.f(0.0f, 1.0f),
         g.f(20.0f, 20000.0f), g.f(20.0f, 20000.0f), g.f(0.0f, 1.0f), g.f(-24.0f, 24.0f), g.f(0.0f, 1.0f)};
    auto& b = p.bitcrusher;
    b = {g.f(1.0f, 24.0f), g.coin(), g.coin() ? 44100.0f : g.f(44.0f, 44100.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f), g.f(0.0f, 1.0f)};
    for (auto& q : p.dynamics) {
        q.mode = g.e<DynamicsMode>(2);
        q.thresholdDb = g.f(-48.0f, 0.0f);
        q.ratio = g.f(1.0f, 20.0f);
        q.attackMs = g.f(1.0f, 500.0f);
        q.holdMs = g.f(0.0f, 500.0f);
        q.releaseMs = g.f(1.0f, 500.0f);
        q.makeupDb = g.f(0.0f, 24.0f);
        q.sidechain = g.coin(0.3) ? g.voice() : kNoSource;
        for (auto& t : q.triggers) t = g.voice();
        q.vcaDepth = g.f(-1.0f, 1.0f);
        q.mix = g.f(0.0f, 1.0f);
    }
    auto& f = p.filter;
    f.type = g.e<FilterType>(4);
    f.cutoffHz = g.f(20.0f, 20000.0f);
    f.reso = g.f(0.0f, 1.0f);
    f.source = g.e<ModSource>(2);
    f.lfoSync = g.coin();
    f.lfoRateHz = g.f(0.01f, 20.0f);
    f.lfoDiv = g.e<SyncDiv>(6);
    f.lfoWave = g.e<LfoWave>(5);
    f.lfoSpread = g.f(0.0f, 1.0f);
    f.envAttackMs = g.f(1.0f, 500.0f);
    f.envHoldMs = g.f(0.0f, 500.0f);
    f.envReleaseMs = g.f(1.0f, 500.0f);
    for (auto& t : f.envTriggers) t = g.voice();
    f.modAmount = g.f(-1.0f, 1.0f);
    f.mix = g.f(0.0f, 1.0f);
    return p;
}

MasterParams randMaster(Rng& g) {
    MasterParams p;
    p.stage = g.e<OutputStage>(3);
    p.drive = g.f(0.0f, 1.0f);
    p.compOn = g.coin();
    p.xoverLowHz = g.f(40.0f, 1000.0f);
    p.xoverHighHz = g.f(1000.0f, 12000.0f);
    for (auto& b : p.band) {
        b = {g.coin(0.7), g.coin(0.15), g.f(-48.0f, 0.0f), g.f(1.0f, 20.0f), g.f(1.0f, 500.0f), g.f(1.0f, 500.0f), g.f(0.0f, 24.0f)};
    }
    p.limiterOn = g.coin(0.7);
    p.limThresholdDb = g.f(-24.0f, 0.0f);
    p.limAttackMs = g.f(0.1f, 10.0f);
    p.limReleaseMs = g.f(10.0f, 1000.0f);
    p.softClip = g.coin();
    p.volumeDb = g.f(-60.0f, 12.0f);
    return p;
}

// One block under test: fresh state per render.
struct Target {
    std::function<void(Rng&)> event;                    // a parameter change / trigger
    std::function<void(float*, float*, int)> process;   // stereo in place (mono targets use L)
};
using Factory = std::function<Target(double fs, Rng& setup)>;

// Input: segments of silence, noise, a sine and sparse clicks, all from the seed.
std::vector<float> makeInput(int n, std::uint64_t seed, int ch) {
    Rng g(seed * 31u + static_cast<std::uint64_t>(ch));
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    int i = 0;
    while (i < n) {
        const int len = std::min(n - i, g.range(100, 20000));
        const int kind = g.range(0, 3);
        const double amp = std::pow(10.0, -3.0 * g.uni());
        for (int k = 0; k < len; ++k) {
            double v = 0.0;
            if (kind == 1) v = amp * (2.0 * g.uni() - 1.0);
            else if (kind == 2) v = amp * std::sin(0.03 * (i + k));
            else if (kind == 3) v = ((i + k) % 977 == 0) ? amp : 0.0;
            x[static_cast<std::size_t>(i + k)] = static_cast<float>(v);
        }
        i += len;
    }
    return x;
}

std::vector<float> renderTarget(const Factory& make, double fs, int n, std::uint64_t seed, bool randomBlocks) {
    Rng setup(seed), sched(seed ^ 0x5151u), part(seed ^ 0xA0A0u);
    Target t = make(fs, setup);
    auto l = makeInput(n, seed, 0), r = makeInput(n, seed, 1);
    std::vector<int> events;
    for (int pos = 0; pos < n;) {
        pos += static_cast<int>(std::exp(std::log(fs * 0.3) * sched.uni())) + 1;   // 1 sample .. 300 ms apart
        if (pos < n) events.push_back(pos);
    }
    Rng ev(seed ^ 0x77u);
    std::size_t k = 0;
    int pos = 0;
    while (pos < n) {
        while (k < events.size() && events[k] == pos) {
            t.event(ev);
            ++k;
        }
        int b = randomBlocks ? 1 + static_cast<int>(std::exp(std::log(4096.0) * part.uni())) : 64;
        b = std::min({b, 4096, n - pos});
        if (k < events.size()) b = std::min(b, events[k] - pos);
        t.process(l.data() + pos, r.data() + pos, b);
        pos += b;
    }
    l.insert(l.end(), r.begin(), r.end());
    return l;
}

void checkPartition(const char* name, const Factory& make, double fs, double seconds, std::uint64_t seed) {
    const int n = static_cast<int>(fs * seconds);
    const auto a = renderTarget(make, fs, n, seed, false);
    const auto b = renderTarget(make, fs, n, seed, true);
    double maxDiff = 0.0;
    long first = -1;
    bool finite = true;
    for (std::size_t i = 0; i < a.size(); ++i) {
        finite = finite && std::isfinite(a[i]);
        const double d = std::abs(static_cast<double>(a[i]) - b[i]);
        if (d > 0.0 && first < 0) first = static_cast<long>(i);
        maxDiff = std::max(maxDiff, d);
    }
    INFO(name << " at " << fs << " Hz, seed " << seed << ": max difference " << maxDiff << ", first at sample "
              << (first < 0 ? -1 : first % n));
    CHECK(finite);
    CHECK(first < 0);   // every sample equal (+0 and -0 count as equal)
}

template <typename Fx, typename Set>
Factory stereoFx(Set set) {
    return [set](double fs, Rng& g) {
        auto fx = std::make_shared<Fx>();
        fx->prepare(fs, 4096);
        set(*fx, g);
        return Target{[fx, set](Rng& r) {
                          if (r.coin(0.03)) fx->reset();
                          set(*fx, r);
                      },
                      [fx](float* l, float* r, int n) { fx->process(l, r, n); }};
    };
}

}  // namespace

TEST_CASE("fx partition: every block is partition independent under parameter changes", "[fx][partition]") {
    const double rates[] = {44100.0, 48000.0, 96000.0};
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        const double fs = rates[seed % 3];
        checkPartition("distortion", stereoFx<Distortion>([](Distortion& o, Rng& g) { o.setParams(randFx(g).distortion); }), fs, 2.0, seed);
        checkPartition("parametric eq", stereoFx<ParamEq>([](ParamEq& o, Rng& g) { o.setParams(randFx(g).eq[0]); }), fs, 2.0, seed);
        checkPartition("delay", stereoFx<Delay>([](Delay& o, Rng& g) { o.setParams(randFx(g).delay, 40.0 + 200.0 * g.uni()); }), fs, 2.0, seed);
        checkPartition("reverb", stereoFx<Reverb>([](Reverb& o, Rng& g) { o.setParams(randFx(g).reverb); }), fs, 2.0, seed);
        checkPartition("chorus", stereoFx<Chorus>([](Chorus& o, Rng& g) { o.setParams(randFx(g).chorus); }), fs, 2.0, seed);
        checkPartition("bitcrusher", stereoFx<Bitcrusher>([](Bitcrusher& o, Rng& g) { o.setParams(randFx(g).bitcrusher); }), fs, 2.0, seed);
        checkPartition("filter fx", stereoFx<FilterFx>([](FilterFx& o, Rng& g) {
                           o.setParams(randFx(g).filter, 40.0 + 200.0 * g.uni());
                           if (g.coin(0.3)) o.noteTrigger(g.range(0, kNumVoices - 1));
                       }), fs, 2.0, seed);
        checkPartition("dynamics", [](double rate, Rng& g) {
            auto d = std::make_shared<Dynamics>();
            auto key = std::make_shared<std::vector<float>>(4096);
            d->prepare(rate, 4096);
            d->setParams(randFx(g).dynamics[0]);
            return Target{[d](Rng& r) {
                              d->setParams(randFx(r).dynamics[0]);
                              if (r.coin(0.4)) d->noteTrigger(r.range(0, kNumVoices - 1));
                          },
                          [d, key](float* l, float* r, int n) {
                              for (int i = 0; i < n; ++i) (*key)[static_cast<std::size_t>(i)] = 2.0f * r[i];
                              d->process(l, r, n, key->data());
                          }};
        }, fs, 2.0, seed);
        checkPartition("channel strip", [](double rate, Rng& g) {
            auto s = std::make_shared<ChannelStrip>();
            s->prepare(rate);
            s->setParams(randStrip(g));
            return Target{[s](Rng& r) { s->setParams(randStrip(r)); },
                          [s](float* l, float* r, int n) {
                              s->process(l, n);
                              std::memset(r, 0, sizeof(float) * static_cast<std::size_t>(n));
                          }};
        }, fs, 3.0, seed);
        checkPartition("master chain", [](double rate, Rng& g) {
            auto m = std::make_shared<MasterChain>();
            m->prepare(rate, 4096);
            m->setParams(randMaster(g), Revision::Late);
            return Target{[m](Rng& r) {
                              if (r.coin(0.02)) m->reset();
                              m->setParams(randMaster(r), r.coin(0.8) ? Revision::Late : Revision::Early);
                          },
                          [m](float* l, float* r, int n) { m->process(l, r, n); }};
        }, fs, 3.0, seed);
        checkPartition("trigger click", [](double rate, Rng&) {
            auto t = std::make_shared<TriggerClick>();
            t->prepare(rate);
            return Target{[t](Rng& r) {
                              if (r.coin(0.3)) t->trigger(Hit{});
                              if (r.coin(0.1)) t->setRevision(r.coin() ? Revision::Late : Revision::Early);
                          },
                          [t](float* l, float* r, int n) {
                              t->render(l, n);
                              std::memset(r, 0, sizeof(float) * static_cast<std::size_t>(n));
                          }};
        }, fs, 2.0, seed);
        checkPartition("fx rack", [](double rate, Rng& g) {
            auto rack = std::make_shared<FxRack>();
            auto aux = std::make_shared<std::array<std::vector<float>, 3>>();
            for (auto& v : *aux) v.assign(4096, 0.0f);
            rack->prepare(rate, 1024);
            rack->setParams(randFx(g), 120.0);
            return Target{[rack](Rng& r) {
                              if (r.coin(0.02)) rack->reset();
                              rack->setParams(randFx(r), 40.0 + 200.0 * r.uni());
                              if (r.coin(0.5)) rack->noteTrigger(static_cast<VoiceId>(r.range(0, kNumVoices - 1)));
                          },
                          [rack, aux](float* l, float* r, int n) {
                              auto& [l2, r2, key] = *aux;
                              for (int i = 0; i < n; ++i) {
                                  const auto ii = static_cast<std::size_t>(i);
                                  l2[ii] = 0.5f * r[i];
                                  r2[ii] = 0.5f * l[i];
                                  key[ii] = l[i];
                              }
                              std::array<const float*, kNumVoices> k{};
                              for (int v = 0; v < kNumVoices; v += 2) k[static_cast<std::size_t>(v)] = key.data();
                              float* const bus[kNumFxBuses][2] = {{l, r}, {l2.data(), r2.data()}};
                              rack->process(bus, k, n);
                              for (int i = 0; i < n; ++i) {
                                  l[i] += l2[static_cast<std::size_t>(i)];
                                  r[i] += r2[static_cast<std::size_t>(i)];
                              }
                          }};
        }, fs, 3.0, seed);
    }
}

// ---------------------------------------------------------------------------

namespace {

FxPlacement on(int bus, int order) { return {true, static_cast<std::uint8_t>(bus), static_cast<std::uint8_t>(order)}; }

// Bus 1 of a rack: `p` from the start, `q` at sample `at`; bus 1 input `in`.
std::vector<float> rackBus1(double fs, const FxParams& p, const FxParams* q, int at, const std::vector<float>& in, int blk) {
    FxRack rack;
    rack.prepare(fs, 4096);
    rack.setParams(p, 120.0);
    std::vector<float> l = in, r = in, l2(in.size(), 0.0f), r2(in.size(), 0.0f);
    const std::array<const float*, kNumVoices> key{};
    const int n = static_cast<int>(in.size());
    for (int pos = 0; pos < n;) {
        if (q != nullptr && pos == at) rack.setParams(*q, 120.0);
        int b = std::min(blk, n - pos);
        if (q != nullptr && pos < at) b = std::min(b, at - pos);
        float* const bus[kNumFxBuses][2] = {{l.data() + pos, r.data() + pos}, {l2.data() + pos, r2.data() + pos}};
        rack.process(bus, key, b);
        pos += b;
    }
    return l;
}

double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
    return e;
}

}  // namespace

TEST_CASE("fx rack: a reorder that changes the chain keeps the moved instance's tail", "[fx][rack][partition]") {
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 1.5);
    FxParams p;
    p.place[static_cast<int>(FxId::Reverb)] = on(0, 0);
    p.place[static_cast<int>(FxId::EqA)] = on(0, 1);
    p.eq[0].mid[0] = {true, 1000.0f, 1.0f, 3.0f};
    p.reverb.decay = 0.7f;
    FxParams moved = p;
    moved.place[static_cast<int>(FxId::Reverb)].order = 5;   // now after the EQ: the chain order changes
    FxParams toBus2 = p;
    toBus2.place[static_cast<int>(FxId::Reverb)] = on(1, 0);
    std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < 2400; ++i) in[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(i * 0.05));
    const int at = static_cast<int>(fs * 0.8);
    const auto still = rackBus1(fs, p, nullptr, 0, in, 256);
    const auto reordered = rackBus1(fs, p, &moved, at, in, 256);
    const auto reorderedOdd = rackBus1(fs, p, &moved, at, in, 333);
    const auto away = rackBus1(fs, p, &toBus2, at, in, 256);
    const auto from = static_cast<std::size_t>(fs * 0.9), to = static_cast<std::size_t>(n);
    const double eStill = energy(still, from, to), eMoved = energy(reordered, from, to);
    INFO("tail energy from 0.9 s: unchanged " << eStill << ", reordered " << eMoved << ", moved to bus 2 "
                                               << energy(away, from, to));
    CHECK(eMoved > 0.7 * eStill);   // the tail rings on (only the EQ order changed)
    CHECK(energy(away, from, to) == 0.0);   // moved to the other bus: bus 1 is empty, bus 2 starts from silence
    CHECK(std::memcmp(reordered.data(), reorderedOdd.data(), sizeof(float) * reordered.size()) == 0);
}

TEST_CASE("fx rack: switched on in silence an effect is fully in at once; into audio it fades", "[fx][rack]") {
    const double fs = 48000.0;
    const int n = 24000;
    FxParams p;
    p.place[static_cast<int>(FxId::Reverb)] = on(0, 0);
    // Silence, then an impulse 10 ms after the switch-on: wet only (no dry impulse leaks).
    std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
    in[480] = 1.0f;
    for (int blk : {1, 64, 4096}) {
        const auto y = rackBus1(fs, p, nullptr, 0, in, blk);
        INFO("block " << blk << ": sample at the impulse " << y[480]);
        CHECK(std::abs(y[480]) < 0.05f);   // a dry leak would put ~0.5 here
    }
    // Into a running signal: the fade starts at the switch, no step.
    FxParams none;
    auto sig = sine(300.0, fs, n, 0.5);
    const auto y = rackBus1(fs, none, &p, 4800, sig, 256);
    double worstStep = 0.0;
    for (int i = 4790; i < 4810; ++i)
        worstStep = std::max(worstStep, std::abs(static_cast<double>(y[static_cast<std::size_t>(i + 1)]) - y[static_cast<std::size_t>(i)]));
    const double sineStep = 0.5 * 2.0 * kPi * 300.0 / fs;
    INFO("largest sample step around the switch-on " << worstStep << " (sine " << sineStep << ")");
    CHECK(worstStep < 1.5 * sineStep);
}

TEST_CASE("fx rack and master: one NaN/Inf input sample does not poison the output", "[fx][rack][master]") {
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 2.0);
    auto x = noise(n, 0.3f, 99u);
    x[1000] = std::numeric_limits<float>::quiet_NaN();
    x[2000] = std::numeric_limits<float>::infinity();
    FxParams p;
    for (int id = 0; id < kNumFxInstances; ++id) p.place[static_cast<std::size_t>(id)] = on(id & 1, id);
    p.filter.type = FilterType::LowPass;
    const auto y = rackBus1(fs, p, nullptr, 0, x, 512);
    long bad = 0;
    for (std::size_t i = static_cast<std::size_t>(fs); i < y.size(); ++i) bad += std::isfinite(y[i]) ? 0 : 1;
    CHECK(bad == 0);
    MasterChain m;
    m.prepare(fs, 512);
    MasterParams mp;
    mp.stage = OutputStage::Original;
    mp.compOn = true;
    m.setParams(mp, Revision::Late);
    std::vector<float> l = x, r = x;
    for (int o = 0; o < n; o += 512) m.process(l.data() + o, r.data() + o, std::min(512, n - o));
    bad = 0;
    for (std::size_t i = 0; i < l.size(); ++i) bad += std::isfinite(l[i]) && std::isfinite(r[i]) ? 0 : 1;
    CHECK(bad == 0);
}

TEST_CASE("delay: synced times beyond the line halve and stay on the grid", "[fx][delay]") {
    struct C {
        SyncDiv div;
        SyncKind kind;
        double bpm, seconds;
    };
    const C cases[] = {{SyncDiv::D1_1, SyncKind::Dotted, 40.0, 9.0},     // fits the 10 s line
                       {SyncDiv::D1_1, SyncKind::Straight, 24.0, 10.0},  // exactly the line
                       {SyncDiv::D1_1, SyncKind::Dotted, 30.0, 6.0},     // 12 s -> 1/2 dotted
                       {SyncDiv::D1_1, SyncKind::Straight, 10.0, 6.0}};  // 24 s -> 12 -> 6
    for (double fs : {44100.0, 48000.0}) {
        for (const auto& c : cases) {
            Delay d;
            d.prepare(fs);
            DelayParams p;
            p.sync = true;
            p.div = c.div;
            p.kind = c.kind;
            p.feedback = 0.0f;
            d.setParams(p, c.bpm);
            INFO("fs " << fs << " bpm " << c.bpm << ": " << d.timeSamples() / fs << " s, expected " << c.seconds);
            CHECK(std::abs(d.timeSamples() - c.seconds * fs) < 1.0e-6);
        }
    }
}

TEST_CASE("master: the default chain is linear to -0.5 dBFS and never exceeds 0 dBFS", "[fx][master]") {
    const double fs = 48000.0;
    for (double levelDb : {-6.0, -1.0, -0.6, 0.0, 6.0}) {
        MasterChain m;
        m.prepare(fs, 512);
        m.setParams(MasterParams{}, Revision::Late);
        const int n = 48000;
        auto x = sine(1000.0, fs, n, std::pow(10.0, levelDb / 20.0));
        std::vector<float> l = x, r = x;
        for (int o = 0; o < n; o += 512) m.process(l.data() + o, r.data() + o, std::min(512, n - o));
        // Second half (the Modern stage's 10 Hz DC blocker has settled): 500
        // whole cycles, so the plain DFT needs no window.
        const std::vector<float> tail(l.begin() + n / 2, l.end());
        double pk = 0.0;
        for (float v : tail) pk = std::max(pk, static_cast<double>(std::abs(v)));
        const double h1 = magDb(dftAt(tail, 1000.0, fs));
        const double h3 = magDb(dftAt(tail, 3000.0, fs)) - h1;
        INFO(levelDb << " dBFS in: peak out " << 20.0 * std::log10(pk) << " dBFS, H3 " << h3 << " dBc");
        CHECK(pk <= 1.0);
        if (levelDb <= -0.6) CHECK(h3 < -120.0);   // linear: no shaping below -0.5 dBFS
    }
}

TEST_CASE("trigger click: the early revision has no C600 (sharp edge, higher level)", "[fx][trigger]") {
    for (double fs : {44100.0, 96000.0}) {
        TriggerClick late, early;
        late.prepare(fs);
        early.prepare(fs);
        early.setRevision(Revision::Early);
        late.trigger(Hit{});
        early.trigger(Hit{});
        const int n = static_cast<int>(fs * 0.05);
        std::vector<float> a(static_cast<std::size_t>(n)), b(static_cast<std::size_t>(n));
        late.render(a.data(), n);
        early.render(b.data(), n);
        // Early: the jack follows the collector, the first sample is the full step.
        CHECK(std::abs(b[0] - TriggerClick::kHighVoltsEarly * TriggerClick::kOutputScale) < 0.01);
        // Late: C600 rounds the edge (0.1 ms), so the first sample is well below it.
        CHECK(a[0] < 0.5f * b[0]);
        // The fall: early drops to its negative swing within the 1.7 ms turn-off ramp.
        const auto off = static_cast<std::size_t>(fs * (TriggerClick::kFullOnSeconds + TriggerClick::kTurnOffSeconds + 0.0002));
        CHECK(b[off] < 0.0f);
        CHECK(a[off] > b[off]);
    }
}
