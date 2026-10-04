// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The synthetic metal source (dsp/MetalSynth.h) and the source switch of the
// sample voices (ROM / synth / user image).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "PcmTestUtil.h"
#include "mw909/dsp/MetalSynth.h"
#include "mw909/dsp/PcmClock.h"

using namespace mw909;
using dsp::MetalSynth;

namespace {

std::vector<float> codes(MetalSynth::Kind kind, float color, float density, std::uint32_t seed, int n) {
    MetalSynth s;
    s.prepare(seed);
    s.configure(kind, color, density, 32000.0, n);
    s.start();
    std::vector<float> out(static_cast<std::size_t>(n));
    int pos = 0;
    for (auto& v : out) v = static_cast<float>(s.next(pos++) - 32);
    return out;
}

double centroid(const std::vector<float>& x) {
    return pcmtest::spectrumOf(x, 32000.0, 0.0, 0.25).centroid(100.0, 16000.0);
}

}  // namespace

TEST_CASE("pcm synth: deterministic, seeded, restarts identically, not free running", "[pcm][synth]") {
    STATIC_REQUIRE_FALSE(MetalSynth::kFreeRunning);
    for (auto kind : {MetalSynth::Kind::HatClosed, MetalSynth::Kind::HatOpen, MetalSynth::Kind::Crash, MetalSynth::Kind::Ride}) {
        const auto a = codes(kind, 0.5f, 0.5f, 11U, 8192), b = codes(kind, 0.5f, 0.5f, 11U, 8192);
        REQUIRE(a == b);
        REQUIRE(a != codes(kind, 0.5f, 0.5f, 12U, 8192));
        MetalSynth s;
        s.prepare(11U);
        s.configure(kind, 0.5f, 0.5f, 32000.0, 8192);
        s.start();
        for (int i = 0; i < 3000; ++i) (void)s.next(i);
        s.start();
        std::vector<float> again(8192);
        int pos = 0;
        for (auto& v : again) v = static_cast<float>(s.next(pos++) - 32);
        REQUIRE(again == a);
        // the material ends at mid-scale (a 2 ms fade, the last word exactly 32),
        // as a romFromAudio image does, so the DAC latch rests at mid-scale
        REQUIRE(a.back() == 0.0f);
        const auto fade = static_cast<std::size_t>(0.002 * 32000.0);
        float tail = 0.0f, body = 0.0f;
        for (std::size_t i = a.size() - fade / 4; i < a.size(); ++i) tail = std::max(tail, std::abs(a[i]));
        for (std::size_t i = a.size() - 4 * fade; i < a.size() - 2 * fade; ++i) body = std::max(body, std::abs(a[i]));
        INFO("largest |code - 32| in the last quarter of the fade " << tail << ", before the fade " << body);
        REQUIRE(tail <= 0.3f * body + 1.0f);
    }
}

TEST_CASE("pcm synth: 6-bit companded material, flat over the region", "[pcm][synth]") {
    for (auto kind : {MetalSynth::Kind::HatClosed, MetalSynth::Kind::HatOpen, MetalSynth::Kind::Crash, MetalSynth::Kind::Ride}) {
        for (float d : {0.0f, 0.5f, 1.0f}) {
            const auto x = codes(kind, 0.5f, d, 3U, kRomWords);
            float lo = 99.0f, hi = -99.0f;
            for (float v : x) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            REQUIRE(lo >= -32.0f);
            REQUIRE(hi <= 31.0f);
            const double early = pcmtest::rms(x, 32000.0, 0.01, 0.1), late = pcmtest::rms(x, 32000.0, 0.9, 1.0);
            INFO("kind " << static_cast<int>(kind) << " density " << d << " rms early " << early << " late " << late);
            REQUIRE(early > 6.0);
            REQUIRE(early < 16.0);
            REQUIRE(std::abs(20.0 * std::log10(late / early)) < 3.0);
        }
    }
}

TEST_CASE("pcm synth: Color brightens, Density adds partials, the kinds differ", "[pcm][synth]") {
    for (auto kind : {MetalSynth::Kind::HatOpen, MetalSynth::Kind::Crash, MetalSynth::Kind::Ride}) {
        const double dark = centroid(codes(kind, 0.0f, 0.5f, 5U, 8192)), bright = centroid(codes(kind, 1.0f, 0.5f, 5U, 8192));
        INFO("kind " << static_cast<int>(kind) << " centroid dark " << dark << " bright " << bright);
        REQUIRE(bright > dark * 1.15);
        MetalSynth lo, hi;
        lo.prepare(1U);
        hi.prepare(1U);
        lo.configure(kind, 0.5f, 0.0f, 32000.0, 1000);
        hi.configure(kind, 0.5f, 1.0f, 32000.0, 1000);
        REQUIRE(hi.numPartials() > lo.numPartials() + 16);
    }
    const double ch = centroid(codes(MetalSynth::Kind::HatClosed, 0.5f, 0.5f, 5U, 8192));
    const double cr = centroid(codes(MetalSynth::Kind::Crash, 0.5f, 0.5f, 5U, 8192));
    INFO("closed hat centroid " << ch << " crash " << cr);
    REQUIRE(ch > cr * 1.2);
    // NaN controls take the defaults
    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(codes(MetalSynth::Kind::Ride, nan, nan, 5U, 4096) == codes(MetalSynth::Kind::Ride, 0.5f, 0.5f, 5U, 4096));
}

TEST_CASE("pcm synth: the voices' Source switch (synth, user image, fallback)", "[pcm][synth]") {
    const double fs = 48000.0;
    auto render = [&](float source, float color, const RomImage* user) {
        voices::Cymbal c;
        c.prepare(pcmtest::setupFor(fs), CR);
        c.setRoms(&factoryRom(RomSlot::Crash), user);
        VoiceParams p = defaultParams(CR);
        p.k[knob::PcmSource] = source;
        p.k[knob::PcmColor] = color;
        c.setParams(p);
        c.trigger(Hit::normal());
        std::vector<float> out(static_cast<std::size_t>(0.5 * fs));
        pcmtest::renderBlocks(c, out.data(), out.size());
        return out;
    };
    const auto rom = render(0.0f, 0.5f, nullptr);
    const auto synth = render(1.0f, 0.5f, nullptr);
    REQUIRE(synth == render(1.0f, 0.5f, nullptr));
    REQUIRE(synth != rom);
    REQUIRE(render(1.0f, 0.9f, nullptr) != synth);   // Color shapes the synth
    REQUIRE(render(0.0f, 0.9f, nullptr) == rom);     // ... and is inert for the ROM
    REQUIRE(render(2.0f, 0.5f, nullptr) == rom);     // User with no image falls back to the ROM
    // a user image is played
    RomImage user{};
    user.data.fill(kRomMidScale);
    std::vector<float> tone(48000);
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<float>(std::sin(2.0 * dsp::kPi * 1000.0 * static_cast<double>(i) / 48000.0));
    REQUIRE(romFromAudio(tone.data(), static_cast<int>(tone.size()), 48000.0, RomRegion::Crash, user));
    const auto u = render(2.0f, 0.5f, &user);
    REQUIRE(u != rom);
    const auto spec = pcmtest::spectrumOf(u, fs, 0.05, 0.3);
    // the 1 kHz source plays at its natural pitch with TUNE at centre
    double best = 0.0, at = 0.0;
    for (std::size_t i = 1; i < spec.power.size(); ++i)
        if (spec.power[i] > best) {
            best = spec.power[i];
            at = static_cast<double>(i) * spec.binHz;
        }
    INFO("user image tone at " << at << " Hz");
    REQUIRE(std::abs(at - 1000.0) < 10.0);
}
