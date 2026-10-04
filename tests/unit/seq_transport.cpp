// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Sequencer transport handling: relocation, host loops, stop/start, tempo
// changes, the Internal and HostNote clocks and mode switches.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "SeqTestUtil.h"

using namespace seqtest;

namespace {
constexpr double kSr = 48000.0;
constexpr double kSpp = 24000.0;  // samples per quarter note at 120 BPM / 48 kHz
}  // namespace

TEST_CASE("a forward jump re-anchors and skips the steps in between", "[seq][transport]") {
    auto s = makeSeq(kSr);
    Pattern p = everyStep(BD);
    p.at(0, SD, 3).setLevel(StepLevel::Normal);
    p.at(0, SD, 3).setFlam(true);
    p.flamMs = 100;  // 4800 samples: the main SD hit is still pending at the jump
    p.flamGrace = 60;
    s->setPattern(0, p);
    Settings st = hostSettings();
    Driver d(*s);
    for (int i = 0; i < 38; ++i) d.host(i * 500 / kSpp, 120.0, st, 500);  // to sample 19000
    REQUIRE(onlyVoice(d.events, SD).size() == 1);                         // grace at 18000
    const auto after = d.host(8.5, 120.0, st, 7000);
    // 8.5 qn is step 34 = step 2 of measure 2: fires at offset 0, then step 3 at 6000.
    REQUIRE(after.size() == 3);
    CHECK(after[0].sample - 19000 == 0);
    CHECK(after[0].voice == BD);
    CHECK(after[1].sample - 19000 == 6000);
    CHECK(after[2].sample - 19000 == 6000);
    CHECK(after[2].voice == SD);  // the new step 3 grace; the old pending main hit is gone
    CHECK(after[2].volts < kNormalV);
    CHECK(s->measureCount() == 2);
    CHECK(s->currentStep() == 3);
}

TEST_CASE("a backward jump re-anchors without replaying the stale pending hits", "[seq][transport]") {
    auto s = makeSeq(kSr);
    Pattern p = everyStep(BD);
    p.shuffle = 8;  // odd steps 8/96 beat = 2000 samples late
    s->setPattern(0, p);
    Driver d(*s);
    const Settings st = hostSettings();
    d.host(0.0, 120.0, st, 7000);  // step 0 at 0; step 1 due at 8000 is pending
    const auto ev = d.host(2.0, 120.0, st, 1000);  // jump back... to 2.0 (a jump either way)
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].sample == 7000);  // step 8 at offset 0
    const auto ev2 = d.host(2.0 + 1000 / kSpp, 120.0, st, 8000);
    REQUIRE(ev2.size() == 1);
    CHECK(ev2[0].sample == 7000 + 8000);  // step 9 (odd) at 6000 + 2000 after the jump
}

TEST_CASE("host loop: the loop start downbeat fires and nothing stale leaks across", "[seq][transport]") {
    auto s = makeSeq(kSr);
    Pattern p = everyStep(BD);
    p.shuffle = 8;
    p.flamMs = 100;
    p.at(0, SD, 15).setLevel(StepLevel::Normal);
    p.at(0, SD, 15).setFlam(true);
    s->setPattern(0, p);
    const Settings st = hostSettings();
    // Step 15 sits at 3.75 + 1/12 qn (sample 92000); its main hit would be
    // at 96800, past the loop end at 4 qn (96000).
    for (int blockSize : {500, 512, 4096}) {
        s->reset();
        Driver d(*s);
        for (int pass = 0; pass < 3; ++pass) {
            std::int64_t local = 0;
            while (local < 96000) {
                const int n = static_cast<int>(std::min<std::int64_t>(blockSize, 96000 - local));
                d.host(static_cast<double>(local) / kSpp, 120.0, st, n);
                local += n;
            }
        }
        INFO("block size " << blockSize);
        const auto bd = onlyVoice(d.events, BD);
        const auto sd = onlyVoice(d.events, SD);
        REQUIRE(bd.size() == 48);   // 16 per pass, every downbeat included
        // Graces only: each main hit is dropped by the loop relocation (and
        // the last one is still pending when the run ends).
        REQUIRE(sd.size() == 3);
        CHECK(sd[0].sample == 92000);
        CHECK(sd[1].sample == 96000 + 92000);
        CHECK(sd[2].sample == 192000 + 92000);
        for (int pass = 0; pass < 3; ++pass) CHECK(bd[static_cast<std::size_t>(pass * 16)].sample == pass * 96000);
    }
}

TEST_CASE("transport stop drops pending hits and resets the step; restart is fresh", "[seq][transport]") {
    auto s = makeSeq(kSr);
    Pattern p = patternWith(SD, {0});
    p.at(0, SD, 0).setFlam(true);
    p.flamMs = 50;  // main hit at 2400
    s->setPattern(0, p);
    const Settings st = hostSettings();
    Driver d(*s);
    d.host(0.0, 120.0, st, 1000);
    CHECK(s->isPlaying());
    CHECK(s->currentStep() == 0);
    const auto stopped = d.host(1000 / kSpp, 120.0, st, 5000, false);
    CHECK(stopped.empty());
    CHECK_FALSE(s->isPlaying());
    CHECK(s->currentStep() == -1);
    // Many stopped blocks: still nothing.
    for (int i = 0; i < 10; ++i) CHECK(d.host(1.0, 120.0, st, 512, false).empty());
    // Restart at 0: the grace fires at 0 and the main at 2400, once each.
    const auto again = d.host(0.0, 120.0, st, 4000);
    REQUIRE(again.size() == 2);
    CHECK(again[0].sample - (d.pos - 4000) == 0);
    CHECK(again[1].sample - (d.pos - 4000) == 2400);
}

TEST_CASE("Internal clock: run starts at 0, tempo changes take effect per call", "[seq][transport]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    Settings st = internalSettings(120.0);
    Driver d(*s);
    d.runUntil(st, 15000, 1000);  // steps at 0, 6000, 12000; PPQ now 0.625
    st.internalBpm = 60.0;
    d.runUntil(st, 70000, 777);
    const auto t = samplesOf(d.events);
    // The remaining 0.125 qn to step 3 takes 6000 samples at 60 BPM.
    REQUIRE(t == std::vector<std::int64_t>{0, 6000, 12000, 21000, 33000, 45000, 57000, 69000});
    CHECK(s->tempo() == 60.0);

    // Stop, then start again: back to PPQ 0, measure 0, step 0 at offset 0.
    st.run = false;
    CHECK(d.block(Transport{}, st, 3000).empty());
    CHECK_FALSE(s->isPlaying());
    st.run = true;
    const auto restart = d.block(Transport{}, st, 100);
    REQUIRE(restart.size() == 1);
    CHECK(restart[0].sample == d.pos - 100);
    CHECK(s->measureCount() == 0);
    CHECK(s->currentStep() == 0);
}

TEST_CASE("HostSync follows host tempo changes without seeing a relocation", "[seq][transport]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    const Settings st = hostSettings();
    Driver d(*s);
    d.host(0.0, 120.0, st, 15000);
    d.host(0.625, 60.0, st, 60000);
    REQUIRE(samplesOf(d.events) == std::vector<std::int64_t>{0, 6000, 12000, 21000, 33000, 45000, 57000, 69000});
}

TEST_CASE("Internal mode uses host tempo when the host provides one", "[seq][transport]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    const Settings st = internalSettings(120.0);
    Driver d(*s);
    Transport t;
    t.hostValid = true;
    t.playing = false;  // the host transport does not matter in Internal mode
    t.bpm = 60.0;
    t.ppq = 123.0;
    d.block(t, st, 30000);
    CHECK(samplesOf(d.events) == std::vector<std::int64_t>{0, 12000, 24000});
    CHECK(s->tempo() == 60.0);
}

TEST_CASE("HostNote: a held pattern note plays that pattern from its top", "[seq][transport]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    s->setPattern(3, everyStep(SD));
    Settings st;
    st.mode = Mode::HostNote;
    Driver d(*s);
    CHECK(d.block(Transport{}, st, 1000).empty());
    CHECK_FALSE(s->isPlaying());

    s->notePatternOn(3);
    auto ev = d.block(Transport{}, st, 7000);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0] == Ev{1000, SD, kNormalV, StepLevel::Normal, false});
    CHECK(ev[1].sample == 7000);
    CHECK(s->selectedPattern() == 3);
    CHECK(s->currentPattern() == 3);

    s->notePatternOn(0);  // a new note restarts from the top with its pattern
    ev = d.block(Transport{}, st, 100);
    REQUIRE(ev.size() == 1);
    CHECK(ev[0] == Ev{8000, BD, kNormalV, StepLevel::Normal, false});
    CHECK(s->currentStep() == 0);

    s->notePatternOff();
    CHECK(d.block(Transport{}, st, 20000).empty());
    CHECK_FALSE(s->isPlaying());
    CHECK(s->currentStep() == -1);
}

TEST_CASE("mode switches and invalid hosts", "[seq][transport]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    Driver d(*s);
    SECTION("Off is silent") {
        Settings st;
        st.mode = Mode::Off;
        st.run = true;
        CHECK(d.host(0.0, 120.0, st, 48000).empty());
        CHECK_FALSE(s->isPlaying());
    }
    SECTION("HostSync without a host transport runs as Internal") {
        Transport t;
        t.playing = true;
        t.hostValid = false;
        CHECK(d.block(t, hostSettings(), 48000).empty());  // run is off
        CHECK_FALSE(s->isPlaying());
        Settings st = hostSettings();
        st.run = true;
        st.internalBpm = 60.0;
        const auto ev = d.block(t, st, 48000);
        CHECK(samplesOf(ev) == std::vector<std::int64_t>{48000, 60000, 72000, 84000});
        CHECK(s->tempo() == 60.0);
        // The host appears: playback restarts on the host's clock.
        const auto host = d.host(2.0, 120.0, hostSettings(), 100);
        REQUIRE(host.size() == 1);
        CHECK(s->measureCount() == 0);
        CHECK(s->currentStep() == 8);
    }
    SECTION("a host with a non-finite or absurd position does not play") {
        for (double ppq : {std::nan(""), HUGE_VAL, -HUGE_VAL, 2.0e9}) {
            Transport bad{true, true, ppq, 120.0};
            CHECK(d.block(bad, hostSettings(), 48000).empty());
            CHECK_FALSE(s->isPlaying());
        }
    }
    SECTION("switching HostSync -> Internal while playing restarts at 0") {
        d.host(1.1, 120.0, hostSettings(), 1000);
        CHECK(s->measureCount() == 0);
        const auto ev = d.block(Transport{}, internalSettings(120.0), 100);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].sample == 1000);
        CHECK(s->currentStep() == 0);
    }
    SECTION("zero-length calls change nothing") {
        d.host(0.0, 120.0, hostSettings(), 1000);
        for (int i = 0; i < 5; ++i) CHECK(d.host(1000 / kSpp, 120.0, hostSettings(), 0).empty());
        const auto ev = d.host(1000 / kSpp, 120.0, hostSettings(), 6000);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0].sample == 6000);
    }
}

TEST_CASE("reset stops and rewinds but keeps the bank", "[seq][transport]") {
    auto s = makeSeq(kSr);
    Pattern p = everyStep(RD);
    s->setPattern(9, p);
    s->selectPattern(9);
    Driver d(*s);
    d.runUntil(internalSettings(120.0), 30000, 512);
    s->reset();
    CHECK_FALSE(s->isPlaying());
    CHECK(s->currentStep() == -1);
    CHECK(s->bank().patterns[9] == p);
    CHECK(s->selectedPattern() == 9);
}

TEST_CASE("setBank replaces the memory and keeps the play position", "[seq][transport]") {
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    Driver d(*s);
    d.host(0.0, 120.0, hostSettings(), 30000);  // steps 0..4 fired
    CHECK(s->currentStep() == 4);
    auto bank = std::make_unique<PatternBank>();
    bank->patterns[0] = everyStep(SD);
    bank->tracks[1].length = 1;
    s->setBank(*bank);
    CHECK(s->bank().patterns[0] == bank->patterns[0]);
    CHECK(s->bank().tracks[1] == bank->tracks[1]);
    CHECK(s->isPlaying());
    CHECK(s->currentStep() == 4);
    const auto ev = d.host(30000 / kSpp, 120.0, hostSettings(), 66000);
    REQUIRE(ev.size() == 11);  // steps 5..15 of the same measure, now SD
    CHECK(ev[0].sample == 30000);
    for (const Ev& e : ev) CHECK(e.voice == SD);
    CHECK(s->measureCount() == 0);
}

TEST_CASE("events carry the step class and the total-accent flag for MIDI out", "[seq][transport]") {
    auto s = makeSeq(kSr);
    Pattern p;
    p.at(0, BD, 0).setLevel(StepLevel::Weak);
    p.at(0, kAccentTrack, 0).setLevel(StepLevel::Normal);
    p.at(0, SD, 1).setLevel(StepLevel::Accent);
    s->setPattern(0, p);
    Driver d(*s);
    const auto ev = d.runUntil(internalSettings(120.0), 7000, 512);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].level == StepLevel::Weak);
    CHECK(ev[0].ta);
    CHECK(ev[1].level == StepLevel::Accent);
    CHECK_FALSE(ev[1].ta);
}

TEST_CASE("a host that holds its position while rolling fires the step there once", "[seq][transport]") {
    // Some hosts keep PPQ still during a count-in or while buffering.
    auto s = makeSeq(kSr);
    s->setPattern(0, everyStep(BD));
    Settings st = hostSettings();
    for (int n : {1, 64, 512, 4096}) {
        INFO("block " << n);
        s->reset();
        Driver d(*s);
        for (int i = 0; i < 10; ++i) d.host(0.0, 120.0, st, n);
        CHECK(onlyVoice(d.events, BD).size() == 1);
        // ...then it rolls on from there without losing or repeating a step.
        const std::int64_t held = d.pos;
        for (std::int64_t p = 0; p + n <= 4 * 6000; p += n) d.host(static_cast<double>(p) / kSpp, 120.0, st, n);
        const auto bd = samplesOf(onlyVoice(d.events, BD));
        REQUIRE(bd.size() == 4);
        for (std::size_t k = 1; k < bd.size(); ++k) CHECK(bd[k] == held + static_cast<std::int64_t>(k) * 6000);
    }
    // A real jump back to the same spot after time has passed still replays.
    s->reset();
    Driver d(*s);
    for (int i = 0; i < 20; ++i) d.host(i * 512 / kSpp, 120.0, st, 512);
    d.host(0.0, 120.0, st, 512);
    CHECK(onlyVoice(d.events, BD).size() == 3);  // steps 0 and 1, then step 0 again
}

TEST_CASE("a host position without a tempo still plays every step", "[seq][transport]") {
    // hostValid with a NaN or zero bpm: the PPQ advances at the host's real
    // rate (140 BPM here), not at internalBpm.
    for (double badBpm : {std::nan(""), 0.0, -5.0}) {
        auto s = makeSeq(kSr);
        s->setPattern(0, everyStep(BD));
        Settings st = hostSettings();
        st.internalBpm = 120.0;
        Driver d(*s);
        const double spp = kSr * 60.0 / 140.0;
        for (std::int64_t p = 0; p < 10 * 48000; p += 512) d.host(static_cast<double>(p) / spp, badBpm, st, 512);
        const auto bd = samplesOf(onlyVoice(d.events, BD));
        // Steps up to the end of the last block: 10.0107 s at 140 BPM = 23.358 qn.
        CHECK(bd.size() == 94);
        for (std::size_t k = 1; k < bd.size(); ++k) {
            const auto gap = bd[k] - bd[k - 1];
            CHECK(std::abs(static_cast<double>(gap) - spp / 4.0) <= 1.0 + (k == 1 ? spp / 4.0 : 0.0));
        }
        CHECK(std::abs(s->tempo() - 140.0) < 0.01);
    }
}
