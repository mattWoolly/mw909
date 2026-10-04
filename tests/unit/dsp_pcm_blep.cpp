// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The band-limited ZOH renderer of the sample voices (dsp/PcmBlep.h): step
// response, latency, and the fold-back requirement (research/05 §5.5,
// architecture §3.5): everything above the host Nyquist that would fold back
// into the pass band is rejected by >= 90 dB, while the hardware's images
// below the host Nyquist are kept; clock rates above the host rate work.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PcmTestUtil.h"
#include "mw909/dsp/PcmBlep.h"
#include "mw909/dsp/PcmCommon.h"

using namespace mw909::dsp;
using pcmtest::kRates;

namespace {

PcmStepKernel makeKernel(double fs, int taps) {
    AnalogLowpass2 s1, s2;
    pcm::reconstructionFilter(0U, 0U, 0.0f, s1, s2);
    PcmStepKernel k;
    k.build(fs, taps, s1, s2);
    return k;
}

// A sine held at the clock rate (unquantised), rendered through the kernel.
std::vector<float> zohSine(const PcmStepKernel& k, double fs, double clock, double f0, double seconds) {
    PcmStepAccumulator acc;
    acc.reset();
    const auto n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> out(n);
    double t = 0.0, last = 0.0;
    long step = 0;
    for (std::size_t i = 0; i < n; ++i) {
        while (t < static_cast<double>(i + 1)) {
            const double v = std::sin(2.0 * kPi * f0 * static_cast<double>(step) / clock);
            const double frac = t - static_cast<double>(i);
            acc.addStep(k, static_cast<std::uint32_t>(i) + 1U, frac, static_cast<float>(v - last));
            last = v;
            ++step;
            t = static_cast<double>(step) * fs / clock;
        }
        out[i] = acc.tick(static_cast<std::uint32_t>(i));
    }
    return out;
}

}  // namespace

TEST_CASE("pcm blep: the step response settles to 1 and starts at 0", "[pcm][blep]") {
    for (double fs : kRates) {
        for (int taps : {16, 32, 48, 64}) {
            const PcmStepKernel k = makeKernel(fs, taps);
            INFO("fs " << fs << " taps " << taps << " width " << k.width() << " end " << k.endValue());
            REQUIRE(k.latency() == taps / 2);
            REQUIRE(k.width() <= PcmStepKernel::kMaxWidth);
            REQUIRE(std::abs(k.endValue() - 1.0f) < 2.0e-5f);
            REQUIRE(std::abs(k.stepAt(-k.latency() + 0.001)) < 1.0e-6f);
            // the 50 % point is the latency plus the analog filter's delay (~13 µs)
            double cross = -1.0;
            for (double t = -k.latency(); t < k.width() - k.latency(); t += 0.01)
                if (k.stepAt(t) >= 0.5f) {
                    cross = t;
                    break;
                }
            REQUIRE(cross > -0.5);
            REQUIRE(cross < 0.5 + 25.0e-6 * fs);
        }
    }
}

TEST_CASE("pcm blep: a single step through the accumulator holds its height", "[pcm][blep]") {
    const PcmStepKernel k = makeKernel(48000.0, 48);
    PcmStepAccumulator acc;
    acc.reset();
    acc.addStep(k, 11U, 0.37, 0.8f);
    std::vector<float> y(400);
    for (std::uint32_t i = 0; i < 400U; ++i) y[i] = acc.tick(i);
    for (int i = 0; i <= 10; ++i) REQUIRE(y[static_cast<std::size_t>(i)] == 0.0f);
    REQUIRE(std::abs(y[399] - 0.8f) < 1.0e-5f);
    REQUIRE(acc.drained());
}

TEST_CASE("pcm blep: fold-back below -90 dB, images kept, clocks above the host rate", "[pcm][blep]") {
    struct Case {
        double clock, f0;
    };
    // the last three put the hat's first image just above a 44.1/48 kHz
    // Nyquist, where it folds to 18.7..20 kHz
    const Case cases[] = {{31900.0, 9000.0},  {28300.0, 5300.0}, {56600.0, 11300.0}, {37733.0, 7700.0},
                          {31900.0, 7000.0},  {31900.0, 7700.0}, {31900.0, 6500.0}};
    for (double fs : kRates) {
        for (int os : {1, 2, 4}) {
            const int taps = pcm::kernelTaps(os, fs);
            const PcmStepKernel k = makeKernel(fs, taps);
            // fold-back is checked up to 20 kHz (0.45 fs at 44.1 kHz), or 0.30 fs
            // at high rates (the kernels' clean band, pcm::kernelCleanHz)
            const double scanTo = std::max(std::min(20000.0, 0.45 * fs), 0.30 * fs);
            REQUIRE(pcm::kernelCleanHz(taps, fs) >= scanTo);
            for (const Case& c : cases) {
                const auto y = zohSine(k, fs, c.clock, c.f0, 1.3);
                const auto spec = pcmtest::spectrumOf(y, fs, 0.2, 1.2, 20.0);
                const double bin = spec.binHz;
                // allowed: the tone and its ZOH images m*clock +- f0
                std::vector<double> lines;
                for (int m = 0; m < 20; ++m) {
                    lines.push_back(m * c.clock + c.f0);
                    if (m > 0) lines.push_back(m * c.clock - c.f0);
                }
                auto near = [&](double f) {
                    for (double l : lines)
                        if (std::abs(f - l) < 40.0) return true;
                    return false;
                };
                double ref = 0.0, worst = 0.0, worstAt = 0.0;
                for (std::size_t i = 1; i < spec.power.size(); ++i) {
                    const double f = static_cast<double>(i) * bin;
                    if (std::abs(f - c.f0) < 40.0) ref = std::max(ref, spec.power[i]);
                }
                for (std::size_t i = static_cast<std::size_t>(30.0 / bin); i < spec.power.size(); ++i) {
                    const double f = static_cast<double>(i) * bin;
                    if (f > scanTo) break;
                    if (near(f)) continue;
                    if (spec.power[i] > worst) {
                        worst = spec.power[i];
                        worstAt = f;
                    }
                }
                const double aliasDb = 10.0 * std::log10(worst / ref + 1e-300);
                // the first image, when below the host's pass band, is kept at its hardware level
                const double img = c.clock - c.f0;
                double imgDb = 0.0;
                if (img < 0.36 * fs) {   // inside every kernel's pass band
                    double p = 0.0;
                    for (std::size_t i = 0; i < spec.power.size(); ++i)
                        if (std::abs(static_cast<double>(i) * bin - img) < 40.0) p = std::max(p, spec.power[i]);
                    imgDb = 10.0 * std::log10(p / ref);
                    // ZOH droop sinc(f/clock) and the analog LPF, relative to the tone
                    auto sinc = [](double x) { return std::sin(kPi * x) / (kPi * x); };
                    auto lpf = [&](double f) {
                        AnalogLowpass2 s1, s2;
                        pcm::reconstructionFilter(0U, 0U, 0.0f, s1, s2);
                        auto mag = [&](const AnalogLowpass2& s) {
                            const double r = f / s.f0;
                            return 1.0 / std::sqrt((1 - r * r) * (1 - r * r) + (r / s.q) * (r / s.q));
                        };
                        return mag(s1) * mag(s2);
                    };
                    const double expect = 20.0 * std::log10(std::abs(sinc(img / c.clock)) * lpf(img) /
                                                            (std::abs(sinc(c.f0 / c.clock)) * lpf(c.f0)));
                    INFO("image " << img << " Hz at " << imgDb << " dB, expected " << expect);
                    REQUIRE(std::abs(imgDb - expect) < 0.5);
                }
                INFO("fs " << fs << " oversample " << os << " taps " << taps << " clock " << c.clock << " tone " << c.f0 << ": worst alias "
                           << aliasDb << " dB at " << worstAt << " Hz");
                REQUIRE(aliasDb < -90.0);
            }
        }
    }
}

TEST_CASE("pcm blep: kernel lengths per rate and quality", "[pcm][blep]") {
    // 44.1 kHz needs 64 taps and 48 kHz 48 for a clean band to 20 kHz
    for (int os : {1, 2, 4}) {
        REQUIRE(pcm::kernelTaps(os, 44100.0) == 64);
        REQUIRE(pcm::kernelTaps(os, 48000.0) >= 48);
        REQUIRE(pcm::kernelCleanHz(pcm::kernelTaps(os, 44100.0), 44100.0) > 19900.0);
        REQUIRE(pcm::kernelCleanHz(pcm::kernelTaps(os, 48000.0), 48000.0) > 20000.0);
        for (double fs : {22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0}) {
            const int t = pcm::kernelTaps(os, fs);
            REQUIRE(t == PcmStepKernel::roundTaps(t));
            REQUIRE(t / 2 <= pcm::maxKernelLatency(fs));
        }
    }
    REQUIRE(pcm::kernelTaps(1, 96000.0) == 32);
    REQUIRE(pcm::kernelTaps(4, 96000.0) == 48);
    REQUIRE(pcm::kernelTaps(1, 192000.0) == 16);
    REQUIRE(pcm::kernelTaps(2, 192000.0) == 32);
}

// prepare() rebuilds the kernel and runs on the audio thread when the engine
// is reconfigured (architecture §1): the three sample voices together stay well
// inside a 128-sample block. On a quiet machine they take 8 / 9 / 15 / 23 % of
// it at 44.1 / 48 / 96 / 192 kHz (the 25 % worst-block rule, architecture §4.2);
// the hard bound is 50 %, so a loaded test machine does not fail it while a
// regression to the old per-voice Kaiser evaluation (3..4 ms, 110..600 %) does.
TEST_CASE("pcm blep: the sample voices' prepare() fits in a block", "[pcm][blep][cpu]") {
    static mw909::voices::HiHat hat;
    static mw909::voices::Cymbal crash, ride;
    for (double fs : kRates) {
        double worst = 0.0;
        for (int os : {1, 2, 4}) {
            if (fs >= 88200.0 && os == 4) continue;   // the engine's High is 2x from 88.2 kHz
            double best = 1.0e30;
            for (int rep = 0; rep < 25; ++rep) {
                // a new seed with variance on, so the kernels are really rebuilt
                const mw909::VoiceSetup s = pcmtest::setupFor(fs, os, static_cast<std::uint32_t>(rep + 1), 1.0f);
                const auto t0 = std::chrono::steady_clock::now();
                hat.prepare(s);
                crash.prepare(s, mw909::CR);
                ride.prepare(s, mw909::RD);
                const auto t1 = std::chrono::steady_clock::now();
                best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            worst = std::max(worst, best);
        }
        const double block = 128.0 / fs * 1000.0;
        std::printf("[cpu] HiHat + Crash + Ride prepare at %.0f Hz: %.3f ms (worst quality), %.0f %% of a 128-sample block\n", fs,
                    worst, 100.0 * worst / block);
#ifdef NDEBUG
        REQUIRE(worst < 0.5 * block);
#endif
    }
}
