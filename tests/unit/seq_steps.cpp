// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Step semantics (architecture.md §5.2-5.4): shuffle (per scale, per track),
// flam (ms, grace level), sub-steps (level ramp), timing jitter, probability,
// polymeter, accent-DAC dynamics, variations, pattern switching, chains, song
// tracks, fills and tap write.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <vector>

#include "SeqTestUtil.h"
#include "mw909/seq/SeqHash.h"

using namespace seqtest;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kSr = 48000.0;
constexpr std::int64_t kStep = 6000;     // a sixteenth at 120 BPM / 48 kHz
constexpr std::int64_t kMeasure = 96000;  // 16 sixteenths
}  // namespace

TEST_CASE("shuffle: 16th scale delays odd steps by ticks/96 beat x swingScale", "[seq][steps]") {
    struct Case {
        int ticks;
        float swing;
        std::int64_t delay;  // 1 tick = 1/96 beat = 250 samples at 120 BPM / 48 kHz
    };
    for (const Case c : {Case{12, 1.0f, 3000}, Case{6, 1.0f, 1500}, Case{12, 0.5f, 1500}, Case{16, 1.0f, 4000},
                         Case{1, 1.0f, 250}, Case{0, 1.0f, 0}, Case{12, 0.0f, 0}, Case{200, 1.0f, 4000}}) {
        auto s = makeSeq(kSr);
        Pattern p = everyStep(BD);
        p.shuffle = static_cast<std::uint8_t>(c.ticks);
        s->setPattern(0, p);
        Settings st = internalSettings(120.0);
        st.swingScale = c.swing;
        Driver d(*s);
        d.runUntil(st, kMeasure, 512);
        INFO("ticks " << c.ticks << " swing " << c.swing);
        REQUIRE(d.events.size() == 16);
        for (int i = 0; i < 16; ++i)
            CHECK(d.events[static_cast<std::size_t>(i)].sample == i * kStep + ((i & 1) != 0 ? c.delay : 0));
    }
}

TEST_CASE("shuffle: the hardware keys, the 32nd pairing, triplets and the 8th extension", "[seq][steps]") {
    // Keys 1..7 store 0, 2, ... 12 ticks (research/03 §2.5).
    for (int key = 1; key <= 7; ++key) CHECK(shuffleTicksForKey(key) == 2 * (key - 1));
    // 32nd scale: delay 2*floor(t/4)/96 beat, so keys pair up 1=2, 3=4, 5=6, 7.
    const std::int64_t want32[] = {0, 0, 500, 500, 1000, 1000, 1500};
    // 8th (extension): 2t/96.
    for (int key = 1; key <= 7; ++key) {
        for (Scale sc : {Scale::ThirtySecond, Scale::EighthTriplet, Scale::SixteenthTriplet, Scale::Eighth}) {
            auto s = makeSeq(kSr);
            Pattern p = everyStep(SD, 16, sc);
            p.shuffle = shuffleTicksForKey(key);
            s->setPattern(0, p);
            Driver d(*s);
            const auto stepSamples = static_cast<std::int64_t>(std::lround(stepLengthQn(sc) * 24000.0));
            d.runUntil(internalSettings(120.0), 4 * stepSamples, 333);
            REQUIRE(d.events.size() == 4);
            std::int64_t delay = 0;
            if (sc == Scale::ThirtySecond) delay = want32[key - 1];
            if (sc == Scale::Eighth) delay = 2 * 250 * shuffleTicksForKey(key);
            INFO("key " << key << " scale " << static_cast<int>(sc));
            for (int i = 0; i < 4; ++i) {
                const auto exact = static_cast<std::int64_t>(std::floor(i * stepLengthQn(sc) * 24000.0 + 0.5 + 1e-6));
                CHECK(d.events[static_cast<std::size_t>(i)].sample == exact + ((i & 1) != 0 ? delay : 0));
            }
        }
    }
}

TEST_CASE("shuffle: odd-length patterns restart the alternation; per-track override", "[seq][steps]") {
    SECTION("odd length: the last step is not delayed and the next pass starts straight") {
        auto s = makeSeq(kSr);
        Pattern p = everyStep(BD, 3);
        p.shuffle = 12;
        s->setPattern(0, p);
        Driver d(*s);
        d.runUntil(internalSettings(120.0), 6 * kStep, 500);
        CHECK(samplesOf(d.events) ==
              std::vector<std::int64_t>{0, kStep + 3000, 2 * kStep, 3 * kStep, 4 * kStep + 3000, 5 * kStep});
    }
    SECTION("15 steps: step 14 (the 15th) stays on the grid") {
        auto s = makeSeq(kSr);
        Pattern p = everyStep(BD, 15);
        p.shuffle = 12;
        s->setPattern(0, p);
        Driver d(*s);
        d.runUntil(internalSettings(120.0), 16 * kStep, 500);
        REQUIRE(d.events.size() == 16);
        CHECK(d.events[13].sample == 13 * kStep + 3000);
        CHECK(d.events[14].sample == 14 * kStep);
        CHECK(d.events[15].sample == 15 * kStep);  // step 0 of the next pass
    }
    SECTION("a voice's own shuffle replaces the pattern's; swingScale scales both") {
        auto s = makeSeq(kSr);
        Pattern p = everyStep(BD);
        for (int i = 0; i < 16; ++i) p.at(0, SD, i).setLevel(StepLevel::Normal);
        p.shuffle = 12;
        p.trackShuffle[SD] = 1 + 4;   // 4 ticks
        p.trackShuffle[CH] = 1 + 0;   // explicitly straight
        p.at(0, CH, 1).setLevel(StepLevel::Normal);
        CHECK(p.shuffleOf(SD) == 4);
        CHECK(p.shuffleOf(kAccentTrack) == 12);
        s->setPattern(0, p);
        Settings st = internalSettings(120.0);
        st.swingScale = 0.5f;
        Driver d(*s);
        d.runUntil(st, 2 * kStep, 500);
        CHECK(samplesOf(onlyVoice(d.events, BD)) == std::vector<std::int64_t>{0, kStep + 1500});
        CHECK(samplesOf(onlyVoice(d.events, SD)) == std::vector<std::int64_t>{0, kStep + 500});
        CHECK(samplesOf(onlyVoice(d.events, CH)) == std::vector<std::int64_t>{kStep});
    }
}

TEST_CASE("flam: grace on the step, main the pattern's flamMs later whatever the tempo", "[seq][steps]") {
    for (double bpm : {37.0, 120.0, 290.0}) {
        for (int key = 1; key <= 8; ++key) {
            auto s = makeSeq(kSr);
            Pattern p;
            p.flamMs = flamMsForKey(key);
            p.flamGrace = 100;
            p.at(0, BD, 0).setLevel(StepLevel::Normal);
            p.at(0, BD, 0).setFlam(true);
            p.at(0, SD, 0).setLevel(StepLevel::Accent);
            p.at(0, SD, 0).setFlam(true);
            s->setPattern(0, p);
            Driver d(*s);
            d.runUntil(internalSettings(bpm), 4000, 100);
            const std::int64_t gap = (20 + 4 * (key - 1)) * 48;
            INFO("bpm " << bpm << " key " << key);
            REQUIRE(d.events.size() == 4);
            CHECK(d.events[0] == Ev{0, BD, kNormalV, StepLevel::Normal, false});
            CHECK(d.events[1] == Ev{0, SD, kAccentV, StepLevel::Accent, false});
            CHECK(d.events[2] == Ev{gap, BD, kNormalV, StepLevel::Normal, false});
            CHECK(d.events[3] == Ev{gap, SD, kAccentV, StepLevel::Accent, false});
        }
    }
    SECTION("flamGrace sets the grace level; a lighter grace is a weak hit for MIDI out") {
        auto s = makeSeq(kSr);
        Pattern p;
        p.flamMs = 30;
        p.flamGrace = 40;
        p.at(0, LT, 0).setLevel(StepLevel::Accent);
        p.at(0, LT, 0).setFlam(true);
        p.at(0, kAccentTrack, 0).setLevel(StepLevel::Normal);
        s->setPattern(0, p);
        Settings st = internalSettings(120.0);
        st.totalAccent = 1.0f;
        Driver d(*s);
        d.runUntil(st, 4000, 100);
        const float main = voltsOf(24 + 16 + 39);
        REQUIRE(d.events.size() == 2);
        CHECK(d.events[0] == Ev{0, LT, main * 0.4f, StepLevel::Weak, false});
        CHECK(d.events[1] == Ev{1440, LT, main, StepLevel::Accent, true});
    }
}

TEST_CASE("sub-steps: N hits a step/N apart with the level ramp; only the first sub-hit flams", "[seq][steps]") {
    for (int n = 1; n <= 4; ++n) {
        auto s = makeSeq(kSr);
        Pattern p;
        p.at(0, CH, 1).setLevel(StepLevel::Normal);
        p.at(0, CH, 1).setSubSteps(n);
        p.shuffle = 12;  // step 1 is odd: every sub-hit moves with it
        p.subStart = 25;
        p.subEnd = 100;
        s->setPattern(0, p);
        Driver d(*s);
        d.runUntil(internalSettings(120.0), kMeasure, 333);
        REQUIRE(d.events.size() == static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) {
            const auto& e = d.events[static_cast<std::size_t>(k)];
            CHECK(e.sample == kStep + 3000 + k * kStep / n);
            // lerp(subStart, subEnd, k/(N-1)) % of the step's level; a single hit is unscaled.
            const float pct = n == 1 ? 100.0f : 25.0f + 75.0f * static_cast<float>(k) / static_cast<float>(n - 1);
            CHECK_THAT(e.volts, WithinAbs(kNormalV * pct / 100.0f, 1e-6));
            CHECK(e.level == StepLevel::Normal);
        }
    }
    auto s = makeSeq(kSr);
    Pattern p;
    p.flamMs = 24;
    p.flamGrace = 50;
    p.subStart = 100;
    p.subEnd = 40;
    p.at(0, CH, 0).setLevel(StepLevel::Accent);
    p.at(0, CH, 0).setSubSteps(4);
    p.at(0, CH, 0).setFlam(true);
    s->setPattern(0, p);
    Driver d(*s);
    d.runUntil(internalSettings(120.0), kMeasure, 4096);
    REQUIRE(samplesOf(d.events) == std::vector<std::int64_t>{0, 1152, 1500, 3000, 4500});
    CHECK_THAT(d.events[0].volts, WithinAbs(kAccentV * 0.5f, 1e-6));  // grace of the first sub-hit
    CHECK_THAT(d.events[1].volts, WithinAbs(kAccentV, 1e-6));
    CHECK_THAT(d.events[2].volts, WithinAbs(kAccentV * 0.8f, 1e-6));
    CHECK_THAT(d.events[3].volts, WithinAbs(kAccentV * 0.6f, 1e-6));
    CHECK_THAT(d.events[4].volts, WithinAbs(kAccentV * 0.4f, 1e-6));
    for (std::size_t i = 1; i < 5; ++i) CHECK(d.events[i].level == StepLevel::Accent);
}

TEST_CASE("timing jitter: one deterministic delay of 0..4 ms per step, shared by all its hits", "[seq][steps]") {
    auto s = makeSeq(kSr);
    Pattern p = everyStep(BD);
    for (int i = 0; i < 16; ++i) {
        p.at(0, SD, i).setLevel(StepLevel::Normal);
        p.at(0, CH, i).setLevel(StepLevel::Normal);
        p.at(0, CH, i).setSubSteps(2);
    }
    s->setPattern(0, p);
    Settings st = internalSettings(120.0);
    st.jitter = true;
    st.jitterSeed = 1234;
    const auto a = runInternal(*s, st, fixedBlocks(8 * kMeasure, 4096));
    s->reset();
    const auto b = runInternal(*s, st, fixedBlocks(8 * kMeasure, 77));
    REQUIRE(a == b);  // deterministic for any partition
    std::vector<double> delays;
    for (std::int64_t n = 0; n < 8 * 16; ++n) {
        std::vector<std::int64_t> mine;
        for (const Ev& e : a)
            if (e.sample >= n * kStep && e.sample < n * kStep + 400 && e.voice != CH) mine.push_back(e.sample);
        // BD and SD of the step share the delay; CH's first sub-hit too.
        REQUIRE(mine.size() == 2);
        CHECK(mine[0] == mine[1]);
        const std::int64_t want =
            n * kStep + static_cast<std::int64_t>(std::floor(jitterMs(1234, n / 16, static_cast<int>(n % 16)) * 48.0 +
                                                             0.5 + 1e-6));
        CHECK(mine[0] == want);
        delays.push_back(static_cast<double>(mine[0] - n * kStep) / 48.0);
        for (const Ev& e : a)
            if (e.voice == CH && e.sample >= n * kStep && e.sample < n * kStep + 400) CHECK(e.sample == mine[0]);
    }
    const double mx = *std::max_element(delays.begin(), delays.end());
    const double mn = *std::min_element(delays.begin(), delays.end());
    CHECK(mx < 4.0 + 1e-9);
    CHECK(mn >= 0.0);
    CHECK(mx - mn > 3.0);  // spread over the range
    double mean = 0.0, var = 0.0;
    for (double x : delays) mean += x / static_cast<double>(delays.size());
    for (double x : delays) var += (x - mean) * (x - mean) / static_cast<double>(delays.size());
    CHECK(std::sqrt(var) > 0.8);   // the original's ~1.2 ms RMS
    CHECK(std::sqrt(var) < 1.6);
    // Another seed gives another sequence; jitter off gives the grid.
    s->reset();
    const auto first = runInternal(*s, st, fixedBlocks(kMeasure, 512));
    st.jitterSeed = 99;
    s->reset();
    CHECK(runInternal(*s, st, fixedBlocks(kMeasure, 512)) != first);
    st.jitter = false;
    s->reset();
    for (const Ev& e : runInternal(*s, st, fixedBlocks(kMeasure, 512))) CHECK(e.sample % (kStep / 2) == 0);
}

TEST_CASE("probability: frequencies match the code and repeat exactly", "[seq][steps]") {
    auto s = makeSeq(44100.0);
    Pattern p;
    p.length = 16;
    p.scale = Scale::ThirtySecond;
    for (int code = 0; code < 8; ++code)
        for (int st = 0; st < 16; ++st) {
            p.at(0, code, st).setLevel(StepLevel::Normal);
            p.at(0, code, st).setProbabilityCode(code);
        }
    s->setPattern(0, p);
    const Settings st = internalSettings(400.0);
    const int measures = 1500;
    const std::int64_t total = static_cast<std::int64_t>(measures * 2.0 * 60.0 / 400.0 * 44100.0);
    auto first = runInternal(*s, st, fixedBlocks(total, 8192));
    s->reset();
    auto second = runInternal(*s, st, fixedBlocks(total, 1000));
    REQUIRE(first == second);

    std::array<int, 8> counts{};
    for (const Ev& e : first) ++counts[static_cast<std::size_t>(e.voice)];
    const double trials = measures * 16.0;
    for (int code = 0; code < 8; ++code) {
        const double expected = (8 - code) / 8.0;
        INFO("code " << code);
        CHECK_THAT(counts[static_cast<std::size_t>(code)] / trials, WithinAbs(expected, 0.02));
    }
    // Decisions differ from measure to measure (hash of measure, step, track).
    auto onsIn = [&](int voice, std::int64_t measure) {
        std::vector<int> ons;
        const double measureSamples = 2.0 * 60.0 / 400.0 * 44100.0;
        for (const Ev& e : first)
            if (e.voice == voice && static_cast<std::int64_t>(static_cast<double>(e.sample) / measureSamples) == measure)
                ons.push_back(static_cast<int>(std::lround(
                    (static_cast<double>(e.sample) - static_cast<double>(measure) * measureSamples) / (measureSamples / 16))));
        return ons;
    };
    int same = 0;
    for (int m = 0; m < 20; ++m) same += onsIn(4, m) == onsIn(4, m + 1) ? 1 : 0;
    CHECK(same < 3);
}

TEST_CASE("per-track length: a voice of length L reads (measure*length + step) mod L", "[seq][steps]") {
    auto s = makeSeq(kSr);
    Pattern p;
    p.at(0, BD, 0).setLevel(StepLevel::Normal);
    p.trackLength[BD] = 3;
    p.at(0, SD, 2).setLevel(StepLevel::Normal);
    p.trackLength[SD] = 5;
    p.at(0, CH, 20).setLevel(StepLevel::Normal);  // longer than the pattern
    p.trackLength[CH] = 24;
    p.at(0, kAccentTrack, 0).setLevel(StepLevel::Normal);  // follows the pattern (16)
    s->setPattern(0, p);
    Driver d(*s);
    d.runUntil(internalSettings(120.0), 4 * kMeasure, 1024);
    for (const Ev& e : d.events) {
        const std::int64_t n = e.sample / kStep;  // global step index
        REQUIRE(e.sample % kStep == 0);
        if (e.voice == BD) CHECK(n % 3 == 0);
        if (e.voice == SD) CHECK(n % 5 == 2);
        if (e.voice == CH) CHECK(n % 24 == 20);
        // The total-accent row follows the pattern: only step 0 of each measure.
        CHECK(e.ta == (n % 16 == 0));
        CHECK(e.level == StepLevel::Normal);
    }
    CHECK(onlyVoice(d.events, BD).size() == 22);  // ceil(64 / 3)
    CHECK(onlyVoice(d.events, SD).size() == 13);  // n = 2, 7, ..., 62
    CHECK(onlyVoice(d.events, CH).size() == 2);   // n = 20, 44
}

TEST_CASE("dynamics: accent-DAC code = base + voice accent + total accent x knob", "[seq][steps]") {
    for (float knob : {0.0f, 0.3f, 0.7f, 1.0f}) {
        auto s = makeSeq(kSr);
        Pattern p;
        for (int st = 0; st < 2; ++st) {
            p.at(0, BD, st).setLevel(StepLevel::Weak);
            p.at(0, SD, st).setLevel(StepLevel::Normal);
            p.at(0, LT, st).setLevel(StepLevel::Accent);
        }
        p.at(0, kAccentTrack, 0).setLevel(StepLevel::Weak);  // any non-zero level means "on"
        s->setPattern(0, p);
        Settings st = internalSettings(120.0);
        st.totalAccent = knob;
        Driver d(*s);
        d.runUntil(st, 2 * kStep, 512);
        const int ta = totalCodes(knob);
        CHECK(ta == static_cast<int>(std::lround(39.0 * knob)));
        const std::vector<Ev> want = {
            {0, BD, voltsOf(12 + ta), StepLevel::Weak, true},
            {0, SD, voltsOf(24 + ta), StepLevel::Normal, true},
            {0, LT, voltsOf(24 + 16 + ta), StepLevel::Accent, true},
            {kStep, BD, kWeakV, StepLevel::Weak, false},
            {kStep, SD, kNormalV, StepLevel::Normal, false},
            {kStep, LT, kAccentV, StepLevel::Accent, false},
        };
        INFO("knob " << knob);
        REQUIRE(d.events == want);
        // The same codes Hit builds.
        CHECK(want[2].volts == Hit::accented(true, true, knob).acc);
        CHECK(want[1].volts == Hit::accented(false, true, knob).acc);
        CHECK(want[5].volts == Hit::accented(true, false, knob).acc);
    }
    // Full scale clamps at code 63 (5 V).
    CHECK(voltsOf(24 + 16 + 39) == AccentDac::kVolts);
}

TEST_CASE("AB variation alternates by measure index and starts on A", "[seq][steps]") {
    auto s = makeSeq(kSr);
    Pattern p;
    p.at(0, BD, 0).setLevel(StepLevel::Normal);  // A
    p.at(1, SD, 0).setLevel(StepLevel::Normal);  // B
    s->setPattern(0, p);
    Settings st = internalSettings(120.0);
    st.variation = VariationMode::AB;
    Driver d(*s);
    d.runUntil(st, 4 * kMeasure, 999);
    REQUIRE(d.events.size() == 4);
    CHECK(d.events[0].voice == BD);
    CHECK(d.events[1].voice == SD);
    CHECK(d.events[2].voice == BD);
    CHECK(d.events[3].voice == SD);
    CHECK(s->currentVariation() == 1);

    SECTION("host start in an odd measure plays B") {
        Settings hs = hostSettings();
        hs.variation = VariationMode::AB;
        s->reset();
        Driver h(*s);
        const auto e = h.host(4.0, 120.0, hs, 100);
        REQUIRE(e.size() == 1);
        CHECK(e[0].voice == SD);
        CHECK(s->currentVariation() == 1);
    }
    SECTION("a variation switch takes effect at the next measure") {
        s->reset();
        Driver h(*s);
        Settings a = internalSettings(120.0);
        h.runUntil(a, kMeasure / 2, 512);
        a.variation = VariationMode::B;
        h.runUntil(a, 2 * kMeasure, 512);
        REQUIRE(h.events.size() == 2);
        CHECK(h.events[0].voice == BD);
        CHECK(h.events[1].voice == SD);
    }
}

TEST_CASE("pattern selection: queued at the measure end, immediate, and while stopped", "[seq][steps]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    s->setPattern(1, everyStep(SD));
    s->setPattern(2, everyStep(CH, 8));
    const Settings st = internalSettings(120.0);
    Driver d(*s);

    SECTION("queued") {
        d.runUntil(st, 33000, 1000);
        s->selectPattern(1);
        CHECK(s->queuedPattern() == 1);
        CHECK(s->currentPattern() == 0);
        CHECK(s->selectedPattern() == 0);
        d.runUntil(st, kMeasure, 1000);  // the downbeat is not processed yet
        CHECK(s->queuedPattern() == 1);
        d.runUntil(st, 2 * kMeasure, 1000);
        for (const Ev& e : d.events) CHECK(e.voice == (e.sample < kMeasure ? BD : SD));
        CHECK(d.events.size() == 32);
        CHECK(s->queuedPattern() == -1);
        CHECK(s->selectedPattern() == 1);
        CHECK(s->currentPattern() == 1);
    }
    SECTION("queued in the block just before the downbeat still lands on it") {
        d.runUntil(st, kMeasure, 4000);
        s->selectPattern(1);
        const auto e = d.runUntil(st, kMeasure + 10, 10);
        REQUIRE(e.size() == 1);
        CHECK(e[0].voice == SD);
    }
    SECTION("re-selecting the current pattern cancels the queue") {
        d.runUntil(st, 10000, 1000);
        s->selectPattern(1);
        s->selectPattern(0);
        CHECK(s->queuedPattern() == -1);
        d.runUntil(st, 2 * kMeasure, 1000);
        for (const Ev& e : d.events) CHECK(e.voice == BD);
    }
    SECTION("immediate keeps the step position") {
        d.runUntil(st, 33000, 1000);  // steps 0..5 fired
        s->selectPattern(1, true);
        CHECK(s->currentPattern() == 1);
        CHECK(s->currentStep() == 5);
        d.runUntil(st, 2 * kMeasure, 1000);
        CHECK(d.events.size() == 32);
        for (const Ev& e : d.events) CHECK(e.voice == (e.sample < 33000 ? BD : SD));
    }
    SECTION("immediate to a shorter pattern ends the measure at its length") {
        d.runUntil(st, 33000, 1000);
        s->selectPattern(2, true);
        d.runUntil(st, 48000 + 8 * kStep, 1000);
        const auto t = samplesOf(onlyVoice(d.events, CH));
        // Steps 6, 7 of pattern 2, then a new 8-step measure from 48000.
        REQUIRE(t.front() == 36000);
        REQUIRE(t[2] == 48000);
        CHECK(t.size() == 10);
        CHECK(s->measureCount() == 1);
    }
    SECTION("while stopped the selection is immediate") {
        s->selectPattern(2);
        CHECK(s->currentPattern() == 2);
        CHECK(s->selectedPattern() == 2);
        CHECK(s->queuedPattern() == -1);
        const auto e = d.runUntil(st, 100, 100);
        REQUIRE(e.size() == 1);
        CHECK(e[0].voice == CH);
    }
    SECTION("a queue pending at stop takes over at once") {
        d.runUntil(st, 10000, 1000);
        s->selectPattern(1);
        Settings off = st;
        off.run = false;
        d.block(Transport{}, off, 100);
        CHECK(s->selectedPattern() == 1);
        CHECK(s->queuedPattern() == -1);
    }
}

TEST_CASE("chains: each measure plays the next entry; relocation walks from PPQ 0", "[seq][steps]") {
    auto s = makeSeq(kSr);
    s->setPattern(2, patternWith(BD, {0}));
    s->setPattern(5, patternWith(SD, {0}, 8));  // 2 qn long
    s->setPattern(7, patternWith(CH, {0}));
    Settings st = hostSettings();
    st.play = PlayMode::Chain;

    SECTION("equal lengths") {
        Chain c;
        c.length = 3;
        c.entries[0] = 2;
        c.entries[1] = 7;
        c.entries[2] = 2;
        s->setChain(c);
        const auto ev = runHost(*s, st, kSr, 120.0, 0.0, fixedBlocks(5 * kMeasure, 700));
        REQUIRE(ev.size() == 5);
        const int voices[] = {BD, CH, BD, BD, CH};
        for (int i = 0; i < 5; ++i) {
            CHECK(ev[static_cast<std::size_t>(i)].voice == voices[i]);
            CHECK(ev[static_cast<std::size_t>(i)].sample == i * kMeasure);
        }
        // Relocate into measure 4 (entry 1): pattern 7 is playing.
        s->reset();
        Driver d(*s);
        d.host(17.0, 120.0, st, 100);
        CHECK(s->currentPattern() == 7);
        CHECK(s->measureCount() == 4);
        CHECK(s->songPosition() == 1);
        CHECK(s->selectedPattern() == 0);  // the chain does not touch the selection
    }
    SECTION("mixed lengths: 4 + 2 qn cycle") {
        Chain c;
        c.length = 2;
        c.entries[0] = 2;
        c.entries[1] = 5;
        s->setChain(c);
        Driver d(*s);
        d.host(7.0, 120.0, st, 100);  // cycle 1 (6..12): measure 2 (pattern 2) from 6.0
        CHECK(s->currentPattern() == 2);
        CHECK(s->measureCount() == 2);
        CHECK(s->currentStep() == 4);
        double ppq = 7.0 + 100 / 24000.0;
        while (d.pos < 6 * 24000) {
            d.host(ppq, 120.0, st, 1000);
            ppq += 1000 / 24000.0;
        }
        // Downbeats: SD at 10.0 qn, BD at 12.0 (start was 7.0 at sample 0).
        REQUIRE(d.events.size() == 2);
        CHECK(d.events[0] == Ev{72000, SD, kNormalV, StepLevel::Normal, false});
        CHECK(d.events[1] == Ev{120000, BD, kNormalV, StepLevel::Normal, false});
        // A pure function of PPQ: the same blocks from 0 give the same events.
        s->reset();
        const auto ref = runHost(*s, st, kSr, 120.0, 0.0, fixedBlocks(24000 * 13, 4096));
        std::vector<std::int64_t> late;
        for (const Ev& e : ref)
            if (e.sample >= 7 * 24000) late.push_back(e.sample - 7 * 24000);
        CHECK(late == samplesOf(d.events));
    }
}

TEST_CASE("song tracks: TRACK play walks the selected track, loops, and reports its position", "[seq][steps]") {
    auto s = makeSeq(kSr);
    s->setPattern(patternIndex(0, 0, 0), patternWith(BD, {0}));
    s->setPattern(patternIndex(0, 1, 3), patternWith(SD, {0}, 12, Scale::EighthTriplet));  // 4 qn
    s->setPattern(patternIndex(1, 2, 15), patternWith(RD, {0}, 16, Scale::ThirtySecond));  // 2 qn
    SongTrack t;
    t.length = 4;
    t.entries[0] = static_cast<std::uint8_t>(patternIndex(0, 0, 0));
    t.entries[1] = static_cast<std::uint8_t>(patternIndex(0, 1, 3));
    t.entries[2] = static_cast<std::uint8_t>(patternIndex(1, 2, 15));
    t.entries[3] = static_cast<std::uint8_t>(patternIndex(0, 0, 0));
    s->setSongTrack(5, t);
    CHECK(s->bank().tracks[5] == t);
    Settings st = hostSettings();
    st.play = PlayMode::Track;
    st.songTrack = 5;

    const auto ev = runHost(*s, st, kSr, 120.0, 0.0, fixedBlocks(24000 * 30, 1000));
    // Measures: 4 + 4 + 2 + 4 = 14 qn per pass, then it loops.
    const std::vector<std::int64_t> want = {0, 4 * 24000, 8 * 24000, 10 * 24000, 14 * 24000, 18 * 24000,
                                            22 * 24000, 24 * 24000, 28 * 24000};
    REQUIRE(samplesOf(ev) == want);
    const int voices[] = {BD, SD, RD, BD, BD, SD, RD, BD, BD};
    for (std::size_t i = 0; i < want.size(); ++i) CHECK(ev[i].voice == voices[i]);
    CHECK(s->songPosition() == 0);
    CHECK(s->measureCount() == 8);

    SECTION("relocation into the second pass finds the entry by walking from PPQ 0") {
        s->reset();
        Driver d(*s);
        d.host(14.0 + 9.0, 120.0, st, 100);  // 9 qn into pass 2: entry 2 (8..10)
        CHECK(s->songPosition() == 2);
        CHECK(s->currentPattern() == patternIndex(1, 2, 15));
        CHECK(s->measureCount() == 6);
        CHECK(s->currentStep() == 8);  // 1 qn into a 32nd pattern: step 8 is on the range start
    }
    SECTION("an empty track or chain plays the selected pattern") {
        st.songTrack = 0;
        s->reset();
        s->selectPattern(patternIndex(0, 1, 3));
        Driver d(*s);
        d.host(0.0, 120.0, st, 100);
        CHECK(s->songPosition() == -1);
        CHECK(s->currentPattern() == patternIndex(0, 1, 3));
        st.play = PlayMode::Chain;
        d.host(100.0 / 24000.0, 120.0, st, 100);
        CHECK(s->songPosition() == -1);
    }
    SECTION("stopping clears the position; Pattern mode reports none") {
        Driver d(*s);
        d.host(0.0, 120.0, st, 100, false);
        CHECK(s->songPosition() == -1);
        st.play = PlayMode::Pattern;
        d.host(0.0, 120.0, st, 100);
        CHECK(s->songPosition() == -1);
    }
    SECTION("entries beyond the pattern memory are clamped, never read out of range") {
        SongTrack bad;
        bad.length = 2;
        bad.entries[0] = 200;
        bad.entries[1] = 255;
        s->setSongTrack(5, bad);
        s->reset();
        const auto e = runHost(*s, st, kSr, 120.0, 0.0, fixedBlocks(24000 * 8, 512));
        CHECK(s->currentPattern() == kNumPatterns - 1);
        REQUIRE(e.size() == 4);  // pattern 96 is the 2 qn ride pattern
        for (const Ev& x : e) CHECK(x.voice == RD);
    }
}

TEST_CASE("auto fill-in replaces the last measure of each group; manual fill once", "[seq][steps]") {
    // The fill is slot `fillSlot` of the playing pattern's group.
    auto s = makeSeq(kSr);
    s->setPattern(patternIndex(0, 0, 1), patternWith(BD, {0}));
    s->setPattern(patternIndex(0, 0, 2), patternWith(SD, {0}));   // group 1 of bank I, slot 3
    s->setPattern(patternIndex(1, 2, 5), patternWith(RD, {0}));   // bank II, group 3
    s->setPattern(patternIndex(1, 2, 2), patternWith(CR, {0}));   // its fill
    s->selectPattern(patternIndex(0, 0, 1));
    Settings st = internalSettings(120.0);
    st.fillSlot = 2;

    SECTION("every 4") {
        st.autoFill = AutoFill::Every4;
        Driver d(*s);
        d.runUntil(st, 3 * kMeasure + 100, 1000);
        CHECK(s->currentPattern() == patternIndex(0, 0, 2));
        CHECK(s->selectedPattern() == patternIndex(0, 0, 1));
        d.runUntil(st, 9 * kMeasure, 1000);
        const int voices[] = {BD, BD, BD, SD, BD, BD, BD, SD, BD};
        REQUIRE(d.events.size() == 9);
        for (int i = 0; i < 9; ++i) CHECK(d.events[static_cast<std::size_t>(i)].voice == voices[i]);
    }
    SECTION("the fill comes from the group of the playing pattern") {
        st.autoFill = AutoFill::Every2;
        s->selectPattern(patternIndex(1, 2, 5));
        Driver d(*s);
        d.runUntil(st, 4 * kMeasure, 1000);
        const int voices[] = {RD, CR, RD, CR};
        REQUIRE(d.events.size() == 4);
        for (int i = 0; i < 4; ++i) CHECK(d.events[static_cast<std::size_t>(i)].voice == voices[i]);
    }
    SECTION("manual fill plays once, at the next measure") {
        Driver d(*s);
        d.runUntil(st, kMeasure + 5000, 1000);
        s->requestFill();
        d.runUntil(st, 5 * kMeasure, 1000);
        const int voices[] = {BD, BD, SD, BD, BD};
        REQUIRE(d.events.size() == 5);
        for (int i = 0; i < 5; ++i) CHECK(d.events[static_cast<std::size_t>(i)].voice == voices[i]);
    }
    SECTION("fill requested while stopped plays the first measure, counted") {
        s->requestFill();
        Driver d(*s);
        d.runUntil(st, 100, 100);
        CHECK(s->currentPattern() == patternIndex(0, 0, 2));
        CHECK(s->selectedPattern() == patternIndex(0, 0, 1));
        CHECK(s->measureCount() == 0);
        d.runUntil(st, 3 * kMeasure, 1000);
        const int voices[] = {SD, BD, BD};
        REQUIRE(d.events.size() == 3);
        for (int i = 0; i < 3; ++i) CHECK(d.events[static_cast<std::size_t>(i)].voice == voices[i]);
        CHECK(s->measureCount() == 2);
    }
    SECTION("a host start mid-measure keeps a requested fill for the next downbeat") {
        s->requestFill();
        Settings hs = hostSettings();
        hs.fillSlot = 2;
        Driver d(*s);
        d.host(1.0, 120.0, hs, 100);
        CHECK(s->currentPattern() == patternIndex(0, 0, 1));
        double ppq = 1.0 + 100.0 / 24000.0;
        while (d.pos < 4 * 24000) {
            d.host(ppq, 120.0, hs, 1000);
            ppq += 1000.0 / 24000.0;
        }
        REQUIRE(d.events.size() == 1);
        CHECK(d.events[0].voice == SD);
        CHECK(d.events[0].sample == 3 * 24000);
    }
    SECTION("in a chain the fill follows the group of each entry") {
        Chain c;
        c.length = 2;
        c.entries[0] = static_cast<std::uint8_t>(patternIndex(0, 0, 1));
        c.entries[1] = static_cast<std::uint8_t>(patternIndex(1, 2, 5));
        s->setChain(c);
        st.play = PlayMode::Chain;
        st.autoFill = AutoFill::Every4;
        Driver d(*s);
        d.runUntil(st, 8 * kMeasure, 1000);
        const int voices[] = {BD, RD, BD, CR, BD, RD, BD, CR};
        REQUIRE(d.events.size() == 8);
        for (int i = 0; i < 8; ++i) CHECK(d.events[static_cast<std::size_t>(i)].voice == voices[i]);
    }
    SECTION("host sync with a fill of a different length stays a pure function of PPQ") {
        Pattern shortFill = patternWith(SD, {0}, 8);  // 2 qn
        s->setPattern(patternIndex(0, 0, 2), shortFill);
        Settings hs = hostSettings();
        hs.fillSlot = 2;
        hs.autoFill = AutoFill::Every4;
        // Group = 3 x 4 + 2 = 14 qn. PPQ 27 is in the fill of group 1 (26..28).
        Driver d(*s);
        d.host(27.0, 120.0, hs, 100);
        CHECK(s->measureCount() == 7);
        CHECK(s->currentPattern() == patternIndex(0, 0, 2));
        s->reset();
        const auto ref = runHost(*s, hs, kSr, 120.0, 0.0, fixedBlocks(24000 * 30, 2048));
        const auto t = samplesOf(ref);
        const std::vector<std::int64_t> want = {0, 4 * 24000, 8 * 24000, 12 * 24000, 14 * 24000,
                                                18 * 24000, 22 * 24000, 26 * 24000, 28 * 24000};
        CHECK(t == want);
        CHECK(ref[3].voice == SD);
        CHECK(ref[7].voice == SD);
    }
}

TEST_CASE("live recording quantises to the nearest step and never double-fires", "[seq][steps]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, patternWith(BD, {0}));
    Settings st = internalSettings(120.0);
    st.record = true;
    Driver d(*s);
    d.runUntil(st, 23000, 1000);  // next step: 4 at 24000

    SECTION("hit just before the upcoming step: recorded there, not fired again this pass") {
        const RecordResult r = s->recordHit(SD, false, 500);  // at 23500
        CHECK(r.changed);
        CHECK(r.pattern == 0);
        CHECK(r.variation == 0);
        CHECK(r.track == SD);
        CHECK(r.step == 4);
        CHECK(r.cell.level() == StepLevel::Normal);
        CHECK(s->bank().patterns[0].at(0, SD, 4).level() == StepLevel::Normal);
        d.runUntil(st, 2 * kMeasure, 1000);
        const auto sd = onlyVoice(d.events, SD);
        REQUIRE(sd.size() == 1);
        CHECK(sd[0].sample == kMeasure + 24000);  // the next pass plays it
    }
    SECTION("hit just after a step that already fired: recorded, plays next pass") {
        d.runUntil(st, 25000, 1000);
        const RecordResult r = s->recordHit(SD, true, 1000);  // at 26000 -> step 4
        CHECK(r.changed);
        CHECK(r.step == 4);
        CHECK(r.cell.level() == StepLevel::Accent);
        d.runUntil(st, 2 * kMeasure, 1000);
        const auto sd = onlyVoice(d.events, SD);
        REQUIRE(sd.size() == 1);
        CHECK(sd[0].sample == kMeasure + 24000);
        CHECK(sd[0].level == StepLevel::Accent);
    }
    SECTION("hit at the end of the measure goes to step 0 and skips the next downbeat once") {
        d.runUntil(st, kMeasure - 1000, 1000);
        const RecordResult r = s->recordHit(BD, true, 800);  // 200 samples before the downbeat
        CHECK(r.step == 0);
        d.runUntil(st, 3 * kMeasure, 1000);
        const auto bd = samplesOf(onlyVoice(d.events, BD));
        CHECK(bd == std::vector<std::int64_t>{0, 2 * kMeasure});
    }
    SECTION("no effect without record or when stopped") {
        Settings noRec = st;
        noRec.record = false;
        d.block(Transport{}, noRec, 100);
        CHECK_FALSE(s->recordHit(SD, false, 0).changed);
        CHECK_FALSE(s->bank().patterns[0].at(0, SD, 4).on());
        auto stopped = makeSeq(kSr);
        CHECK_FALSE(stopped->recordHit(SD, false, 0).changed);
    }
    SECTION("polymeter: the recorded cell is the one that plays at that time") {
        Pattern p = patternWith(BD, {0});
        p.trackLength[CH] = 3;
        s->setPattern(0, p);
        const RecordResult r = s->recordHit(CH, false, 500);  // step 4 of measure 0 -> CH cell 4 mod 3 = 1
        CHECK(r.step == 1);
    }
}

TEST_CASE("tap write near the measure end never silences another pattern or variation", "[seq][steps][record]") {
    // The nearest step is step 0 of the NEXT measure, but that measure plays
    // something else: the hit is written into the playing pattern/variation
    // and the next measure keeps its own downbeat.
    SECTION("chain: the next entry is another pattern") {
        auto s = makeSeq(kSr);
        s->setPattern(0, Pattern{});                // no BD
        Pattern accented;
        accented.at(0, BD, 0).setLevel(StepLevel::Accent);
        s->setPattern(1, accented);
        Chain c;
        c.length = 2;
        c.entries[0] = 0;
        c.entries[1] = 1;
        s->setChain(c);
        Settings st = internalSettings(120.0);
        st.play = PlayMode::Chain;
        st.record = true;
        Driver d(*s);
        d.runUntil(st, kMeasure - 1000, 1000);
        const RecordResult r = s->recordHit(BD, false, 800);  // 200 samples before the downbeat
        CHECK(r.changed);
        CHECK(r.pattern == 0);
        CHECK(r.step == 0);
        d.runUntil(st, 3 * kMeasure + 1000, 1000);
        const auto bd = onlyVoice(d.events, BD);
        REQUIRE(bd.size() == 3);
        CHECK(bd[0].sample == kMeasure);            // pattern 1's own accented downbeat
        CHECK(bd[0].level == StepLevel::Accent);
        CHECK(bd[1].sample == 2 * kMeasure);        // pattern 0, recorded
        CHECK(bd[1].level == StepLevel::Normal);
        CHECK(bd[2].sample == 3 * kMeasure);
    }
    SECTION("AB: the next measure plays variation B") {
        auto s = makeSeq(kSr);
        Pattern p;
        p.at(1, BD, 0).setLevel(StepLevel::Accent);  // only B has a BD
        s->setPattern(0, p);
        Settings st = internalSettings(120.0);
        st.variation = VariationMode::AB;
        st.record = true;
        Driver d(*s);
        d.runUntil(st, kMeasure - 1000, 1000);
        const RecordResult r = s->recordHit(BD, false, 800);
        CHECK(r.variation == 0);
        CHECK(r.step == 0);
        d.runUntil(st, 2 * kMeasure + 1000, 1000);
        const auto bd = onlyVoice(d.events, BD);
        REQUIRE(bd.size() == 2);
        CHECK(bd[0].sample == kMeasure);  // B's downbeat still plays
        CHECK(bd[0].level == StepLevel::Accent);
        CHECK(bd[1].sample == 2 * kMeasure);  // A, recorded
    }
    SECTION("a queued pattern takes over at the downbeat") {
        auto s = makeSeq(kSr);
        s->setPattern(3, patternWith(BD, {0}));
        Settings st = internalSettings(120.0);
        st.record = true;
        Driver d(*s);
        d.runUntil(st, kMeasure - 1000, 1000);
        s->selectPattern(3);
        CHECK(s->recordHit(BD, false, 800).pattern == 0);
        d.runUntil(st, kMeasure + 1000, 1000);
        const auto bd = samplesOf(onlyVoice(d.events, BD));
        CHECK(bd == std::vector<std::int64_t>{kMeasure});
    }
}

TEST_CASE("a fast live roll into one large block never double-fires", "[seq][steps][record]") {
    // Host at 1000 BPM, 32nd scale, 44.1 kHz: one 8192-sample block holds 24
    // steps. Every one of them played live (and recorded) before the block
    // must stay silent on this pass.
    constexpr double sr = 44100.0;
    constexpr double bpm = 1000.0;
    const double spp = sr * 60.0 / bpm;
    const double stepSamples = spp / 8.0;
    for (int rollLen : {8, 9, 16, 24}) {
        INFO("roll " << rollLen);
        auto s = makeSeq(sr);
        Pattern p;
        p.scale = Scale::ThirtySecond;
        p.length = 64;
        s->setPattern(0, p);
        Settings st = hostSettings();
        st.record = true;
        Driver d(*s);
        d.host(0.0, bpm, st, 1);
        for (int k = 1; k <= rollLen; ++k) {
            const RecordResult r = s->recordHit(BD, false, static_cast<int>(std::lround(k * stepSamples)) - 2);
            REQUIRE(r.step == k);
        }
        d.host(1.0 / spp, bpm, st, 8192);
        CHECK(onlyVoice(d.events, BD).empty());
        // The next pass plays every recorded step.
        d.host((1.0 + 8192.0) / spp, bpm, st, 8 * 8192);
        CHECK(static_cast<int>(onlyVoice(d.events, BD).size()) >= rollLen);
    }
}
