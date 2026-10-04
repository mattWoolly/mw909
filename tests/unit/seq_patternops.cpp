// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Pattern editing helpers (architecture.md §5.6): mw808's set plus the
// original machine's rules (scale change, song-track measures, hardware
// conformance).

#include <catch2/catch_test_macros.hpp>

#include <random>

#include "SeqTestUtil.h"
#include "mw909/seq/PatternOps.h"

using namespace seqtest;

namespace {

int countOn(const Pattern& p, int var, int track, int upTo = kMaxSteps) {
    int n = 0;
    for (int s = 0; s < upTo; ++s) n += p.at(var, track, s).on() ? 1 : 0;
    return n;
}

}  // namespace

TEST_CASE("clear, copy and variation copy", "[seq][ops]") {
    std::mt19937 rng(1);
    Pattern p = randomPattern(rng, 0.5f);
    const Pattern orig = p;

    clearTrack(p, 0, SD);
    CHECK(countOn(p, 0, SD) == 0);
    CHECK(p.steps[1] == orig.steps[1]);

    copyTrack(p, 0, BD, CH);
    for (int s = 0; s < kMaxSteps; ++s) CHECK(p.at(0, CH, s) == p.at(0, BD, s));
    CHECK(p.trackLength[CH] == p.trackLength[BD]);
    CHECK(p.trackShuffle[CH] == p.trackShuffle[BD]);

    Pattern q;
    copyTrack(orig, 1, kAccentTrack, q, 0, kAccentTrack);
    for (int s = 0; s < kMaxSteps; ++s) CHECK(q.at(0, kAccentTrack, s) == orig.at(1, kAccentTrack, s));

    copyVariation(p, 0, 1);
    CHECK(p.steps[0] == p.steps[1]);

    clearPattern(p, 0);
    for (int t = 0; t < kNumTracks; ++t) CHECK(countOn(p, 0, t) == 0);
    CHECK(countOn(p, 1, BD) > 0);
    clearPattern(p);
    for (int t = 0; t < kNumTracks; ++t) CHECK(countOn(p, 1, t) == 0);
    CHECK(p.length == orig.length);
    CHECK(p.scale == orig.scale);

    // Out-of-range arguments are no-ops.
    Pattern r = orig;
    clearTrack(r, 2, 0);
    clearTrack(r, 0, kNumTracks);
    copyTrack(r, 0, -1, 3);
    shiftTrack(r, -1, 0, 3);
    invertTrack(r, 0, 99);
    copyVariation(r, 0, 5);
    setEuclidean(r, 3, 0, 3, 0);
    randomizeTrack(r, 0, 40, 1.0f, 3);
    setScale(r, static_cast<Scale>(9));
    CHECK(r == orig);
}

TEST_CASE("shift wraps within the track length", "[seq][ops]") {
    Pattern p;
    p.length = 16;
    p.trackLength[BD] = 5;
    p.at(0, BD, 0).setLevel(StepLevel::Accent);
    p.at(0, BD, 7).setLevel(StepLevel::Normal);  // beyond the 5-step loop: untouched
    shiftTrack(p, 0, BD, 3);
    CHECK(p.at(0, BD, 3).level() == StepLevel::Accent);
    CHECK_FALSE(p.at(0, BD, 0).on());
    CHECK(p.at(0, BD, 7).on());
    shiftTrack(p, 0, BD, -4);  // 3 - 4 = -1 -> 4
    CHECK(p.at(0, BD, 4).level() == StepLevel::Accent);
    shiftTrack(p, 0, BD, 5 * 1000 + 1);
    CHECK(p.at(0, BD, 0).level() == StepLevel::Accent);

    Pattern q = patternWith(SD, {15});
    q.at(0, kAccentTrack, 15).setLevel(StepLevel::Normal);
    shiftPattern(q, 0, 1);
    CHECK(q.at(0, SD, 0).on());
    CHECK(q.at(0, kAccentTrack, 0).on());
    CHECK_FALSE(q.at(0, SD, 15).on());
    CHECK(q.at(1, SD, 15).on());  // variation B untouched
}

TEST_CASE("invert within the track length", "[seq][ops]") {
    Pattern p = patternWith(CH, {0, 2}, 8);
    p.at(0, CH, 0).setFlam(true);
    invertTrack(p, 0, CH);
    CHECK(countOn(p, 0, CH) == 6);
    CHECK_FALSE(p.at(0, CH, 0).on());
    CHECK(p.at(0, CH, 1).bits == static_cast<std::uint8_t>(StepLevel::Normal));
    CHECK_FALSE(p.at(0, CH, 8).on());
}

TEST_CASE("randomize is deterministic, honours density and keeps the hats apart", "[seq][ops]") {
    std::array<float, kNumTracks> dens{};
    dens.fill(0.5f);
    dens[OH] = 0.9f;
    dens[CH] = 0.9f;
    Pattern a, b, c;
    randomizePattern(a, 0, dens, 1234);
    randomizePattern(b, 0, dens, 1234);
    randomizePattern(c, 0, dens, 1235);
    CHECK(a == b);
    CHECK_FALSE(a == c);
    for (int s = 0; s < 16; ++s) CHECK_FALSE((a.at(0, OH, s).on() && a.at(0, CH, s).on()));
    for (int t = 0; t < kNumTracks; ++t)
        for (int s = 0; s < kMaxSteps; ++s) {
            const Step st = a.at(0, t, s);
            CHECK_FALSE(st.flam());
            CHECK(st.subSteps() == 1);
            CHECK(st.probabilityCode() == 0);
        }
    // Density over many seeds.
    int on = 0, total = 0;
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        Pattern p;
        randomizeTrack(p, 0, SD, 0.3f, seed);
        on += countOn(p, 0, SD, 16);
        total += 16;
        CHECK(countOn(p, 0, SD) == countOn(p, 0, SD, 16));  // nothing beyond the length
    }
    CHECK(static_cast<double>(on) / total > 0.26);
    CHECK(static_cast<double>(on) / total < 0.34);
    // Writing one hat clears the other where they would coincide.
    Pattern h = everyStep(CH);
    randomizeTrack(h, 0, OH, 0.5f, 9);
    for (int s = 0; s < 16; ++s) CHECK(h.at(0, OH, s).on() != h.at(0, CH, s).on());
    Pattern zero;
    randomizeTrack(zero, 0, BD, 0.0f, 9);
    CHECK(countOn(zero, 0, BD) == 0);
    randomizeTrack(zero, 0, BD, 1.0f, 9);
    CHECK(countOn(zero, 0, BD) == 16);
}

TEST_CASE("expand doubles by repetition; Euclidean rhythms", "[seq][ops]") {
    Pattern p = patternWith(BD, {0, 3}, 12);
    p.trackLength[SD] = 5;
    p.at(0, SD, 1).setLevel(StepLevel::Normal);
    REQUIRE(expandPattern(p));
    CHECK(p.length == 24);
    CHECK(p.at(0, BD, 12).on());
    CHECK(p.at(0, BD, 15).on());
    CHECK_FALSE(p.at(0, SD, 13).on());  // own length: not repeated
    p.length = 40;
    CHECK_FALSE(expandPattern(p));
    CHECK(p.length == 40);

    Pattern e;
    e.length = 8;
    setEuclidean(e, 0, RD, 3, 0);  // tresillo x..x..x.
    const bool want[] = {true, false, false, true, false, false, true, false};
    for (int s = 0; s < 8; ++s) CHECK(e.at(0, RD, s).on() == want[s]);
    setEuclidean(e, 0, RD, 3, 1);
    for (int s = 0; s < 8; ++s) CHECK(e.at(0, RD, s).on() == want[(s + 7) % 8]);
    for (int k = 0; k <= 16; ++k) {
        Pattern f;
        setEuclidean(f, 0, RS, k, 0);
        CHECK(countOn(f, 0, RS) == k);
        // Maximally even: gaps differ by at most one.
        int minGap = 99, maxGap = 0, last = -1, first = -1;
        for (int s = 0; s < 16; ++s)
            if (f.at(0, RS, s).on()) {
                if (last >= 0) {
                    minGap = std::min(minGap, s - last);
                    maxGap = std::max(maxGap, s - last);
                } else {
                    first = s;
                }
                last = s;
            }
        if (k >= 2) {
            const int wrap = first + 16 - last;
            minGap = std::min(minGap, wrap);
            maxGap = std::max(maxGap, wrap);
            CHECK(maxGap - minGap <= 1);
        }
    }
    Pattern g;
    setEuclidean(g, 0, RS, 99, 0);
    CHECK(countOn(g, 0, RS) == 16);
}

TEST_CASE("promotedLevel follows the global accent rule", "[seq][ops]") {
    CHECK(promotedLevel(StepLevel::Weak, true) == StepLevel::Normal);
    CHECK(promotedLevel(StepLevel::Normal, true) == StepLevel::Accent);
    CHECK(promotedLevel(StepLevel::Accent, true) == StepLevel::Accent);
    CHECK(promotedLevel(StepLevel::Off, true) == StepLevel::Off);
    CHECK(promotedLevel(StepLevel::Weak, false) == StepLevel::Weak);
}

TEST_CASE("randomize never puts CH and OH on one step, for any seed and density", "[seq][ops]") {
    for (std::uint32_t seed = 0; seed < 500; ++seed) {
        std::array<float, kNumTracks> dens{};
        for (int t = 0; t < kNumTracks; ++t) dens[static_cast<std::size_t>(t)] = static_cast<float>((seed * 7 + static_cast<unsigned>(t) * 13) % 11) / 10.0f;
        dens[CH] = 1.0f;
        dens[OH] = seed % 2 == 0 ? 1.0f : 0.6f;
        Pattern p;
        p.length = static_cast<std::uint8_t>(1 + seed % 64);
        randomizePattern(p, static_cast<int>(seed % 2), dens, seed);
        for (int s = 0; s < kMaxSteps; ++s)
            REQUIRE_FALSE((p.at(static_cast<int>(seed % 2), OH, s).on() && p.at(static_cast<int>(seed % 2), CH, s).on()));
        // Both hats still get steps (neither always wins).
        if (seed % 2 == 0 && p.length >= 16) {
            CHECK(countOn(p, 0, CH) > 0);
            CHECK(countOn(p, 0, OH) > 0);
        }
    }
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        Pattern h = everyStep(OH);
        randomizeTrack(h, 0, CH, 1.0f, seed);
        for (int s = 0; s < 16; ++s) REQUIRE_FALSE((h.at(0, OH, s).on() && h.at(0, CH, s).on()));
    }
}

TEST_CASE("setScale: the hardware's automatic LAST STEP", "[seq][ops]") {
    CHECK(defaultLengthFor(Scale::Sixteenth) == 16);
    CHECK(defaultLengthFor(Scale::ThirtySecond) == 16);
    CHECK(defaultLengthFor(Scale::EighthTriplet) == 12);
    CHECK(defaultLengthFor(Scale::SixteenthTriplet) == 12);
    CHECK(defaultLengthFor(Scale::Eighth) == 16);
    Pattern p;  // 16 steps, 16th
    setScale(p, Scale::ThirtySecond);
    CHECK(p.scale == Scale::ThirtySecond);
    CHECK(p.length == 16);
    setScale(p, Scale::EighthTriplet);
    CHECK(p.length == 12);
    setScale(p, Scale::SixteenthTriplet);
    CHECK(p.length == 12);
    setScale(p, Scale::Sixteenth);  // the SCALE key cycles back to 16th
    CHECK(p.length == 16);
    // A length the user set is kept.
    p.length = 7;
    p.trackLength[BD] = 12;
    setScale(p, Scale::EighthTriplet);
    CHECK(p.length == 7);
    CHECK(p.trackLength[BD] == 12);
    p.length = 16;
    setScale(p, Scale::Eighth);
    CHECK(p.length == 16);
    // Steps are untouched.
    Pattern q = everyStep(SD);
    const auto steps = q.steps;
    setScale(q, Scale::EighthTriplet);
    CHECK(q.steps == steps);
}

TEST_CASE("song tracks: insert, delete and copy measures", "[seq][ops]") {
    SongTrack t;
    REQUIRE(insertMeasure(t, 0, 5));
    REQUIRE(insertMeasure(t, 1, 7));
    REQUIRE(insertMeasure(t, 0, 3));  // 3 5 7
    CHECK(t.length == 3);
    CHECK(t.entries[0] == 3);
    CHECK(t.entries[1] == 5);
    CHECK(t.entries[2] == 7);
    CHECK_FALSE(insertMeasure(t, 5, 1));            // beyond the end
    CHECK_FALSE(insertMeasure(t, 0, kNumPatterns));  // no such pattern
    CHECK_FALSE(insertMeasure(t, -1, 1));
    CHECK(t.length == 3);

    // COPY: insert a copy of [0, 2) before 3 -> 3 5 7 3 5
    REQUIRE(copyMeasures(t, 0, 2, 3));
    const std::uint8_t a[] = {3, 5, 7, 3, 5};
    REQUIRE(t.length == 5);
    for (int i = 0; i < 5; ++i) CHECK(t.entries[static_cast<std::size_t>(i)] == a[i]);
    // Before the source and at its ends is fine; into its middle is refused.
    REQUIRE(copyMeasures(t, 2, 2, 0));  // 7 3 | 3 5 7 3 5
    const std::uint8_t b[] = {7, 3, 3, 5, 7, 3, 5};
    REQUIRE(t.length == 7);
    for (int i = 0; i < 7; ++i) CHECK(t.entries[static_cast<std::size_t>(i)] == b[i]);
    CHECK_FALSE(copyMeasures(t, 1, 3, 2));
    CHECK_FALSE(copyMeasures(t, 1, 3, 3));
    CHECK(copyMeasures(t, 1, 1, 2));  // to == from + count
    CHECK(t.length == 8);
    CHECK_FALSE(copyMeasures(t, 0, 0, 0));
    CHECK_FALSE(copyMeasures(t, 6, 3, 0));  // past the end
    CHECK_FALSE(copyMeasures(t, 0, 1, 9));

    // DEL: single, range, and range clipped at the end.
    REQUIRE(deleteMeasure(t, 0));
    CHECK(t.length == 7);
    CHECK(t.entries[0] == 3);
    REQUIRE(deleteMeasure(t, 1, 3));
    CHECK(t.length == 4);
    REQUIRE(deleteMeasure(t, 2, 100));
    CHECK(t.length == 2);
    for (int i = 2; i < kMaxSongLength; ++i) CHECK(t.entries[static_cast<std::size_t>(i)] == 0);
    CHECK_FALSE(deleteMeasure(t, 2));
    CHECK_FALSE(deleteMeasure(t, 0, 0));

    // Capacity.
    SongTrack full;
    for (int i = 0; i < kMaxSongLength; ++i) REQUIRE(insertMeasure(full, i, i % 32));
    CHECK(full.length == kMaxSongLength);
    CHECK_FALSE(insertMeasure(full, 0, 1));
    CHECK_FALSE(copyMeasures(full, 0, 1, 0));
    SongTrack half = full;
    REQUIRE(deleteMeasure(half, 128, 128));
    REQUIRE(copyMeasures(half, 0, 128, 128));
    CHECK(half == full);
    // An out-of-range stored length is treated as the maximum, never overrun.
    SongTrack bad;
    bad.length = 60000;
    CHECK_FALSE(insertMeasure(bad, 0, 1));
    REQUIRE(deleteMeasure(bad, 0));
    CHECK(bad.length == kMaxSongLength - 1);
}

TEST_CASE("applyHardwareRules makes a pattern the original machine could hold", "[seq][ops]") {
    std::mt19937 rng(21);
    for (int c = 0; c < 500; ++c) {
        Pattern p = randomPattern(rng, 0.5f);
        p.shuffle = static_cast<std::uint8_t>(rng() % 256);
        p.flamMs = static_cast<std::uint8_t>(rng() % 256);
        if (c % 7 == 0) p.scale = static_cast<Scale>(rng() % 256);
        if (c % 5 == 0) p.length = static_cast<std::uint8_t>(rng() % 256);
        const Pattern before = p;
        applyHardwareRules(p);
        INFO("case " << c);
        REQUIRE(followsHardwareRules(p));
        Pattern again = p;
        applyHardwareRules(again);
        REQUIRE(again == p);  // idempotent
        CHECK(static_cast<int>(p.scale) < kNumHardwareScales);
        CHECK(p.length >= 1);
        CHECK(p.length <= 16);
        CHECK(p.trackLength == decltype(p.trackLength){});
        CHECK(p.trackShuffle == decltype(p.trackShuffle){});
        bool keyShuffle = false, keyFlam = false;
        for (int k = 1; k <= 7; ++k) keyShuffle = keyShuffle || p.shuffle == shuffleTicksForKey(k);
        for (int k = 1; k <= 8; ++k) keyFlam = keyFlam || p.flamMs == flamMsForKey(k);
        CHECK(keyShuffle);
        CHECK(keyFlam);
        if (before.shuffle <= 12) CHECK(std::abs(static_cast<int>(p.shuffle) - before.shuffle) <= 1);
        for (int var = 0; var < kNumVariations; ++var)
            for (int s = 0; s < kMaxSteps; ++s) {
                for (int t = 0; t < kNumTracks; ++t) {
                    const Step x = p.at(var, t, s);
                    const Step o = before.at(var, t, s);
                    REQUIRE(x.subSteps() == 1);
                    REQUIRE(x.probabilityCode() == 0);
                    REQUIRE(x.level() != StepLevel::Weak);
                    if (t == kAccentTrack) {
                        REQUIRE(x.on() == o.on());
                        continue;
                    }
                    const auto& info = kVoiceInfo[static_cast<std::size_t>(t)];
                    if (x.level() == StepLevel::Accent) REQUIRE(info.hardwareAccent);
                    if (x.flam()) {
                        REQUIRE(info.hardwareFlam);
                        REQUIRE(x.level() != StepLevel::Accent);
                    }
                    if (t != CH) REQUIRE(x.on() == o.on());  // nothing appears or disappears
                    // An accent is kept where the voice has one.
                    if (o.level() == StepLevel::Accent && info.hardwareAccent && !(t == CH && x == Step{}))
                        REQUIRE(x.level() == StepLevel::Accent);
                }
                REQUIRE_FALSE((p.at(var, CH, s).on() && p.at(var, OH, s).on()));
                if (before.at(var, OH, s).on()) REQUIRE(p.at(var, OH, s).on());  // the step plays open
            }
    }
    // Specific values.
    Pattern p;
    p.scale = Scale::Eighth;
    p.length = 40;
    p.shuffle = 5;     // -> 6 (key 4)
    p.flamMs = 31;     // -> 32 (key 4)
    p.at(0, RS, 0).setLevel(StepLevel::Accent);
    p.at(0, BD, 0).setLevel(StepLevel::Accent);
    p.at(0, BD, 0).setFlam(true);
    p.at(0, SD, 1).setLevel(StepLevel::Weak);
    p.at(0, SD, 1).setFlam(true);
    p.at(0, CP, 2).setLevel(StepLevel::Normal);
    p.at(0, CP, 2).setFlam(true);
    p.at(0, kAccentTrack, 3).setLevel(StepLevel::Accent);
    CHECK_FALSE(followsHardwareRules(p));
    applyHardwareRules(p);
    CHECK(p.scale == Scale::Sixteenth);
    CHECK(p.length == 16);
    CHECK(p.shuffle == 6);
    CHECK(p.flamMs == 32);
    CHECK(p.at(0, RS, 0).level() == StepLevel::Normal);
    CHECK(p.at(0, BD, 0).level() == StepLevel::Accent);
    CHECK_FALSE(p.at(0, BD, 0).flam());
    CHECK(p.at(0, SD, 1).level() == StepLevel::Normal);
    CHECK(p.at(0, SD, 1).flam());
    CHECK_FALSE(p.at(0, CP, 2).flam());
    CHECK(p.at(0, kAccentTrack, 3).level() == StepLevel::Normal);
    CHECK(followsHardwareRules(Pattern{}));
}

TEST_CASE("randomizePattern keeps the hats apart in playback whatever their track lengths", "[seq][ops]") {
    // Playback reads hat cell (i mod L) at pass-wide step i (polymeter), so
    // check every step over lcm(L_OH, L_CH) passes-worth of steps.
    std::mt19937 rng(77);
    int bothSurvive = 0, cases = 0;
    for (std::uint32_t seed = 0; seed < 600; ++seed) {
        Pattern p;
        p.length = static_cast<std::uint8_t>(1 + rng() % 64);
        p.trackLength[OH] = static_cast<std::uint8_t>(rng() % 65);
        p.trackLength[CH] = static_cast<std::uint8_t>(rng() % 65);
        std::array<float, kNumTracks> dens{};
        dens.fill(0.3f);
        dens[OH] = 0.2f + 0.8f * static_cast<float>(rng() % 100) / 100.0f;
        dens[CH] = 0.2f + 0.8f * static_cast<float>(rng() % 100) / 100.0f;
        const int var = static_cast<int>(seed % 2);
        randomizePattern(p, var, dens, seed);
        const int lo = p.lengthOf(OH), lc = p.lengthOf(CH);
        INFO("seed " << seed << " L_OH " << lo << " L_CH " << lc);
        for (int i = 0; i < lo * lc; ++i)
            REQUIRE_FALSE((p.at(var, OH, i % lo).on() && p.at(var, CH, i % lc).on()));
        // Same seed, same result.
        Pattern q;
        q.length = p.length;
        q.trackLength = p.trackLength;
        randomizePattern(q, var, dens, seed);
        REQUIRE(q == p);
        ++cases;
        bool oh = false, ch = false;
        for (int s = 0; s < lo; ++s) oh = oh || p.at(var, OH, s).on();
        for (int s = 0; s < lc; ++s) ch = ch || p.at(var, CH, s).on();
        if (oh && ch) ++bothSurvive;
    }
    CHECK(bothSurvive > cases / 4);  // lengths with a common factor still keep both hats
    // The reviewer's case: OH length 3 against CH length 16 (coprime): one hat only.
    for (std::uint32_t seed = 0; seed < 200; ++seed) {
        Pattern p;
        p.trackLength[OH] = 3;
        std::array<float, kNumTracks> dens{};
        dens[OH] = 0.6f;
        dens[CH] = 0.6f;
        randomizePattern(p, 0, dens, seed);
        for (int i = 0; i < 48; ++i) REQUIRE_FALSE((p.at(0, OH, i % 3).on() && p.at(0, CH, i % 16).on()));
    }
}

TEST_CASE("applyHardwareRules sets the hardware flam: grace and ramp at 100 %", "[seq][ops]") {
    Pattern p;
    p.flamGrace = 30;
    p.subStart = 40;
    p.subEnd = 90;
    p.flamMs = 24;
    p.at(0, BD, 0).setLevel(StepLevel::Normal);
    p.at(0, BD, 0).setFlam(true);
    CHECK_FALSE(followsHardwareRules(p));
    applyHardwareRules(p);
    CHECK(followsHardwareRules(p));
    CHECK(p.flamGrace == 100);
    CHECK(p.subStart == 100);
    CHECK(p.subEnd == 100);
    CHECK(p.at(0, BD, 0).flam());
    // Both flam triggers now carry the same accent line, the grace at Normal.
    auto s = makeSeq(48000.0);
    s->setPattern(0, p);
    Settings st = internalSettings(120.0);
    Driver d(*s);
    d.runUntil(st, 4800, 480);
    const auto bd = onlyVoice(d.events, BD);
    REQUIRE(bd.size() == 2);
    CHECK(bd[0].sample == 0);
    CHECK(bd[1].sample == 1152);
    CHECK(bd[0].volts == bd[1].volts);
    CHECK(bd[0].level == StepLevel::Normal);
}
