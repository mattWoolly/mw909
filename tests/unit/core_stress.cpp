// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Two threads, as in the plugin: an audio thread running Machine::process on
// a dense pattern, and a UI thread hammering every cross-thread path at once
// (commands with generation stamps, whole-bank handoffs, user-ROM handoffs per
// slot, draining the retired queues and the notices, reading PlayState). Run
// it under ThreadSanitizer (data races) and AddressSanitizer (leaks,
// use-after-free of retired handoffs); in a plain build it checks the
// bookkeeping: every handoff is handed back exactly once.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <random>
#include <set>
#include <thread>

#include "CoreTestUtil.h"

using namespace mw909;
using namespace coretest;

TEST_CASE("UI and audio threads share the Machine safely", "[core][stress]") {
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 128;
    constexpr int kBlocks = 600;

    auto m = std::make_unique<Machine>();
    m->prepare(kFs, kBlock);
    postPattern(*m, 0, densePattern());

    std::atomic<bool> uiStarted{false};
    std::atomic<bool> audioDone{false};
    std::atomic<std::uint64_t> blocksRun{0};
    bool finite = true;
    std::uint32_t hitsSeen = 0;

    std::thread audio([&] {
        Buses buses(kBlock);
        MidiOutBuffer midiOut;
        MachineParams p = defaultMachineParams();
        p.seq.mode = seq::Mode::HostSync;
        p.seq.record = true;
        p.midi.midiOut = true;
        p.engine.channel[SD].sendBus = 2;
        p.engine.channel[SD].send = 0.4f;
        p.engine.fx.place[static_cast<std::size_t>(FxId::Reverb)] = {true, 1, 0};
        for (int v = CH; v <= RD; ++v) p.engine.voice[static_cast<std::size_t>(v)].k[knob::PcmSource] = 2.0f;   // User
        std::vector<MidiMessage> midi;
        while (!uiStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int b = 0; b < kBlocks; ++b) {
            midi.clear();
            if (b % 3 == 0) midi.push_back(noteOn(b % kBlock, kVoiceNote[static_cast<std::size_t>(b % kNumVoices)], 100));
            Machine::ProcessArgs a;
            a.transport.hostValid = true;
            a.transport.playing = true;
            a.transport.bpm = 140.0;
            a.transport.ppq = static_cast<double>(b) * kBlock * 140.0 / (60.0 * kFs);
            a.midiIn = midi.data();
            a.numMidiIn = static_cast<int>(midi.size());
            a.midiOut = &midiOut;
            a.out = buses.view();
            a.numSamples = kBlock;
            p.quality = (b / 100) % 2 == 0 ? Quality::Standard : Quality::Eco;   // re-prepares now and then
            m->process(a, p);
            finite = finite && allFinite(buses.left()) && allFinite(buses.right());
            blocksRun.fetch_add(1, std::memory_order_relaxed);
        }
        audioDone.store(true, std::memory_order_release);
    });

    // --- UI thread (this one) -------------------------------------------------
    std::mt19937 rng(11);
    std::uint32_t generation = 0;
    int banksPosted = 0, banksBack = 0, romsPosted = 0, romsBack = 0, notices = 0;
    std::set<const void*> live;   // handoffs owned by the machine right now
    auto drain = [&] {
        while (BankHandoff* h = m->takeRetiredBank()) {
            REQUIRE(live.erase(h) == 1U);   // handed back exactly once
            delete h;
            ++banksBack;
        }
        while (RomHandoff* h = m->takeRetiredRom()) {
            REQUIRE(live.erase(h) == 1U);
            delete h;
            ++romsBack;
        }
        Notice n;
        while (m->poll(n)) ++notices;
    };
    int iteration = 0;
    uiStarted.store(true, std::memory_order_release);
    // Until the audio thread is done, and at least 300 rounds even if this
    // thread was starved meanwhile (a loaded test machine).
    while (!audioDone.load(std::memory_order_acquire) || iteration < 300) {
        ++iteration;
        Command c;
        switch (rng() % 6) {
            case 0:
                c.type = Command::Type::SetPattern;
                c.index = static_cast<int>(rng() % 4);
                c.pattern = densePattern();
                c.generation = generation - (rng() % 2);   // sometimes stale
                break;
            case 1: c.type = Command::Type::SelectPattern; c.index = static_cast<int>(rng() % 4); break;
            case 2: c.type = Command::Type::Fill; break;
            case 3: c.type = Command::Type::Audition; c.index = static_cast<int>(rng() % kNumVoices); c.value = 3.0f; break;
            case 4: c.type = Command::Type::SetChain; c.chain.length = 2; c.chain.entries[1] = 1; c.generation = generation; break;
            default: c.type = (rng() % 50 == 0) ? Command::Type::Panic : Command::Type::Fill; break;
        }
        m->post(c);   // a full queue just drops this one (the plugin re-posts)

        if (iteration % 7 == 0) {
            auto* h = new BankHandoff;
            h->bank.patterns[0] = densePattern();
            h->generation = ++generation;
            live.insert(h);
            ++banksPosted;
            if (BankHandoff* old = m->postBank(h)) {   // superseded before the audio side saw it
                REQUIRE(live.erase(old) == 1U);
                delete old;
                ++banksBack;
            }
        }
        if (iteration % 5 == 0) {
            auto* h = new RomHandoff;
            h->slot = static_cast<RomSlot>(rng() % kNumRomSlots);
            h->clear = rng() % 4 == 0;
            for (auto& w : h->image.data) w = static_cast<std::uint8_t>(rng() % 64);
            live.insert(h);
            ++romsPosted;
            if (RomHandoff* old = m->postUserRom(h)) {
                REQUIRE(live.erase(old) == 1U);
                delete old;
                ++romsBack;
            }
        }
        drain();

        const auto& s = m->playState();
        const std::uint64_t pos = s.position.load(std::memory_order_acquire);
        REQUIRE((pos & 0xFFU) <= static_cast<std::uint64_t>(seq::kMaxSteps));
        (void)s.hits[BD].load(std::memory_order_relaxed);
        (void)s.channelPeak[SD].load(std::memory_order_relaxed);
        (void)s.tempo.load(std::memory_order_relaxed);
        std::this_thread::yield();
    }
    audio.join();
    drain();
    hitsSeen = m->playState().hits[BD].load();

    REQUIRE(blocksRun.load() == static_cast<std::uint64_t>(kBlocks));
    REQUIRE(finite);
    REQUIRE(hitsSeen > 0U);
    REQUIRE(banksPosted > 0);
    REQUIRE(romsPosted > 0);
    // What is not back yet is still the machine's: at most the pending bank
    // and, per ROM slot, one pending and one active handoff.
    REQUIRE(banksPosted - banksBack <= 1);
    REQUIRE(romsPosted - romsBack <= 2 * kNumRomSlots);
    INFO("notices " << notices);
    m.reset();   // the destructor frees the rest (ASan checks for leaks)
}
