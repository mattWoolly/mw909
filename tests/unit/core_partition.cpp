// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The whole Machine is a pure function of its inputs, whatever the host's
// block partition (1..8192 samples): a dense pattern with flams and sub-steps,
// live MIDI hits, channel strips, sends, FX on both buses, spread, trigger
// clicks and individual outs all give the same bytes on every bus. Also: the
// same render twice is identical, and an extreme-density pattern loses no
// hit to the block size (the sequencer runs in ranges of at most 2048).

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>

#include "CoreTestUtil.h"

using namespace mw909;
using namespace coretest;

namespace {

constexpr double kFs = 48000.0;
constexpr int kMaxBlock = 8192;

MachineParams busyParams(seq::Mode mode) {
    MachineParams p = defaultMachineParams();
    p.seq.mode = mode;
    p.seq.run = true;
    p.seq.internalBpm = 137.0;
    p.seq.jitter = true;
    p.unitSeed = 4242;
    p.variance = 0.7f;
    auto& e = p.engine;
    e.channel[BD].drive = 0.4f;
    e.channel[SD].spread = 0.5f;
    e.channel[SD].sendBus = 2;
    e.channel[SD].send = 0.6f;
    e.channel[CP].route = ChannelRoute::FxBus1;
    e.channel[CP].pan = -0.4f;
    e.channel[RS].sendBus = 1;
    e.channel[RS].send = 0.5f;
    e.channel[RS].sendPre = true;
    e.channel[CH].output = 3;
    e.channel[OH].output = 3;
    e.channel[OH].pan = 0.7f;
    e.channel[LT].output = 5;
    e.channel[RD].output = 9;   // not connected in one of the runs: falls back to main
    e.strip[BD].compOn = true;
    e.strip[BD].compThresholdDb = -30.0f;
    e.strip[BD].eqOn = true;
    e.strip[BD].band[0] = {true, 90.0f, 1.0f, 4.0f};
    e.strip[SD].eqOn = true;
    e.strip[SD].highCutHz = 6000.0f;
    e.strip[SD].resonance = 0.5f;
    e.strip[CH].compOn = true;
    e.strip[CH].compPreEq = true;
    e.strip[CH].eqOn = true;
    e.strip[CH].lowCutHz = 400.0f;
    e.fx.place[static_cast<std::size_t>(FxId::Delay)] = {true, 0, 0};
    e.fx.place[static_cast<std::size_t>(FxId::Distortion)] = {true, 0, 1};
    e.fx.place[static_cast<std::size_t>(FxId::Reverb)] = {true, 1, 0};
    e.fx.place[static_cast<std::size_t>(FxId::DynamicsA)] = {true, 1, 1};
    e.fx.dynamics[0].sidechain = BD;
    e.fx.place[static_cast<std::size_t>(FxId::Filter)] = {true, 1, 2};
    e.fx.filter.lfoSync = true;
    e.fx.bus[0].toBus2 = 0.3f;
    e.fx.bus[1].output = 2;
    e.trig[0].on = true;
    e.trig[0].output = 6;
    e.trig[1] = {true, CP, ChannelRoute::FxBus2, 0, 0.3f, 0.6f};
    e.master.compOn = true;
    e.master.stage = OutputStage::Original;
    return p;
}

std::vector<MidiMessage> liveNotes(int total) {
    std::vector<MidiMessage> out;
    std::mt19937 rng(5);
    for (int t = 1000; t < total; t += 1000 + static_cast<int>(rng() % 9000)) {
        const int notes[] = {36, 38, 39, 42, 46, 49, 51, 37, 45};
        out.push_back(noteOn(t, notes[rng() % 9], 30 + static_cast<int>(rng() % 98), 10));
    }
    return out;
}

struct Render {
    std::unique_ptr<Buses> buses;
    std::vector<MidiMessage> midiOut;
    std::array<std::uint32_t, kNumVoices> hits{};
};

Render render(const MachineParams& params, const std::vector<int>& partition, int total, int connected,
              double bpm = 120.0, const seq::Pattern& pattern = densePattern()) {
    auto m = std::make_unique<Machine>();
    m->prepare(kFs, kMaxBlock);
    postPattern(*m, 0, pattern);
    Runner r{*m};
    r.params = params;
    r.params.midi.midiOut = true;
    r.bpm = bpm;
    r.startPpq = -0.37;   // pre-roll: the transport starts mid-beat before zero
    r.midi = liveNotes(total);
    Render out;
    out.buses = std::make_unique<Buses>(total, connected);
    r.runPartition(*out.buses, total, partition);
    out.midiOut = r.midiOut;
    for (int v = 0; v < kNumVoices; ++v) out.hits[static_cast<std::size_t>(v)] = m->playState().hits[static_cast<std::size_t>(v)].load();
    return out;
}

bool sameBuses(const Render& a, const Render& b) {
    for (int bus = 0; bus < kNumBuses; ++bus)
        for (int c = 0; c < 2; ++c)
            if (a.buses->data[bus][c] != b.buses->data[bus][c]) return false;
    return true;
}

bool sameMidi(const std::vector<MidiMessage>& a, const std::vector<MidiMessage>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].offset != b[i].offset || a[i].status != b[i].status || a[i].data1 != b[i].data1 ||
            a[i].data2 != b[i].data2)
            return false;
    return true;
}

}  // namespace

TEST_CASE("Machine output is bytewise the same for every block partition", "[core][partition]") {
    const int total = static_cast<int>(kFs * 3.0);
    for (seq::Mode mode : {seq::Mode::HostSync, seq::Mode::Internal}) {
        for (int connected : {kNumBuses, 4}) {
            const MachineParams p = busyParams(mode);
            const Render ref = render(p, {512}, total, connected);
            REQUIRE(energy(ref.buses->left()) > 0.0);
            REQUIRE(ref.hits[BD] > 10U);
            std::vector<std::vector<int>> partitions = {{1},   {2},    {3},    {5},    {16},   {64},   {100},
                                                        {127}, {128},  {255},  {333},  {1000}, {2047}, {2048},
                                                        {2049}, {4096}, {8192}, {1, 8192, 7, 4095, 64, 3}};
            std::mt19937 rng(17);
            std::vector<int> random;
            for (int i = 0; i < 200; ++i) random.push_back(1 + static_cast<int>(rng() % 8192));
            partitions.push_back(random);
            for (const auto& part : partitions) {
                INFO("mode " << static_cast<int>(mode) << ", connected buses " << connected << ", partition starts "
                             << part[0] << " (" << part.size() << " sizes)");
                const Render x = render(p, part, total, connected);
                REQUIRE(x.hits == ref.hits);
                REQUIRE(sameBuses(ref, x));
                REQUIRE(sameMidi(ref.midiOut, x.midiOut));
            }
        }
    }
}

TEST_CASE("Rendering twice gives the same bytes", "[core][partition]") {
    const MachineParams p = busyParams(seq::Mode::HostSync);
    const int total = static_cast<int>(kFs * 2.0);
    const Render a = render(p, {333}, total, kNumBuses);
    const Render b = render(p, {333}, total, kNumBuses);
    REQUIRE(sameBuses(a, b));
}

TEST_CASE("An extreme-density pattern loses no hit to the host block size", "[core][partition]") {
    // 32nd notes at 1000 BPM, four sub-steps and a flam on every step of every
    // voice: 55 hits per 360-sample step, ~1250 in an 8192-sample block (more
    // than one sequencer EventBuffer holds; the Machine's 2048-sample ranges
    // keep each under ~420).
    seq::Pattern p;
    p.scale = seq::Scale::ThirtySecond;
    p.length = 16;
    p.flamMs = 1;
    for (int v = 0; v < kNumVoices; ++v)
        for (int s = 0; s < 16; ++s) {
            auto& c = p.at(0, v, s);
            c.setLevel(seq::StepLevel::Normal);
            c.setSubSteps(4);
            c.setFlam(true);
        }
    MachineParams params = defaultMachineParams();
    params.seq.mode = seq::Mode::HostSync;
    const int total = static_cast<int>(kFs * 0.5);
    const Render ref = render(params, {64}, total, 1, 1000.0, p);
    std::uint32_t sum = 0;
    for (auto h : ref.hits) sum += h;
    REQUIRE(sum > 3500U);
    for (int block : {8192, 4096, 2049, 1000}) {
        INFO("block " << block);
        const Render x = render(params, {block}, total, 1, 1000.0, p);
        REQUIRE(x.hits == ref.hits);
        REQUIRE(sameBuses(ref, x));
    }
}
