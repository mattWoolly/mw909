// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Cross-thread stress: an audio thread hammers processBlock (with host
// automation delivered on that thread, as the VST3 wrapper does) while a host
// worker thread loads and saves state, changes programs and re-prepares, and
// the message thread edits patterns, loads presets, learns MIDI, opens editors
// and runs the service timer. Afterwards the model and the audio side must
// agree, and the state must still round-trip exactly.
//
// Meant for the sanitizer lanes. The duration is MW909_STRESS_SECONDS (default
// 2 s so the normal suite stays quick; the `stress:` CTest entry runs 30 s).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <thread>

#include "TestSupport.h"

using namespace plugintest;

namespace {

double stressSeconds() {
    if (const char* text = std::getenv("MW909_STRESS_SECONDS")) {
        const double s = std::atof(text);
        if (s > 0.0 && s < 3600.0) return s;
    }
    return 2.0;
}

std::vector<juce::MemoryBlock> donorStates(Rng& rng, int count) {
    std::vector<juce::MemoryBlock> states;
    for (int i = 0; i < count; ++i) {
        auto donor = makeProcessor(kRate, kBlock, false);
        randomiseParameters(*donor, rng);
        auto bank = std::make_unique<mw909::seq::PatternBank>();
        for (auto& pattern : bank->patterns)
            if (randInt(rng, 0, 2) != 0) pattern = randomPattern(rng);
        bank->chain = randomChain(rng);
        donor->patterns().replaceBank(*bank);
        donor->patterns().selectPattern(randInt(rng, 0, mw909::seq::kNumPatterns - 1));
        donor->setUnitSeed(static_cast<std::uint32_t>(rng()));
        donor->midiLearn().assign(randInt(rng, 0, 119), "sd_tone");
        for (auto& track : bank->tracks)
            if (randInt(rng, 0, 1) == 0) track = randomSongTrack(rng);
        donor->patterns().replaceBank(*bank);
        if (randInt(rng, 0, 1) == 0) donor->patterns().setUserRom(static_cast<mw909::RomSlot>(randInt(rng, 0, 2)), randomRom(rng), "donor");
        // Keep the transport running in every donor so loads happen mid-play.
        setParam(*donor, "seq_mode", 2.0f);  // Internal
        setParam(*donor, "seq_run", 1.0f);
        states.push_back(saveState(*donor));
    }
    return states;
}

}  // namespace

TEST_CASE("stress: audio host and message threads hammering at once stay consistent", "[plugin][stress][threads]") {
    const double seconds = stressSeconds();
    auto p = makeProcessor(kRate, kBlock, true);
    const juce::File presetRoot = scratchDirectory("stress-presets");
    p->presets().setUserRoot(presetRoot);
    const juce::File wavDir = scratchDirectory("stress-wavs");
    const juce::File wavs[] = {writeTestWav(wavDir.getChildFile("a.wav"), 44100.0, 0.8, 1U),
                               writeTestWav(wavDir.getChildFile("b.wav"), 48000.0, 0.2, 2U)};
    std::atomic<long> romImports{0};

    Rng setup(4242U);
    const std::vector<juce::MemoryBlock> states = donorStates(setup, 6);

    std::atomic<bool> stop{false};
    std::atomic<bool> finite{true};
    std::atomic<bool> badSave{false};
    std::atomic<long> blocks{0}, hostOps{0};

    // --- audio thread ---------------------------------------------------------------
    std::thread audio([&] {
        Rng rng(1U);
        FakePlayHead head;
        p->setPlayHead(&head);
        juce::AudioBuffer<float> storage(2, 2048);
        juce::MidiBuffer midi;
        midi.ensureSize(2048);
        const auto& params = p->getParameters();
        int n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const int size = (n % 97 == 0) ? 0 : randInt(rng, 1, (n % 13 == 0) ? 2048 : 512);
            midi.clear();
            for (int e = 0; e < randInt(rng, 0, 4); ++e) {
                const int offset = randInt(rng, 0, std::max(0, size - 1));
                const int note = randInt(rng, 30, 100);
                if (randInt(rng, 0, 1) == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(randInt(rng, 1, 16), note, static_cast<juce::uint8>(randInt(rng, 1, 127))), offset);
                else
                    midi.addEvent(juce::MidiMessage::noteOff(randInt(rng, 1, 16), note), offset);
            }
            if (n % 5 == 0) midi.addEvent(juce::MidiMessage::controllerEvent(1, randInt(rng, 0, 127), randInt(rng, 0, 127)), 0);
            // Host automation on the audio thread (VST3 delivers it there).
            for (int a = 0; a < 3; ++a) {
                auto* param = params[randInt(rng, 0, params.size() - 1)];
                const juce::String id = static_cast<juce::RangedAudioParameter*>(param)->paramID;
                if (id == "quality" || id == "variance" || id == "revision") continue;  // settings, re-prepare the voices
                param->setValueNotifyingHost(rand01(rng));
            }
            head.available = (n % 211) != 0;
            head.playing = (n % 503) < 400;
            if (n % 331 == 0) head.ppq = rand01(rng) * 64.0;  // loop / jump

            juce::AudioBuffer<float> view(storage.getArrayOfWritePointers(), 2, size);
            p->processBlock(view, midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < size; ++i)
                    if (!std::isfinite(storage.getSample(ch, i))) finite.store(false);
            if (head.playing) head.ppq += static_cast<double>(size) * head.bpm / (60.0 * kRate);
            ++n;
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
        p->setPlayHead(nullptr);
    });

    // --- a host worker thread ------------------------------------------------------------
    std::thread hostThread([&] {
        Rng rng(2U);
        int n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            switch (randInt(rng, 0, 9)) {
                case 0: case 1: case 2:
                    loadState(*p, states[static_cast<std::size_t>(randInt(rng, 0, static_cast<int>(states.size()) - 1))]);
                    break;
                case 3: case 4: case 5: {
                    const juce::MemoryBlock out = saveState(*p);
                    if (out.getSize() <= 8 || xmlFromState(out) == nullptr) badSave.store(true);
                    break;
                }
                case 6:
                    p->setCurrentProgram(randInt(rng, -1, p->getNumPrograms()));
                    break;
                case 7:
                    p->reset();
                    break;
                case 8:
                    if (n % 4 == 0) p->prepareToPlay(randInt(rng, 0, 1) == 0 ? 44100.0 : 96000.0, randInt(rng, 16, 1024));
                    break;
                default:
                    p->releaseResources();
                    break;
            }
            ++n;
            hostOps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // --- the message thread (this one) ----------------------------------------------------
    Rng rng(3U);
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    long rounds = 0;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        switch (randInt(rng, 0, 18)) {
            case 0: case 1: case 2:
                p->patterns().setPattern(randInt(rng, 0, 95), randomPattern(rng));
                break;
            case 3:
                p->patterns().edit(randInt(rng, 0, 95), [&](mw909::seq::Pattern& pat) {
                    pat.at(randInt(rng, 0, 1), randInt(rng, 0, 11), randInt(rng, 0, 63)).bits = static_cast<std::uint8_t>(rng());
                });
                break;
            case 4:
                p->patterns().setChain(randomChain(rng));
                break;
            case 5:
                p->patterns().selectPattern(randInt(rng, 0, 95), randInt(rng, 0, 1) == 1);
                break;
            case 16:
                p->patterns().setSongTrack(randInt(rng, 0, 7), randomSongTrack(rng));
                break;
            case 17:
                // A user ROM import: decode, build, hand over (message thread).
                if (rounds % 4 == 0) {
                    p->importUserRom(wavs[randInt(rng, 0, 1)], static_cast<mw909::RomRegion>(randInt(rng, 0, 3)));
                    romImports.fetch_add(1, std::memory_order_relaxed);
                } else if (rounds % 4 == 1) {
                    p->clearUserRom(static_cast<mw909::RomSlot>(randInt(rng, 0, 2)));
                } else {
                    p->patterns().setUserRom(static_cast<mw909::RomSlot>(randInt(rng, 0, 2)), randomRom(rng), "stress");
                }
                break;
            case 18:
                setParam(*p, voiceParam(randInt(rng, 7, 10), "source"), static_cast<float>(randInt(rng, 0, 2)));
                break;
            case 6: {
                const auto kind = static_cast<PresetKind>(randInt(rng, 0, 2));
                p->presets().load(kind, randInt(rng, 0, std::max(0, p->presets().numFactoryPresets(kind) - 1)));
                break;
            }
            case 7:
                if (rounds % 8 == 0) {
                    p->presets().saveUser(PresetKind::Full, "stress");
                    p->presets().loadFile(p->presets().userDirectory(PresetKind::Full).getChildFile("stress.mw909"));
                }
                break;
            case 8:
                loadState(*p, states[static_cast<std::size_t>(randInt(rng, 0, static_cast<int>(states.size()) - 1))]);
                break;
            case 9:
                if (saveState(*p).getSize() <= 8) badSave.store(true);
                break;
            case 10:
                p->midiLearn().assign(randInt(rng, 0, 119), juce::String(mw909::plugin::allParameterIds()[static_cast<std::size_t>(randInt(rng, 0, 607))]));
                if (rounds % 3 == 0) p->midiLearn().beginLearn("sd_level");
                break;
            case 11:
                p->audition(static_cast<mw909::VoiceId>(randInt(rng, 0, 10)), randInt(rng, 0, 1) == 1);
                p->requestFill();
                if (rounds % 50 == 0) p->panic();
                break;
            case 12: {
                UiState ui = p->uiState();
                ui.page = randInt(rng, 0, 3);
                p->setUiState(ui);
                if (rounds % 20 == 0) p->setUnitSeed(static_cast<std::uint32_t>(rng()));
                break;
            }
            case 13:
                if (editor == nullptr) editor.reset(p->createEditor());
                else editor.reset();
                break;
            case 14:
                setParam(*p, juce::String(mw909::plugin::allParameterIds()[static_cast<std::size_t>(randInt(rng, 0, 607))]), 0.0f);
                break;
            default:
                p->patterns().replaceBank(p->patterns().bank());
                break;
        }
        p->serviceMessageThread();
        ++rounds;
    }
    stop.store(true);
    audio.join();
    hostThread.join();
    editor.reset();

    INFO(blocks.load() << " blocks, " << hostOps.load() << " host operations, " << rounds << " message rounds, "
                       << romImports.load() << " ROM imports");
    CHECK(finite.load());
    CHECK_FALSE(badSave.load());
    // Each thread really ran (loose: a 2 s run under TSan is slow).
    CHECK(blocks.load() > 50);
    CHECK(hostOps.load() > 5);
    CHECK(rounds > 20);

    // Settle: the model and the audio side converge on the same pattern memory.
    p->prepareToPlay(kRate, kBlock);
    {
        Host host(*p);
        for (int i = 0; i < 20; ++i) {
            host.process(256);
            p->serviceMessageThread();
        }
    }
    CHECK(p->patterns().numDirty() == 0);
    CHECK_FALSE(p->patterns().hasStagedRestore());
    CHECK(sameBank(p->patterns().bank(), p->machineForTests().sequencer().bank()));
    if (seconds >= 10.0) CHECK(romImports.load() > 0);

    // And what it holds still round-trips exactly.
    const juce::MemoryBlock saved = saveState(*p);
    auto fresh = makeProcessor(kRate, kBlock, false);
    loadState(*fresh, saved);
    CHECK(saveState(*fresh) == saved);
}

// The window the stress test above can only hit by chance, held open on
// purpose: a restore from a host thread lands after the message thread has
// adopted an earlier staged restore but before its own edit is made. Here a
// listener (called during that adoption) runs the second restore and waits
// for it; in a host the same interleaving is a plain race between threads.
TEST_CASE("stress: an edit racing a background restore never leaves model and audio apart", "[plugin][stress][threads]") {
    auto p = makeProcessor(kRate, kBlock, true);
    Rng setup(77U);
    const std::vector<juce::MemoryBlock> states = donorStates(setup, 2);

    std::thread([&] { loadState(*p, states[0]); }).join();  // staged, not adopted yet
    REQUIRE(p->patterns().hasStagedRestore());

    struct Interloper final : mw909::plugin::PatternModel::Listener {
        mw909Processor& processor;
        const juce::MemoryBlock& state;
        bool fired = false;
        Interloper(mw909Processor& pr, const juce::MemoryBlock& st) : processor(pr), state(st) {}
        void bankChanged() override {
            if (fired) return;
            fired = true;
            std::thread([this] { loadState(processor, state); }).join();
        }
    } interloper(*p, states[1]);
    p->patterns().addListener(&interloper);

    Rng rng(5U);
    const mw909::seq::Pattern edit = randomPattern(rng);
    p->patterns().setPattern(5, edit);
    p->patterns().removeListener(&interloper);
    REQUIRE(interloper.fired);

    Host host(*p);
    for (int i = 0; i < 4; ++i) {
        host.process(128);
        p->serviceMessageThread();
    }
    CHECK(p->patterns().numDirty() == 0);
    CHECK(sameBank(p->patterns().bank(), p->machineForTests().sequencer().bank()));
    // The later restore wins on both sides.
    auto reference = makeProcessor(kRate, kBlock, false);
    loadState(*reference, states[1]);
    CHECK(sameBank(p->patterns().bank(), reference->patterns().bank()));
}

// The placeholder editor once was juce::GenericAudioProcessorEditor, whose rows
// add and remove themselves as AudioProcessorListeners while JUCE walks that
// list unlocked on every parameter change. With automation arriving on the
// audio thread, opening and closing the editor was a use-after-free (ASan).
TEST_CASE("stress: editors open and close while the audio thread automates", "[plugin][stress][editor][threads]") {
    auto p = makeProcessor(kRate, kBlock, true);
    std::atomic<bool> stop{false};
    std::thread automation([&] {
        Rng rng(11U);
        const auto& params = p->getParameters();
        juce::AudioBuffer<float> buffer(2, 64);
        juce::MidiBuffer midi;
        midi.ensureSize(2048);
        while (!stop.load()) {
            for (int a = 0; a < 16; ++a) {
                auto* param = params[randInt(rng, 0, params.size() - 1)];
                param->beginChangeGesture();
                param->setValueNotifyingHost(rand01(rng));
                param->endChangeGesture();
            }
            midi.clear();
            p->processBlock(buffer, midi);
        }
    });
    for (int i = 0; i < 60; ++i) {
        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        REQUIRE(editor != nullptr);
        editor->setSize(400 + i, 500);
    }
    stop.store(true);
    automation.join();
}
