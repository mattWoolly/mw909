// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// PatternModel (docs/design/plugin.md §3): edits reach the audio thread, none
// is lost when the command queue is full, whole banks supersede queued edits,
// and what the audio thread records or selects flows back into the model.

#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "TestSupport.h"

using namespace plugintest;
using mw909::seq::Pattern;
using mw909::seq::StepLevel;

namespace {

struct Recorder final : mw909::plugin::PatternModel::Listener {
    std::vector<int> patterns, tracks, roms;
    int bank = 0, selection = 0, chain = 0;
    void patternChanged(int index) override { patterns.push_back(index); }
    void bankChanged() override { ++bank; }
    void selectionChanged() override { ++selection; }
    void chainChanged() override { ++chain; }
    void songTrackChanged(int index) override { tracks.push_back(index); }
    void userRomChanged(int slot) override { roms.push_back(slot); }
};

const mw909::seq::PatternBank& audioBank(mw909Processor& p) { return p.machineForTests().sequencer().bank(); }

Pattern rimShotOn(std::initializer_list<int> steps) {
    Pattern p;
    for (int s : steps) p.at(0, mw909::RS, s).setLevel(StepLevel::Normal);
    return p;
}

}  // namespace

TEST_CASE("patterns: a fresh instance has handed its bank to the audio thread", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    host.process(64);  // no timer, no service call
    CHECK(sameBank(audioBank(*p), p->patterns().bank()));
    CHECK(p->playState().selectedPattern.load() == p->patterns().selectedPattern());
}

TEST_CASE("patterns: single edits arrive with the next block", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    Recorder recorder;
    p->patterns().addListener(&recorder);
    Rng rng(11U);

    const Pattern a = randomPattern(rng);
    p->patterns().setPattern(17, a);
    CHECK(p->patterns().pattern(17) == a);
    CHECK(recorder.patterns == std::vector<int>{17});
    host.process(32);
    CHECK(audioBank(*p).patterns[17] == a);

    // edit(): read-modify-write of one pattern.
    p->patterns().edit(17, [](Pattern& pattern) { pattern.at(1, mw909::RD, 63).setLevel(StepLevel::Accent); });
    CHECK(p->patterns().pattern(17).at(1, mw909::RD, 63).level() == StepLevel::Accent);
    CHECK(recorder.patterns == std::vector<int>{17, 17});
    host.process(32);
    CHECK(audioBank(*p).patterns[17] == p->patterns().pattern(17));

    // Setting the same content again is not an edit.
    p->patterns().setPattern(17, p->patterns().pattern(17));
    CHECK(recorder.patterns.size() == 2U);

    const mw909::seq::Chain chain = randomChain(rng);
    p->patterns().setChain(chain);
    CHECK(recorder.chain == 1);
    host.process(32);
    CHECK(audioBank(*p).chain == chain);
    CHECK(p->patterns().chain() == chain);

    // Song tracks, the same way.
    const mw909::seq::SongTrack track = randomSongTrack(rng);
    p->patterns().setSongTrack(6, track);
    CHECK(recorder.tracks == std::vector<int>{6});
    CHECK(p->patterns().songTrack(6) == track);
    host.process(32);
    CHECK(audioBank(*p).tracks[6] == track);
    p->patterns().setSongTrack(6, track);  // unchanged: not an edit
    p->patterns().setSongTrack(8, track);  // out of range
    p->patterns().setSongTrack(-1, track);
    CHECK(recorder.tracks.size() == 1U);
    mw909::seq::SongTrack badTrack = track;
    badTrack.length = 9999;
    badTrack.entries[0] = 200;
    p->patterns().setSongTrack(1, badTrack);
    host.process(32);
    CHECK(audioBank(*p).tracks[1].length <= mw909::seq::kMaxSongLength);
    CHECK(audioBank(*p).tracks[1].entries[0] < mw909::seq::kNumPatterns);
    CHECK(audioBank(*p).tracks[1] == p->patterns().songTrack(1));

    // Out-of-range indices are ignored.
    p->patterns().setPattern(-1, a);
    p->patterns().setPattern(96, a);
    p->patterns().edit(99, [](Pattern&) { FAIL("must not be called"); });
    CHECK(recorder.patterns.size() == 2U);

    // Illegal field values never reach the audio thread.
    Pattern bad = a;
    bad.length = 0;
    bad.shuffle = 250;
    bad.flamMs = 0;
    bad.flamGrace = 255;
    bad.reserved = 7;
    bad.trackShuffle[3] = 200;
    bad.scale = static_cast<mw909::seq::Scale>(99);
    p->patterns().setPattern(3, bad);
    host.process(32);
    CHECK(audioBank(*p).patterns[3].length >= 1);
    CHECK(audioBank(*p).patterns[3].shuffle <= mw909::seq::kMaxShuffleTicks);
    CHECK(audioBank(*p).patterns[3].flamMs >= mw909::seq::kMinFlamMs);
    CHECK(audioBank(*p).patterns[3].flamGrace <= 100);
    CHECK(audioBank(*p).patterns[3].reserved == 0);
    CHECK(audioBank(*p).patterns[3].trackShuffle[3] <= mw909::seq::kMaxShuffleTicks + 1);
    CHECK(static_cast<int>(audioBank(*p).patterns[3].scale) < static_cast<int>(mw909::seq::Scale::kNumScales));
    CHECK(audioBank(*p).patterns[3] == p->patterns().pattern(3));

    p->patterns().removeListener(&recorder);
}

TEST_CASE("patterns: a full command queue marks edits dirty and re-posts them", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    host.process(32);
    Rng rng(12U);

    // No audio block runs while 5 rounds of edits to all 96 patterns are made:
    // far more than the queue holds.
    for (int round = 0; round < 5; ++round)
        for (int i = 0; i < mw909::seq::kNumPatterns; ++i) p->patterns().setPattern(i, randomPattern(rng));
    const mw909::seq::Chain chain = randomChain(rng);
    p->patterns().setChain(chain);
    for (int t = 0; t < mw909::seq::kNumSongTracks; ++t) p->patterns().setSongTrack(t, randomSongTrack(rng));
    p->patterns().selectPattern(41, true);
    REQUIRE(p->patterns().numDirty() > 0);

    // The model is already right; the audio side is not yet.
    CHECK_FALSE(sameBank(audioBank(*p), p->patterns().bank()));

    // Blocks drain the queue, service passes re-post what did not fit.
    int passes = 0;
    while (p->patterns().numDirty() > 0 && passes < 50) {
        host.process(32);
        p->serviceMessageThread();
        ++passes;
    }
    host.process(32);
    CHECK(p->patterns().numDirty() == 0);
    CHECK(passes < 50);
    CHECK(sameBank(audioBank(*p), p->patterns().bank()));  // every edit arrived, latest content
    CHECK(audioBank(*p).chain == chain);
    CHECK(p->playState().selectedPattern.load() == 41);
    CHECK(p->patterns().selectedPattern() == 41);
}

TEST_CASE("patterns: a new bank supersedes edits still in the queue", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    host.process(32);
    Rng rng(13U);
    Recorder recorder;
    p->patterns().addListener(&recorder);

    const auto generationBefore = p->patterns().generation();
    for (int i = 0; i < 30; ++i) p->patterns().setPattern(i, randomPattern(rng));  // queued, older generation

    auto bank = std::make_unique<mw909::seq::PatternBank>();
    bank->patterns[2] = rimShotOn({0, 4, 8, 12});
    bank->chain = randomChain(rng);
    p->patterns().replaceBank(*bank);
    CHECK(p->patterns().generation() == generationBefore + 1U);
    CHECK(recorder.bank == 1);
    CHECK(sameBank(p->patterns().bank(), *bank));

    host.process(32);
    CHECK(sameBank(audioBank(*p), *bank));  // none of the 30 stale edits applied

    // Edits made after the replacement do apply.
    const Pattern later = randomPattern(rng);
    p->patterns().setPattern(2, later);
    host.process(32);
    CHECK(audioBank(*p).patterns[2] == later);

    // Many replacements in a row: the last one wins, and nothing piles up.
    for (int i = 0; i < 100; ++i) {
        bank->patterns[10] = randomPattern(rng);
        p->patterns().replaceBank(*bank);
        if ((i % 7) == 0) host.process(16);
        if ((i % 13) == 0) p->serviceMessageThread();
    }
    host.process(32);
    p->serviceMessageThread();
    CHECK(sameBank(audioBank(*p), *bank));
    CHECK(sameBank(p->patterns().bank(), *bank));
    p->patterns().removeListener(&recorder);
}

TEST_CASE("patterns: steps recorded on the audio thread reach the model", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    auto bank = std::make_unique<mw909::seq::PatternBank>();  // empty memory
    p->patterns().replaceBank(*bank);
    p->patterns().selectPattern(0, true);
    setParam(*p, "record", 1.0f);
    host.run(4800, 512);  // rolling
    p->serviceMessageThread();
    REQUIRE(p->playState().playing.load());

    Recorder recorder;
    p->patterns().addListener(&recorder);
    HostListener hostListener(*p);

    // Play the ride (normal) and the clap (accent) live.
    host.noteOn(mw909::kVoiceNote[mw909::RD], 50, 100);  // below the accent threshold
    host.process(512);
    host.midi.clear();
    host.run(6000, 512);
    host.noteOn(mw909::kVoiceNote[mw909::CP], 127, 10);
    host.process(512);
    host.midi.clear();
    host.run(1024, 512);

    // Nothing in the model until the service pass drains the notices.
    CHECK(p->patterns().pattern(0) == Pattern{});
    p->serviceMessageThread();

    const Pattern& recorded = p->patterns().pattern(0);
    int rides = 0, claps = 0;
    for (int s = 0; s < 16; ++s) {
        if (recorded.at(0, mw909::RD, s).level() == StepLevel::Normal) ++rides;
        if (recorded.at(0, mw909::CP, s).level() == StepLevel::Accent) ++claps;
    }
    CHECK(rides == 1);
    CHECK(claps == 1);
    CHECK(recorder.patterns == std::vector<int>{0});
    CHECK(hostListener.stateChanges == 1);  // the host is told the session changed

    // Model and audio hold the same pattern, and it is in the saved state.
    host.process(64);
    p->serviceMessageThread();
    CHECK(sameBank(audioBank(*p), p->patterns().bank()));
    auto other = makeProcessor(kRate, kBlock, false);
    loadState(*other, saveState(*p));
    CHECK(other->patterns().pattern(0) == recorded);
    p->patterns().removeListener(&recorder);
}

TEST_CASE("patterns: recording and editing the same pattern at once converges", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    p->patterns().replaceBank(*bank);
    setParam(*p, "record", 1.0f);
    host.run(4800, 256);
    p->serviceMessageThread();

    Rng rng(14U);
    for (int i = 0; i < 300; ++i) {
        if ((i % 3) == 0) host.noteOn(mw909::kVoiceNote[static_cast<std::size_t>(randInt(rng, 0, 10))], 100, randInt(rng, 0, 255));
        if ((i % 5) == 0)
            p->patterns().edit(0, [&rng](Pattern& pattern) {
                pattern.at(0, randInt(rng, 0, 10), randInt(rng, 0, 15)).setLevel(StepLevel::Weak);
            });
        if ((i % 41) == 0) p->patterns().replaceBank(*bank);
        host.process(256);
        host.midi.clear();
        if ((i % 2) == 0) p->serviceMessageThread();
    }
    // Stop playing and editing: a few passes later both sides are identical.
    for (int i = 0; i < 8; ++i) {
        host.process(256);
        p->serviceMessageThread();
    }
    CHECK(p->patterns().numDirty() == 0);
    CHECK(sameBank(audioBank(*p), p->patterns().bank()));
}

TEST_CASE("patterns: selection goes both ways", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    p->patterns().replaceBank(*std::make_unique<mw909::seq::PatternBank>());  // 16-step patterns throughout
    host.head.playing = false;  // stopped: a selection takes over at once
    Recorder recorder;
    p->patterns().addListener(&recorder);

    p->patterns().selectPattern(9);
    CHECK(p->patterns().selectedPattern() == 9);
    CHECK(recorder.selection == 1);
    host.process(64);
    CHECK(p->playState().selectedPattern.load() == 9);
    p->serviceMessageThread();
    CHECK(p->patterns().selectedPattern() == 9);
    CHECK(recorder.selection == 1);  // no echo of our own selection

    p->patterns().selectPattern(96);  // ignored
    p->patterns().selectPattern(-1);
    CHECK(p->patterns().selectedPattern() == 9);

    // While playing, a selection is queued to the end of the measure and the
    // model already shows the target.
    host.head.playing = true;
    host.run(2400, 512);
    p->patterns().selectPattern(11);
    host.process(64);
    CHECK(p->playState().queuedPattern.load() == 11);
    CHECK(p->playState().selectedPattern.load() == 9);
    p->serviceMessageThread();
    CHECK(p->patterns().selectedPattern() == 11);
    host.run(96000 + 4800, 512);  // one measure later
    p->serviceMessageThread();
    CHECK(p->playState().selectedPattern.load() == 11);
    CHECK(p->patterns().selectedPattern() == 11);

    // Host Note mode: a held key selects the pattern on the audio thread, and
    // the model follows through a notice.
    host.head.playing = false;
    setParam(*p, "seq_mode", 3.0f);
    host.run(1024, 512);
    p->serviceMessageThread();
    const int selectionsBefore = recorder.selection;
    host.noteOn(mw909::kPatternNoteBase + 20, 100, 0);
    host.process(512);
    host.midi.clear();
    host.run(48000, 512);
    p->serviceMessageThread();
    if (p->playState().selectedPattern.load() == 20) {
        CHECK(p->patterns().selectedPattern() == 20);
        CHECK(recorder.selection == selectionsBefore + 1);
    } else {
        // The core does not move the basic selection for pattern notes: then
        // the model must not have moved either.
        CHECK(p->patterns().selectedPattern() == p->playState().selectedPattern.load());
    }
    p->patterns().removeListener(&recorder);
}

TEST_CASE("patterns: the service timer runs without an editor", "[plugin][patterns]") {
    // The one test that pumps the message loop, to see the real 30 Hz timer
    // (rather than a direct service call) drain a notice into the model.
    auto p = makeProcessor();
    Host host(*p);
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    p->patterns().replaceBank(*bank);
    setParam(*p, "record", 1.0f);
    host.run(4800, 512);
    // Let the timer see that the audio thread has taken the new bank (until it
    // has, recorded steps are treated as belonging to the old one).
    juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
    host.noteOn(mw909::kVoiceNote[mw909::RS], 96, 0);
    host.process(512);
    host.midi.clear();
    REQUIRE(p->patterns().pattern(p->patterns().selectedPattern()) == Pattern{});

    bool arrived = false;
    for (int i = 0; i < 100 && !arrived; ++i) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        arrived = !(p->patterns().pattern(p->patterns().selectedPattern()) == Pattern{});
    }
    CHECK(arrived);
}

TEST_CASE("patterns: audition, fill and panic reach the audio thread", "[plugin][patterns]") {
    auto p = makeProcessor();
    Host host(*p);
    host.head.playing = false;
    host.process(64);

    const auto before = hits(*p, mw909::CR);
    p->audition(mw909::CR, false);
    p->audition(mw909::CR, true);
    host.process(512);
    CHECK(hits(*p, mw909::CR) == before + 2U);
    CHECK(host.peak(0, 512) > 0.0f);

    // Auditions ignore mute.
    setParam(*p, "cr_mute", 1.0f);
    p->audition(mw909::CR, false);
    host.process(512);
    CHECK(hits(*p, mw909::CR) == before + 3U);

    // Panic silences the ringing cymbal.
    p->panic();
    host.process(512);
    host.process(512);
    CHECK(host.peak(0, 512) == 0.0f);

    // A panic that finds the queue full is retried by the service pass.
    Rng rng(15U);
    p->audition(mw909::CR, true);
    host.process(512);
    REQUIRE(host.peak(0, 512) > 0.0f);
    for (int round = 0; round < 4; ++round)
        for (int i = 0; i < 96; ++i) p->patterns().setPattern(i, randomPattern(rng));
    p->panic();
    for (int i = 0; i < 6; ++i) {
        host.process(512);
        p->serviceMessageThread();
    }
    host.process(512);
    CHECK(host.peak(0, 512) == 0.0f);

    p->requestFill();  // must simply be accepted
    host.process(64);
    CHECK(host.allFinite(64));
}

namespace {

// A ROM image of silence (mid-scale everywhere).
mw909::RomImage silentRom() {
    mw909::RomImage image;
    image.data.fill(mw909::kRomMidScale);
    return image;
}

// Peak of one crash hit, rendered dry.
float crashPeak(mw909Processor& p, Host& host) {
    p.panic();
    host.run(1024, 512);
    host.midi.clear();
    host.noteOn(mw909::kVoiceNote[mw909::CR], 127);
    host.process(4096);
    host.midi.clear();
    return std::max(host.peak(0, 4096), host.peak(1, 4096));
}

}  // namespace

TEST_CASE("patterns: user ROMs reach the voices with the next block", "[plugin][patterns][roms]") {
    auto p = makeProcessor();
    Host host(*p);
    host.head.playing = false;
    dryMix(*p);
    Recorder recorder;
    p->patterns().addListener(&recorder);

    const float factory = crashPeak(*p, host);
    REQUIRE(factory > 1.0e-3f);

    // Source = User with no user image falls back to the factory ROM.
    setParam(*p, "cr_source", 2.0f);
    CHECK(crashPeak(*p, host) > 0.5f * factory);

    // A silent image: the very next block plays it (no timer, no service call).
    REQUIRE(p->patterns().setUserRom(mw909::RomSlot::Crash, silentRom(), "silence"));
    CHECK(recorder.roms == std::vector<int>{static_cast<int>(mw909::RomSlot::Crash)});
    CHECK(p->userRom(mw909::RomSlot::Crash).name == "silence");
    CHECK(crashPeak(*p, host) < 0.05f * factory);

    // The factory source ignores the user slot.
    setParam(*p, "cr_source", 0.0f);
    CHECK(crashPeak(*p, host) > 0.5f * factory);

    // Clearing the slot: User falls back to the factory image again.
    setParam(*p, "cr_source", 2.0f);
    p->clearUserRom(mw909::RomSlot::Crash);
    CHECK(p->userRom(mw909::RomSlot::Crash).empty());
    CHECK(crashPeak(*p, host) > 0.5f * factory);

    // An image with values above 6 bits is refused and changes nothing.
    mw909::RomImage bad = silentRom();
    bad.data[100] = 64;
    CHECK_FALSE(p->patterns().setUserRom(mw909::RomSlot::Crash, bad, "bad"));
    CHECK(p->userRom(mw909::RomSlot::Crash).empty());

    // Many replacements in a row, without service passes: every handoff is
    // retired and freed (ASan would see a leak), the last one plays.
    Rng rng(16U);
    for (int i = 0; i < 300; ++i) {
        p->patterns().setUserRom(static_cast<mw909::RomSlot>(i % 3), randomRom(rng), "r");
        if (i % 3 == 0) host.process(16);
    }
    p->patterns().setUserRom(mw909::RomSlot::Crash, silentRom(), "last");
    CHECK(crashPeak(*p, host) < 0.05f * factory);
    p->patterns().removeListener(&recorder);
}

TEST_CASE("patterns: user ROMs come with a state load and an offline bounce plays them", "[plugin][patterns][roms]") {
    auto donor = makeProcessor(kRate, kBlock, false);
    REQUIRE(donor->patterns().setUserRom(mw909::RomSlot::Crash, silentRom(), "silence"));
    setParam(*donor, "cr_source", 2.0f);
    const juce::MemoryBlock saved = saveState(*donor);

    for (const bool foreignThread : {false, true}) {
        auto p = makeProcessor();
        Host host(*p);
        host.head.playing = false;
        dryMix(*p);
        const float factory = crashPeak(*p, host);
        if (foreignThread)
            std::thread([&] { loadState(*p, saved); }).join();
        else
            loadState(*p, saved);
        dryMix(*p);
        INFO((foreignThread ? "loaded on another thread" : "loaded on the message thread"));
        CHECK(crashPeak(*p, host) < 0.05f * factory);  // straight away
        CHECK(p->userRom(mw909::RomSlot::Crash).name == "silence");
        p->serviceMessageThread();
        CHECK_FALSE(p->patterns().hasStagedRestore());
        CHECK(p->userRom(mw909::RomSlot::Crash).name == "silence");
        CHECK(p->userRom(mw909::RomSlot::Hat).empty());
    }
}

TEST_CASE("patterns: importUserRom builds a slot from an audio file", "[plugin][patterns][roms]") {
    auto p = makeProcessor();
    Host host(*p);
    const juce::File dir = scratchDirectory("rom-import");
    const juce::File wav = writeTestWav(dir.getChildFile("cymbal.wav"), 44100.0, 1.5, 7U);
    REQUIRE(wav.existsAsFile());

    // The open-hat region only: the closed-hat words keep the factory content.
    REQUIRE(p->importUserRom(wav, mw909::RomRegion::HatOpen));
    const auto hat = p->userRom(mw909::RomSlot::Hat);
    REQUIRE_FALSE(hat.empty());
    CHECK(hat.name == "cymbal.wav");
    const auto& factoryHat = mw909::factoryRom(mw909::RomSlot::Hat);
    bool openChanged = false;
    for (int w = mw909::kHatOpenStart; w < mw909::kHatOpenEnd; ++w)
        openChanged = openChanged || hat.image->data[static_cast<std::size_t>(w)] != factoryHat.data[static_cast<std::size_t>(w)];
    CHECK(openChanged);
    bool closedKept = true;
    for (int w = mw909::kHatClosedStart; w < mw909::kHatClosedEnd; ++w)
        closedKept = closedKept && hat.image->data[static_cast<std::size_t>(w)] == factoryHat.data[static_cast<std::size_t>(w)];
    CHECK(closedKept);
    for (const auto word : hat.image->data) CHECK(word <= 63);

    // Then the closed region into the same slot: the open region stays.
    const juce::File other = writeTestWav(dir.getChildFile("closed.wav"), 96000.0, 0.3, 8U);
    REQUIRE(p->importUserRom(other, mw909::RomRegion::HatClosed));
    const auto both = p->userRom(mw909::RomSlot::Hat);
    CHECK(std::equal(both.image->data.begin() + mw909::kHatOpenStart, both.image->data.begin() + mw909::kHatOpenEnd,
                     hat.image->data.begin() + mw909::kHatOpenStart));

    // Crash and ride land in their own slots.
    REQUIRE(p->importUserRom(wav, mw909::RomRegion::Ride));
    CHECK_FALSE(p->userRom(mw909::RomSlot::Ride).empty());
    CHECK(p->userRom(mw909::RomSlot::Crash).empty());

    // Refusals change nothing.
    const auto before = p->userRom(mw909::RomSlot::Crash);
    CHECK_FALSE(p->importUserRom(dir.getChildFile("missing.wav"), mw909::RomRegion::Crash));
    dir.getChildFile("junk.wav").replaceWithText("not audio at all");
    CHECK_FALSE(p->importUserRom(dir.getChildFile("junk.wav"), mw909::RomRegion::Crash));
    dir.getChildFile("empty.wav").replaceWithText("");
    CHECK_FALSE(p->importUserRom(dir.getChildFile("empty.wav"), mw909::RomRegion::Crash));
    CHECK_FALSE(p->importUserRom(wav, static_cast<mw909::RomRegion>(9)));
    CHECK(p->userRom(mw909::RomSlot::Crash) == before);

    // The imported slots are in the session and play.
    auto q = makeProcessor();
    loadState(*q, saveState(*p));
    CHECK(q->userRom(mw909::RomSlot::Hat) == p->userRom(mw909::RomSlot::Hat));
    CHECK(q->userRom(mw909::RomSlot::Ride) == p->userRom(mw909::RomSlot::Ride));
    setParam(*p, "oh_source", 2.0f);
    host.head.playing = false;
    host.noteOn(mw909::kVoiceNote[mw909::OH], 127);
    host.process(4096);
    CHECK(host.allFinite(4096));
    CHECK(host.peak(0, 4096) > 0.0f);
}
