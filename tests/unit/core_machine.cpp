// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Machine: sample-accurate MIDI and sequencer hits, the receive map,
// velocities, the bass channel, Host Note, MIDI out, tap write, mute/solo,
// the hat rule, PlayState publishing, commands, bank and user-ROM handoffs.

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>

#include "CoreTestUtil.h"

using namespace mw909;
using namespace coretest;

namespace {

constexpr double kFs = 48000.0;
constexpr int kMaxBlock = 512;
constexpr int kSixteenth = 6000;  // samples per 16th at 120 BPM, 48 kHz

std::unique_ptr<Machine> makeMachine(int maxBlock = kMaxBlock, double fs = kFs) {
    auto m = std::make_unique<Machine>();
    m->prepare(fs, maxBlock);
    return m;
}

std::uint32_t hits(const Machine& m, int v) { return m.playState().hits[static_cast<std::size_t>(v)].load(); }

// Every voice on its own individual out.
MachineParams ownOuts() {
    MachineParams p = defaultMachineParams();
    for (int v = 0; v < kNumVoices; ++v) p.engine.channel[static_cast<std::size_t>(v)].output = static_cast<std::uint8_t>(v + 1);
    return p;
}

std::vector<int> onsets(const std::vector<float>& x, float threshold, int gap) {
    std::vector<int> out;
    int last = -gap;
    for (int i = 0; i < static_cast<int>(x.size()); ++i)
        if (std::abs(x[static_cast<std::size_t>(i)]) > threshold && i - last >= gap) {
            out.push_back(i);
            last = i;
        } else if (std::abs(x[static_cast<std::size_t>(i)]) > threshold) {
            last = i;
        }
    return out;
}

}  // namespace

TEST_CASE("A MIDI note sounds at its exact sample offset plus the latency", "[core][machine]") {
    for (int block : {512, 64, 7}) {
        auto m = makeMachine();
        Runner r{*m};
        r.playing = false;
        r.midi = std::vector<MidiMessage>{noteOn(300, kVoiceNote[RS])};
        Buses buses(4096, 1);
        r.run(buses, 4096, block);
        const int l = m->latencySamples();
        REQUIRE(peak(buses.left(), 0, 300 + l) == 0.0f);
        const int onset = firstAbove(buses.left(), 1.0e-4f);
        REQUIRE(onset >= 300 + l);
        REQUIRE(onset < 300 + l + 48);
        REQUIRE(hits(*m, RS) == 1U);
    }
}

TEST_CASE("Host-synced pattern steps land sample-accurately", "[core][machine]") {
    std::vector<int> ref;
    for (int block : {512, 64, 4096, 333, 1}) {
        INFO("block " << block);
        auto m = makeMachine(4096);
        postPattern(*m, 0, patternWith({RS}, {0, 4, 8, 12}));
        Runner r{*m};
        r.params.seq.mode = seq::Mode::HostSync;
        Buses buses(4 * 4 * kSixteenth, 1);
        r.run(buses, buses.length, block);
        const auto on = onsets(buses.left(), 1.0e-3f, 1000);
        REQUIRE(on.size() == 4U);
        for (std::size_t i = 0; i < on.size(); ++i) {
            REQUIRE(on[i] >= static_cast<int>(i) * 4 * kSixteenth + m->latencySamples());
            REQUIRE(on[i] < static_cast<int>(i) * 4 * kSixteenth + m->latencySamples() + 48);
        }
        if (ref.empty()) ref = on;
        REQUIRE(on == ref);
        REQUIRE(hits(*m, RS) == 4U);
    }
}

TEST_CASE("MIDI in: the receive map, any channel or the receive channel", "[core][machine][midi]") {
    SECTION("every mapped note plays its voice, on any channel when omni") {
        for (int note = 0; note < 128; ++note) {
            const int v = kNoteToVoice[static_cast<std::size_t>(note)];
            auto m = makeMachine();
            Runner r{*m};
            r.playing = false;
            r.midi = std::vector<MidiMessage>{noteOn(10, note, 100, 1 + note % 16)};
            Buses buses(1024, 1);
            r.run(buses, 1024, 256);
            for (int x = 0; x < kNumVoices; ++x) REQUIRE(hits(*m, x) == (x == v ? 1U : 0U));
        }
    }
    SECTION("with a receive channel set, other channels are ignored") {
        auto m = makeMachine();
        Runner r{*m};
        r.playing = false;
        r.params.midi.receiveChannel = 3;
        r.midi = std::vector<MidiMessage>{noteOn(10, 36, 100, 3), noteOn(20, 38, 100, 4), noteOn(30, 42, 100, 3), noteOff(40, 36, 3)};
        Buses buses(1024, 1);
        r.run(buses, 1024, 256);
        REQUIRE(hits(*m, BD) == 1U);
        REQUIRE(hits(*m, SD) == 0U);
        REQUIRE(hits(*m, CH) == 1U);
    }
    SECTION("controllers, pitch bend and note-offs play nothing") {
        auto m = makeMachine();
        Runner r{*m};
        r.playing = false;
        r.midi = std::vector<MidiMessage>{{10, 0xB9, 36, 100}, {20, 0xE9, 0, 64}, noteOff(30, 36), {40, 0x99, 36, 0}};
        Buses buses(1024, 1);
        r.run(buses, 1024, 256);
        for (int x = 0; x < kNumVoices; ++x) REQUIRE(hits(*m, x) == 0U);
        REQUIRE(peak(buses.left()) == 0.0f);
    }
}

TEST_CASE("Velocity sets the dynamics; the bass channel plays the bass drum chromatically", "[core][machine][midi]") {
    auto playOne = [](int note, int velocity, int channel, MachineParams p, VoiceId out = SD) {
        auto m = makeMachine();
        Runner r{*m};
        r.params = p;
        r.playing = false;
        r.midi = std::vector<MidiMessage>{noteOn(0, note, velocity, channel)};
        auto buses = std::make_unique<Buses>(24000);
        r.run(*buses, 24000, 512);
        (void)out;
        return buses;
    };
    const MachineParams p = ownOuts();
    SECTION("louder with velocity, sensitive or in three classes") {
        for (bool sensitive : {true, false}) {
            MachineParams q = p;
            q.midi.velocitySensitive = sensitive;
            const float soft = peak(playOne(38, 40, 10, q)->left(SD + 1));
            const float mid = peak(playOne(38, 96, 10, q)->left(SD + 1));
            const float loud = peak(playOne(38, 127, 10, q)->left(SD + 1));
            REQUIRE(soft > 0.0f);
            REQUIRE(soft < mid);
            REQUIRE(mid < loud);
        }
    }
    SECTION("the total accent knob scales the top velocities") {
        MachineParams lo = p, hi = p;
        lo.seq.totalAccent = 0.0f;
        hi.seq.totalAccent = 1.0f;
        REQUIRE(peak(playOne(38, 127, 10, lo)->left(SD + 1)) < peak(playOne(38, 127, 10, hi)->left(SD + 1)));
        // At or below velocity 96 the knob does nothing.
        REQUIRE(same(playOne(38, 90, 10, lo)->left(SD + 1), playOne(38, 90, 10, hi)->left(SD + 1)));
    }
    SECTION("the bass channel plays BD at the note's pitch") {
        MachineParams q = p;
        q.midi.bassChannel = 2;
        auto root = playOne(kBassRootNote, 100, 2, q);
        auto up = playOne(kBassRootNote + 12, 100, 2, q);
        auto mapped = playOne(38, 100, 2, q);   // the snare's note on the bass channel: a bass drum
        REQUIRE(peak(mapped->left(SD + 1)) == 0.0f);
        REQUIRE(peak(mapped->left(BD + 1)) > 0.0f);
        // An octave up: about twice the zero crossings in the body.
        auto crossings = [](const std::vector<float>& x) {
            int c = 0;
            for (std::size_t i = 4800; i + 1 < 14400; ++i) c += (x[i] < 0.0f) != (x[i + 1] < 0.0f);
            return c;
        };
        const int c0 = crossings(root->left(BD + 1));
        const int c12 = crossings(up->left(BD + 1));
        REQUIRE(c0 > 0);
        REQUIRE(c12 > 3 * c0 / 2);
        // The root note is the stock pitch: the same as the bass drum's own note.
        REQUIRE(same(root->left(BD + 1), playOne(36, 100, 10, q)->left(BD + 1)));
    }
}

TEST_CASE("Host Note mode: notes 24..119 select and hold patterns", "[core][machine][midi]") {
    auto m = makeMachine();
    for (int i : {0, 5, 95}) postPattern(*m, i, patternWith({RS}, {0, 4, 8, 12}));
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostNote;
    r.playing = false;
    r.midi = std::vector<MidiMessage>{noteOn(1000, kPatternNoteBase + 5, 100, 1), noteOff(30000, kPatternNoteBase + 5, 1),
              noteOn(40000, 120, 100, 1), noteOn(50000, kPatternNoteBase + 95, 100, 1)};
    Buses buses(60000, 1);
    r.run(buses, 30000, 256);
    REQUIRE(m->playState().currentPattern.load() == 5);
    REQUIRE(m->playState().playing.load());
    const std::uint32_t during = hits(*m, RS);
    REQUIRE(during >= 1U);
    // A pattern note does not play its voice (note 24 + 5 is no voice anyway; 36 + would be).
    r.run(buses, 20000, 256, 30000);
    REQUIRE_FALSE(m->playState().playing.load());
    REQUIRE(hits(*m, RS) == during);
    r.run(buses, 10000, 256, 50000);
    REQUIRE(m->playState().currentPattern.load() == 95);
}

TEST_CASE("MIDI out: sequencer hits as notes with velocity classes and note-offs", "[core][machine][midi]") {
    seq::Pattern p;
    p.at(0, BD, 0).setLevel(seq::StepLevel::Normal);
    p.at(0, SD, 4).setLevel(seq::StepLevel::Weak);
    p.at(0, BD, 8).setLevel(seq::StepLevel::Accent);
    p.at(0, CH, 12).setLevel(seq::StepLevel::Normal);
    p.at(0, seq::kAccentTrack, 12).setLevel(seq::StepLevel::Normal);   // total accent on step 12
    auto m = makeMachine();
    postPattern(*m, 0, p);
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    r.params.midi.midiOut = true;
    r.params.midi.outChannel = 3;
    r.midi = std::vector<MidiMessage>{noteOn(100, 38, 100, 10)};   // live hits are not echoed
    Buses buses(16 * kSixteenth, 1);
    r.run(buses, buses.length, 333);

    struct Expect { int at; int note; int velocity; };
    const Expect expect[] = {{0, 36, kVelocityNormal}, {4 * kSixteenth, 38, kVelocityWeak},
                             {8 * kSixteenth, 36, kVelocityAccent}, {12 * kSixteenth, 42, kVelocityAccent}};
    std::vector<MidiMessage> ons, offs;
    for (const auto& msg : r.midiOut) {
        REQUIRE((msg.status & 0x0F) == 2);   // channel 3
        if ((msg.status & 0xF0) == 0x90) ons.push_back(msg);
        else if ((msg.status & 0xF0) == 0x80) offs.push_back(msg);
    }
    REQUIRE(ons.size() == 4U);
    REQUIRE(offs.size() == 4U);
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(ons[i].offset == expect[i].at);
        REQUIRE(ons[i].data1 == expect[i].note);
        REQUIRE(ons[i].data2 == expect[i].velocity);
        REQUIRE(offs[i].data1 == expect[i].note);
        REQUIRE(offs[i].offset == expect[i].at + 960);   // 20 ms
    }
    for (std::size_t i = 1; i < r.midiOut.size(); ++i) REQUIRE(r.midiOut[i].offset >= r.midiOut[i - 1].offset);

    SECTION("off when MIDI out is off") {
        auto m2 = makeMachine();
        postPattern(*m2, 0, p);
        Runner r2{*m2};
        r2.params.seq.mode = seq::Mode::HostSync;
        Buses b2(16 * kSixteenth, 1);
        r2.run(b2, b2.length, 512);
        REQUIRE(r2.midiOut.empty());
    }
}

TEST_CASE("MIDI out: a retrigger inside the note length closes the note first", "[core][machine][midi]") {
    seq::Pattern p;
    p.scale = seq::Scale::ThirtySecond;
    for (int s = 0; s < 16; ++s) p.at(0, RS, s).setLevel(seq::StepLevel::Normal);
    auto m = makeMachine();
    postPattern(*m, 0, p);
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    r.params.midi.midiOut = true;
    r.bpm = 400.0;   // 32nds 225 samples apart: shorter than the 960-sample notes
    Buses buses(48000, 1);
    r.run(buses, buses.length, 128);
    int sounding = 0;
    for (std::size_t i = 0; i < r.midiOut.size(); ++i) {
        const auto& msg = r.midiOut[i];
        if (i > 0) REQUIRE(msg.offset >= r.midiOut[i - 1].offset);
        if ((msg.status & 0xF0) == 0x90) {
            REQUIRE(sounding == 0);
            ++sounding;
        } else {
            REQUIRE(sounding == 1);
            --sounding;
        }
    }
    REQUIRE(r.midiOut.size() > 20U);
}

TEST_CASE("Tap write: a played hit is recorded into the playing pattern", "[core][machine][midi]") {
    auto m = makeMachine();
    postPattern(*m, 0, patternWith({BD}, {0}));
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    r.params.seq.record = true;
    // SD at step 4 (normal), CH just before step 8 (accent: velocity >= threshold)
    r.midi = std::vector<MidiMessage>{noteOn(4 * kSixteenth + 20, 38, kAccentVelocityThreshold - 1),
              noteOn(8 * kSixteenth - 30, 42, kAccentVelocityThreshold)};
    Buses buses(16 * kSixteenth, 1);
    r.run(buses, buses.length, 256);
    std::vector<Notice> got;
    Notice n;
    while (m->poll(n))
        if (n.type == Notice::Type::StepRecorded) got.push_back(n);
    REQUIRE(got.size() == 2U);
    REQUIRE(got[0].track == SD);
    REQUIRE(got[0].step == 4);
    REQUIRE(got[0].cell.level() == seq::StepLevel::Normal);
    REQUIRE(got[1].track == CH);
    REQUIRE(got[1].step == 8);
    REQUIRE(got[1].cell.level() == seq::StepLevel::Accent);
    // The recorded step is in the audio side's pattern and plays on the next pass,
    // and the live hit was not doubled on this pass.
    REQUIRE(m->sequencer().bank().patterns[0].at(0, SD, 4).level() == seq::StepLevel::Normal);
    REQUIRE(hits(*m, CH) == 1U);
    r.midi.clear();
    r.run(buses, 16 * kSixteenth, 256, 0);
    REQUIRE(hits(*m, SD) == 2U);
}

TEST_CASE("Mute and solo gate triggers; tails finish; auditions bypass them", "[core][machine]") {
    auto run = [](MachineParams p, std::vector<MidiMessage> midi, int total, std::function<void(Runner&, int)> mid = nullptr) {
        auto m = makeMachine();
        Runner r{*m};
        r.params = p;
        r.playing = false;
        r.midi = std::move(midi);
        auto b = std::make_unique<Buses>(total);
        if (mid) {
            r.run(*b, total / 2, 256);
            mid(r, total / 2);
            r.run(*b, total - total / 2, 256, total / 2);
        } else {
            r.run(*b, total, 256);
        }
        std::array<std::uint32_t, kNumVoices> h{};
        for (int v = 0; v < kNumVoices; ++v) h[static_cast<std::size_t>(v)] = hits(*m, v);
        return std::pair{std::move(b), h};
    };
    const MachineParams base = ownOuts();
    const std::vector<MidiMessage> notes = {noteOn(0, 36), noteOn(0, 38), noteOn(0, 39), noteOn(12000, 36), noteOn(12000, 38)};

    SECTION("a muted voice does not fire") {
        MachineParams p = base;
        p.mute[SD] = true;
        auto [b, h] = run(p, notes, 24000);
        REQUIRE(h[SD] == 0U);
        REQUIRE(h[BD] == 2U);
        REQUIRE(peak(b->left(SD + 1)) == 0.0f);
    }
    SECTION("muting while a voice rings lets the tail finish") {
        auto [ref, h0] = run(base, {noteOn(0, 39)}, 24000);
        MachineParams p = base;
        auto [b, h] = run(p, {noteOn(0, 39), noteOn(13000, 39)}, 24000, [](Runner& r, int) { r.params.mute[CP] = true; });
        REQUIRE(h[CP] == 1U);
        REQUIRE(same(b->left(CP + 1), ref->left(CP + 1)));
    }
    SECTION("solo: only soloed voices fire; mute wins over solo") {
        MachineParams p = base;
        p.solo[BD] = true;
        p.solo[CP] = true;
        p.mute[CP] = true;
        auto [b, h] = run(p, notes, 24000);
        REQUIRE(h[BD] == 2U);
        REQUIRE(h[SD] == 0U);
        REQUIRE(h[CP] == 0U);
    }
    SECTION("auditions sound even when muted") {
        auto m = makeMachine();
        Runner r{*m};
        r.params = base;
        r.params.mute[RS] = true;
        r.playing = false;
        Command c;
        c.type = Command::Type::Audition;
        c.index = RS;
        c.value = AccentDac::volts(AccentDac::kNormalCode);
        REQUIRE(m->post(c));
        Buses b(4096);
        r.run(b, 4096, 512);
        REQUIRE(hits(*m, RS) == 1U);
        REQUIRE(peak(b.left(RS + 1)) > 0.0f);
    }
}

TEST_CASE("The hat rule: CH and OH on one step play the open hat", "[core][machine]") {
    auto m = makeMachine();
    postPattern(*m, 0, patternWith({CH, OH}, {0}));
    Runner r{*m};
    r.params = ownOuts();
    r.params.seq.mode = seq::Mode::HostSync;
    Buses b(4 * kSixteenth);
    r.run(b, b.length, 512);
    REQUIRE(hits(*m, CH) == 1U);
    REQUIRE(hits(*m, OH) == 1U);
    REQUIRE(peak(b.left(CH + 1)) == 0.0f);
    REQUIRE(peak(b.left(OH + 1)) > 0.01f);
}

TEST_CASE("PlayState: position packs measure and step in one word", "[core][machine]") {
    seq::Pattern p = patternWith({RS}, {0});
    p.length = 12;
    auto m = makeMachine();
    postPattern(*m, 0, p);
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    Buses b(kSixteenth * 40, 1);
    REQUIRE(m->playState().position.load() == 0U);
    for (int pos = 0; pos < b.length; pos += 1500) {
        r.run(b, 1500, 1500, pos);
        const auto& s = m->playState();
        const std::uint64_t word = s.position.load();
        REQUIRE(s.playing.load());
        REQUIRE(static_cast<int>(word & 0xFF) == s.step.load() + 1);
        REQUIRE(static_cast<std::int64_t>(word >> 8) == s.measure.load());
        // The step the transport is in: (pos + 1500 - 1) / 6000, within a 12-step measure.
        const int expectStep = ((pos + 1499) / kSixteenth) % 12;
        REQUIRE(s.step.load() == expectStep);
        REQUIRE(s.measure.load() == (pos + 1499) / kSixteenth / 12);
    }
    r.playing = false;
    r.run(b, 512, 512, 0);
    REQUIRE(m->playState().position.load() == 0U);
    REQUIRE(m->playState().step.load() == -1);
}

TEST_CASE("PlayState: the song position walks the chain", "[core][machine]") {
    auto m = makeMachine();
    postPattern(*m, 0, patternWith({RS}, {0}));
    postPattern(*m, 1, patternWith({SD}, {0}));
    Command c;
    c.type = Command::Type::SetChain;
    c.chain.length = 2;
    c.chain.entries[0] = 0;
    c.chain.entries[1] = 1;
    REQUIRE(m->post(c));
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    r.params.seq.play = seq::PlayMode::Chain;
    Buses b(16 * kSixteenth * 3, 1);
    REQUIRE(m->playState().songPosition.load() == -1);
    r.run(b, 16 * kSixteenth, 512);
    REQUIRE(m->playState().songPosition.load() == 0);
    REQUIRE(m->playState().currentPattern.load() == 0);
    r.run(b, 16 * kSixteenth, 512, 16 * kSixteenth);
    REQUIRE(m->playState().songPosition.load() == 1);
    REQUIRE(m->playState().currentPattern.load() == 1);
    REQUIRE(hits(*m, RS) == 1U);
    REQUIRE(hits(*m, SD) == 1U);
}

TEST_CASE("PlayState: hit counters, channel peaks and the main peak", "[core][machine]") {
    auto m = makeMachine();
    Runner r{*m};
    r.params = ownOuts();
    r.params.engine.channel[BD].output = 0;
    r.playing = false;
    r.midi = std::vector<MidiMessage>{noteOn(0, 36), noteOn(0, 38)};
    Buses b(512);
    r.run(b, 512, 512);
    const auto& s = m->playState();
    REQUIRE(s.hits[BD].load() == 1U);
    REQUIRE(s.hits[SD].load() == 1U);
    REQUIRE(s.channelPeak[BD].load() > 0.0f);
    REQUIRE(s.channelPeak[SD].load() > 0.0f);
    REQUIRE(s.channelPeak[CP].load() == 0.0f);
    REQUIRE(s.peakLeft.load() == peak(b.left()));
    REQUIRE(s.peakLeft.load() > 0.0f);
}

TEST_CASE("Commands: select, fill, audition, panic and song tracks reach the audio side", "[core][machine]") {
    auto m = makeMachine();
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    Buses b(4096, 1);

    Command c;
    c.type = Command::Type::SelectPattern;
    c.index = 7;
    c.flag = true;
    REQUIRE(m->post(c));
    r.run(b, 512, 512);
    REQUIRE(m->playState().selectedPattern.load() == 7);
    Notice n;
    bool echoed = false;
    while (m->poll(n)) echoed = echoed || n.type == Notice::Type::PatternSelected;
    REQUIRE_FALSE(echoed);   // the UI asked for it

    seq::SongTrack t;
    t.length = 2;
    t.entries[0] = 3;
    t.entries[1] = 4;
    c = Command{};
    c.type = Command::Type::SetSongTrack;
    c.index = 5;
    c.songTrack = t;
    REQUIRE(m->post(c));
    seq::Chain chain;
    chain.length = 1;
    chain.entries[0] = 9;
    c = Command{};
    c.type = Command::Type::SetChain;
    c.chain = chain;
    REQUIRE(m->post(c));
    r.run(b, 512, 512, 512);
    REQUIRE(m->sequencer().bank().tracks[5] == t);
    REQUIRE(m->sequencer().bank().chain == chain);

    c = Command{};
    c.type = Command::Type::Audition;
    c.index = CP;
    c.value = 4.0f;
    REQUIRE(m->post(c));
    r.run(b, 512, 512, 1024);
    REQUIRE(hits(*m, CP) == 1U);
    REQUIRE(m->engine().anyVoiceActive());

    c = Command{};
    c.type = Command::Type::Panic;
    REQUIRE(m->post(c));
    r.run(b, 512, 512, 1536);
    REQUIRE_FALSE(m->engine().anyVoiceActive());
}

TEST_CASE("Bank handoff: the next block uses the newest bank; stale edits are dropped", "[core][machine][handoff]") {
    auto m = makeMachine();
    Runner r{*m};
    Buses b(512, 1);
    const seq::Pattern a = patternWith({BD}, {0}), bb = patternWith({SD}, {1}), cc = patternWith({CP}, {2});

    // Two banks before one block: only the second is adopted, the first comes back at once.
    auto* h1 = new BankHandoff;
    h1->bank.patterns[0] = a;
    h1->generation = 1;
    auto* h2 = new BankHandoff;
    h2->bank.patterns[0] = bb;
    h2->generation = 2;
    REQUIRE(m->postBank(h1) == nullptr);
    BankHandoff* superseded = m->postBank(h2);
    REQUIRE(superseded == h1);
    delete superseded;
    // An edit made against generation 1 is stale once generation 2 is in.
    postPattern(*m, 0, cc, 1);
    // An edit against generation 2 applies to pattern 1.
    postPattern(*m, 1, cc, 2);
    r.run(b, 512, 512);
    REQUIRE(m->sequencer().bank().patterns[0] == bb);
    REQUIRE(m->sequencer().bank().patterns[1] == cc);
    REQUIRE(m->takeRetiredBank() == h2);
    REQUIRE(m->takeRetiredBank() == nullptr);
    delete h2;

    // An edit newer than the current bank, behind which a bank was posted
    // first: the bank is taken before the edit, so the edit survives it.
    auto* h3 = new BankHandoff;
    h3->bank.patterns[2] = a;
    h3->generation = 3;
    REQUIRE(m->postBank(h3) == nullptr);
    postPattern(*m, 2, bb, 3);
    r.run(b, 512, 512);
    REQUIRE(m->sequencer().bank().patterns[2] == bb);
    REQUIRE(m->takeRetiredBank() == h3);
    delete h3;

    // A bank posted after an edit stamped with an older generation wins over it.
    postPattern(*m, 3, bb, 3);
    auto* h4 = new BankHandoff;
    h4->bank.patterns[3] = a;
    h4->generation = 4;
    REQUIRE(m->postBank(h4) == nullptr);
    r.run(b, 512, 512);
    REQUIRE(m->sequencer().bank().patterns[3] == a);
    delete m->takeRetiredBank();

    // Generations wrap around.
    auto* h5 = new BankHandoff;
    h5->generation = 0xFFFFFFFFU;
    delete m->postBank(h5);
    r.run(b, 512, 512);
    delete m->takeRetiredBank();
    postPattern(*m, 4, a, 0x00000001U);   // newer than 0xFFFFFFFF after wrap
    postPattern(*m, 5, a, 0xFFFFFFF0U);   // older
    r.run(b, 512, 512);
    REQUIRE(m->sequencer().bank().patterns[4] == a);
    REQUIRE_FALSE(m->sequencer().bank().patterns[5] == a);
}

TEST_CASE("User ROM handoff: adopted per slot, previous image retired after the swap", "[core][machine][handoff]") {
    // A user image that is not the factory one: a square wave.
    auto makeRom = [](RomSlot slot, int period) {
        auto* h = new RomHandoff;
        h->slot = slot;
        for (int i = 0; i < kRomWords; ++i)
            h->image.data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(((i / period) & 1) ? 56 : 8);
        return h;
    };
    auto render = [](Machine& m, MachineParams p) {
        Runner r{m};
        r.params = p;
        r.playing = false;
        Command c;
        c.type = Command::Type::Audition;
        c.index = CR;
        c.value = AccentDac::volts(AccentDac::kNormalCode);
        m.post(c);
        auto b = std::make_unique<Buses>(9600);
        r.run(*b, 9600, 512);
        Command panic;
        panic.type = Command::Type::Panic;
        m.post(panic);
        r.run(*b, 512, 512, 0);   // silence again (overwrites the head; the body is kept)
        return std::vector<float>(b->left(CR + 1).begin() + 512, b->left(CR + 1).end());
    };
    MachineParams p = ownOuts();
    p.engine.voice[CR].k[knob::PcmSource] = static_cast<float>(PcmSource::User);
    auto m = makeMachine();
    const auto factory = render(*m, p);   // empty user slot: falls back to the ROM
    MachineParams rom = p;
    rom.engine.voice[CR].k[knob::PcmSource] = static_cast<float>(PcmSource::Rom);
    REQUIRE(same(factory, render(*m, rom)));

    RomHandoff* a = makeRom(RomSlot::Crash, 7);
    RomHandoff* b = makeRom(RomSlot::Crash, 13);
    REQUIRE(m->postUserRom(a) == nullptr);
    const auto withA = render(*m, p);
    REQUIRE_FALSE(same(withA, factory));
    REQUIRE(m->takeRetiredRom() == nullptr);   // a is in use

    REQUIRE(m->postUserRom(b) == nullptr);
    const auto withB = render(*m, p);
    REQUIRE_FALSE(same(withB, withA));
    REQUIRE(m->takeRetiredRom() == a);         // a retired once b took over
    delete a;

    auto* clear = new RomHandoff;
    clear->slot = RomSlot::Crash;
    clear->clear = true;
    REQUIRE(m->postUserRom(clear) == nullptr);
    REQUIRE(same(render(*m, p), factory));
    std::vector<RomHandoff*> back;
    while (RomHandoff* h = m->takeRetiredRom()) back.push_back(h);
    REQUIRE(back.size() == 2U);   // b and the clear request itself
    for (auto* h : back) delete h;

    // Another slot is independent; the machine frees what it still holds.
    REQUIRE(m->postUserRom(makeRom(RomSlot::Hat, 5)) == nullptr);
    RomHandoff* superseded = m->postUserRom(makeRom(RomSlot::Hat, 9));
    REQUIRE(superseded != nullptr);
    delete superseded;
}

TEST_CASE("Blocks larger than the prepared size and empty blocks are handled", "[core][machine]") {
    auto m = makeMachine(256);
    postPattern(*m, 0, patternWith({BD, SD}, {0, 2, 4, 6, 8, 10, 12, 14}));
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostSync;
    Buses b(32768, 1);
    r.run(b, 32768, 5000);
    REQUIRE(allFinite(b.left()));
    REQUIRE(hits(*m, BD) == 3U);   // eighths at 0, 12000 and 24000

    Machine::ProcessArgs a;
    a.out = b.view();
    a.numSamples = 0;
    m->process(a, r.params);   // nothing to do, nothing breaks
    Machine unprepared;
    a.numSamples = 64;
    unprepared.process(a, r.params);
    SUCCEED();
}

TEST_CASE("Tap write does not depend on the block size", "[core][machine][midi][partition]") {
    // BD is on step 4 (sample 24000 at 120 BPM). A tap 100 samples after it
    // must not cancel it (it already sounded); a tap 100 samples before it
    // takes it over. Either way the result is the same for every block size.
    auto run = [](int tapAt, int block) {
        auto m = makeMachine(8192);
        seq::PatternBank bank;
        bank.patterns[0].at(0, BD, 4).setLevel(seq::StepLevel::Normal);
        m->sequencer().setBank(bank);
        Runner r{*m};
        r.params.seq.mode = seq::Mode::Internal;
        r.params.seq.run = true;
        r.params.seq.internalBpm = 120.0;
        r.params.seq.record = true;
        r.midi = std::vector<MidiMessage>{noteOn(tapAt, 36, 100, 10)};
        auto b = std::make_unique<Buses>(30000, 1);
        r.run(*b, 30000, block);
        int recorded = 0;
        Notice n;
        while (m->poll(n))
            if (n.type == Notice::Type::StepRecorded) ++recorded;
        return std::pair<std::uint32_t, int>{hits(*m, BD), recorded};
    };
    for (int block : {1, 100, 1000, 4096, 8192}) {
        INFO("block " << block);
        REQUIRE(run(24100, block).first == 2U);    // the step, then the tap
        REQUIRE(run(23900, block).first == 1U);    // the tap takes the step over
    }
}

TEST_CASE("Host Note: the pattern plays until the last held pattern note comes up", "[core][machine][midi]") {
    auto m = makeMachine();
    postPattern(*m, 0, patternWith({BD}, {0, 4, 8, 12}));
    postPattern(*m, 1, patternWith({SD}, {0, 4, 8, 12}));
    Runner r{*m};
    r.params.seq.mode = seq::Mode::HostNote;
    r.playing = false;
    r.midi = std::vector<MidiMessage>{noteOn(0, kPatternNoteBase, 100, 1), noteOn(10000, kPatternNoteBase + 1, 100, 1),
                                      noteOff(20000, kPatternNoteBase + 1, 1), noteOff(60000, kPatternNoteBase, 1)};
    Buses b(70000, 1);
    r.run(b, 20000, 512);
    REQUIRE(m->playState().currentPattern.load() == 1);
    r.run(b, 30000, 512, 20000);   // the newer note is up, the older still down
    REQUIRE(m->playState().playing.load());
    const std::uint32_t played = hits(*m, BD) + hits(*m, SD);
    r.run(b, 10000, 512, 50000);
    REQUIRE(hits(*m, BD) + hits(*m, SD) > played);
    r.run(b, 10000, 512, 60000);   // the last one up
    REQUIRE_FALSE(m->playState().playing.load());

    SECTION("leaving Host Note with a key down does not leave the pattern held") {
        auto m2 = makeMachine();
        postPattern(*m2, 0, patternWith({BD}, {0, 4, 8, 12}));
        Runner r2{*m2};
        r2.params.seq.mode = seq::Mode::HostNote;
        r2.playing = false;
        r2.midi = std::vector<MidiMessage>{noteOn(0, kPatternNoteBase, 100, 1), noteOff(15000, kPatternNoteBase, 1)};
        Buses b2(30000, 1);
        r2.run(b2, 10000, 512);
        REQUIRE(m2->playState().playing.load());
        r2.params.seq.mode = seq::Mode::Off;
        r2.run(b2, 10000, 512, 10000);   // the note-off arrives while in Off
        r2.params.seq.mode = seq::Mode::HostNote;
        r2.run(b2, 10000, 512, 20000);
        REQUIRE_FALSE(m2->playState().playing.load());
    }
}

TEST_CASE("MIDI out: a re-prepare keeps the note-offs of notes already sent", "[core][machine][midi]") {
    auto m = makeMachine();
    postPattern(*m, 0, patternWith({BD}, {0}));
    Runner r{*m};
    r.params.seq.mode = seq::Mode::Internal;
    r.params.seq.run = true;
    r.params.midi.midiOut = true;
    Buses b(20 * 256 + 512, 1);
    r.run(b, 512, 512);
    m->prepare(kFs, 256);   // a host buffer-size change while playing
    r.run(b, 20 * 256, 256, 512);
    // Every note sent is closed, including the one sounding at the re-prepare
    // (the sequencer restarts, so step 0 may sound again after it).
    int open = 0, ons = 0;
    for (const auto& msg : r.midiOut) {
        if ((msg.status & 0xF0) == 0x90) {
            REQUIRE(open == 0);
            ++open;
            ++ons;
        } else {
            REQUIRE(open == 1);
            --open;
        }
    }
    REQUIRE(ons >= 1);
    REQUIRE(open == 0);
}

TEST_CASE("MIDI out: a full buffer drops note-ons, never note-offs", "[core][machine][midi]") {
    // 11 voices, 32nds, x4 sub-steps and flams at 1000 BPM and 22.05 kHz: more
    // than MidiOutBuffer::kCapacity messages per 8192-sample block.
    seq::PatternBank bank;
    auto& p = bank.patterns[0];
    p.length = 64;
    p.scale = seq::Scale::ThirtySecond;
    for (int v = 0; v < kNumVoices; ++v)
        for (int s = 0; s < p.length; ++s) {
            auto& st = p.at(0, v, s);
            st.setLevel(static_cast<seq::StepLevel>(1 + (s + v) % 3));
            st.setSubSteps(4);
            st.setFlam(true);
        }
    auto m = makeMachine(8192, 22050.0);
    m->sequencer().setBank(bank);
    Runner r{*m};
    r.sampleRate = 22050.0;
    r.bpm = 1000.0;   // host tempos are followed up to 1000 BPM
    r.params.seq.mode = seq::Mode::HostSync;
    r.params.midi.midiOut = true;
    Buses b(22050 * 4, 1);
    std::size_t before = 0;
    bool overflowed = false;
    for (int pos = 0; pos < 22050 * 3; pos += 8192) {
        r.block(b, pos, std::min(8192, 22050 * 3 - pos));
        REQUIRE(r.midiOut.size() - before <= static_cast<std::size_t>(MidiOutBuffer::kCapacity));
        overflowed = overflowed || r.midiOut.size() - before >= static_cast<std::size_t>(MidiOutBuffer::kCapacity) - 12;
        before = r.midiOut.size();
    }
    r.playing = false;
    r.block(b, 22050 * 3, 8192);
    REQUIRE(overflowed);
    std::array<int, 128> open{};
    for (const auto& msg : r.midiOut) {
        if ((msg.status & 0xF0) == 0x90) {
            REQUIRE(open[msg.data1] == 0);
            ++open[msg.data1];
        } else {
            REQUIRE(open[msg.data1] == 1);   // never an orphan note-off
            --open[msg.data1];
        }
    }
    for (int n : open) REQUIRE(n == 0);   // nothing left hanging
}

TEST_CASE("Handoffs are never leaked when the UI stops draining", "[core][machine][handoff]") {
    auto m = makeMachine();
    Runner r{*m};
    Buses b(512, 1);
    std::vector<BankHandoff*> posted;
    int superseded = 0;
    for (int i = 0; i < 200; ++i) {
        auto* h = new BankHandoff;
        h->generation = static_cast<std::uint32_t>(i + 1);
        h->bank.patterns[0] = patternWith({BD}, {i % 16});
        posted.push_back(h);
        if (BankHandoff* old = m->postBank(h)) {
            ++superseded;
            delete old;
        }
        r.run(b, 512, 512);
    }
    // An edit against the newest bank waits behind it, in order.
    postPattern(*m, 1, patternWith({SD}, {3}), 200);
    r.run(b, 512, 512);
    int retired = 0;
    while (BankHandoff* h = m->takeRetiredBank()) {
        ++retired;
        delete h;
    }
    REQUIRE(superseded > 0);   // adoption waited for room instead of dropping a handoff
    REQUIRE_FALSE(m->sequencer().bank().patterns[1] == patternWith({SD}, {3}));
    r.run(b, 512, 512);   // room again: the newest bank, then the edit
    REQUIRE(m->sequencer().bank().patterns[0] == patternWith({BD}, {199 % 16}));
    REQUIRE(m->sequencer().bank().patterns[1] == patternWith({SD}, {3}));
    for (int i = 0; i < 2; ++i) {
        while (BankHandoff* h = m->takeRetiredBank()) {
            ++retired;
            delete h;
        }
        r.run(b, 512, 512);
    }
    REQUIRE(retired + superseded == 200);   // every handoff came back

    // The same for user ROMs.
    int romBack = 0;
    for (int i = 0; i < 200; ++i) {
        auto* h = new RomHandoff;
        h->slot = RomSlot::Crash;
        h->image.data[0] = static_cast<std::uint8_t>(i);
        if (RomHandoff* old = m->postUserRom(h)) {
            ++romBack;
            delete old;
        }
        r.run(b, 512, 512);
    }
    r.run(b, 512, 512);
    while (RomHandoff* h = m->takeRetiredRom()) {
        ++romBack;
        delete h;
    }
    r.run(b, 512, 512);
    while (RomHandoff* h = m->takeRetiredRom()) {
        ++romBack;
        delete h;
    }
    REQUIRE(romBack == 199);   // the newest image is in use (the machine frees it)
}
