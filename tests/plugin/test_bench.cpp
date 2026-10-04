// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// processBlock cost over a long run, block by block (hidden: run it on purpose,
// on a Release build, with `mw909_plugin_tests "[.bench]"`). It reports the
// mean, the percentiles and the worst block, and looks for periodic spikes:
// the blocks over 4x the median are binned by their position modulo a few
// suspicious periods (the 30 Hz service timer, parameter pushes, a pattern
// measure). MW909_BENCH_SECONDS sets the length (default 60 s).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <time.h>

#include "TestSupport.h"

using namespace plugintest;
using mw909::seq::StepLevel;

namespace {

mw909::seq::Pattern densePattern() {
    mw909::seq::Pattern p;
    p.shuffle = 10;
    for (int v = 0; v < mw909::kNumVoices; ++v)
        for (int s = 0; s < 16; ++s) {
            auto& step = p.at(0, v, s);
            step.setLevel((s + v) % 4 == 0 ? StepLevel::Accent : StepLevel::Normal);
            if (s % 4 == 2) step.setSubSteps(2);
            if (s % 8 == 5) step.setFlam(true);
        }
    for (int s = 0; s < 16; s += 4) p.at(0, mw909::seq::kAccentTrack, s).setLevel(StepLevel::Normal);
    return p;
}

// CPU time of the calling thread: unlike wall time, it does not count the
// stretches in which the scheduler gave the core to someone else.
double threadCpuMicros() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1.0e6 + static_cast<double>(ts.tv_nsec) * 1.0e-3;
}

struct Stats {
    double mean = 0, p50 = 0, p99 = 0, p999 = 0, worst = 0;
    long worstIndex = -1;
    long spikes = 0;
};

Stats summarise(const std::vector<float>& us) {
    Stats s;
    std::vector<float> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (float v : us) sum += v;
    s.mean = sum / static_cast<double>(us.size());
    const auto at = [&](double q) { return sorted[static_cast<std::size_t>(q * static_cast<double>(sorted.size() - 1))]; };
    s.p50 = at(0.5);
    s.p99 = at(0.99);
    s.p999 = at(0.999);
    const auto worst = std::max_element(us.begin(), us.end());
    s.worst = *worst;
    s.worstIndex = worst - us.begin();
    for (float v : us)
        if (v > 4.0 * s.p50) ++s.spikes;
    return s;
}

// Share of the spikes that fall in the most popular residue modulo `period`
// (1/period would be "no periodicity").
double periodicity(const std::vector<float>& us, double threshold, int period) {
    std::vector<long> bins(static_cast<std::size_t>(period), 0);
    long total = 0;
    for (std::size_t i = 0; i < us.size(); ++i)
        if (us[i] > threshold) {
            ++bins[i % static_cast<std::size_t>(period)];
            ++total;
        }
    if (total == 0) return 0.0;
    return static_cast<double>(*std::max_element(bins.begin(), bins.end())) / static_cast<double>(total);
}

void report(const char* label, const std::vector<float>& us, double blockMicros) {
    const Stats s = summarise(us);
    std::cout << label << ": " << us.size() << " blocks; mean " << s.mean << " us, p50 " << s.p50 << ", p99 "
              << s.p99 << ", p99.9 " << s.p999 << ", worst " << s.worst << " us (block " << s.worstIndex
              << "); budget " << blockMicros << " us; blocks > 4x median: " << s.spikes << "\n";
    for (const int period : {25, 50, 100, 375, 1500, 1600}) {
        const double share = periodicity(us, 4.0 * s.p50, period);
        if (s.spikes >= 10) std::cout << "    spikes in the busiest residue mod " << period << ": " << share * 100.0 << "% (uniform " << 100.0 / period << "%)\n";
    }
}

}  // namespace

TEST_CASE("bench: processBlock cost at 48 kHz, 64-sample blocks", "[.bench]") {
    double seconds = 60.0;
    if (const char* text = std::getenv("MW909_BENCH_SECONDS")) seconds = std::max(1.0, std::atof(text));
    constexpr double rate = 48000.0;
    constexpr int block = 64;
    const long numBlocks = static_cast<long>(seconds * rate / block);

    // Standard and High with the strips and FX off (architecture.md §4.2's
    // 8 % budget), then Standard with a typical FX set (the 12 % budget).
    for (const char* quality : {"Standard", "High", "Standard + FX"}) {
        auto p = makeProcessor(rate, block, true);
        p->patterns().setPattern(0, densePattern());
        p->patterns().selectPattern(0, true);
        dryMix(*p);
        for (int v = 0; v < mw909::kNumVoices; ++v) {
            setParam(*p, voiceParam(v, "eq_on"), 0.0f);
            setParam(*p, voiceParam(v, "comp_on"), 0.0f);
        }
        if (juce::String(quality).contains("FX")) {
            setParam(*p, "fx_reverb_on", 1.0f);
            setParam(*p, "fx_delay_on", 1.0f);
            setParam(*p, "fx_dyna_on", 1.0f);
            for (const int v : {mw909::SD, mw909::CP, mw909::CH, mw909::OH, mw909::RS}) {
                setParam(*p, voiceParam(v, "send_bus"), v == mw909::RS ? 1.0f : 2.0f);
                setParam(*p, voiceParam(v, "send"), 0.4f);
            }
            for (const int v : {mw909::BD, mw909::SD}) {
                setParam(*p, voiceParam(v, "comp_on"), 1.0f);
                setParam(*p, voiceParam(v, "eq_on"), 1.0f);
                setParam(*p, voiceParam(v, "eq1_on"), 1.0f);
            }
        }
        setParam(*p, "quality", juce::String(quality).startsWith("High") ? 2.0f : 1.0f);
        setParam(*p, "seq_mode", 2.0f);  // Internal
        setParam(*p, "seq_run", 1.0f);
        setParam(*p, "tempo", 160.0f);
        setParam(*p, "midi_out", 1.0f);
        if (juce::String(quality).contains("FX")) setParam(*p, "mcomp_on", 1.0f);
        setParam(*p, "master_drive", 0.3f);
        p->serviceMessageThread();

        // The audio loop runs on its own thread; this (message) thread does the
        // 30 Hz service work alongside, as the processor's timer would in a host.
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        midi.ensureSize(2048);
        std::vector<float> us(static_cast<std::size_t>(numBlocks));
        std::vector<float> cpu(static_cast<std::size_t>(numBlocks));
        std::atomic<bool> done{false};
        std::thread audio([&] {
            auto* tune = p->apvts().getParameter("bd_tune");
            auto* level = p->apvts().getParameter("sd_level");
            for (long i = 0; i < numBlocks; ++i) {
                midi.clear();
                if (i % 8 == 0) {  // automation on the audio thread, every 8 blocks (~10 ms)
                    tune->setValueNotifyingHost(static_cast<float>((i / 8) % 100) / 100.0f);
                    level->setValueNotifyingHost(0.5f + 0.4f * static_cast<float>((i / 8) % 2));
                }
                const double c0 = threadCpuMicros();
                const auto t0 = std::chrono::steady_clock::now();
                p->processBlock(buffer, midi);
                const auto t1 = std::chrono::steady_clock::now();
                cpu[static_cast<std::size_t>(i)] = static_cast<float>(threadCpuMicros() - c0);
                us[static_cast<std::size_t>(i)] = std::chrono::duration<float, std::micro>(t1 - t0).count();
                // Pace roughly like a device would, so the service passes interleave.
                if (i % 16 == 15) std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            done.store(true);
        });
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            p->serviceMessageThread();
        }
        audio.join();
        report((juce::String("dense pattern, quality ") + quality + ", wall").toRawUTF8(), us, 1.0e6 * block / rate);
        report((juce::String("dense pattern, quality ") + quality + ", thread CPU").toRawUTF8(), cpu, 1.0e6 * block / rate);

        // Then stop the sequencer and let every tail decay into the denormal
        // range: the cost must fall, not climb.
        setParam(*p, "seq_run", 0.0f);
        std::vector<float> tail(static_cast<std::size_t>(10.0 * rate / block));
        for (std::size_t i = 0; i < tail.size(); ++i) {
            midi.clear();
            const auto t0 = std::chrono::steady_clock::now();
            p->processBlock(buffer, midi);
            tail[i] = std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - t0).count();
        }
        report((juce::String("decaying tails after stop, quality ") + quality).toRawUTF8(), tail, 1.0e6 * block / rate);
        CHECK(buffer.getMagnitude(0, block) < 1.0e-3f);
    }
}
