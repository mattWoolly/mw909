// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Engine: silence, overwrite contract, routing matrix (every voice to every
// bus, null-bus fallback, FX routes and sends, individual outs bypassing the
// master), LEVEL / pan / spread, trigger clicks, latency and its alignment,
// the shared accent DACs, the hat choke, the late multi-out RC, determinism
// and NaN/Inf parameter fuzzing.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdio>
#include <limits>
#include <memory>
#include <random>

#include "CoreTestUtil.h"
#include "mw909/dsp/Noise.h"
#include "mw909/dsp/PcmCommon.h"
#include "mw909/voices/BassDrum.h"
#include "mw909/voices/Cymbal.h"
#include "mw909/voices/HandClap.h"
#include "mw909/voices/HiHat.h"
#include "mw909/voices/RimShot.h"
#include "mw909/voices/SnareDrum.h"
#include "mw909/voices/Tom.h"

using namespace mw909;
using namespace coretest;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kFs = 48000.0;
constexpr int kBlock = 512;

std::unique_ptr<Engine> makeEngine(double fs = kFs, Quality q = Quality::Standard,
                                   const EngineParams& p = defaultEngineParams(), int block = kBlock) {
    auto e = std::make_unique<Engine>();
    e->prepare(fs, block);
    e->configure(q, 0, 0.0f, Revision::Late);
    e->setParams(p);
    return e;
}

// Each voice on its own individual out (voice v -> bus v + 1).
EngineParams ownOuts() {
    EngineParams p = defaultEngineParams();
    for (int v = 0; v < kNumVoices; ++v) p.channel[static_cast<std::size_t>(v)].output = static_cast<std::uint8_t>(v + 1);
    return p;
}

double busEnergy(Buses& b, int bus) { return energy(b.left(bus)) + energy(b.right(bus)); }

}  // namespace

TEST_CASE("Engine is exactly silent until something is triggered", "[core][engine]") {
    auto e = makeEngine();
    Buses buses(8192);
    renderEngine(*e, buses, {}, kBlock);
    for (int b = 0; b < kNumBuses; ++b) {
        REQUIRE(peak(buses.left(b)) == 0.0f);
        REQUIRE(peak(buses.right(b)) == 0.0f);
    }
    REQUIRE_FALSE(e->anyVoiceActive());
}

TEST_CASE("Every voice sounds, stays bounded and returns to exact silence", "[core][engine]") {
    auto e = makeEngine(kFs, Quality::Standard, ownOuts());
    for (int v = 0; v < kNumVoices; ++v) {
        INFO("voice " << kVoiceInfo[static_cast<std::size_t>(v)].code);
        e->reset();
        Buses buses(static_cast<int>(kFs * 12.0));
        renderEngine(*e, buses, {{0, static_cast<VoiceId>(v), Hit::accented(true, true, 1.0f)}}, kBlock);
        const int bus = v + 1;
        REQUIRE(allFinite(buses.left(bus)));
        REQUIRE(allFinite(buses.right(bus)));
        const float pk = std::max(peak(buses.left(bus)), peak(buses.right(bus)));
        REQUIRE(pk > 0.01f);
        REQUIRE(pk < 1.0f);
        // Nothing reached the main mix or another out.
        for (int b = 0; b < kNumBuses; ++b)
            if (b != bus) REQUIRE(busEnergy(buses, b) == 0.0);
        // Exact silence at the end, and the engine asleep.
        REQUIRE(peak(buses.left(bus), buses.length - 4800) == 0.0f);
        REQUIRE_FALSE(e->anyVoiceActive());
    }
}

TEST_CASE("render overwrites its range of every connected bus and nothing else", "[core][engine]") {
    auto e = makeEngine();
    Buses buses(1024);
    for (auto& bus : buses.data)
        for (auto& c : bus) std::fill(c.begin(), c.end(), 7.0f);
    e->trigger(BD, Hit::normal());
    e->render(buses.view(), 100, 300);
    for (int b = 0; b < kNumBuses; ++b)
        for (int c = 0; c < 2; ++c) {
            const auto& x = buses.data[b][c];
            for (int i = 0; i < 1024; ++i) {
                const bool inside = i >= 100 && i < 400;
                if (inside) REQUIRE(x[static_cast<std::size_t>(i)] != 7.0f);
                else REQUIRE(x[static_cast<std::size_t>(i)] == 7.0f);
            }
        }
}

TEST_CASE("Routing matrix: every voice reaches exactly the bus it is routed to", "[core][engine][routing]") {
    auto e = makeEngine();
    EngineParams p = defaultEngineParams();
    for (int v = 0; v < kNumVoices; ++v) {
        for (int bus = 0; bus < kNumBuses; ++bus) {
            INFO("voice " << kVoiceInfo[static_cast<std::size_t>(v)].code << " -> bus " << bus);
            p.channel[static_cast<std::size_t>(v)].output = static_cast<std::uint8_t>(bus);
            e->reset();
            e->setParams(p);
            Buses buses(2048);
            renderEngine(*e, buses, {{0, static_cast<VoiceId>(v)}}, kBlock);
            for (int b = 0; b < kNumBuses; ++b) {
                if (b == bus) REQUIRE(busEnergy(buses, b) > 0.0);
                else REQUIRE(busEnergy(buses, b) == 0.0);
            }
        }
        p.channel[static_cast<std::size_t>(v)].output = 0;
    }
}

TEST_CASE("A voice routed to an unconnected bus falls back to the main mix", "[core][engine][routing]") {
    for (int v = 0; v < kNumVoices; ++v) {
        INFO("voice " << kVoiceInfo[static_cast<std::size_t>(v)].code);
        EngineParams p = defaultEngineParams();
        auto ref = makeEngine(kFs, Quality::Standard, p);
        Buses a(4096, 1);
        renderEngine(*ref, a, {{0, static_cast<VoiceId>(v)}}, kBlock);
        REQUIRE(energy(a.left()) > 0.0);
        for (int bus : {1, 7, 15}) {
            p.channel[static_cast<std::size_t>(v)].output = static_cast<std::uint8_t>(bus);
            auto e = makeEngine(kFs, Quality::Standard, p);
            Buses b(4096, 1);   // only the main bus connected
            renderEngine(*e, b, {{0, static_cast<VoiceId>(v)}}, kBlock);
            REQUIRE(same(a.left(), b.left()));
            REQUIRE(same(a.right(), b.right()));
        }
    }
}

TEST_CASE("Individual outs bypass the master chain", "[core][engine][routing]") {
    EngineParams p = defaultEngineParams();
    p.channel[SD].output = 3;
    auto e1 = makeEngine(kFs, Quality::Standard, p);
    Buses a(4096);
    renderEngine(*e1, a, {{0, BD}, {0, SD}}, kBlock);

    p.master.volumeDb = -20.0f;
    p.master.stage = OutputStage::Original;
    p.master.compOn = true;
    auto e2 = makeEngine(kFs, Quality::Standard, p);
    Buses b(4096);
    renderEngine(*e2, b, {{0, BD}, {0, SD}}, kBlock);

    REQUIRE(same(a.left(3), b.left(3)));
    REQUIRE(same(a.right(3), b.right(3)));
    REQUIRE(peak(b.left()) < 0.2f * peak(a.left()));
}

TEST_CASE("FX routes: a channel into an FX bus comes out on the bus's output", "[core][engine][routing]") {
    // No effect placed: an FX bus returns its input, so routing through it at
    // 0 dB must equal routing straight to the same out, sample for sample.
    EngineParams direct = defaultEngineParams();
    direct.channel[CP].output = 5;
    auto e1 = makeEngine(kFs, Quality::Standard, direct);
    Buses a(4096);
    renderEngine(*e1, a, {{0, CP}}, kBlock);
    REQUIRE(busEnergy(a, 5) > 0.0);

    for (int fb = 0; fb < kNumFxBuses; ++fb) {
        INFO("FX bus " << fb + 1);
        EngineParams p = defaultEngineParams();
        p.channel[CP].route = fb == 0 ? ChannelRoute::FxBus1 : ChannelRoute::FxBus2;
        p.channel[CP].output = 9;   // ignored when routed to an FX bus
        p.fx.bus[static_cast<std::size_t>(fb)].output = 5;
        auto e = makeEngine(kFs, Quality::Standard, p);
        Buses b(4096);
        renderEngine(*e, b, {{0, CP}}, kBlock);
        // The multi-out RC sees the unplugged (mix) pole on the FX route, so
        // compare against the same channel routed to an FX bus that returns to
        // out 5 vs the main fallback below; here only the bus placement counts.
        REQUIRE(busEnergy(b, 5) > 0.0);
        for (int x = 0; x < kNumBuses; ++x)
            if (x != 5) REQUIRE(busEnergy(b, x) == 0.0);
    }

    SECTION("an FX return to an unconnected bus falls back to main") {
        EngineParams p = defaultEngineParams();
        p.channel[CP].route = ChannelRoute::FxBus1;
        p.fx.bus[0].output = 12;
        auto e = makeEngine(kFs, Quality::Standard, p);
        Buses b(4096, 1);
        renderEngine(*e, b, {{0, CP}}, kBlock);
        REQUIRE(energy(b.left()) > 0.0);
    }
    SECTION("the return level scales the bus, -60 dB is off") {
        EngineParams p = defaultEngineParams();
        p.channel[CP].route = ChannelRoute::FxBus1;
        p.fx.bus[0].output = 5;
        auto e = makeEngine(kFs, Quality::Standard, p);
        Buses b0(4096);
        renderEngine(*e, b0, {{0, CP}}, kBlock);
        p.fx.bus[0].levelDb = -6.0f;
        auto e6 = makeEngine(kFs, Quality::Standard, p);
        Buses b6(4096);
        renderEngine(*e6, b6, {{0, CP}}, kBlock);
        REQUIRE_THAT(static_cast<double>(peak(b6.left(5)) / peak(b0.left(5))), WithinRel(0.5012, 1.0e-4));
        p.fx.bus[0].levelDb = -60.0f;
        auto eOff = makeEngine(kFs, Quality::Standard, p);
        Buses bOff(4096);
        renderEngine(*eOff, bOff, {{0, CP}}, kBlock);
        REQUIRE(busEnergy(bOff, 5) == 0.0);
        REQUIRE(busEnergy(bOff, 0) == 0.0);
    }
    SECTION("bus 1 feeds bus 2") {
        EngineParams p = defaultEngineParams();
        p.channel[CP].route = ChannelRoute::FxBus1;
        p.fx.bus[0].output = 3;
        p.fx.bus[1].output = 4;
        p.fx.bus[0].toBus2 = 1.0f;
        auto e = makeEngine(kFs, Quality::Standard, p);
        Buses b(4096);
        renderEngine(*e, b, {{0, CP}}, kBlock);
        REQUIRE(busEnergy(b, 3) > 0.0);
        REQUIRE(busEnergy(b, 4) > 0.0);
    }
}

TEST_CASE("Sends: post-LEVEL and pre-LEVEL into either FX bus", "[core][engine][routing]") {
    // Reference: the channel straight to out 7 at LEVEL 0.5 (unity), via an FX
    // route so the multi-out RC pole is the same as on the send.
    EngineParams ref = defaultEngineParams();
    ref.channel[SD].route = ChannelRoute::FxBus2;
    ref.fx.bus[1].output = 7;
    auto e0 = makeEngine(kFs, Quality::Standard, ref);
    Buses a(4096);
    renderEngine(*e0, a, {{0, SD}}, kBlock);

    for (int fb = 1; fb <= 2; ++fb) {
        INFO("send bus " << fb);
        EngineParams p = defaultEngineParams();
        p.channel[SD].output = 2;   // dry on out 2
        p.channel[SD].sendBus = static_cast<std::uint8_t>(fb);
        p.channel[SD].send = 1.0f;
        p.fx.bus[static_cast<std::size_t>(fb - 1)].output = 7;
        p.channel[SD].route = ChannelRoute::Output;
        auto e = makeEngine(kFs, Quality::Standard, p);
        Buses b(4096);
        renderEngine(*e, b, {{0, SD}}, kBlock);
        REQUIRE(busEnergy(b, 2) > 0.0);
        // Same signal, but this channel's dry route is a plugged out (15.9 kHz
        // RC) where the reference ran on the mix pole: equal within the RC's
        // small high-frequency difference.
        REQUIRE_THAT(energy(b.left(7)), WithinRel(energy(a.left(7)), 0.05));

        // Pre-LEVEL: LEVEL at 0 silences the dry path, the send stays.
        p.channel[SD].level = 0.0f;
        p.channel[SD].sendPre = true;
        auto ep = makeEngine(kFs, Quality::Standard, p);
        Buses c(4096);
        renderEngine(*ep, c, {{0, SD}}, kBlock);
        REQUIRE(busEnergy(c, 2) == 0.0);
        REQUIRE(same(b.left(7), c.left(7)));
        // Post-LEVEL with LEVEL 0: nothing anywhere.
        p.channel[SD].sendPre = false;
        auto eq = makeEngine(kFs, Quality::Standard, p);
        Buses d(4096);
        renderEngine(*eq, d, {{0, SD}}, kBlock);
        for (int x = 0; x < kNumBuses; ++x) REQUIRE(busEnergy(d, x) == 0.0);
    }
}

TEST_CASE("Trigger clicks fire on their source voice and route like a channel", "[core][engine][routing]") {
    EngineParams p = defaultEngineParams();
    p.trig[0] = {true, RS, ChannelRoute::Output, 4, 0.0f, 0.5f};
    p.trig[1] = {true, SD, ChannelRoute::FxBus1, 0, 0.0f, 0.5f};
    p.fx.bus[0].output = 6;
    p.channel[RS].output = 1;
    p.channel[SD].output = 2;
    auto e = makeEngine(kFs, Quality::Standard, p);
    Buses b(9600);
    renderEngine(*e, b, {{0, RS}}, kBlock);
    REQUIRE(busEnergy(b, 4) > 0.0);
    REQUIRE(busEnergy(b, 6) == 0.0);
    // The click is aligned with the voices: nothing before the common latency.
    REQUIRE(peak(b.left(4), 0, e->latencySamples()) == 0.0f);
    REQUIRE(peak(b.left(4), e->latencySamples(), e->latencySamples() + 48) > 0.0f);

    e->reset();
    Buses c(9600);
    renderEngine(*e, c, {{0, SD}}, kBlock);
    REQUIRE(busEnergy(c, 4) == 0.0);
    REQUIRE(busEnergy(c, 6) > 0.0);

    // Out 4 not connected: the click falls back to the main mix.
    {
        EngineParams q = p;
        q.channel[RS].output = 0;
        q.channel[RS].level = 0.0f;   // only the click on main
        auto e2 = makeEngine(kFs, Quality::Standard, q);
        Buses m(9600, 1);
        renderEngine(*e2, m, {{0, RS}}, kBlock);
        REQUIRE(energy(m.left()) > 0.0);
    }

    p.trig[0].on = false;
    e->setParams(p);
    e->reset();
    Buses d(9600);
    renderEngine(*e, d, {{0, RS}}, kBlock);
    REQUIRE(busEnergy(d, 4) == 0.0);
}

TEST_CASE("LEVEL, pan and spread", "[core][engine]") {
    auto render = [](const EngineParams& p) {
        auto e = makeEngine(kFs, Quality::Standard, p);
        auto b = std::make_unique<Buses>(4096);
        renderEngine(*e, *b, {{0, MT}}, kBlock);
        return b;
    };
    EngineParams p = defaultEngineParams();
    p.channel[MT].output = 1;
    const auto unity = render(p);
    const float pk = peak(unity->left(1));
    REQUIRE(pk > 0.0f);
    REQUIRE(same(unity->left(1), unity->right(1)));

    SECTION("LEVEL: 1.0 is +7 dB, 0 is silence, below the centre an audio taper") {
        p.channel[MT].level = 1.0f;
        REQUIRE_THAT(static_cast<double>(peak(render(p)->left(1)) / pk), WithinRel(2.2387, 1.0e-3));
        p.channel[MT].level = 0.25f;
        REQUIRE_THAT(static_cast<double>(peak(render(p)->left(1)) / pk), WithinRel(0.25, 1.0e-3));
        p.channel[MT].level = 0.0f;
        REQUIRE(peak(render(p)->left(1)) == 0.0f);
    }
    SECTION("pan: constant power, -3 dB at the centre") {
        p.channel[MT].pan = -1.0f;
        const auto left = render(p);
        REQUIRE(peak(left->right(1)) < 1.0e-7f * peak(left->left(1)));
        REQUIRE_THAT(static_cast<double>(pk / peak(left->left(1))), WithinRel(0.70710678, 1.0e-4));
        p.channel[MT].pan = 1.0f;
        const auto right = render(p);
        REQUIRE(peak(right->left(1)) < 1.0e-7f * peak(right->right(1)));
    }
    SECTION("spread: the right side delayed by up to 20 ms") {
        p.channel[MT].spread = 1.0f;
        const auto s = render(p);
        const int d = 960;
        REQUIRE(same(s->left(1), unity->left(1)));
        for (int i = 0; i < 4096; ++i) {
            const float expect = i < d ? 0.0f : s->left(1)[static_cast<std::size_t>(i - d)];
            REQUIRE(s->right(1)[static_cast<std::size_t>(i)] == expect);
        }
    }
}

TEST_CASE("Latency is the same at every quality and reported per rate", "[core][engine][latency]") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        auto e = std::make_unique<Engine>();
        e->prepare(fs, 256);
        const int l = e->latencySamples();
        std::printf("[latency] %.0f Hz: L = %d samples (%.3f ms)\n", fs, l, 1000.0 * l / fs);
        REQUIRE(l == dsp::pcm::maxKernelLatency(fs));
        for (Quality q : {Quality::Eco, Quality::Standard, Quality::High, Quality::Eco}) {
            e->configure(q, 1234U, 0.5f, Revision::Late);
            REQUIRE(e->latencySamples() == l);
        }
    }
}

TEST_CASE("Every voice is aligned to the common latency at every quality", "[core][engine][latency]") {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        // Onset per voice: the first sample above 2 % of the voice's peak.
        std::array<std::array<int, kNumVoices>, 3> onset{};
        int l = 0;
        for (int qi = 0; qi < 3; ++qi) {
            auto e = makeEngine(fs, static_cast<Quality>(qi), ownOuts());
            l = e->latencySamples();
            const int at = 1000;
            Buses b(static_cast<int>(fs * 0.1));
            std::vector<EngineHit> hits;
            for (int v = 0; v < kNumVoices; ++v) {
                if (v == OH) continue;   // CH and OH on one sample play open; OH gets its own run
                hits.push_back({at, static_cast<VoiceId>(v), Hit::accented(true, true, 1.0f)});
            }
            renderEngine(*e, b, hits, 256);
            e->reset();
            Buses bo(static_cast<int>(fs * 0.1));
            renderEngine(*e, bo, {{at, OH, Hit::accented(true, true, 1.0f)}}, 256);
            for (int v = 0; v < kNumVoices; ++v) {
                auto& x = (v == OH ? bo : b).left(v + 1);
                const float pk = peak(x);
                REQUIRE(pk > 0.0f);
                onset[static_cast<std::size_t>(qi)][static_cast<std::size_t>(v)] = firstAbove(x, 0.02f * pk) - at;
            }
        }
        for (int v = 0; v < kNumVoices; ++v) {
            const auto k = static_cast<std::size_t>(v);
            INFO(fs << " Hz voice " << kVoiceInfo[k].code << " onsets (Eco/Std/High) " << onset[0][k] << " "
                    << onset[1][k] << " " << onset[2][k] << ", L = " << l);
            // Never before the common latency, and within the voice's own attack after it.
            for (int qi = 0; qi < 3; ++qi) {
                REQUIRE(onset[static_cast<std::size_t>(qi)][k] >= l - 2);
                REQUIRE(onset[static_cast<std::size_t>(qi)][k] <= l + static_cast<int>(0.0025 * fs));
            }
            // The quality moves no voice by more than a sample or two.
            REQUIRE(std::abs(onset[0][k] - onset[2][k]) <= 2);
            REQUIRE(std::abs(onset[1][k] - onset[2][k]) <= 2);
        }
    }
}

namespace {

// A voice rendered on its own (no engine): its output and its latency.
std::vector<float> standalone(int v, double fs, int oversample, int n, int& latency) {
    VoiceSetup s;
    s.sampleRate = fs;
    s.oversample = oversample;
    s.revision = Revision::Early;
    std::vector<float> out(static_cast<std::size_t>(n)), other(static_cast<std::size_t>(n));
    std::vector<float> noise(static_cast<std::size_t>(n));
    dsp::WhiteNoise nz;
    nz.prepare(fs);
    // The engine's noise after a reset (its generator restarts from the unit
    // seed and the sample clock when the first noise voice wakes).
    nz.seed(dsp::hash32(dsp::hash32(0U, 0x300U), 0U));
    nz.fill(noise.data(), n);
    const Hit h = Hit::accented(true, true, 1.0f);
    auto go = [&](auto& voice) {
        voice.setParams(defaultParams(v));
        voice.trigger(h);
        if constexpr (requires { voice.render(out.data(), noise.data(), n); }) voice.render(out.data(), noise.data(), n);
        else voice.render(out.data(), n);
        latency = voice.latencySamples();
    };
    switch (v) {
        case BD: { voices::BassDrum x; x.prepare(s); go(x); break; }
        case SD: { voices::SnareDrum x; x.prepare(s); go(x); break; }
        case LT: case MT: case HT: { voices::Tom x; x.prepare(s, static_cast<VoiceId>(v)); go(x); break; }
        case RS: { voices::RimShot x; x.prepare(s); go(x); break; }
        case CP: { voices::HandClap x; x.prepare(s); go(x); break; }
        case CR: case RD: {
            voices::Cymbal x;
            x.prepare(s, static_cast<VoiceId>(v));
            x.setRoms(&factoryRom(v == CR ? RomSlot::Crash : RomSlot::Ride), nullptr);
            go(x);
            break;
        }
        default: {
            voices::HiHat x;
            x.prepare(s);
            x.setRoms(&factoryRom(RomSlot::Hat), nullptr);
            x.setParams(defaultParams(CH), defaultParams(OH));
            if (v == CH) x.triggerClosed(h); else x.triggerOpen(h);
            x.render(v == CH ? out.data() : other.data(), v == CH ? other.data() : out.data(), n);
            latency = x.latencySamples();
            break;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Engine onsets are the voices' own onsets moved to the common latency", "[core][engine][latency]") {
    // Early units: no multi-out RC, so the engine adds nothing to a voice's
    // onset but the alignment delay and linear gains.
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        auto e = makeEngine(fs, Quality::Standard, ownOuts());
        e->configure(Quality::Standard, 0U, 0.0f, Revision::Early);
        const int l = e->latencySamples();
        const int n = static_cast<int>(fs * 0.05);
        for (int v = 0; v < kNumVoices; ++v) {
            INFO(fs << " Hz voice " << kVoiceInfo[static_cast<std::size_t>(v)].code);
            int lat = 0;
            const auto ref = standalone(v, fs, e->oversampleFactor(), n, lat);
            e->reset();
            Buses b(n);
            renderEngine(*e, b, {{0, static_cast<VoiceId>(v), Hit::accented(true, true, 1.0f)}}, 256);
            const int refOnset = firstAbove(ref, 0.05f * peak(ref));
            const int onset = firstAbove(b.left(v + 1), 0.05f * peak(b.left(v + 1)));
            INFO("standalone onset " << refOnset << " (latency " << lat << "), engine onset " << onset << ", L " << l);
            REQUIRE(refOnset >= 0);
            REQUIRE(onset == refOnset + l - lat);
        }
    }
}

TEST_CASE("The shared accent DACs: a partner's hit rewrites the ringing voice's line", "[core][engine]") {
    auto run = [](VoiceId ringing, VoiceId partner, bool firePartner, Hit partnerHit) {
        EngineParams p = defaultEngineParams();
        p.channel[ringing].output = 1;
        p.channel[partner].output = 2;
        auto e = makeEngine(kFs, Quality::Standard, p);
        auto b = std::make_unique<Buses>(48000);
        std::vector<EngineHit> hits{{0, ringing, Hit::normal()}};
        if (firePartner) hits.push_back({9600, partner, partnerHit});
        renderEngine(*e, *b, hits, kBlock);
        return b;
    };
    SECTION("ride follows a crash hit, crash follows a ride hit") {
        for (auto [ringing, partner] : {std::pair{RD, CR}, std::pair{CR, RD}}) {
            const auto alone = run(ringing, partner, false, {});
            const auto loud = run(ringing, partner, true, Hit::accented(false, true, 1.0f));
            const auto same = run(ringing, partner, true, Hit::normal());
            const int from = 9600 + 2400;   // after the control smoothing has moved
            REQUIRE(energy(loud->left(1), from, 48000) > 1.5 * energy(alone->left(1), from, 48000));
            // The same voltage again changes nothing.
            REQUIRE(coretest::same(same->left(1), alone->left(1)));
            // Before the partner fires, nothing differs.
            for (int i = 0; i < 9600; ++i)
                REQUIRE(loud->left(1)[static_cast<std::size_t>(i)] == alone->left(1)[static_cast<std::size_t>(i)]);
        }
    }
    SECTION("rim shot and clap share a DAC") {
        // The clap reads its line while its trigger gate is open: a rim shot
        // inside that window with another voltage changes the clap.
        const auto alone = run(CP, RS, false, {});
        EngineParams p = defaultEngineParams();
        p.channel[CP].output = 1;
        p.channel[RS].output = 2;
        auto e = makeEngine(kFs, Quality::Standard, p);
        Buses b(9600);
        renderEngine(*e, b, {{0, CP, Hit::normal()}, {10, RS, Hit::accented(false, true, 1.0f)}}, kBlock);
        REQUIRE_FALSE(coretest::same(std::vector<float>(b.left(1).begin(), b.left(1).end()),
                                     std::vector<float>(alone->left(1).begin(), alone->left(1).begin() + 9600)));
    }
}

TEST_CASE("Hat choke: a closed hit cuts the open hat; CH + OH on one sample plays open", "[core][engine]") {
    auto run = [](bool choke, std::vector<EngineHit> hits) {
        EngineParams p = defaultEngineParams();
        p.channel[CH].output = 1;
        p.channel[OH].output = 2;
        p.hatChoke = choke;
        auto e = makeEngine(kFs, Quality::Standard, p);
        auto b = std::make_unique<Buses>(48000);
        renderEngine(*e, *b, hits, kBlock);
        return b;
    };
    const int cut = 9600;
    const auto chokeOn = run(true, {{0, OH}, {cut, CH}});
    const auto chokeOff = run(false, {{0, OH}, {cut, CH}});
    const auto openAlone = run(true, {{0, OH}});
    const int after = cut + 4800;
    REQUIRE(energy(chokeOn->left(2), after, 48000) < 1.0e-6 * energy(openAlone->left(2), after, 48000) + 1.0e-12);
    REQUIRE_THAT(energy(chokeOff->left(2), after, 48000), WithinRel(energy(openAlone->left(2), after, 48000), 1.0e-6));
    REQUIRE(energy(chokeOn->left(1)) > 0.0);
    REQUIRE(energy(chokeOff->left(1)) > 0.0);

    // Same sample, either call order: open, and nothing on the closed channel.
    const auto both1 = run(true, {{0, CH}, {0, OH}});
    const auto both2 = run(true, {{0, OH}, {0, CH}});
    REQUIRE(energy(both1->left(1)) == 0.0);
    REQUIRE(same(both1->left(2), openAlone->left(2)));
    REQUIRE(same(both2->left(2), openAlone->left(2)));
}

TEST_CASE("Late units: the multi-out RC softens the top on a plugged individual out", "[core][engine]") {
    // Plugged out (15.9 kHz pole) vs. the same channel through an empty FX bus
    // to the same out (the voice's jack unplugged: 18.7 kHz).
    EngineParams p = defaultEngineParams();
    p.channel[CH].output = 3;
    auto e1 = makeEngine(kFs, Quality::Standard, p);
    Buses a(9600);
    renderEngine(*e1, a, {{0, CH}}, kBlock);
    p.channel[CH].route = ChannelRoute::FxBus1;
    p.fx.bus[0].output = 3;
    auto e2 = makeEngine(kFs, Quality::Standard, p);
    Buses b(9600);
    renderEngine(*e2, b, {{0, CH}}, kBlock);
    REQUIRE(energy(a.left(3)) < energy(b.left(3)));
    REQUIRE(energy(a.left(3)) > 0.85 * energy(b.left(3)));
}

TEST_CASE("Same parameters, hits and seed give the same bytes; the seed matters", "[core][engine]") {
    auto render = [](std::uint32_t seed, float variance) {
        auto e = std::make_unique<Engine>();
        e->prepare(kFs, kBlock);
        e->configure(Quality::Standard, seed, variance, Revision::Late);
        e->setParams(defaultEngineParams());
        auto b = std::make_unique<Buses>(24000, 1);
        std::vector<EngineHit> hits;
        for (int v = 0; v < kNumVoices; ++v) hits.push_back({v * 1000, static_cast<VoiceId>(v)});
        renderEngine(*e, *b, hits, 333);
        return b;
    };
    const auto a = render(7, 1.0f);
    const auto b = render(7, 1.0f);
    const auto c = render(8, 1.0f);
    REQUIRE(same(a->left(), b->left()));
    REQUIRE(same(a->right(), b->right()));
    REQUIRE_FALSE(same(a->left(), c->left()));
}

TEST_CASE("configure re-prepares only when something changes", "[core][engine]") {
    auto e1 = makeEngine();
    auto e2 = makeEngine();
    Buses a(9600), b(9600);
    e1->trigger(LT, Hit::normal());
    e2->trigger(LT, Hit::normal());
    e1->render(a.view(), 0, 4800);
    e2->render(b.view(), 0, 4800);
    e2->configure(Quality::Standard, 0, 0.0f, Revision::Late);   // unchanged: a no-op
    e1->render(a.view(), 4800, 4800);
    e2->render(b.view(), 4800, 4800);
    REQUIRE(same(a.left(), b.left()));
    REQUIRE(e1->oversampleFactor() == 2);

    e2->configure(Quality::High, 0, 0.0f, Revision::Late);   // silences the voices
    REQUIRE(e2->oversampleFactor() == 4);
    REQUIRE_FALSE(e2->anyVoiceActive());
}

TEST_CASE("NaN, Inf and out-of-range parameters never reach the output", "[core][engine]") {
    std::mt19937 rng(99);
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                         -std::numeric_limits<float>::infinity(), 1.0e30f, -1.0e30f, -5.0f, 5.0e4f};
    auto pick = [&] { return bad[rng() % std::size(bad)]; };
    auto e = makeEngine(kFs, Quality::Standard, defaultEngineParams(), 256);
    for (int round = 0; round < 12; ++round) {
        EngineParams p = defaultEngineParams();
        for (auto& v : p.voice)
            for (auto& k : v.k) k = pick();
        for (auto& c : p.channel) {
            c.level = pick();
            c.pan = pick();
            c.spread = pick();
            c.drive = pick();
            c.send = pick();
            c.output = static_cast<std::uint8_t>(rng());
            c.route = static_cast<ChannelRoute>(rng() % 5);
            c.sendBus = static_cast<std::uint8_t>(rng() % 5);
        }
        for (auto& s : p.strip) {
            s.eqOn = true;
            s.compOn = true;
            s.lowCutHz = pick();
            s.highCutHz = pick();
            s.resonance = pick();
            s.band[0] = {true, pick(), pick(), pick()};
            s.compThresholdDb = pick();
            s.compRatio = pick();
            s.compAttackMs = pick();
            s.compReleaseMs = pick();
            s.compMakeupDb = pick();
        }
        for (auto& t : p.trig) {
            t.on = true;
            t.source = static_cast<std::uint8_t>(rng());
            t.pan = pick();
            t.level = pick();
            t.output = static_cast<std::uint8_t>(rng());
            t.route = static_cast<ChannelRoute>(rng() % 5);
        }
        for (auto& pl : p.fx.place) pl = {true, static_cast<std::uint8_t>(rng() % 3), static_cast<std::uint8_t>(rng())};
        for (auto& b : p.fx.bus) {
            b.levelDb = pick();
            b.toBus2 = pick();
            b.output = static_cast<std::uint8_t>(rng());
        }
        p.fx.reverb.decay = pick();
        p.fx.reverb.mix = pick();
        p.fx.delay.feedback = pick();
        p.fx.delay.timeMs = pick();
        p.fx.delay.sync = false;
        p.fx.distortion.preampDb = pick();
        p.fx.chorus.rateHz = pick();
        p.fx.bitcrusher.bits = pick();
        p.fx.filter.cutoffHz = pick();
        p.fx.dynamics[0].sidechain = static_cast<std::uint8_t>(rng() % 14);
        p.master.volumeDb = pick();
        p.master.drive = pick();
        p.master.compOn = true;
        p.master.limThresholdDb = pick();
        p.bpm = static_cast<double>(pick());
        e->setParams(p);
        Buses b(4800);
        std::vector<EngineHit> hits;
        for (int v = 0; v < kNumVoices; ++v) {
            Hit h{pick(), pick()};
            hits.push_back({static_cast<int>(rng() % 4000), static_cast<VoiceId>(v), h});
        }
        renderEngine(*e, b, hits, 256);
        for (int bus = 0; bus < kNumBuses; ++bus) {
            REQUIRE(allFinite(b.left(bus)));
            REQUIRE(allFinite(b.right(bus)));
        }
    }
    // Sane parameters again: still sounding, still finite.
    e->setParams(defaultEngineParams());
    e->reset();
    Buses b(4800);
    renderEngine(*e, b, {{0, BD}, {0, SD}, {0, CH}}, 256);
    REQUIRE(allFinite(b.left()));
    REQUIRE(peak(b.left()) > 0.01f);
}

TEST_CASE("Engine output does not depend on how the block is split", "[core][engine]") {
    EngineParams p = defaultEngineParams();
    p.channel[SD].spread = 0.4f;
    p.channel[SD].sendBus = 2;
    p.channel[SD].send = 0.5f;
    p.channel[CP].route = ChannelRoute::FxBus1;
    p.channel[CH].output = 4;
    p.strip[BD].compOn = true;
    p.strip[BD].compThresholdDb = -24.0f;
    p.strip[RS].eqOn = true;
    p.strip[RS].band[0] = {true, 800.0f, 1.0f, 9.0f};
    p.fx.place[static_cast<std::size_t>(FxId::Reverb)] = {true, 1, 0};
    p.fx.place[static_cast<std::size_t>(FxId::Delay)] = {true, 0, 0};
    p.trig[0].on = true;
    std::vector<EngineHit> hits;
    for (int i = 0; i < 40; ++i) hits.push_back({i * 1777 % 70000, static_cast<VoiceId>(i % kNumVoices)});
    auto render = [&](int block) {
        auto e = makeEngine(kFs, Quality::Standard, p, 512);
        auto b = std::make_unique<Buses>(96000);
        renderEngine(*e, *b, hits, block);
        return b;
    };
    const auto ref = render(512);
    for (int block : {1, 3, 64, 255, 256, 257, 500}) {
        INFO("block " << block);
        const auto x = render(block);
        for (int bus : {0, 4})
            for (int c = 0; c < 2; ++c) REQUIRE(same(ref->data[bus][c], x->data[bus][c]));
    }
}

TEST_CASE("Parameter glides end the same way whatever the partition", "[core][engine]") {
    // The parameters change at sample 4800 in every run; after that the block
    // sizes differ, so each glide ends at a different place inside a block.
    EngineParams a = defaultEngineParams();
    a.channel[SD].sendBus = 1;
    a.channel[SD].send = 0.2f;
    a.trig[0].on = true;
    a.fx.place[static_cast<std::size_t>(FxId::Delay)] = {true, 0, 0};
    EngineParams b = a;
    b.channel[BD].drive = 0.7f;
    b.channel[BD].level = 0.8f;
    b.channel[BD].pan = 0.5f;
    b.channel[SD].send = 0.9f;
    b.channel[SD].spread = 0.3f;
    b.channel[SD].drive = 0.0f;
    b.channel[LT].drive = 0.0f;
    a.channel[LT].drive = 0.5f;   // a glide down to drive 0 (a bypass)
    b.fx.bus[0].levelDb = -9.0f;
    b.trig[0].level = 0.9f;
    std::vector<EngineHit> hits;
    for (int i = 0; i < 24; ++i) hits.push_back({i * 2000, static_cast<VoiceId>(i % 7)});
    auto render = [&](int before, int after) {
        auto e = makeEngine(kFs, Quality::Standard, a, 512);
        auto out = std::make_unique<Buses>(48000);
        std::sort(hits.begin(), hits.end(), [](const EngineHit& x, const EngineHit& y) { return x.at < y.at; });
        std::size_t h = 0;
        for (int pos = 0; pos < out->length;) {
            if (pos == 4800) e->setParams(b);
            int n = std::min(pos < 4800 ? before : after, out->length - pos);
            if (pos < 4800) n = std::min(n, 4800 - pos);
            while (h < hits.size() && hits[h].at <= pos) e->trigger(hits[h].voice, hits[h++].hit);
            if (h < hits.size() && hits[h].at < pos + n) n = hits[h].at - pos;
            e->render(out->view(), pos, n);
            pos += n;
        }
        return out;
    };
    const auto ref = render(480, 512);
    for (auto [before, after] : {std::pair{1, 1}, std::pair{100, 7}, std::pair{4800, 333}, std::pair{32, 256}, std::pair{75, 129}}) {
        INFO("blocks " << before << " / " << after);
        const auto x = render(before, after);
        REQUIRE(same(ref->left(), x->left()));
        REQUIRE(same(ref->right(), x->right()));
    }
}

// Render with one parameter change and one configure at fixed samples, in a
// given block size, splitting at the hits.
namespace {
struct Change {
    int at = -1;
    EngineParams params{};
    Quality quality = Quality::Standard;
    Revision revision = Revision::Late;
    bool configure = false;
};

std::unique_ptr<Buses> renderWithChanges(const EngineParams& start, Quality q, Revision rev,
                                         std::vector<EngineHit> hits, std::vector<Change> changes, int total,
                                         int block) {
    auto e = std::make_unique<Engine>();
    e->prepare(kFs, 8192);
    e->configure(q, 0, 0.0f, rev);
    e->setParams(start);
    auto out = std::make_unique<Buses>(total);
    std::sort(hits.begin(), hits.end(), [](const EngineHit& x, const EngineHit& y) { return x.at < y.at; });
    std::size_t h = 0, c = 0;
    for (int pos = 0; pos < total;) {
        while (c < changes.size() && changes[c].at <= pos) {
            if (changes[c].configure) e->configure(changes[c].quality, 0, 0.0f, changes[c].revision);
            else e->setParams(changes[c].params);
            ++c;
        }
        while (h < hits.size() && hits[h].at <= pos) {
            e->trigger(hits[h].voice, hits[h].hit);
            ++h;
        }
        int n = std::min(block, total - pos);
        if (h < hits.size() && hits[h].at < pos + n) n = hits[h].at - pos;
        if (c < changes.size() && changes[c].at < pos + n) n = changes[c].at - pos;
        e->render(out->view(), pos, n);
        pos += n;
    }
    return out;
}
}  // namespace

TEST_CASE("A sleeping channel's spread line holds silence: raising spread later replays nothing", "[core][engine]") {
    SECTION("after a configure() cut") {
        // A crash rings with spread 0, a quality change cuts it, then spread
        // goes to 0.7 (672 samples) while the channel sleeps and a new hit
        // comes in: the right side stays silent until the new hit's Haas delay.
        EngineParams p = defaultEngineParams();
        p.channel[CR].output = 1;
        EngineParams wide = p;
        wide.channel[CR].spread = 0.7f;
        Change cut;
        cut.at = 20000;
        cut.configure = true;
        cut.quality = Quality::High;
        Change spread;
        spread.at = 22000;
        spread.params = wide;
        const auto b = renderWithChanges(p, Quality::Standard, Revision::Late, {{0, CR}, {25000, CR, Hit::weak()}},
                                         {cut, spread}, 30000, 512);
        REQUIRE(peak(b->left(1), 0, 20000) > 0.01f);
        // (Up to the Haas delay itself: the oversampling filters may let the
        // new hit's onset start a few samples before the latency.)
        const int quietUntil = 25000 + 672;
        REQUIRE(peak(b->right(1), 20000, quietUntil) == 0.0f);
        REQUIRE(peak(b->right(1), quietUntil, 30000) > 0.0f);
    }
    SECTION("after a natural sleep, whatever the partition") {
        for (VoiceId v : {BD, RS, CH})
            for (Revision rev : {Revision::Early, Revision::Late}) {
                EngineParams p = defaultEngineParams();
                p.channel[v].output = 1;
                EngineParams q = p;
                q.channel[v].spread = 0.7f;
                q.channel[v].level = 0.7f;
                q.channel[v].drive = 0.3f;
                Change c;
                c.at = 40000;
                c.params = q;
                const std::vector<EngineHit> hits{{0, v}, {41000, v}};
                const auto ref = renderWithChanges(p, Quality::Eco, rev, hits, {c}, 48000, 8192);
                for (int block : {1, 333}) {
                    INFO("voice " << v << " revision " << static_cast<int>(rev) << " block " << block);
                    const auto x = renderWithChanges(p, Quality::Eco, rev, hits, {c}, 48000, block);
                    REQUIRE(same(ref->left(1), x->left(1)));
                    REQUIRE(same(ref->right(1), x->right(1)));
                }
            }
    }
}

TEST_CASE("A revision change mid-tail leaves no multi-out RC state behind", "[core][engine]") {
    // Late -> Early while ringing, then back to Late with nothing playing: the
    // output stays exactly silent (no stale RC state resumes), and the channel
    // can sleep in between.
    EngineParams p = defaultEngineParams();
    for (int v = 0; v < kNumVoices; ++v) p.channel[static_cast<std::size_t>(v)].output = static_cast<std::uint8_t>(v + 1);
    Change early;
    early.at = 2000;
    early.configure = true;
    early.revision = Revision::Early;
    Change late = early;
    late.at = 30000;
    late.revision = Revision::Late;
    std::vector<EngineHit> hits;
    for (int v = 0; v < kNumVoices; ++v)
        if (v != CH) hits.push_back({0, static_cast<VoiceId>(v)});   // CH + OH together would play open only
    const auto b = renderWithChanges(p, Quality::Standard, Revision::Late, hits, {early, late}, 40000, 512);
    for (int v = 0; v < kNumVoices; ++v) {
        INFO("voice " << v);
        if (v != CH) REQUIRE(peak(b->left(v + 1), 0, 2000) > 0.0f);
        REQUIRE(peak(b->left(v + 1), 30000) == 0.0f);
        REQUIRE(peak(b->right(v + 1), 30000) == 0.0f);
    }
}
