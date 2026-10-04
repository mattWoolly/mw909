// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Independent review checks for the mixer and FX area. Measurements use their
// own helpers (not fx_test_util.h). Two groups:
//  * contract numbers that hold today (RT60 and compressor timing at the
//    extreme rates, delay sync arithmetic to the sample, limiter ceiling under
//    random block sizes, malloc-level allocation counting on the realtime
//    calls);
//  * reproductions of review findings (fixed; they now guard against a
//    regression): block-partition dependence after placement/bypass changes,
//    a no-op reorder that kills the reverb tail, the bitcrusher's top rate
//    that was only transparent at a 44.1 kHz host rate.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <vector>

#include "mw909/fx/Bitcrusher.h"
#include "mw909/fx/ChannelStrip.h"
#include "mw909/fx/Delay.h"
#include "mw909/fx/FilterFx.h"
#include "mw909/fx/FxBus.h"
#include "mw909/fx/MasterChain.h"
#include "mw909/fx/Reverb.h"
#include "mw909/fx/TriggerOut.h"

// ---------------------------------------------------------------------------
// malloc-family counter (operator new goes through malloc here). Not under
// AddressSanitizer, which owns malloc itself.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FXREVIEW_COUNT_ALLOCS 0
#else
#define FXREVIEW_COUNT_ALLOCS 1
#endif

namespace {
std::atomic<bool> gArmed{false};
std::atomic<long> gAllocs{0};
}  // namespace

#if FXREVIEW_COUNT_ALLOCS
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void __libc_free(void*);
void* malloc(std::size_t n) {
    if (gArmed) ++gAllocs;
    return __libc_malloc(n);
}
void* calloc(std::size_t a, std::size_t b) {
    if (gArmed) ++gAllocs;
    return __libc_calloc(a, b);
}
void* realloc(void* p, std::size_t n) {
    if (gArmed) ++gAllocs;
    return __libc_realloc(p, n);
}
void free(void* p) {
    if (gArmed && p != nullptr) ++gAllocs;
    __libc_free(p);
}
}
#endif

using namespace mw909;
using namespace mw909::fx;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Armed {
    Armed() { gArmed = true; }
    ~Armed() { gArmed = false; }
};

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t x) : s(x * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() {
        s ^= s << 7;
        s ^= s >> 9;
        return s * 0x2545F4914F6CDD1Dull;
    }
    double uni() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double bi() { return 2.0 * uni() - 1.0; }
};

double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(static_cast<double>(a[i]) - b[i]));
    return m;
}

// Schroeder backward integral, T60 from the -5..-35 dB fit.
double t60(const std::vector<float>& h, double fs) {
    std::vector<double> e(h.size() + 1, 0.0);
    for (std::size_t i = h.size(); i-- > 0;) e[i] = e[i + 1] + static_cast<double>(h[i]) * h[i];
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    double c = 0;
    for (std::size_t i = 0; i < h.size(); ++i) {
        const double d = 10.0 * std::log10(e[i] / e[0] + 1e-300);
        if (d > -5.0) continue;
        if (d < -35.0) break;
        const double t = static_cast<double>(i) / fs;
        sx += t;
        sy += d;
        sxx += t * t;
        sxy += t * d;
        c += 1.0;
    }
    return -60.0 / ((c * sxy - sx * sy) / (c * sxx - sx * sx));
}

// Hann-windowed single-bin level of x[a .. a+n) at f, re a full-scale sine.
double toneDb(const std::vector<float>& x, std::size_t a, std::size_t n, double f, double fs) {
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        const double ph = 2.0 * kPi * f * static_cast<double>(i) / fs;
        re += x[a + i] * w * std::cos(ph);
        im -= x[a + i] * w * std::sin(ph);
    }
    return 20.0 * std::log10(std::sqrt(re * re + im * im) / (0.25 * static_cast<double>(n)) + 1e-300);
}

FxPlacement at(int bus, int order) {
    return FxPlacement{true, static_cast<std::uint8_t>(bus), static_cast<std::uint8_t>(order)};
}

// Render bus 1 of a rack: `p0` from the start, `p1` (if any) applied at sample
// `change`, blocks of `blk` samples cut at the change point.
std::vector<float> rackRender(double fs, int n, const FxParams& p0, const FxParams* p1, int change,
                              const std::vector<float>& in, int blk) {
    FxRack rack;
    rack.prepare(fs, 8192);
    rack.setParams(p0, 120.0);
    std::vector<float> l = in, r = in, l2(in.size(), 0.0f), r2(in.size(), 0.0f);
    std::array<const float*, kNumVoices> key{};
    int pos = 0;
    bool applied = false;
    while (pos < n) {
        if (p1 != nullptr && !applied && pos == change) {
            rack.setParams(*p1, 120.0);
            applied = true;
        }
        int b = std::min(blk, n - pos);
        if (p1 != nullptr && !applied) b = std::min(b, change - pos);
        float* const bus[kNumFxBuses][2] = {{l.data() + pos, r.data() + pos}, {l2.data() + pos, r2.data() + pos}};
        rack.process(bus, key, b);
        pos += b;
    }
    return l;
}

// Generic mono/stereo renderer with a parameter schedule and fixed block size.
std::vector<float> scheduled(int n, int blk, const std::vector<int>& at, const std::function<void(std::size_t)>& apply,
                             const std::function<void(float*, int)>& proc) {
    std::vector<float> x(static_cast<std::size_t>(n));
    Rng g(7);
    for (auto& v : x) v = static_cast<float>(0.3 * g.bi());
    int pos = 0;
    std::size_t k = 0;
    while (pos < n) {
        if (k < at.size() && pos == at[k]) apply(k++);
        int b = std::min(blk, n - pos);
        if (k < at.size()) b = std::min(b, at[k] - pos);
        proc(x.data() + pos, b);
        pos += b;
    }
    return x;
}

}  // namespace

// ---------------------------------------------------------------------------
// Contract numbers that hold.

TEST_CASE("fx review: reverb RT60 matches the decay setting at 22.05 and 384 kHz", "[fx][review]") {
    for (double fs : {22050.0, 384000.0}) {
        for (double decay : {0.25, 0.5}) {
            Reverb rv;
            rv.prepare(fs);
            ReverbParams p;
            p.earlyLate = 1.0f;
            p.modulation = 0.0f;
            p.lateColor = 1.0f;
            p.predelayMs = 0.0f;
            p.decay = static_cast<float>(decay);
            rv.setParams(p);
            const double target = 0.2 * std::pow(50.0, decay);
            const int n = static_cast<int>(fs * (1.2 * target + 0.3));
            std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(static_cast<std::size_t>(n), 0.0f);
            l[0] = r[0] = 1.0f;
            for (int o = 0; o < n; o += 512) rv.process(l.data() + o, r.data() + o, std::min(512, n - o));
            const double t = t60(l, fs);
            INFO("fs " << fs << " decay " << decay << ": RT60 " << t << " s, target " << target);
            CHECK(std::abs(t / target - 1.0) < 0.03);
        }
    }
}

TEST_CASE("fx review: strip compressor attack/release time constants at 22.05..384 kHz", "[fx][review]") {
    for (double fs : {22050.0, 44100.0, 192000.0, 384000.0}) {
        ChannelStrip s;
        s.prepare(fs);
        StripParams p;
        p.compOn = true;
        p.compThresholdDb = -30.0f;
        p.compRatio = 20.0f;
        p.compAttackMs = 10.0f;
        p.compReleaseMs = 10.0f;
        s.setParams(p);
        const int pre = static_cast<int>(fs * 0.05), on = static_cast<int>(fs * 0.11), n = static_cast<int>(fs * 0.17);
        std::vector<float> x(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = (i >= pre && i < on) ? 1.0f : 1.0e-3f;
        const std::vector<float> in = x;
        for (int o = 0; o < n; o += 64) s.process(x.data() + o, std::min(64, n - o));
        auto gr = [&](int i) { return 20.0 * std::log10(in[static_cast<std::size_t>(i)] / x[static_cast<std::size_t>(i)]); };
        const double grMax = gr(on - 1);
        int ta = -1, tr = -1;
        for (int i = pre; i < on && ta < 0; ++i)
            if (gr(i) >= grMax * (1.0 - std::exp(-1.0))) ta = i - pre;
        for (int i = on; i < n && tr < 0; ++i)
            if (gr(i) <= grMax * std::exp(-1.0)) tr = i - on;
        INFO("fs " << fs << ": attack " << ta / fs * 1e3 << " ms, release " << tr / fs * 1e3 << " ms");
        CHECK(std::abs(ta / fs * 1e3 - 10.0) < 0.2);
        CHECK(std::abs(tr / fs * 1e3 - 10.0) < 0.2);
    }
}

TEST_CASE("fx review: delay echo centroid sits on the synced time to 1e-3 samples", "[fx][review]") {
    struct C {
        SyncDiv div;
        SyncKind kind;
        double bpm, seconds;
    };
    const C cases[] = {{SyncDiv::D1_8, SyncKind::Dotted, 120.0, 0.375},
                       {SyncDiv::D1_4, SyncKind::Triplet, 90.0, (60.0 / 90.0) * 2.0 / 3.0},
                       {SyncDiv::D1_16, SyncKind::Straight, 174.0, 15.0 / 174.0},
                       {SyncDiv::D1_32, SyncKind::Triplet, 333.3, 7.5 / 333.3 * 2.0 / 3.0}};
    for (double fs : {22050.0, 48000.0, 384000.0}) {
        for (const auto& c : cases) {
            Delay d;
            d.prepare(fs);
            DelayParams p;
            p.sync = true;
            p.div = c.div;
            p.kind = c.kind;
            p.feedback = 0.0f;
            p.mix = 1.0f;
            d.setParams(p, c.bpm);
            const int n = static_cast<int>(c.seconds * fs) + 64;
            std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(static_cast<std::size_t>(n), 0.0f);
            l[0] = r[0] = 1.0f;
            for (int o = 0; o < n; o += 333) d.process(l.data() + o, r.data() + o, std::min(333, n - o));
            double s = 0.0, sw = 0.0;
            for (int i = 0; i < n; ++i) {
                s += l[static_cast<std::size_t>(i)];
                sw += l[static_cast<std::size_t>(i)] * static_cast<double>(i);
            }
            INFO("fs " << fs << " expected " << c.seconds * fs << " samples, centroid " << sw / s);
            CHECK(std::abs(sw / s - c.seconds * fs) < 1.0e-3);
        }
    }
}

TEST_CASE("fx review: limiter never exceeds its threshold with random blocks at 22.05..384 kHz", "[fx][review]") {
    for (double fs : {22050.0, 48000.0, 384000.0}) {
        MasterChain m;
        m.prepare(fs, 4096);
        MasterParams p;
        p.stage = OutputStage::Off;
        p.limThresholdDb = -6.0f;
        p.limAttackMs = 10.0f;
        p.limReleaseMs = 10.0f;
        p.softClip = false;
        m.setParams(p, Revision::Late);
        Rng g(3);
        const int n = static_cast<int>(fs * 1.5);
        std::vector<float> a(static_cast<std::size_t>(n)), b(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double env = ((i / static_cast<int>(fs * 0.05)) % 3 == 0) ? 100.0 : 0.3;
            a[static_cast<std::size_t>(i)] = static_cast<float>(env * g.bi());
            b[static_cast<std::size_t>(i)] = static_cast<float>(env * g.bi());
        }
        for (int o = 0; o < n;) {
            const int bl = std::min(1 + static_cast<int>(g.uni() * 4096.0), n - o);
            m.process(a.data() + o, b.data() + o, bl);
            o += bl;
        }
        double peak = 0.0;
        for (int i = 0; i < n; ++i)
            peak = std::max({peak, std::abs(static_cast<double>(a[static_cast<std::size_t>(i)])),
                             std::abs(static_cast<double>(b[static_cast<std::size_t>(i)]))});
        CHECK(peak <= std::pow(10.0, -6.0 / 20.0) * (1.0 + 1e-6));
    }
}

#if FXREVIEW_COUNT_ALLOCS
TEST_CASE("fx review: realtime calls never touch malloc/calloc/realloc/free", "[fx][review]") {
    const double fs = 48000.0;
    FxRack rack;
    rack.prepare(fs, 1024);
    MasterChain master;
    master.prepare(fs, 1024);
    ChannelStrip strip;
    TriggerClick click;
    std::vector<float> l(1024), r(1024), l2(1024), r2(1024), key(1024);
    Rng g(5);
    gAllocs = 0;
    {
        Armed armed;
        strip.prepare(fs);   // noexcept DSP-block prepare: allocation-free by contract
        click.prepare(fs);
        for (int k = 0; k < 400; ++k) {
            FxParams p;
            for (auto& pl : p.place) pl = FxPlacement{g.uni() < 0.6, static_cast<std::uint8_t>(g.uni() * 2.0),
                                                      static_cast<std::uint8_t>(g.uni() * 10.0)};
            p.dynamics[0].mode = DynamicsMode::Vca;
            p.dynamics[0].triggers[0] = BD;
            p.dynamics[1].sidechain = SD;
            p.filter.source = k % 2 ? ModSource::Envelope : ModSource::Lfo;
            p.filter.envTriggers[0] = BD;
            p.bus[0].toBus2 = static_cast<float>(g.uni());
            p.bus[0].toBus2Pre = k % 3 == 0;
            rack.setParams(p, 60.0 + 200.0 * g.uni());
            if (k % 50 == 0) rack.reset();
            rack.noteTrigger(BD);
            MasterParams mp;
            mp.stage = static_cast<OutputStage>(k % 3);
            mp.compOn = k % 2 == 0;
            master.setParams(mp, k % 5 ? Revision::Late : Revision::Early);
            StripParams sp;
            sp.eqOn = k % 2 == 0;
            sp.compOn = k % 3 == 0;
            sp.compPreEq = k % 4 == 0;
            sp.band[0].on = true;
            strip.setParams(sp);
            if (k % 7 == 0) click.trigger(Hit{});
            const int n = 1 + static_cast<int>(g.uni() * 1023.0);
            for (int i = 0; i < n; ++i) {
                const float v = (k % 4 == 0) ? 0.0f : static_cast<float>(0.3 * g.bi());
                l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = v;
                l2[static_cast<std::size_t>(i)] = r2[static_cast<std::size_t>(i)] = 0.5f * v;
                key[static_cast<std::size_t>(i)] = v;
            }
            std::array<const float*, kNumVoices> keys{};
            keys[SD] = key.data();
            float* const bus[kNumFxBuses][2] = {{l.data(), r.data()}, {l2.data(), r2.data()}};
            rack.process(bus, keys, n);
            master.process(l.data(), r.data(), n);
            strip.process(l2.data(), n);
            click.render(r2.data(), n);
        }
    }
    CHECK(gAllocs.load() == 0);
}
#endif

// ---------------------------------------------------------------------------
// Reproductions of review findings (fixed: regression guards).

TEST_CASE("fx review: rack output does not depend on the block partition while a placement fades",
          "[fx][review]") {
    const double fs = 48000.0;
    const int n = 24000;
    SECTION("effect switched on, 10 ms of silence, then an impulse") {
        FxParams p;
        p.place[static_cast<int>(FxId::Reverb)] = at(0, 0);
        std::vector<float> in(static_cast<std::size_t>(n), 0.0f);
        in[480] = 1.0f;
        const auto a = rackRender(fs, n, p, nullptr, 0, in, 64);
        const auto b = rackRender(fs, n, p, nullptr, 0, in, 8192);
        INFO("max difference " << maxDiff(a, b));
        CHECK(maxDiff(a, b) < 1.0e-6);
    }
    SECTION("reorder while audio plays (the move lands at a block end)") {
        FxParams p;
        p.place[static_cast<int>(FxId::Delay)] = at(0, 0);
        p.place[static_cast<int>(FxId::Chorus)] = at(0, 1);
        p.delay.sync = false;
        p.delay.timeMs = 50.0f;
        p.delay.mix = 0.5f;
        FxParams q = p;
        q.place[static_cast<int>(FxId::Delay)] = at(0, 2);
        std::vector<float> in(static_cast<std::size_t>(n));
        Rng g(5);
        for (auto& v : in) v = static_cast<float>(0.3 * g.bi());
        const auto a = rackRender(fs, n, p, &q, 4800, in, 32);
        const auto b = rackRender(fs, n, p, &q, 4800, in, 4096);
        INFO("max difference " << maxDiff(a, b));
        CHECK(maxDiff(a, b) < 1.0e-6);
    }
}

TEST_CASE("fx review: a reorder that keeps the chain order keeps the reverb tail", "[fx][review]") {
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 1.5);
    auto run = [&](bool reorder) {
        FxRack rack;
        rack.prepare(fs, 256);
        FxParams p;
        p.place[static_cast<int>(FxId::Reverb)] = at(0, 0);
        p.reverb.decay = 0.7f;
        rack.setParams(p, 120.0);
        std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r = l, l2 = l, r2 = l;
        for (int i = 0; i < 2400; ++i) l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(i * 0.05));
        std::array<const float*, kNumVoices> key{};
        for (int o = 0; o < n; o += 256) {
            if (reorder && o == 38400) {   // 0.8 s: only the reverb is on the bus, order 0 -> 3
                FxParams q = p;
                q.place[static_cast<int>(FxId::Reverb)].order = 3;
                rack.setParams(q, 120.0);
            }
            float* const bus[kNumFxBuses][2] = {{l.data() + o, r.data() + o}, {l2.data() + o, r2.data() + o}};
            rack.process(bus, key, std::min(256, n - o));
        }
        double e = 0.0;
        for (int i = 43200; i < n; ++i) e += static_cast<double>(l[static_cast<std::size_t>(i)]) * l[static_cast<std::size_t>(i)];
        return e;
    };
    const double kept = run(false), after = run(true);
    INFO("tail energy from 0.9 s: " << kept << " without the reorder, " << after << " with it");
    CHECK(after > 0.5 * kept);
}

TEST_CASE("fx review: filter fx and strip are partition independent across bypass toggles",
          "[fx][review]") {
    const double fs = 48000.0;
    const int n = 24000;
    SECTION("filter fx LowPass -> Off -> LowPass with the LFO running") {
        FilterFx f;
        FilterFxParams on;
        on.type = FilterType::LowPass;
        on.cutoffHz = 800.0f;
        on.reso = 0.5f;
        on.modAmount = 0.5f;
        on.lfoRateHz = 2.0f;
        FilterFxParams off = on;
        off.type = FilterType::Off;
        auto run = [&](int blk) {
            f.prepare(fs);
            f.setParams(on, 120.0);
            return scheduled(n, blk, {0, 4800, 9600}, [&](std::size_t k) { f.setParams(k == 1 ? off : on, 120.0); },
                             [&](float* x, int m) {
                                 std::vector<float> r(x, x + m);
                                 f.process(x, r.data(), m);
                             });
        };
        const auto a = run(64), b = run(1000);
        INFO("max difference " << maxDiff(a, b));
        CHECK(maxDiff(a, b) < 1.0e-6);
    }
    SECTION("strip EQ bell 1 kHz -> off -> bell 5 kHz") {
        ChannelStrip s;
        StripParams e1;
        e1.eqOn = true;
        e1.band[0] = {true, 1000.0f, 1.0f, 12.0f};
        StripParams e2 = e1;
        e2.band[0].freqHz = 5000.0f;
        const StripParams off{};
        auto run = [&](int blk) {
            s.prepare(fs);
            s.setParams(e1);
            return scheduled(n, blk, {0, 4800, 9600}, [&](std::size_t k) { s.setParams(k == 0 ? e1 : (k == 1 ? off : e2)); },
                             [&](float* x, int m) { s.process(x, m); });
        };
        const auto a = run(64), b = run(1000);
        INFO("max difference " << maxDiff(a, b));
        CHECK(maxDiff(a, b) < 1.0e-6);
    }
}

TEST_CASE("fx review: bitcrusher at its top rate (44.1 kHz) is transparent at every host rate",
          "[fx][review]") {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        Bitcrusher b;
        b.prepare(fs);
        b.setParams(BitcrusherParams{});   // 16 bits, 44.1 kHz, filters off
        const int n = static_cast<int>(fs);
        std::vector<float> l(static_cast<std::size_t>(n)), r, x;
        for (int i = 0; i < n; ++i) l[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / fs));
        x = r = l;
        for (int o = 0; o < n; o += 256) b.process(l.data() + o, r.data() + o, std::min(256, n - o));
        double e = 0.0, s = 0.0;
        for (int i = 4800; i < n; ++i) {
            const double d = l[static_cast<std::size_t>(i)] - x[static_cast<std::size_t>(i)];
            e += d * d;
            s += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
        }
        const double errDb = 10.0 * std::log10(e / s + 1e-30);
        // The hold's first image of 1 kHz (44.1k - 1k), folded into the host band.
        const double fImg = 43100.0 < fs / 2 ? 43100.0 : std::abs(fs - 43100.0);
        const double alias = toneDb(l, 8192, 32768, fImg, fs) - toneDb(l, 8192, 32768, 1000.0, fs);
        INFO("fs " << fs << ": error " << errDb << " dB re signal, hold image of 1 kHz at " << fImg << " Hz: " << alias << " dBc");
        CHECK(errDb < -80.0);
    }
}
