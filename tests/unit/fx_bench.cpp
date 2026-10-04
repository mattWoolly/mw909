// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// CPU cost of every mixer/FX block in ns per (stereo) sample at 48 kHz on
// drum-like material, printed as a table. The assertions are loose ceilings
// (the numbers are for the report; a shared CI machine is noisy) and only
// apply to optimised builds. Silent and switched-off blocks must cost next to
// nothing.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "fx_test_util.h"
#include "mw909/fx/ChannelStrip.h"
#include "mw909/fx/FxBus.h"
#include "mw909/fx/MasterChain.h"
#include "mw909/fx/TriggerOut.h"

using namespace mw909;
using namespace mw909::fx;
using namespace fxtest;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 256;
constexpr int kSeconds = 4;
const int kN = static_cast<int>(kFs) * kSeconds;

#ifdef NDEBUG
constexpr bool kOptimised = true;
#else
constexpr bool kOptimised = false;
#endif

struct Row {
    std::string name;
    double ns;
};
std::vector<Row>& rows() {
    static std::vector<Row> r;
    return r;
}

void report(const std::string& name, double ns) {
    rows().push_back({name, ns});
    std::printf("  %-44s %8.1f ns/sample  %6.3f %% of a core at 48 kHz\n", name.c_str(), ns, ns * kFs * 1e-7);
}

template <typename Fx>
double benchStereo(Fx& fx, const std::vector<float>& x) {
    std::vector<float> l(x.size()), r(x.size());
    return nsPerSample(
        [&]() {
            std::copy(x.begin(), x.end(), l.begin());
            std::copy(x.begin(), x.end(), r.begin());
            for (int i = 0; i < kN; i += kBlock) fx.process(l.data() + i, r.data() + i, std::min(kBlock, kN - i));
        },
        kN);
}

const std::vector<float>& material() {
    static const std::vector<float> x = bursts(kN, 12000, kFs, 0.5f);   // a hit every 250 ms
    return x;
}

}  // namespace

TEST_CASE("per-block CPU cost (ns/sample at 48 kHz)", "[fx][bench]") {
    std::printf("\nmixer / FX CPU cost, 48 kHz, %d-sample blocks, drum bursts every 250 ms\n", kBlock);
    const auto& x = material();
    const std::vector<float> silence(static_cast<std::size_t>(kN), 0.0f);

    // ---- channel strip (mono)
    auto strip = [&](const char* name, bool eq, bool comp, const std::vector<float>& in) {
        ChannelStrip s;
        s.prepare(kFs);
        StripParams p;
        p.eqOn = eq;
        p.lowCutHz = 40.0f;
        p.highCutHz = 12000.0f;
        p.resonance = 0.2f;
        p.band[0] = {true, 120.0f, 1.0f, 4.0f};
        p.band[1] = {true, 3500.0f, 1.5f, -3.0f};
        p.compOn = comp;
        p.compThresholdDb = -18.0f;
        p.compRatio = 4.0f;
        s.setParams(p);
        std::vector<float> y(in.size());
        const double ns = nsPerSample(
            [&]() {
                std::copy(in.begin(), in.end(), y.begin());
                for (int i = 0; i < kN; i += kBlock) s.process(y.data() + i, std::min(kBlock, kN - i));
            },
            kN);
        report(name, ns);
        return ns;
    };
    const double sOff = strip("strip: both sections off", false, false, x);
    strip("strip: filter/EQ (2 cuts + 2 bells)", true, false, x);
    strip("strip: compressor", false, true, x);
    const double sFull = strip("strip: filter/EQ + compressor", true, true, x);
    const double sSilent = strip("strip: EQ + comp, silent input (asleep)", true, true, silence);

    // ---- effects (stereo)
    double worst = 0.0;
    auto fx = [&](const char* name, auto& e) {
        const double ns = benchStereo(e, x);
        report(name, ns);
        worst = std::max(worst, ns);
        return ns;
    };
    {
        Distortion d;
        d.prepare(kFs);
        DistortionParams p;
        p.preampDb = 18.0f;
        p.thresholdDb = -12.0f;
        d.setParams(p);
        fx("distortion (4x oversampled)", d);
    }
    {
        ParamEq e;
        e.prepare(kFs);
        ParamEqParams p;
        p.lowShelf = true;
        p.lowGainDb = 3.0f;
        p.lowHz = 100.0f;
        p.mid[0].on = true;
        p.mid[0].gainDb = -4.0f;
        p.mid[1].on = true;
        p.mid[1].gainDb = 2.0f;
        p.highHz = 15000.0f;
        e.setParams(p);
        fx("parametric EQ (4 bands)", e);
    }
    double delayNs = 0.0, reverbNs = 0.0;
    {
        Delay d;
        d.prepare(kFs);
        DelayParams p;
        p.loopFilter = FilterType::LowPass;
        p.spread = 0.6f;
        d.setParams(p, 120.0);
        delayNs = fx("delay (synced, ping-pong, loop filter)", d);
    }
    {
        Reverb rv;
        rv.prepare(kFs);
        rv.setParams(ReverbParams{});
        reverbNs = fx("reverb (ER + 8-line FDN, modulated)", rv);
    }
    {
        Chorus c;
        c.prepare(kFs);
        ChorusParams p;
        p.fatness = 1.0f;
        p.lowCutHz = 100.0f;
        c.setParams(p);
        fx("chorus (fatness 1, low cut)", c);
    }
    {
        Bitcrusher b;
        b.prepare(kFs);
        BitcrusherParams p;
        p.bits = 8.0f;
        p.rateHz = 11025.0f;
        p.dither = true;
        p.jitter = 0.3f;
        p.approxFilter = 0.8f;
        p.imagesFilter = 0.8f;
        b.setParams(p);
        fx("bitcrusher (both filters, dither, jitter)", b);
    }
    {
        Dynamics d;
        d.prepare(kFs);
        DynamicsParams p;
        p.thresholdDb = -20.0f;
        d.setParams(p);
        fx("dynamics: compressor", d);
    }
    {
        Dynamics d;
        d.prepare(kFs);
        DynamicsParams p;
        p.mode = DynamicsMode::Vca;
        p.triggers = {BD, kNoSource, kNoSource};
        d.setParams(p);
        fx("dynamics: VCA", d);
    }
    {
        FilterFx f;
        f.prepare(kFs);
        FilterFxParams p;
        p.modAmount = 0.4f;
        p.lfoSpread = 0.5f;
        f.setParams(p, 120.0);
        fx("filter (LFO swept, stereo)", f);
    }
    {
        Reverb rv;
        rv.prepare(kFs);
        rv.setParams(ReverbParams{});
        std::vector<float> l(silence), r(silence);
        const double ns = nsPerSample(
            [&]() {
                for (int i = 0; i < kN; i += kBlock) rv.process(l.data() + i, r.data() + i, kBlock);
            },
            kN);
        report("reverb, silent input (asleep)", ns);
        if (kOptimised) REQUIRE(ns < 15.0);   // a scan of the input; loose for loaded machines
    }

    // ---- master chain
    double masterDefault = 0.0;
    {
        MasterChain m;
        m.prepare(kFs, kBlock);
        m.setParams(MasterParams{}, Revision::Late);
        masterDefault = fx("master: default (Modern stage, limiter, soft clip)", m);
    }
    {
        MasterChain m;
        m.prepare(kFs, kBlock);
        MasterParams p;
        p.stage = OutputStage::Original;
        p.drive = 0.3f;
        p.compOn = true;
        p.band[0].thresholdDb = -12.0f;
        m.setParams(p, Revision::Late);
        fx("master: Original stage + 3-band comp + limiter", m);
    }

    // ---- trigger click
    {
        TriggerClick t;
        t.prepare(kFs);
        std::vector<float> y(static_cast<std::size_t>(kN));
        const double ns = nsPerSample(
            [&]() {
                for (int i = 0; i < kN; i += kBlock) {
                    if (i % 12288 == 0) t.trigger(Hit::normal());
                    t.render(y.data() + i, std::min(kBlock, kN - i));
                }
            },
            kN);
        report("trigger click (a hit every 256 ms)", ns);
    }

    // ---- the rack: a typical setup and an idle one
    auto rackBench = [&](const char* name, const FxParams& p, const std::vector<float>& in) {
        FxRack rack;
        rack.prepare(kFs, kBlock);
        rack.setParams(p, 120.0);
        rack.reset();
        std::vector<float> b[2][2];
        for (auto& bus : b)
            for (auto& c : bus) c.resize(in.size());
        const std::array<const float*, kNumVoices> key{};
        const double ns = nsPerSample(
            [&]() {
                for (auto& bus : b)
                    for (auto& c : bus) std::copy(in.begin(), in.end(), c.begin());
                for (int i = 0; i < kN; i += kBlock) {
                    float* const ptr[kNumFxBuses][2] = {{b[0][0].data() + i, b[0][1].data() + i},
                                                        {b[1][0].data() + i, b[1][1].data() + i}};
                    rack.process(ptr, key, std::min(kBlock, kN - i));
                }
            },
            kN);
        report(name, ns);
        return ns;
    };
    FxParams typical;
    typical.place[static_cast<int>(FxId::Reverb)] = {true, 0, 0};
    typical.place[static_cast<int>(FxId::Delay)] = {true, 1, 0};
    const double rackTypical = rackBench("rack: reverb on bus 1 + delay on bus 2", typical, x);
    const double rackIdle = rackBench("rack: same, silent buses (asleep)", typical, silence);
    const double rackNone = rackBench("rack: nothing placed", FxParams{}, x);

    // A typical FX setup: rack (reverb + delay) + two strips with EQ+comp, over
    // the default master. As a fraction of one core at 48 kHz:
    const double typicalAdd = rackTypical + 2.0 * sFull;
    std::printf("  typical FX setup (reverb + delay + 2 strips EQ+comp): %.1f ns/sample = %.2f %% of a core\n",
                typicalAdd, typicalAdd * kFs * 1e-7);
    std::printf("  default master chain alone: %.2f %% of a core\n\n", masterDefault * kFs * 1e-7);

    if (kOptimised) {
        // Loose ceilings: shared build machines are noisy; the table has the real figures.
        REQUIRE(sOff < 5.0);
        REQUIRE(sSilent < 15.0);
        REQUIRE(rackIdle < 25.0);
        REQUIRE(rackNone < 10.0);
        REQUIRE(worst < 1500.0);
        REQUIRE(typicalAdd < 1500.0);   // ~7 % of a core: generous, the report has the real figure
        (void)delayNs;
        (void)reverbNs;
    }
}
