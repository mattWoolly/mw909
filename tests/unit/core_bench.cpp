// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// CPU bench (architecture.md §4.2, §7): a dense all-voices pattern at 48 kHz,
// Standard quality, in 128-sample blocks.
//   * strips and FX off: budget 8 % of one core (1.67 us per sample);
//   * a typical FX setup (strips, send reverb and delay, master compressor):
//     budget 12 %;
//   * worst block under 25 % of its deadline (median, 99.9th percentile and
//     worst are printed; each figure is the best of three runs);
//   * idle (nothing playing): reported;
//   * the block that reconfigures the voices (a quality, seed, variance or
//     revision change re-prepares them on the audio thread) under 25 % of its
//     deadline, at 128 and at 64 samples, the dense pattern playing (90th
//     percentile over 120 changes at varied phases; median and worst printed).
// The figures are printed. They are enforced only in an optimised build without
// sanitizers. The ordinary (parallel ctest) run allows headroom for a loaded
// test machine, so a shared CI box does not fail on noise. The real budgets
// (8 %, 12 %, 25 %, with 5 % measurement tolerance) are enforced by a dedicated
// quiet run, outside the parallel ctest pass and pinned to one core:
//   MW909_BENCH_STRICT=1 taskset -c 2 ./mw909_test_core_bench "[bench]"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "CoreTestUtil.h"

using namespace mw909;
using namespace coretest;

namespace {

#if defined(__OPTIMIZE__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
constexpr bool kEnforce = true;
#else
constexpr bool kEnforce = false;
#endif

// Headroom on the budgets: the real budgets plus measurement tolerance in a
// strict run, generous slack for a loaded shared machine otherwise.
bool strictRun() {
    const char* e = std::getenv("MW909_BENCH_STRICT");
    return e != nullptr && std::strcmp(e, "0") != 0 && e[0] != '\0';
}
double cpuSlack() { return strictRun() ? 1.05 : 1.5; }
double blockSlack() { return strictRun() ? 1.05 : 2.0; }

constexpr double kFs = 48000.0;
constexpr int kBlock = 128;

struct BenchResult {
    double nsPerSample = 0.0;
    double corePercent = 0.0;
    double medianBlockPercent = 0.0;
    double p999BlockPercent = 0.0;
    double worstBlockPercent = 0.0;
};

BenchResult benchOnce(const MachineParams& params, bool play, double seconds) {
    auto m = std::make_unique<Machine>();
    m->prepare(kFs, kBlock);
    postPattern(*m, 0, densePattern());
    Buses buses(kBlock);
    MidiOutBuffer midiOut;
    MachineParams p = params;
    p.seq.mode = seq::Mode::Internal;
    p.seq.run = play;
    p.seq.internalBpm = 132.0;

    std::vector<double> times;
    auto runBlocks = [&](int blocks, bool record) {
        for (int b = 0; b < blocks; ++b) {
            Machine::ProcessArgs a;
            a.out = buses.view();
            a.numSamples = kBlock;
            a.midiOut = &midiOut;
            const auto t0 = std::chrono::steady_clock::now();
            m->process(a, p);
            const auto t1 = std::chrono::steady_clock::now();
            if (record) times.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
    };

    runBlocks(static_cast<int>(kFs / kBlock), false);   // 1 s warm-up: caches, first hits
    const int blocks = static_cast<int>(seconds * kFs / kBlock);
    times.reserve(static_cast<std::size_t>(blocks));
    const auto t0 = std::chrono::steady_clock::now();
    runBlocks(blocks, true);
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    std::sort(times.begin(), times.end());
    const double deadline = 1.0e9 * kBlock / kFs;
    BenchResult r;
    r.nsPerSample = ns / (static_cast<double>(blocks) * kBlock);
    r.corePercent = 100.0 * r.nsPerSample / (1.0e9 / kFs);
    r.medianBlockPercent = 100.0 * times[times.size() / 2] / deadline;
    r.p999BlockPercent = 100.0 * times[times.size() * 999 / 1000] / deadline;
    r.worstBlockPercent = 100.0 * times.back() / deadline;
    return r;
}

// Best of three runs per figure: a shared machine's scheduler and clock
// changes only ever add time, so the minimum is the measurement.
BenchResult bench(const MachineParams& params, bool play, double seconds) {
    BenchResult best = benchOnce(params, play, seconds);
    for (int i = 0; i < 2; ++i) {
        const BenchResult r = benchOnce(params, play, seconds);
        best.nsPerSample = std::min(best.nsPerSample, r.nsPerSample);
        best.corePercent = std::min(best.corePercent, r.corePercent);
        best.medianBlockPercent = std::min(best.medianBlockPercent, r.medianBlockPercent);
        best.p999BlockPercent = std::min(best.p999BlockPercent, r.p999BlockPercent);
        best.worstBlockPercent = std::min(best.worstBlockPercent, r.worstBlockPercent);
    }
    return best;
}

MachineParams typicalFx() {
    MachineParams p = defaultMachineParams();
    auto& e = p.engine;
    // Strips: EQ + compressor on the kick and snare, a low cut on the hats.
    for (int v : {BD, SD}) {
        auto& s = e.strip[static_cast<std::size_t>(v)];
        s.eqOn = true;
        s.lowCutHz = 30.0f;
        s.band[0] = {true, 120.0f, 1.0f, 3.0f};
        s.band[1] = {true, 4000.0f, 1.5f, -2.0f};
        s.compOn = true;
        s.compThresholdDb = -18.0f;
        s.compRatio = 4.0f;
    }
    for (int v : {CH, OH}) {
        auto& s = e.strip[static_cast<std::size_t>(v)];
        s.eqOn = true;
        s.lowCutHz = 300.0f;
    }
    // Reverb on bus 2 fed by snare, clap, rim and the hats; delay on bus 1 from the rim and toms.
    auto& fx = e.fx;
    fx.place[static_cast<std::size_t>(FxId::Reverb)] = {true, 1, 0};
    fx.place[static_cast<std::size_t>(FxId::Delay)] = {true, 0, 0};
    fx.place[static_cast<std::size_t>(FxId::EqA)] = {true, 0, 1};
    for (int v : {SD, CP, RS, CH, OH}) {
        auto& c = e.channel[static_cast<std::size_t>(v)];
        c.sendBus = 2;
        c.send = 0.3f;
    }
    for (int v : {LT, MT, HT}) {
        auto& c = e.channel[static_cast<std::size_t>(v)];
        c.sendBus = 1;
        c.send = 0.25f;
    }
    e.master.compOn = true;
    return p;
}

void report(const char* name, const BenchResult& r) {
    std::printf("[bench%s] %-32s %7.1f ns/sample %6.2f %% core | block vs deadline: median %5.2f %%, "
                "p99.9 %5.2f %%, worst %5.2f %%\n",
                strictRun() ? " strict" : "", name, r.nsPerSample, r.corePercent, r.medianBlockPercent,
                r.p999BlockPercent, r.worstBlockPercent);
    std::fflush(stdout);
}

}  // namespace

TEST_CASE("CPU: dense pattern, 48 kHz Standard, strips and FX off", "[core][bench]") {
    const BenchResult r = bench(defaultMachineParams(), true, 6.0);
    report("dense, strips/FX off", r);
    if (kEnforce) {
        CHECK(r.corePercent < 8.0 * cpuSlack());
        CHECK(r.worstBlockPercent < 25.0 * blockSlack());
    }
}

TEST_CASE("CPU: dense pattern with a typical FX setup", "[core][bench]") {
    const BenchResult r = bench(typicalFx(), true, 6.0);
    report("dense, typical strips + FX", r);
    if (kEnforce) {
        CHECK(r.corePercent < 12.0 * cpuSlack());
        CHECK(r.worstBlockPercent < 25.0 * blockSlack());
    }
}

TEST_CASE("CPU: idle machine", "[core][bench]") {
    const BenchResult r = bench(defaultMachineParams(), false, 3.0);
    report("idle (stopped, nothing ringing)", r);
    if (kEnforce) CHECK(r.corePercent < 1.0);
}

// The blocks that apply a change of `kind` (0 quality, 1 seed, 2 variance, 3
// revision), in % of their deadline: `changes` changes, each after a gap of a
// pseudo-random 19..61 blocks so the change lands at every phase of the
// pattern (on a step, where every voice triggers in the dense pattern, and
// between steps, while they ring).
struct ReconfigureResult {
    double median = 0.0;
    double p90 = 0.0;
    double worst = 0.0;
};

ReconfigureResult reconfigureRun(int block, int kind, int changes) {
    auto m = std::make_unique<Machine>();
    m->prepare(kFs, block);
    postPattern(*m, 0, densePattern());
    Buses buses(block);
    MidiOutBuffer midiOut;
    MachineParams p = defaultMachineParams();
    p.seq.mode = seq::Mode::Internal;
    p.seq.run = true;
    p.seq.internalBpm = 132.0;
    p.variance = 0.5f;   // so that a seed change moves every part (and the sample voices' filters)
    auto run = [&]() {
        Machine::ProcessArgs a;
        a.out = buses.view();
        a.numSamples = block;
        a.midiOut = &midiOut;
        const auto t0 = std::chrono::steady_clock::now();
        m->process(a, p);
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(t1 - t0).count();
    };
    for (int b = 0; b < static_cast<int>(kFs / block); ++b) run();   // 1 s warm-up
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(changes));
    std::uint32_t lcg = 0x9E3779B9U + static_cast<std::uint32_t>(block * 4 + kind);
    for (int c = 0; c < changes; ++c) {
        switch (kind) {
            case 0: p.quality = static_cast<Quality>((static_cast<int>(p.quality) + 1) % 3); break;
            case 1: ++p.unitSeed; break;
            case 2: p.variance = p.variance == 0.5f ? 0.7f : 0.5f; break;
            default: p.revision = p.revision == Revision::Late ? Revision::Early : Revision::Late; break;
        }
        times.push_back(run());
        lcg = lcg * 1664525U + 1013904223U;
        const int gap = 19 + static_cast<int>((lcg >> 16) % 43U);
        for (int b = 0; b < gap; ++b) run();   // the voices ring again
    }
    std::sort(times.begin(), times.end());
    const double deadline = 1.0e9 * block / kFs;
    const auto at = [&](double q) {
        return 100.0 * times[static_cast<std::size_t>(q * static_cast<double>(times.size() - 1) + 0.5)] / deadline;
    };
    return {at(0.5), at(0.9), 100.0 * times.back() / deadline};
}

// Enforced on the 90th percentile over many changes: the worst single change
// block is reported, but on a shared machine it is no larger than the worst
// ordinary block (an interrupt or a migration lands on any block). The 25 %
// rule is set at 48 kHz; at 96 and 192 kHz in 64-sample blocks, with variance
// above 0, the three sample voices' kernel builds alone take a quarter of the
// deadline or more (architecture.md §7).
TEST_CASE("CPU: the block that reconfigures the voices", "[core][bench]") {
    static const char* const kKinds[4] = {"quality", "unit seed", "variance", "revision"};
    for (int block : {128, 64}) {
        for (int kind = 0; kind < 4; ++kind) {
            const ReconfigureResult r = reconfigureRun(block, kind, 120);
            std::printf("[bench%s] reconfigure (%-9s), %3d-sample blocks, 120 changes: median %5.2f %%, "
                        "p90 %5.2f %%, worst %5.2f %% of the deadline\n",
                        strictRun() ? " strict" : "", kKinds[kind], block, r.median, r.p90, r.worst);
            std::fflush(stdout);
            if (kEnforce) CHECK(r.p90 < 25.0 * blockSlack());
        }
    }
}

TEST_CASE("CPU: the Eco and High qualities", "[core][bench]") {
    for (Quality q : {Quality::Eco, Quality::High}) {
        MachineParams p = defaultMachineParams();
        p.quality = q;
        const BenchResult r = bench(p, true, 3.0);
        report(q == Quality::Eco ? "dense, Eco" : "dense, High", r);
    }
    SUCCEED();
}
