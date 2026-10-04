// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Sequencer clock: exact step timing, block-size invariance, pre-roll,
// mid-measure starts, extreme tempos and sample rates.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

#include "SeqTestUtil.h"

using namespace seqtest;

namespace {

// The documented rounding rule: nearest sample, half-samples up (with a 1e-6
// sample snap), computed here in long double independently of the sequencer.
std::int64_t expectedSample(long double ppq, long double ppq0, long double bpm, long double sr) {
    return static_cast<std::int64_t>(std::floor((ppq - ppq0) * 60.0L / bpm * sr + 0.5L + 1.0e-6L));
}

}  // namespace

TEST_CASE("steps land on whole samples at 48 kHz / 120 BPM", "[seq][timing]") {
    auto s = makeSeq(48000.0);
    s->setPattern(0, everyStep(BD));
    const auto ev = runHost(*s, hostSettings(), 48000.0, 120.0, 0.0, fixedBlocks(48000 * 4, 512));
    REQUIRE(ev.size() == 32);  // 4 s = 8 quarter notes = 32 sixteenths
    for (std::size_t i = 0; i < ev.size(); ++i) {
        CHECK(ev[i].sample == static_cast<std::int64_t>(i) * 6000);
        CHECK(ev[i].voice == BD);
        CHECK(ev[i].volts == kNormalV);
    }
}

TEST_CASE("half-sample step times round up deterministically (44.1 kHz / 120 BPM)", "[seq][timing]") {
    // A sixteenth is 5512.5 samples here: every odd step sits exactly on a half sample.
    auto s = makeSeq(44100.0);
    s->setPattern(0, everyStep(BD));
    for (int block : {1, 7, 64, 441, 4096}) {
        s->reset();
        const auto ev = runHost(*s, hostSettings(), 44100.0, 120.0, 0.0, fixedBlocks(44100 * 2, block));
        REQUIRE(ev.size() == 16);
        const std::int64_t expect[] = {0, 5513, 11025, 16538, 22050, 27563, 33075, 38588};
        for (int i = 0; i < 8; ++i) CHECK(ev[static_cast<std::size_t>(i)].sample == expect[i]);
    }
}

TEST_CASE("step times follow PPQ at every tempo, sample rate and scale", "[seq][timing]") {
    for (double sr : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        for (double bpm : {20.0, 60.0, 97.3, 120.0, 133.0, 174.0, 400.0}) {
            for (int sc = 0; sc < static_cast<int>(Scale::kNumScales); ++sc) {
                const auto scale = static_cast<Scale>(sc);
                auto s = makeSeq(sr);
                s->setPattern(0, everyStep(CH, 16, scale));
                const double ppq0 = 0.37;  // not on the grid
                const double qn = 6.0;
                const auto total = static_cast<std::int64_t>(qn * 60.0 / bpm * sr);
                const auto ev = runHost(*s, hostSettings(), sr, bpm, ppq0, fixedBlocks(total, 1000));
                const long double L = stepLengthQn(scale);
                std::vector<std::int64_t> want;
                for (int n = 0;; ++n) {
                    const long double p = n * L;
                    if (p < ppq0 - 1e-9) continue;
                    const std::int64_t smp = expectedSample(p, ppq0, bpm, sr);
                    if (smp >= total) break;
                    want.push_back(smp);
                }
                REQUIRE(ev.size() == want.size());
                for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(ev[i].sample == want[i]);
            }
        }
    }
}

TEST_CASE("a start exactly on a step fires it at offset 0; mid-step starts wait for the next", "[seq][timing]") {
    auto s = makeSeq(48000.0);
    Pattern p;
    for (int i = 0; i < 16; ++i) p.at(0, i % kNumVoices, i).setLevel(StepLevel::Normal);  // step i plays voice i
    s->setPattern(0, p);

    SECTION("on the step") {
        const auto ev = runHost(*s, hostSettings(), 48000.0, 120.0, 1.25, fixedBlocks(6000, 256));
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].sample == 0);
        CHECK(ev[0].voice == 5);
        CHECK(s->currentStep() == 5);
    }
    SECTION("a fraction of a sample after the step still lands on sample 0") {
        const auto ev = runHost(*s, hostSettings(), 48000.0, 120.0, 1.25 + 0.3 / 24000.0, fixedBlocks(100, 100));
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].sample == 0);
        CHECK(ev[0].voice == 5);
    }
    SECTION("mid-measure start") {
        const auto ev = runHost(*s, hostSettings(), 48000.0, 120.0, 1.1, fixedBlocks(24000, 300));
        REQUIRE(ev.size() == 4);
        CHECK(ev[0].sample == 3600);  // 0.15 qn * 24000
        CHECK(ev[0].voice == 5);
        CHECK(ev[1].voice == 6);
        CHECK(ev[3].voice == 8);
        CHECK(s->measureCount() == 0);
    }
    SECTION("start in the middle of measure 3 of a long song") {
        const auto ev = runHost(*s, hostSettings(), 48000.0, 120.0, 4.0 * 3 + 2.0, fixedBlocks(6000, 512));
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].voice == 8);
        CHECK(s->measureCount() == 3);
    }
}

TEST_CASE("negative PPQ pre-roll plays the end of measure -1 and lands the downbeat", "[seq][timing]") {
    auto s = makeSeq(48000.0);
    Pattern p;
    p.at(0, SD, 12).setLevel(StepLevel::Normal);
    p.at(0, BD, 0).setLevel(StepLevel::Normal);
    p.at(0, CH, 14).setLevel(StepLevel::Normal);
    s->setPattern(0, p);
    const auto ev = runHost(*s, hostSettings(), 48000.0, 120.0, -1.0, fixedBlocks(24000 + 100, 333));
    REQUIRE(ev.size() == 3);
    CHECK(ev[0] == Ev{0, SD, kNormalV, StepLevel::Normal, false});
    CHECK(ev[1] == Ev{12000, CH, kNormalV, StepLevel::Normal, false});
    CHECK(ev[2] == Ev{24000, BD, kNormalV, StepLevel::Normal, false});
    CHECK(s->measureCount() == 0);
}

TEST_CASE("pre-roll measure index is floor(ppq / measure length)", "[seq][timing]") {
    auto s = makeSeq(48000.0);
    s->setPattern(0, everyStep(BD));
    auto buf = std::make_unique<EventBuffer>();
    for (double ppq : {-0.01, -3.99, -4.0, -4.01, -9.5, 0.0, 3.99, 4.0, 123.4}) {
        s->reset();
        Transport t{true, true, ppq, 120.0};
        buf->clear();
        s->process(t, hostSettings(), 64, *buf);
        CHECK(s->measureCount() == static_cast<std::int64_t>(std::floor(ppq / 4.0)));
    }
}

TEST_CASE("extreme tempos, 192 kHz, 1-step and 64-step patterns", "[seq][timing]") {
    SECTION("20 BPM at 192 kHz, one-step pattern of eighth notes") {
        auto s = makeSeq(192000.0);
        s->setPattern(0, everyStep(BD, 1, Scale::Eighth));
        const auto ev = runHost(*s, hostSettings(), 192000.0, 20.0, 0.0, fixedBlocks(192000 * 9, 4096));
        REQUIRE(ev.size() == 6);  // an eighth at 20 BPM is 1.5 s; [0, 9 s) holds hits at 0, 1.5 .. 7.5
        for (std::size_t i = 0; i < ev.size(); ++i) CHECK(ev[i].sample == static_cast<std::int64_t>(i) * 288000);
        CHECK(s->currentStep() == 0);
    }
    SECTION("400 BPM, 32nd notes, 64 steps") {
        auto s = makeSeq(44100.0);
        s->setPattern(0, everyStep(CH, 64, Scale::ThirtySecond));
        const auto ev = runHost(*s, hostSettings(), 44100.0, 400.0, 0.0, fixedBlocks(44100 * 3, 128));
        // 3 s at 400 BPM = 20 quarter notes = 160 thirty-seconds.
        REQUIRE(ev.size() == 160);
        for (std::size_t i = 0; i < ev.size(); ++i)
            CHECK(ev[i].sample == expectedSample(static_cast<long double>(i) * 0.125L, 0.0L, 400.0L, 44100.0L));
        CHECK(s->measureCount() == 2);
    }
    SECTION("tempo outside 20..400 is clamped") {
        auto s = makeSeq(48000.0);
        auto buf = std::make_unique<EventBuffer>();
        Settings st = internalSettings(1000.0);
        s->process(Transport{}, st, 64, *buf);
        CHECK(s->tempo() == 400.0);
        st.internalBpm = 1.0;
        s->process(Transport{}, st, 64, *buf);
        CHECK(s->tempo() == 20.0);
        st.internalBpm = std::nan("");
        s->process(Transport{}, st, 64, *buf);
        CHECK(s->tempo() == 120.0);
    }
}

// ---------------------------------------------------------------------------
// Block-size invariance: the property the whole design rests on.

namespace {

struct Scenario {
    double sr, bpm, ppqStart;
    std::int64_t total;
    Settings st;
};

Scenario randomScenario(std::mt19937& rng, Sequencer& s, Mode mode) {
    std::uniform_int_distribution<int> coin(0, 99), srI(0, 3), var(0, 2), fill(0, 5), slot(0, 15), chainLen(1, 6),
        entry(0, 15), play(0, 2);
    std::uniform_real_distribution<double> bpmD(20.0, 400.0), ppqD(-6.0, 40.0), u(0.0, 1.0);
    const double rates[] = {44100.0, 48000.0, 96000.0, 192000.0};
    Scenario sc;
    sc.sr = rates[srI(rng)];
    sc.bpm = coin(rng) < 30 ? 120.0 : std::round(bpmD(rng) * 10.0) / 10.0;
    sc.ppqStart = coin(rng) < 30 ? 0.0 : ppqD(rng);
    sc.st = mode == Mode::HostSync ? hostSettings() : internalSettings(sc.bpm);
    sc.st.totalAccent = static_cast<float>(u(rng));
    sc.st.jitter = coin(rng) < 30;
    sc.st.jitterSeed = static_cast<std::uint32_t>(rng());
    sc.st.swingScale = coin(rng) < 50 ? 1.0f : static_cast<float>(u(rng));
    sc.st.variation = static_cast<VariationMode>(var(rng));
    sc.st.autoFill = static_cast<AutoFill>(fill(rng));
    sc.st.fillSlot = static_cast<std::uint8_t>(slot(rng));
    sc.st.play = static_cast<PlayMode>(play(rng));
    sc.st.songTrack = 6;
    const float density = static_cast<float>(0.03 + 0.25 * u(rng));
    for (int i = 0; i < 16; ++i) s.setPattern(i, randomPattern(rng, density));
    Chain c;
    c.length = static_cast<std::uint8_t>(chainLen(rng));
    for (int i = 0; i < c.length; ++i) c.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(entry(rng));
    s.setChain(c);
    SongTrack t;
    t.length = static_cast<std::uint16_t>(chainLen(rng) * 3);
    for (int i = 0; i < t.length; ++i) t.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(entry(rng));
    s.setSongTrack(6, t);
    s.selectPattern(entry(rng));
    // About 3..10 measures of a typical pattern, capped for the per-sample run.
    const double seconds = std::min(6.0, (6.0 + 30.0 * u(rng)) * 60.0 / sc.bpm);
    sc.total = static_cast<std::int64_t>(seconds * sc.sr);
    return sc;
}

void checkInvariance(Mode mode, int cases, std::uint32_t seed) {
    std::mt19937 rng(seed);
    int compared = 0;
    std::size_t totalEvents = 0;
    for (int c = 0; c < cases; ++c) {
        auto s = makeSeq(48000.0);
        const std::uint32_t patSeed = static_cast<std::uint32_t>(rng());
        std::mt19937 prng(patSeed);
        Scenario sc = randomScenario(prng, *s, mode);
        s->prepare(sc.sr);
        INFO("case " << c << " sr " << sc.sr << " bpm " << sc.bpm << " ppq " << sc.ppqStart << " total " << sc.total);

        auto run = [&](const std::vector<int>& blocks, bool* ovf) {
            s->reset();
            return mode == Mode::HostSync ? runHost(*s, sc.st, sc.sr, sc.bpm, sc.ppqStart, blocks, ovf)
                                          : runInternal(*s, sc.st, blocks, ovf);
        };
        bool ovfRef = false;
        const auto ref = run(fixedBlocks(sc.total, 4096), &ovfRef);
        if (ovfRef) continue;  // too dense for one buffer per block: not a fair comparison
        ++compared;
        totalEvents += ref.size();

        bool ovf = false;
        const auto one = run(fixedBlocks(sc.total, static_cast<int>(sc.total)), &ovf);
        if (!ovf) REQUIRE(one == ref);
        for (int bs : {1, 7, 64, 512}) {
            if (bs == 1 && c % 4 != 0) continue;  // per-sample runs are slow: every 4th case
            INFO("block size " << bs);
            REQUIRE(run(fixedBlocks(sc.total, bs), nullptr) == ref);
        }
        std::mt19937 brng(patSeed ^ 0xabcdefU);
        INFO("random partition");
        REQUIRE(run(randomBlocks(sc.total, brng, 3000), nullptr) == ref);
        REQUIRE(run(randomBlocks(sc.total, brng, 40), nullptr) == ref);
        // Same calls, same output: determinism.
        REQUIRE(run(fixedBlocks(sc.total, 4096), nullptr) == ref);
    }
    CHECK(compared > cases / 2);
    CHECK(totalEvents > static_cast<std::size_t>(compared) * 20);  // the property was exercised
}

}  // namespace

TEST_CASE("block-size invariance under host sync (random patterns, all features)", "[seq][timing][property]") {
    checkInvariance(Mode::HostSync, 120, 0x5EC0001U);
}

TEST_CASE("block-size invariance on the internal clock (random patterns, all features)", "[seq][timing][property]") {
    checkInvariance(Mode::Internal, 80, 0x5EC0002U);
}

TEST_CASE("host PPQ noise of 1e-9 qn is not a relocation", "[seq][timing]") {
    std::mt19937 rng(77);
    for (int c = 0; c < 30; ++c) {
        auto s = makeSeq(48000.0);
        Scenario sc = randomScenario(rng, *s, Mode::HostSync);
        s->prepare(sc.sr);
        s->reset();
        const auto clean = runHost(*s, sc.st, sc.sr, sc.bpm, sc.ppqStart, fixedBlocks(sc.total, 256));
        s->reset();
        const auto noisy = runHost(*s, sc.st, sc.sr, sc.bpm, sc.ppqStart, fixedBlocks(sc.total, 256), nullptr, 1.0e-9,
                                   static_cast<std::uint32_t>(c + 1));
        INFO("case " << c);
        REQUIRE(noisy.size() == clean.size());
        for (std::size_t i = 0; i < clean.size(); ++i) {
            REQUIRE(std::llabs(noisy[i].sample - clean[i].sample) <= 1);
            REQUIRE(noisy[i].voice == clean[i].voice);
            REQUIRE(noisy[i].volts == clean[i].volts);
        }
    }
}
