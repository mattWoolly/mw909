// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The `bench` subcommand: realtime cost of a full Machine (docs/design/
// architecture.md §4.2 and §7).
//
//   dense      every voice retriggered on every step (x2..x4 sub-steps,
//              flams, accents, total accent, shuffle) so all 11 ring
//              continuously; 48 kHz, Standard quality, 128-sample blocks.
//                * "no strips/FX": the stock machine (default master)
//                * "typical FX":   the tool's typical mix (strips on five
//                                  voices, two FX buses with four effects,
//                                  trigger out, the full master chain)
//   silent     the same machine with the sequencer off: what an idle
//              instance costs (free-running sources, master chain)
//   steady     every voice auditioned at the start of every block, so the
//              load has no musical cadence (period 1): any fold here, as in
//              the silent engine, is the engine's own periodic work
//
// Each scenario runs `repeats` independent renders of `seconds` of audio
// after a 0.5 s untimed warm-up and times every process() call. Reported:
//   ns/sample  median over repeats of (total time / samples)
//   CPU %      that as a share of one core in real time
//   worst %    the slowest block against its deadline (block / rate): the
//              best-of-N value (smallest per-repeat maximum, which filters
//              one-off scheduler preemptions) and the raw maximum
//   histogram  block times in bands of the deadline
//   periodic   block times folded at every period of 2..--fold-max blocks
//              (default 512, limited to a run's blocks / 8), phase medians
//              pooled over the phase-aligned repeats, against the same fold
//              of a shuffled copy (chance level). Work that happens every N
//              blocks shows as a ratio well above 1 at period N. The dense
//              scenarios' folds include the pattern's own cadence (hits are
//              periodic by nature; the step, shuffle-pair and pattern lengths
//              in blocks are printed, and at most tempos they are not whole
//              block counts, so the fold smears them over neighbouring
//              periods). The silent and steady scenarios have no cadence:
//              their folds are gated. --inject-spike <N> holds every N-th
//              block to 30 % of its deadline, to show the gate fires.
// The benchmark thread is pinned to the CPU it starts on (--no-pin to leave
// it to the scheduler), so a migration does not show as a slow block.
// Gates (exit 1, unless --no-gate): dense no-FX under --budget (8 %), dense
// typical-FX under --fx-budget (12 %), worst block of dense / typical-FX /
// silent under --gate (25 %), and no periodic spike in the silent or steady
// engine: fold ratio > 1.5 with an excess over --spike-gate (5 % of the
// deadline).
// --matrix adds the rate x quality x block table; --per-voice the cost of
// each voice alone.

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Common.h"

#ifndef MW909_VERSION
#define MW909_VERSION "unknown"
#endif
#ifndef MW909_BUILD_TYPE
#define MW909_BUILD_TYPE "unknown"
#endif

namespace mw909::tools {

namespace {

seq::Pattern densePattern()
{
    seq::Pattern p;
    p.length = 16;
    p.scale = seq::Scale::Sixteenth;
    p.shuffle = seq::shuffleTicksForKey(3);
    p.flamMs = 24;
    for (int v = 0; v < kNumVoices; ++v)
    {
        for (int s = 0; s < 16; ++s)
        {
            // CH chokes OH (one hat circuit): keep CH sparse so the open hat
            // rings (the long case) while both stay busy.
            if (v == CH && s % 4 != 3)
                continue;
            if (v == OH && s % 4 == 3)
                continue;
            seq::Step& st = p.at(0, v, s);
            st.setLevel(s % 2 == 0 ? seq::StepLevel::Accent : seq::StepLevel::Normal);
            st.setSubSteps(2 + (s + v) % 3);
            st.setFlam(s % 4 == 2);
        }
    }
    for (int s = 0; s < 16; s += 4)
        p.at(0, seq::kAccentTrack, s).setLevel(seq::StepLevel::Normal);
    return p;
}

enum class Scenario
{
    Dense,
    DenseFx,
    Silent,
    Steady
};

const char* scenarioName(Scenario s)
{
    switch (s)
    {
        case Scenario::Dense:
            return "dense, no strips/FX";
        case Scenario::DenseFx:
            return "dense, typical FX";
        case Scenario::Silent:
            return "silent engine";
        case Scenario::Steady:
            return "steady, every block";
    }
    return "?";
}

MachineParams scenarioParams(Scenario s, Quality q, double tempo)
{
    MachineParams params;
    params.engine = stockEngine();
    for (std::size_t v = 0; v < kNumVoices; ++v)
    {
        params.engine.channel[v].pan = (static_cast<float>(v) - 5.0f) / 6.0f;
    }
    if (s == Scenario::DenseFx)
        applyTypicalMix(params.engine);
    params.engine.bpm = tempo;
    params.seq.mode = (s == Scenario::Silent || s == Scenario::Steady) ? seq::Mode::Off : seq::Mode::HostSync;
    params.seq.totalAccent = 0.8f;
    params.quality = q;
    return params;
}

struct RunResult
{
    double totalNs = 0.0;
    double worstNs = 0.0;
    std::int64_t samples = 0;
    std::vector<double> blockNs;
};

// --inject-spike: extra busy time added inside the timed window of every
// injectEvery-th block (0 = off), to check that the periodicity gate fires.
int g_injectEvery = 0;
double g_injectNs = 0.0;

RunResult runOnce(Scenario sc, double rate, Quality q, int block, double seconds, double tempo,
                  const seq::Pattern& pattern, const std::array<bool, kNumVoices>& mute, bool keepBlocks)
{
    auto machine = std::make_unique<Machine>();
    machine->prepare(rate, block);
    machine->sequencer().setPattern(0, pattern);
    machine->sequencer().selectPattern(0);
    MachineParams params = scenarioParams(sc, q, tempo);
    params.mute = mute;

    // Every bus connected, as a host with all outputs enabled.
    BusStorage storage(block, true);
    const BusBuffers out = storage.view(0);

    const auto warmBlocks = static_cast<std::int64_t>(std::ceil(0.5 * rate / block));
    const auto timedBlocks = static_cast<std::int64_t>(std::ceil(seconds * rate / block));

    RunResult res;
    if (keepBlocks)
        res.blockNs.reserve(static_cast<std::size_t>(timedBlocks));
    std::int64_t pos = 0;
    for (std::int64_t b = 0; b < warmBlocks + timedBlocks; ++b)
    {
        Machine::ProcessArgs a;
        a.transport.hostValid = true;
        a.transport.playing = true;
        a.transport.bpm = tempo;
        a.transport.ppq = static_cast<double>(pos) * tempo / (60.0 * rate);
        a.out = out;
        a.numSamples = block;
        if (sc == Scenario::Steady)
            for (int v = 0; v < kNumVoices; ++v)
            {
                Command c;
                c.type = Command::Type::Audition;
                c.index = v;
                c.value = AccentDac::volts(AccentDac::kNormalCode);
                (void)machine->post(c);
            }

        const auto t0 = std::chrono::steady_clock::now();
        machine->process(a, params);
        if (g_injectEvery > 0 && b % g_injectEvery == 0)
            while (static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count())
                   < g_injectNs)
            {
            }
        const auto t1 = std::chrono::steady_clock::now();
        pos += block;

        if (b < warmBlocks)
            continue;
        const auto ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        res.totalNs += ns;
        res.worstNs = std::max(res.worstNs, ns);
        res.samples += block;
        if (keepBlocks)
            res.blockNs.push_back(ns);
    }
    // Keep the output alive so nothing is optimised away.
    volatile float sink = storage.left(0)[0] + storage.right(0)[static_cast<std::size_t>(block - 1)];
    (void)sink;
    return res;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::string cpuModel()
{
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("model name", 0) == 0)
        {
            const std::size_t colon = line.find(':');
            return colon == std::string::npos ? line : line.substr(colon + 2);
        }
    return "unknown CPU";
}

struct Summary
{
    double nsPerSample = 0.0;
    double cpuPct = 0.0;
    double worstPct = 0.0;    // best-of-N
    double worstAllPct = 0.0; // raw maximum
    double p999Pct = 0.0;
    int foldPeriod = 0;
    double foldRatio = 1.0;
    double foldExcessPct = 0.0;
};

// Repeats one scenario and prints its report; returns the summary.
Summary runScenario(Scenario sc, double rate, Quality q, int block, double seconds, int repeats, double tempo,
                    const seq::Pattern& pattern, int foldMax, bool details)
{
    const std::array<bool, kNumVoices> noMute{};
    std::vector<double> avgs, worsts;
    std::vector<std::vector<double>> runs;
    double worstAll = 0.0;
    for (int i = 0; i < repeats; ++i)
    {
        RunResult res = runOnce(sc, rate, q, block, seconds, tempo, pattern, noMute, true);
        avgs.push_back(res.totalNs / static_cast<double>(res.samples));
        worsts.push_back(res.worstNs);
        worstAll = std::max(worstAll, res.worstNs);
        runs.push_back(std::move(res.blockNs));
    }
    std::vector<double> all;
    for (const auto& r : runs)
        all.insert(all.end(), r.begin(), r.end());
    std::sort(all.begin(), all.end());
    const double deadline = static_cast<double>(block) / rate * 1.0e9;

    Summary s;
    s.nsPerSample = median(avgs);
    s.cpuPct = s.nsPerSample * rate / 1.0e9 * 100.0;
    s.worstPct = *std::min_element(worsts.begin(), worsts.end()) / deadline * 100.0;
    s.worstAllPct = worstAll / deadline * 100.0;
    s.p999Pct = all[std::min(all.size() - 1, static_cast<std::size_t>(0.999 * static_cast<double>(all.size())))]
                / deadline * 100.0;

    // Periodicity: fold the block times at period p. Every run starts at
    // the same position, so the runs are phase-aligned and pooled: each
    // phase's median over all runs (medians, so one preempted block does not
    // make a phase), and the strongest phase is the statistic. The maximum of
    // p noisy medians grows with p by chance alone, so the same fold of a
    // randomly permuted copy of the block times (no periodicity, same
    // distribution) is the baseline: ratio = fold / baseline (1.00 = no
    // periodic work), excess = fold - baseline in % of the deadline. The
    // period with the largest excess is reported.
    const auto strongestPhase = [](const std::vector<std::vector<double>>& rs, int period) {
        double mx = 0.0;
        std::vector<double> phase;
        for (int k = 0; k < period; ++k)
        {
            phase.clear();
            for (const auto& r : rs)
                for (std::size_t i = static_cast<std::size_t>(k); i < r.size(); i += static_cast<std::size_t>(period))
                    phase.push_back(r[i]);
            mx = std::max(mx, median(phase));
        }
        return mx;
    };
    std::vector<std::vector<double>> shuffled = runs;
    std::mt19937 rng(0x909u);
    for (auto& r : shuffled)
        std::shuffle(r.begin(), r.end(), rng);
    double bestRatio = 1.0, bestExcess = 0.0;
    int bestPeriod = 0;
    const std::size_t runLen = runs.empty() ? 0 : runs.front().size();
    for (int p = 2; p <= foldMax && runLen >= static_cast<std::size_t>(p) * 8; ++p)
    {
        const double fold = strongestPhase(runs, p);
        const double base = strongestPhase(shuffled, p);
        const double excess = (fold - base) / deadline * 100.0;
        if (bestPeriod == 0 || excess > bestExcess)
        {
            bestExcess = excess;
            bestRatio = base > 0.0 ? fold / base : 1.0;
            bestPeriod = p;
        }
    }
    s.foldPeriod = bestPeriod;
    s.foldRatio = bestRatio;
    s.foldExcessPct = bestExcess;

    std::printf("%-22s %8.1f %8.2f | %9.2f %9.2f %9.2f | %4d %6.2f %6.2f\n", scenarioName(sc), s.nsPerSample,
                s.cpuPct, s.p999Pct, s.worstPct, s.worstAllPct, s.foldPeriod, s.foldRatio, s.foldExcessPct);
    if (details)
    {
        static constexpr double kEdges[] = {1, 2, 3, 5, 7.5, 10, 15, 20, 25, 50, 100};
        constexpr int kBands = static_cast<int>(sizeof kEdges / sizeof kEdges[0]) + 1;
        std::array<std::size_t, kBands> counts{};
        for (double ns : all)
        {
            const double pct = ns / deadline * 100.0;
            int b = 0;
            while (b < kBands - 1 && pct >= kEdges[b])
                ++b;
            ++counts[static_cast<std::size_t>(b)];
        }
        std::printf("    block-time histogram (%% of the %.0f us deadline, %zu blocks):\n", deadline / 1000.0,
                    all.size());
        for (int b = 0; b < kBands; ++b)
        {
            if (counts[static_cast<std::size_t>(b)] == 0)
                continue;
            char band[32];
            if (b == 0)
                std::snprintf(band, sizeof band, "< %g", kEdges[0]);
            else if (b == kBands - 1)
                std::snprintf(band, sizeof band, ">= %g", kEdges[kBands - 2]);
            else
                std::snprintf(band, sizeof band, "%g - %g", kEdges[b - 1], kEdges[b]);
            const double share = 100.0 * static_cast<double>(counts[static_cast<std::size_t>(b)])
                                 / static_cast<double>(all.size());
            const int bar = static_cast<int>(std::ceil(share / 2.0));
            std::printf("      %-10s %8zu %6.2f%%  %s\n", band, counts[static_cast<std::size_t>(b)], share,
                        std::string(static_cast<std::size_t>(std::min(bar, 50)), '#').c_str());
        }
    }
    std::fflush(stdout);
    return s;
}

} // namespace

int cmdBench(const std::vector<std::string>& args)
{
    double seconds = 4.0, tempo = 160.0, budget = 8.0, fxBudget = 12.0, gatePct = 25.0, spikeGate = 5.0;
    int repeats = 3, block = 128, foldMax = 512;
    bool gate = true, matrix = false, perVoice = false, pin = true;
    std::vector<double> rates{44100.0, 48000.0, 96000.0};
    std::vector<Quality> qualities{Quality::Eco, Quality::Standard, Quality::High};
    std::vector<int> blocks{32, 128, 1024};

    ArgReader r(args);
    while (r.more())
    {
        const std::string opt = r.option();
        if (opt == "--seconds")
            seconds = parseDouble(opt, r.value(), 0.1, 600.0);
        else if (opt == "--repeats")
            repeats = parseInt(opt, r.value(), 1, 100);
        else if (opt == "--tempo")
            tempo = parseDouble(opt, r.value(), 20.0, 400.0);
        else if (opt == "--block")
            block = parseInt(opt, r.value(), 1, 8192);
        else if (opt == "--budget")
            budget = parseDouble(opt, r.value(), 0.1, 1000.0);
        else if (opt == "--fx-budget")
            fxBudget = parseDouble(opt, r.value(), 0.1, 1000.0);
        else if (opt == "--gate")
            gatePct = parseDouble(opt, r.value(), 0.1, 1000.0);
        else if (opt == "--spike-gate")
            spikeGate = parseDouble(opt, r.value(), 0.1, 1000.0);
        else if (opt == "--fold-max")
            foldMax = parseInt(opt, r.value(), 2, 8192);
        else if (opt == "--inject-spike")
            g_injectEvery = parseInt(opt, r.value(), 2, 8192);
        else if (opt == "--no-pin")
        {
            r.flag();
            pin = false;
        }
        else if (opt == "--no-gate")
        {
            r.flag();
            gate = false;
        }
        else if (opt == "--matrix")
        {
            r.flag();
            matrix = true;
        }
        else if (opt == "--per-voice")
        {
            r.flag();
            perVoice = true;
        }
        else if (opt == "--rates")
        {
            rates.clear();
            for (const auto& t : splitList(r.value()))
                rates.push_back(parseDouble(opt, t, 8000.0, 384000.0));
        }
        else if (opt == "--qualities")
        {
            qualities.clear();
            for (const auto& t : splitList(r.value()))
                qualities.push_back(parseQuality(t));
        }
        else if (opt == "--blocks")
        {
            blocks.clear();
            for (const auto& t : splitList(r.value()))
                blocks.push_back(parseInt(opt, t, 1, 8192));
        }
        else
            r.unknown("bench");
    }

    const seq::Pattern pattern = densePattern();
    const double rate = 48000.0;
    const double stepBlocks = 60.0 / tempo / 4.0 * rate / static_cast<double>(block);
    // Folding needs at least 8 periods of data in a run.
    const auto runBlocks = static_cast<int>(std::ceil(seconds * rate / block));
    foldMax = std::max(2, std::min(foldMax, runBlocks / 8));
    // An injected spike: the block takes at least 30 % of its deadline.
    g_injectNs = 0.3 * static_cast<double>(block) / rate * 1.0e9;
    if (g_injectEvery > 0)
        std::printf("INJECTED: every %d-th block of every scenario is held to >= 30 %% of its deadline "
                    "(a check of the periodicity gate)\n",
                    g_injectEvery);

    std::string pinned = "not pinned (--no-pin)";
    if (pin)
    {
        const int cpu = sched_getcpu();
        cpu_set_t set;
        CPU_ZERO(&set);
        if (cpu >= 0)
            CPU_SET(cpu, &set);
        pinned = cpu >= 0 && sched_setaffinity(0, sizeof set, &set) == 0
                     ? "pinned to CPU " + std::to_string(cpu)
                     : "pinning failed, not pinned";
    }
    std::printf("mw909-render bench  (mw909 %s, %s build, g++ %s)\n", MW909_VERSION, MW909_BUILD_TYPE, __VERSION__);
    std::printf("CPU: %s\n", cpuModel().c_str());
    std::printf("48000 Hz, Standard, %d-sample blocks (deadline %.1f us), %.0f BPM; %s.\n"
                "dense cadence in blocks: 16th step %.1f, shuffled step pair %.1f, 16-step pattern %.1f.\n"
                "%d repeat(s) x %.1f s per scenario after a 0.5 s warm-up. worst = best-of-%d slowest block,\n"
                "worst-all = raw maximum; per/ratio/+%%dl = strongest fold of the block times over periods\n"
                "2..%d blocks against the same fold of the shuffled times: period, ratio (1.00 = no\n"
                "periodic work) and excess in %% of the deadline (dense rows include the pattern's cadence;\n"
                "silent/steady rows have none and are gated).\n\n",
                block, static_cast<double>(block) / rate * 1.0e6, tempo, pinned.c_str(), stepBlocks,
                2.0 * stepBlocks, 16.0 * stepBlocks, repeats, seconds, repeats, foldMax);
    std::printf("%-22s %8s %8s | %9s %9s %9s | %4s %6s %6s\n", "scenario", "ns/smp", "CPU %", "p99.9 %dl",
                "worst %dl", "worst-all", "per", "ratio", "+%dl");
    std::printf("-----------------------------------------+---------------------------------+-------------------\n");
    const Summary dense = runScenario(Scenario::Dense, rate, Quality::Standard, block, seconds, repeats, tempo,
                                      pattern, foldMax, true);
    const Summary fx = runScenario(Scenario::DenseFx, rate, Quality::Standard, block, seconds, repeats, tempo,
                                   pattern, foldMax, true);
    const Summary silent = runScenario(Scenario::Silent, rate, Quality::Standard, block, seconds, repeats, tempo,
                                       pattern, foldMax, true);
    const Summary steady = runScenario(Scenario::Steady, rate, Quality::Standard, block, seconds, repeats, tempo,
                                       pattern, foldMax, false);

    if (matrix)
    {
        std::printf("\nmatrix: dense, no strips/FX\n%8s %-8s %6s | %9s %7s | %9s %9s\n", "rate", "quality", "block",
                    "ns/sample", "CPU %", "worst %dl", "worst-all");
        for (double rt : rates)
            for (Quality q : qualities)
                for (int b : blocks)
                {
                    const std::array<bool, kNumVoices> noMute{};
                    std::vector<double> avgs, worsts;
                    double worstAll = 0.0;
                    for (int i = 0; i < repeats; ++i)
                    {
                        const RunResult res = runOnce(Scenario::Dense, rt, q, b, seconds, tempo, pattern, noMute, false);
                        avgs.push_back(res.totalNs / static_cast<double>(res.samples));
                        worsts.push_back(res.worstNs);
                        worstAll = std::max(worstAll, res.worstNs);
                    }
                    const double ns = median(avgs);
                    const double dl = static_cast<double>(b) / rt * 1.0e9;
                    std::printf("%8.0f %-8s %6d | %9.1f %7.2f | %9.2f %9.2f\n", rt, qualityName(q), b, ns,
                                ns * rt / 1.0e9 * 100.0, *std::min_element(worsts.begin(), worsts.end()) / dl * 100.0,
                                worstAll / dl * 100.0);
                    std::fflush(stdout);
                }
    }

    if (perVoice)
    {
        // Incremental cost of each voice alone (mute gates triggers, so a muted
        // voice never wakes), 48 kHz Standard, min over repeats.
        const auto costOf = [&](const std::array<bool, kNumVoices>& mute) {
            double best = 1.0e300;
            for (int i = 0; i < repeats; ++i)
            {
                const RunResult res =
                    runOnce(Scenario::Dense, rate, Quality::Standard, block, seconds, tempo, pattern, mute, false);
                best = std::min(best, res.totalNs / static_cast<double>(res.samples));
            }
            return best;
        };
        std::array<bool, kNumVoices> allMuted{};
        allMuted.fill(true);
        const double base = costOf(allMuted);
        std::printf("\nper-voice cost (dense, no FX; each voice alone minus the all-muted baseline)\n");
        std::printf("  %-4s %-14s %9s %8s\n", "code", "voice", "ns/sample", "CPU %");
        std::printf("  %-4s %-14s %9.1f %8.3f\n", "--", "baseline", base, base * rate / 1.0e9 * 100.0);
        double sum = 0.0;
        for (int v = 0; v < kNumVoices; ++v)
        {
            std::array<bool, kNumVoices> mute = allMuted;
            mute[static_cast<std::size_t>(v)] = false;
            const double cost = std::max(0.0, costOf(mute) - base);
            sum += cost;
            std::printf("  %-4s %-14s %9.1f %8.3f\n", std::string(kVoiceInfo[static_cast<std::size_t>(v)].code).c_str(),
                        std::string(kVoiceInfo[static_cast<std::size_t>(v)].name).c_str(), cost,
                        cost * rate / 1.0e9 * 100.0);
            std::fflush(stdout);
        }
        std::printf("  %-4s %-14s %9.1f %8.3f\n", "sum", "", sum, sum * rate / 1.0e9 * 100.0);
    }

    std::printf("\nbudgets: dense no-FX %.2f %% (limit %.1f %%), dense typical-FX %.2f %% (limit %.1f %%), "
                "worst block %.2f %% (limit %.1f %%), silent engine %.2f %%\n",
                dense.cpuPct, budget, fx.cpuPct, fxBudget, std::max({dense.worstPct, fx.worstPct, silent.worstPct}),
                gatePct, silent.cpuPct);
    const auto spiky = [&](const Summary& sm) { return sm.foldRatio > 1.5 && sm.foldExcessPct > spikeGate; };
    std::printf("periodic spikes (no-cadence scenarios; limit ratio 1.5 and +%.1f %% of the deadline): silent "
                "period %d ratio %.2f +%.2f %%, steady period %d ratio %.2f +%.2f %%%s\n",
                spikeGate, silent.foldPeriod, silent.foldRatio, silent.foldExcessPct, steady.foldPeriod,
                steady.foldRatio, steady.foldExcessPct, spiky(silent) || spiky(steady) ? ": PERIODIC WORK" : "");
    for (const Summary* sm : {&dense, &fx})
        if (sm->foldRatio > 1.5 && sm->foldExcessPct > 0.5)
            std::printf("note: %s block times fold at period %d (ratio %.2f, +%.2f %% of the deadline); compare "
                        "with the cadence above\n",
                        sm == &dense ? "dense" : "typical-FX", sm->foldPeriod, sm->foldRatio, sm->foldExcessPct);
    const bool failed = dense.cpuPct > budget || fx.cpuPct > fxBudget
                        || std::max({dense.worstPct, fx.worstPct, silent.worstPct}) > gatePct || spiky(silent)
                        || spiky(steady);
    if (gate && failed)
    {
        std::printf("GATE FAILED\n");
        return 1;
    }
    if (gate)
        std::printf("gate passed\n");
    else
        std::printf("gate not applied (--no-gate)%s\n", failed ? ": would FAIL" : "");
    return 0;
}

} // namespace mw909::tools
