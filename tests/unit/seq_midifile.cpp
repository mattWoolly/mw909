// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Pattern <-> Standard MIDI File (architecture.md §5.7): byte structure,
// round trips, agreement with the sequencer's timing, the original machine's
// note map and accent-velocity rule on import, hostile input.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <vector>

#include "SeqTestUtil.h"
#include "mw909/MidiMap.h"
#include "mw909/seq/MidiFile.h"

using namespace seqtest;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::array<std::uint8_t, kNumVoices> testMap() { return kVoiceNote; }

struct ParsedNote {
    std::int64_t tick;
    int note;
    int velocity;
};

struct Parsed {
    int format = -1, tracks = -1, division = -1;
    std::int64_t endTick = -1;
    std::vector<ParsedNote> ons;
    int offs = 0;
    int tempoUs = -1;
    bool hasEmbedded = false;
    char metaKind = 0;  // '0' = embedded pattern, 'C' = class marker
};

// Independent strict parser for the files we write: no running status, every
// length checked, end-of-track last.
Parsed parseStrict(const Bytes& b) {
    Parsed p;
    std::size_t i = 0;
    auto need = [&](std::size_t n) { REQUIRE(i + n <= b.size()); };
    auto u8 = [&]() {
        need(1);
        return static_cast<int>(b[i++]);
    };
    auto u16 = [&]() {
        const int hi = u8();
        return (hi << 8) | u8();
    };
    auto u32 = [&]() {
        const std::int64_t hi = u16();
        return (hi << 16) | u16();
    };
    auto varLen = [&]() {
        std::int64_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const int c = u8();
            v = (v << 7) | (c & 0x7F);
            if ((c & 0x80) == 0) return v;
        }
        FAIL("variable-length quantity longer than 4 bytes");
        return v;
    };
    REQUIRE(u8() == 'M');
    REQUIRE(u8() == 'T');
    REQUIRE(u8() == 'h');
    REQUIRE(u8() == 'd');
    REQUIRE(u32() == 6);
    p.format = u16();
    p.tracks = u16();
    p.division = u16();
    REQUIRE(u8() == 'M');
    REQUIRE(u8() == 'T');
    REQUIRE(u8() == 'r');
    REQUIRE(u8() == 'k');
    const std::int64_t len = u32();
    REQUIRE(static_cast<std::size_t>(len) == b.size() - i);  // the chunk length is exact
    std::int64_t tick = 0;
    std::vector<int> held(128, 0);
    for (;;) {
        tick += varLen();
        const int status = u8();
        REQUIRE((status & 0x80) != 0);  // we never write running status
        if (status == 0xFF) {
            const int type = u8();
            const std::int64_t l = varLen();
            need(static_cast<std::size_t>(l));
            if (type == 0x51) {
                REQUIRE(l == 3);
                p.tempoUs = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
            }
            if (type == 0x7F) {
                p.hasEmbedded = true;
                REQUIRE(l >= 6);
                CHECK(b[i] == 0x7D);
                p.metaKind = static_cast<char>(b[i + 4]);
            }
            i += static_cast<std::size_t>(l);
            if (type == 0x2F) {
                REQUIRE(l == 0);
                p.endTick = tick;
                break;
            }
            continue;
        }
        REQUIRE((status == 0x99 || status == 0x89));  // channel 10 only
        const int note = u8(), vel = u8();
        REQUIRE(note < 128);
        REQUIRE(vel < 128);
        if (status == 0x99) {
            REQUIRE(vel > 0);
            REQUIRE(held[static_cast<std::size_t>(note)] == 0);  // no overlapping notes on one key
            held[static_cast<std::size_t>(note)] = 1;
            p.ons.push_back({tick, note, vel});
        } else {
            REQUIRE(held[static_cast<std::size_t>(note)] == 1);
            held[static_cast<std::size_t>(note)] = 0;
            ++p.offs;
        }
    }
    REQUIRE(i == b.size());  // end-of-track is the last event
    for (int h : held) REQUIRE(h == 0);
    return p;
}

Pattern noteFriendlyPattern(std::mt19937& rng) {
    // What notes alone can carry exactly: no probability, no accent row,
    // no shuffle, no per-track lengths, a flam of at least a few ticks.
    Pattern p = randomPattern(rng, 0.25f, false);
    for (int s = 0; s < kMaxSteps; ++s) p.at(0, kAccentTrack, s) = Step{};
    p.shuffle = 0;
    p.trackShuffle.fill(0);
    p.trackLength.fill(0);
    p.flamMs = 10;
    // Cells beyond the length do not reach the file.
    for (int t = 0; t < kNumTracks; ++t)
        for (int s = p.length; s < kMaxSteps; ++s) p.at(0, t, s) = Step{};
    // Off cells carry no modifier bits after a read.
    for (int t = 0; t < kNumTracks; ++t)
        for (int s = 0; s < kMaxSteps; ++s)
            if (!p.at(0, t, s).on()) p.at(0, t, s) = Step{};
    return p;
}

}  // namespace

TEST_CASE("written files are structurally valid format-0 SMF", "[seq][midi]") {
    std::mt19937 rng(42);
    for (int c = 0; c < 200; ++c) {
        Pattern p = randomPattern(rng, 0.3f);
        p.flamMs = static_cast<std::uint8_t>(c % 50);
        MidiFileOptions o;
        o.bpm = 60.0 + c;
        o.oneStepNotes = (c & 1) != 0;
        o.embedPattern = (c % 3) != 0;
        o.includeTempo = (c % 5) != 0;
        o.accentNote = (c % 7) == 0 ? 35 : -1;
        const Bytes b = writeMidiFile(p, c & 1, testMap(), o);
        INFO("case " << c);
        const Parsed f = parseStrict(b);
        CHECK(f.format == 0);
        CHECK(f.tracks == 1);
        CHECK(f.division == 480);
        CHECK(f.endTick == p.length * std::lround(stepLengthQn(p.scale) * 480));
        CHECK(static_cast<int>(f.ons.size()) == f.offs);
        CHECK(f.hasEmbedded);  // the pattern, or the velocity-class marker
        CHECK(f.metaKind == (o.embedPattern ? '0' : 'C'));
        if (o.includeTempo) CHECK(f.tempoUs == static_cast<int>(std::lround(60.0e6 / o.bpm)));
        for (const ParsedNote& n : f.ons) {
            CHECK(n.tick >= 0);
            CHECK(n.tick < f.endTick);
        }
    }
}

TEST_CASE("note positions, lengths and velocities", "[seq][midi]") {
    Pattern p;
    p.length = 16;
    p.shuffle = 8;                                        // 8/96 beat = 40 ticks
    p.flamMs = 24;
    p.flamGrace = 30;                                     // not expressible: the grace is 0.6 x main
    p.subStart = 20;                                      // nor the sub-step ramp
    p.at(0, BD, 0).setLevel(StepLevel::Normal);
    p.at(0, SD, 1).setLevel(StepLevel::Weak);             // odd step: 40 ticks late
    p.at(0, LT, 3).setLevel(StepLevel::Normal);           // odd step, but straight: own shuffle 0
    p.trackShuffle[LT] = 1;
    p.at(0, CH, 2).setLevel(StepLevel::Normal);
    p.at(0, CH, 2).setSubSteps(3);                        // 0, 40, 80 ticks into the step
    p.at(0, OH, 4).setLevel(StepLevel::Accent);
    p.at(0, OH, 4).setFlam(true);                         // grace, then main 24 ms later
    p.at(0, kAccentTrack, 0).setLevel(StepLevel::Normal); // BD becomes an accent
    MidiFileOptions o;
    o.bpm = 125.0;  // 24 ms = 0.05 qn = 24 ticks
    o.accentNote = 20;
    const auto m = testMap();
    const Parsed f = parseStrict(writeMidiFile(p, 0, m, o));
    std::vector<ParsedNote> want = {{0, 20, 127},     {0, m[BD], 127},  {160, m[SD], 48},  {240, m[CH], 96},
                                    {280, m[CH], 96}, {320, m[CH], 96}, {360, m[LT], 96},  {480, m[OH], 76},
                                    {504, m[OH], 127}};
    REQUIRE(f.ons.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        INFO("note " << i);
        CHECK(f.ons[i].tick == want[i].tick);
        CHECK(f.ons[i].note == want[i].note);
        CHECK(f.ons[i].velocity == want[i].velocity);
    }
    CHECK(f.tempoUs == 480000);
}

TEST_CASE("round trip through the embedded pattern is exact, probability included", "[seq][midi]") {
    std::mt19937 rng(7);
    for (int c = 0; c < 300; ++c) {
        const Pattern p = randomPattern(rng, 0.3f, true);
        const int var = c & 1;
        const Bytes b = writeMidiFile(p, var, testMap(), MidiFileOptions{});
        Pattern q;
        REQUIRE(readMidiFile(b, testMap(), q, var));
        CHECK(q.length == p.length);
        CHECK(q.scale == p.scale);
        CHECK(q.shuffle == p.shuffle);
        CHECK(q.flamMs == p.flamMs);
        CHECK(q.flamGrace == p.flamGrace);
        CHECK(q.subStart == p.subStart);
        CHECK(q.subEnd == p.subEnd);
        CHECK(q.trackLength == p.trackLength);
        CHECK(q.trackShuffle == p.trackShuffle);
        CHECK(q.steps[static_cast<std::size_t>(var)] == p.steps[static_cast<std::size_t>(var)]);
        CHECK(q.steps[static_cast<std::size_t>(1 - var)] == Pattern{}.steps[0]);  // other variation untouched
    }
}

TEST_CASE("round trip through the notes alone (no probability, accent row or shuffle)", "[seq][midi]") {
    std::mt19937 rng(11);
    for (int c = 0; c < 300; ++c) {
        const Pattern p = noteFriendlyPattern(rng);
        MidiFileOptions o;
        o.embedPattern = false;
        o.oneStepNotes = (c & 1) != 0;
        const Bytes b = writeMidiFile(p, 0, testMap(), o);
        Pattern q;
        q.scale = p.scale;  // the import grid is the target pattern's scale
        REQUIRE(readMidiFile(b, testMap(), q, 0));
        INFO("case " << c << " length " << int(p.length) << " scale " << int(p.scale));
        CHECK(q.length == p.length);
        for (int t = 0; t < kNumVoices; ++t)
            for (int s = 0; s < kMaxSteps; ++s) {
                INFO("track " << t << " step " << s);
                REQUIRE(q.at(0, t, s) == p.at(0, t, s));
            }
    }
}

TEST_CASE("file note times agree with the sequencer's hit times", "[seq][midi]") {
    // 125 BPM at 48 kHz: 23040 samples per quarter = 48 samples per tick.
    std::mt19937 rng(5);
    for (int c = 0; c < 60; ++c) {
        Pattern p = randomPattern(rng, 0.2f, false);
        p.trackLength.fill(0);  // every measure identical
        p.flamMs = static_cast<std::uint8_t>(c % 3 == 0 ? 0 : 5 + c % 30);
        MidiFileOptions o;
        o.embedPattern = false;
        o.bpm = 125.0;
        o.swingScale = (c % 3 == 0) ? 0.5f : 1.0f;
        const auto m = testMap();
        const Parsed f = parseStrict(writeMidiFile(p, 0, m, o));

        auto s = makeSeq(48000.0);
        s->setPattern(0, p);
        Settings st = internalSettings(125.0);
        st.swingScale = o.swingScale;
        const std::int64_t measureSamples = f.endTick * 48;
        const auto ev = runInternal(*s, st, fixedBlocks(measureSamples * 2, 1024));
        // Measure 1 (whose window also holds measure 0's spill) equals the
        // file, which wraps late hits to the top.
        std::vector<std::pair<int, std::int64_t>> fromSeq, fromFile;
        for (const Ev& e : ev)
            if (e.sample >= measureSamples) fromSeq.push_back({m[static_cast<std::size_t>(e.voice)], e.sample - measureSamples});
        // A file cannot hold two notes of one key on one tick (e.g. a flam's
        // main hit landing on the next sub-hit): the writer keeps one.
        std::sort(fromSeq.begin(), fromSeq.end());
        fromSeq.erase(std::unique(fromSeq.begin(), fromSeq.end()), fromSeq.end());
        for (auto& x : fromSeq) x.second = std::llround(static_cast<double>(x.second) / 48.0);
        for (const ParsedNote& n : f.ons) fromFile.push_back({n.note, n.tick});
        std::sort(fromSeq.begin(), fromSeq.end());
        std::sort(fromFile.begin(), fromFile.end());
        INFO("case " << c);
        REQUIRE(fromSeq.size() == fromFile.size());
        for (std::size_t i = 0; i < fromSeq.size(); ++i) {
            CHECK(fromSeq[i].first == fromFile[i].first);
            CHECK(std::llabs(fromSeq[i].second - fromFile[i].second) <= 1);
        }
    }
}

TEST_CASE("reading foreign files: format 1, other resolutions, running status", "[seq][midi]") {
    const auto m = testMap();
    // Format 1, 2 tracks, 96 PPQ. Track 1: tempo + text. Track 2: BD on beats,
    // SD on 2 and 4 with running status and note-on velocity 0 as note-off,
    // a sysex in between, and a weak hi-hat on the 'e' of beat 1.
    Bytes b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96};
    const Bytes t1 = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0xFF, 0x01, 0x03, 'a', 'b', 'c',
                      0x83, 0x00, 0xFF, 0x2F, 0x00};  // ends at 384 ticks = one 4/4 bar
    const Bytes t2 = {0x00, 0x99, m[BD], 100,           // BD at 0
                      0x18, 0x99, m[CH], 40,            // CH at 24 (step 1), weak
                      0x18, 0x99, m[SD], 120,           // SD at 48 (step 2), accent velocity
                      0x00, m[SD], 0,                   // running status: SD off
                      0x30, 0xF0, 0x03, 0x7E, 0x7F, 0xF7,  // sysex at 96
                      0x00, 0x99, m[BD], 90,            // BD at 96 (step 4)
                      0x81, 0x40, 0x89, m[BD], 0,       // +192: off
                      0x00, 0xFF, 0x2F, 0x00};
    auto chunk = [&](const Bytes& t) {
        b.insert(b.end(), {'M', 'T', 'r', 'k'});
        const auto n = static_cast<std::uint32_t>(t.size());
        b.insert(b.end(), {static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16),
                           static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)});
        b.insert(b.end(), t.begin(), t.end());
    };
    chunk(t1);
    chunk(t2);
    Pattern q;
    REQUIRE(readMidiFile(b, m, q, 1));
    CHECK(q.length == 16);
    // A foreign file: the original machine's rule, velocity >= 65 is an accent.
    CHECK(q.at(1, BD, 0).level() == StepLevel::Accent);
    CHECK(q.at(1, CH, 1).level() == StepLevel::Normal);
    CHECK(q.at(1, SD, 2).level() == StepLevel::Accent);
    CHECK(q.at(1, BD, 4).level() == StepLevel::Accent);
    int on = 0;
    for (int t = 0; t < kNumTracks; ++t)
        for (int s = 0; s < kMaxSteps; ++s) on += q.at(1, t, s).on() ? 1 : 0;
    CHECK(on == 4);
}

TEST_CASE("hostile input never crashes and never half-writes the pattern", "[seq][midi]") {
    std::mt19937 rng(3);
    const Pattern p = randomPattern(rng, 0.4f);
    const Bytes good = writeMidiFile(p, 0, testMap(), MidiFileOptions{});
    Pattern sentinel = everyStep(RD);

    SECTION("every truncation") {
        for (std::size_t n = 0; n < good.size(); ++n) {
            Pattern q = sentinel;
            const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
            const bool ok = readMidiFile(cut, testMap(), q);
            if (!ok) REQUIRE(q == sentinel);
        }
    }
    SECTION("random corruption") {
        std::uniform_int_distribution<int> byte(0, 255);
        for (int i = 0; i < 3000; ++i) {
            Bytes bad = good;
            const int flips = 1 + i % 8;
            for (int k = 0; k < flips; ++k) bad[rng() % bad.size()] = static_cast<std::uint8_t>(byte(rng));
            Pattern q = sentinel;
            const bool ok = readMidiFile(bad, testMap(), q);
            if (!ok) REQUIRE(q == sentinel);
            REQUIRE(q.length >= 1);
            REQUIRE(q.length <= kMaxSteps);
        }
        for (int i = 0; i < 500; ++i) {
            Bytes junk(static_cast<std::size_t>(rng() % 300));
            for (auto& x : junk) x = static_cast<std::uint8_t>(byte(rng));
            if (junk.size() >= 4) {
                junk[0] = 'M';
                junk[1] = 'T';
                junk[2] = 'h';
                junk[3] = 'd';
            }
            Pattern q = sentinel;
            (void)readMidiFile(junk, testMap(), q);
        }
    }
    SECTION("specific rejections") {
        Pattern q = sentinel;
        Bytes smpte = good;
        smpte[12] = 0xE7;  // negative SMPTE format in the division word
        CHECK_FALSE(readMidiFile(smpte, testMap(), q));
        Bytes magic = good;
        magic[0] = 'X';
        CHECK_FALSE(readMidiFile(magic, testMap(), q));
        CHECK_FALSE(readMidiFile(Bytes{}, testMap(), q));
        Bytes noTracks(good.begin(), good.begin() + 14);
        CHECK_FALSE(readMidiFile(noTracks, testMap(), q));
        CHECK(q == sentinel);
    }
}

TEST_CASE("import: the original machine's note map and the accent threshold", "[seq][midi]") {
    // A one-bar format-0 file at 96 PPQ, 16ths, with the alias notes of the
    // receive map (35 BD, 40 SD, 44 CH, 41 LT, 45 MT, 48 HT, 52/55/57 CR,
    // 53/59 RD) and velocities around the threshold.
    struct N {
        int step, note, vel;
        int voice;
        StepLevel level;
    };
    const N notes[] = {{0, 35, 64, BD, StepLevel::Normal},  {1, 40, 65, SD, StepLevel::Accent},
                       {2, 44, 1, CH, StepLevel::Normal},   {3, 41, 127, LT, StepLevel::Accent},
                       {4, 45, 30, MT, StepLevel::Normal},  {5, 48, 66, HT, StepLevel::Accent},
                       {6, 52, 64, CR, StepLevel::Normal},  {7, 55, 64, CR, StepLevel::Normal},
                       {8, 57, 100, CR, StepLevel::Accent}, {9, 53, 64, RD, StepLevel::Normal},
                       {10, 59, 64, RD, StepLevel::Normal}, {11, 37, 64, RS, StepLevel::Normal},
                       {12, 39, 96, CP, StepLevel::Accent}, {13, 42, 64, CH, StepLevel::Normal},
                       {14, 46, 64, OH, StepLevel::Normal}, {15, 60, 127, -1, StepLevel::Off}};
    Bytes b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 0};
    for (const N& n : notes) {
        b.insert(b.end(), {static_cast<std::uint8_t>(n.step == 0 ? 0 : 24), 0x99, static_cast<std::uint8_t>(n.note),
                           static_cast<std::uint8_t>(n.vel)});
    }
    b.insert(b.end(), {24, 0xFF, 0x2F, 0x00});
    const auto len = static_cast<std::uint32_t>(b.size() - 22);
    for (int k = 0; k < 4; ++k) b[18 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(len >> (24 - 8 * k));
    CHECK(kAccentVelocityThreshold == 65);
    Pattern q;
    REQUIRE(readMidiFile(b, kVoiceNote, q, 0));
    CHECK(q.length == 16);
    for (const N& n : notes) {
        INFO("step " << n.step << " note " << n.note);
        for (int v = 0; v < kNumVoices; ++v)
            CHECK(q.at(0, v, n.step).level() == (v == n.voice ? n.level : StepLevel::Off));
    }
    // A custom map wins over the alias of the same note.
    auto map = kVoiceNote;
    map[RD] = 35;  // 35 is BD's alias in the receive map
    Pattern r;
    REQUIRE(readMidiFile(b, map, r, 0));
    CHECK(r.at(0, RD, 0).on());
    CHECK_FALSE(r.at(0, BD, 0).on());
}

TEST_CASE("notes-only files of the writer keep weak/normal/accent through the class marker", "[seq][midi]") {
    Pattern p;
    p.at(0, BD, 0).setLevel(StepLevel::Weak);
    p.at(0, SD, 4).setLevel(StepLevel::Normal);
    p.at(0, CH, 8).setLevel(StepLevel::Accent);
    MidiFileOptions o;
    o.embedPattern = false;
    Bytes b = writeMidiFile(p, 0, testMap(), o);
    Pattern q;
    REQUIRE(readMidiFile(b, testMap(), q, 0));
    CHECK(q.at(0, BD, 0).level() == StepLevel::Weak);
    CHECK(q.at(0, SD, 4).level() == StepLevel::Normal);
    CHECK(q.at(0, CH, 8).level() == StepLevel::Accent);
    // Without the marker the same notes are a foreign file: 48 -> Normal, 96 -> Accent.
    const Bytes tag = {0x7D, 'M', 'W', '9', 'C'};
    auto it = std::search(b.begin(), b.end(), tag.begin(), tag.end());
    REQUIRE(it != b.end());
    *(it + 4) = 'X';
    Pattern f;
    REQUIRE(readMidiFile(b, testMap(), f, 0));
    CHECK(f.at(0, BD, 0).level() == StepLevel::Normal);
    CHECK(f.at(0, SD, 4).level() == StepLevel::Accent);
    CHECK(f.at(0, CH, 8).level() == StepLevel::Accent);
}

TEST_CASE("a dense notes-only file imports quickly", "[seq][midi]") {
    // 11 voices x 1024 random notes in a 4-bar measure, velocities alternating
    // 60/100 (so the grace/main logic is busy): the grouping is O(n log n) per
    // voice, not O(n^2). The bound is loose (sanitizer builds are slow); the
    // quadratic version took about 3 s in Release.
    std::mt19937 rng(1);
    struct N {
        int tick, note, vel;
    };
    std::vector<N> ns;
    for (int v = 0; v < kNumVoices; ++v)
        for (int k = 0; k < 1024; ++k)
            ns.push_back({static_cast<int>(rng() % 7680), kVoiceNote[static_cast<std::size_t>(v)], (k % 2) != 0 ? 100 : 60});
    std::stable_sort(ns.begin(), ns.end(), [](const N& a, const N& b) { return a.tick < b.tick; });
    Bytes trk;
    auto varLen = [&trk](std::uint32_t v) {
        std::array<std::uint8_t, 5> t{};
        int n = 0;
        t[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(v & 0x7F);
        while ((v >>= 7) != 0) t[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(0x80 | (v & 0x7F));
        while (n > 0) trk.push_back(t[static_cast<std::size_t>(--n)]);
    };
    int last = 0;
    for (const N& n : ns) {
        varLen(static_cast<std::uint32_t>(n.tick - last));
        last = n.tick;
        trk.push_back(0x99);
        trk.push_back(static_cast<std::uint8_t>(n.note));
        trk.push_back(static_cast<std::uint8_t>(n.vel));
    }
    varLen(static_cast<std::uint32_t>(7680 - last));
    for (std::uint8_t b : {0xFF, 0x2F, 0x00}) trk.push_back(b);
    Bytes f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
    for (int sh = 24; sh >= 0; sh -= 8) f.push_back(static_cast<std::uint8_t>((trk.size() >> sh) & 0xFF));
    f.insert(f.end(), trk.begin(), trk.end());

    Pattern p;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(readMidiFile(f, kVoiceNote, p, 0));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 750.0);
    CHECK(p.length == 64);
    for (int v = 0; v < kNumVoices; ++v) {
        int on = 0;
        for (int s = 0; s < 64; ++s) on += p.at(0, v, s).on() ? 1 : 0;
        CHECK(on == 64);  // 16 random notes per step: every step gets an onset
    }
}
