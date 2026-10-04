// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// processBlock: realtime safety (no heap activity at all), bus routing,
// transport handling, MIDI out, degenerate calls and a long random fuzz.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "TestSupport.h"

using namespace plugintest;
using mw909::seq::StepLevel;

namespace {

// A busy pattern: every voice somewhere, flams, sub-steps, accents, shuffle.
mw909::seq::Pattern busyPattern() {
    mw909::seq::Pattern p;
    p.shuffle = 8;
    for (int v = 0; v < mw909::kNumVoices; ++v)
        for (int s = v % 4; s < 16; s += 2 + v % 3) p.at(0, v, s).setLevel(s % 5 == 0 ? StepLevel::Accent : StepLevel::Normal);
    p.at(0, mw909::SD, 4).setFlam(true);
    p.at(0, mw909::CH, 6).setSubSteps(4);
    p.at(0, mw909::seq::kAccentTrack, 0).setLevel(StepLevel::Normal);
    return p;
}

void fillWithGarbage(Host& host) {
    for (int ch = 0; ch < host.storage.getNumChannels(); ++ch)
        juce::FloatVectorOperations::fill(host.storage.getWritePointer(ch), 123.0f, host.storage.getNumSamples());
}

}  // namespace

TEST_CASE("processing: the allocation spy sees operator new, malloc and JUCE heap blocks", "[plugin][realtime]") {
    allocspy::reset();
    {
        const allocspy::Scope scope;
        auto boxed = std::make_unique<int>(3);
        REQUIRE(*boxed == 3);
    }
    CHECK(allocspy::allocations() == 1);
    CHECK(allocspy::deallocations() == 1);

    if (allocspy::wrapsMalloc()) {
        allocspy::reset();
        {
            const allocspy::Scope scope;
            juce::HeapBlock<float> block(256);       // std::malloc inside JUCE
            juce::AudioBuffer<float> buffer(2, 64);  // and the usual offender
            REQUIRE(block.get() != nullptr);
            REQUIRE(buffer.getNumSamples() == 64);
        }
        CHECK(allocspy::allocations() >= 2);
        CHECK(allocspy::deallocations() >= 2);
    }

    // Outside a scope nothing is counted.
    allocspy::reset();
    auto ignored = std::make_unique<int>(4);
    CHECK(*ignored == 4);
    CHECK(allocspy::allocations() == 0);
}

TEST_CASE("processing: processBlock never touches the heap", "[plugin][realtime]") {
    REQUIRE(allocspy::wrapsMalloc());  // on Linux the C allocator is watched too

    const std::vector<std::vector<int>> layouts = {
        {},                                                  // main only
        {1, 4, 9, 15},                                       // main + some individual outs
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, // all sixteen
    };
    const std::vector<int> blockSizes = {1, 2, 7, 64, 512, 513, 1024, 4096, 8192, 3, 256, 8192, 1};

    for (std::size_t li = 0; li < layouts.size(); ++li) {
        for (int preparedBlock : {512, 8192}) {
            auto p = makeProcessor(kRate, preparedBlock, false);
            juce::AudioProcessor::BusesLayout layout;
            for (int b = 0; b < mw909::kNumBuses; ++b) layout.outputBuses.add(juce::AudioChannelSet::disabled());
            layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
            for (int b : layouts[li]) layout.outputBuses.getReference(b) = juce::AudioChannelSet::stereo();
            REQUIRE(p->setBusesLayout(layout));

            Host host(*p, 8192);
            Rng rng(100U + static_cast<unsigned>(li));

            // Unprepared calls are part of the contract too.
            host.spy = true;
            allocspy::reset();
            host.process(64);

            p->prepareToPlay(kRate, preparedBlock);

            // Everything on: sequencer, recording, MIDI out, voices spread over the buses.
            p->patterns().setPattern(p->patterns().selectedPattern(), busyPattern());
            setParam(*p, "midi_out", 1.0f);
            setParam(*p, "record", 1.0f);
            for (int v = 0; v < mw909::kNumVoices; ++v)
                setParam(*p, juce::String(std::string(mw909::kVoiceInfo[static_cast<std::size_t>(v)].id)) + "_out",
                         static_cast<float>(v % mw909::kNumBuses));
            p->midiLearn().assign(7, "master_volume");
            p->midiLearn().beginLearn("sd_tone");
            // Every FX instance on, on both buses, with sends: the whole engine runs.
            for (int f = 0; f < mw909::kNumFxInstances; ++f)
                setParam(*p, juce::String(mw909::plugin::paramid::fx(f, "on")), 1.0f);
            for (int v = 0; v < mw909::kNumVoices; ++v) {
                setParam(*p, voiceParam(v, "send_bus"), static_cast<float>(1 + v % 2));
                setParam(*p, voiceParam(v, "send"), 0.5f);
                setParam(*p, voiceParam(v, "comp_on"), 1.0f);
                setParam(*p, voiceParam(v, "eq_on"), 1.0f);
            }
            setParam(*p, "trig1_on", 1.0f);
            setParam(*p, "mcomp_on", 1.0f);

            const auto& params = p->getParameters();
            allocspy::reset();

            for (int round = 0; round < 6; ++round) {
                for (int n : blockSizes) {
                    // --- what a host does between blocks (not counted) ---
                    host.midi.clear();
                    for (int e = 0; e < 6; ++e)
                        host.noteOn(mw909::kVoiceNote[static_cast<std::size_t>(randInt(rng, 0, mw909::kNumVoices - 1))],
                                    randInt(rng, 1, 127), randInt(rng, 0, std::max(0, n - 1)));
                    host.cc(7, randInt(rng, 0, 127), randInt(rng, 0, std::max(0, n - 1)));
                    host.cc(randInt(rng, 0, 119), randInt(rng, 0, 127), 0);
                    host.noteOff(60, 0);

                    // --- what the message thread does between blocks (not counted) ---
                    if (round % 2 == 0) p->patterns().setPattern(randInt(rng, 0, 95), randomPattern(rng));
                    if (n == 2) p->patterns().setSongTrack(randInt(rng, 0, 7), randomSongTrack(rng));
                    if (n == 3) {
                        // A user ROM handoff to adopt and retire.
                        Rng romRng(static_cast<std::uint32_t>(round));
                        p->patterns().setUserRom(static_cast<mw909::RomSlot>(round % 3), randomRom(romRng), "fuzz");
                    }
                    if (n == 513) p->patterns().replaceBank(p->patterns().bank());  // a bank handoff to adopt and retire
                    if (n == 7) p->audition(mw909::CR, true);
                    if (n == 64) p->requestFill();
                    if (n == 1024) p->setUnitSeed(static_cast<std::uint32_t>(rng()));
                    if (n == 4096) p->reset();
                    if (n == 256) p->serviceMessageThread();

                    // --- automation, delivered the way the VST3 wrapper does: on the audio thread ---
                    {
                        const allocspy::Scope audioThread;
                        for (int a = 0; a < 8; ++a)
                            params[randInt(rng, 0, params.size() - 1)]->setValueNotifyingHost(rand01(rng));
                    }
                    // Keep the transport pieces in a state that plays.
                    setParam(*p, "seq_mode", static_cast<float>(1 + round % 2));
                    setParam(*p, "seq_run", 1.0f);
                    setParam(*p, "midi_out", 1.0f);
                    setParam(*p, "record", 1.0f);

                    host.head.available = (round != 3);        // no play head for one round
                    host.head.playing = (round != 4);          // stopped for one round
                    if (round == 5 && n == 512) host.head.ppq = 0.0;  // a loop jump

                    host.process(n);  // counted
                    REQUIRE(host.allFinite(n));
                }
                // A zero-length block.
                host.midi.clear();
                host.process(0);
            }

            INFO("layout " << li << ", prepared block " << preparedBlock);
            CHECK(allocspy::allocations() == 0);
            CHECK(allocspy::deallocations() == 0);
            host.spy = false;
        }
    }
}

TEST_CASE("processing: every block size from 1 to 8192, without touching the heap", "[plugin][realtime]") {
    REQUIRE(allocspy::wrapsMalloc());
    auto p = makeProcessor(kRate, 8192, false);
    REQUIRE(setLayout(*p, {2, 7, 15}));
    p->prepareToPlay(kRate, 8192);
    Host host(*p, 8192);
    p->patterns().setPattern(p->patterns().selectedPattern(), busyPattern());
    setParam(*p, "midi_out", 1.0f);
    setParam(*p, "fx_reverb_on", 1.0f);
    setParam(*p, "fx_delay_on", 1.0f);
    setParam(*p, "sd_send_bus", 2.0f);
    setParam(*p, "sd_send", 0.7f);
    host.head.bpm = 160.0;
    host.run(4096, 512);

    // Every size once, in a shuffled order, the transport rolling throughout.
    std::vector<int> sizes(8192);
    for (int i = 0; i < 8192; ++i) sizes[static_cast<std::size_t>(i)] = i + 1;
    Rng rng(8192U);
    std::shuffle(sizes.begin(), sizes.end(), rng);
    host.spy = true;
    allocspy::reset();
    bool finite = true;
    float worst = 0.0f;
    for (const int n : sizes) {
        host.midi.clear();
        if (n % 3 == 0) host.noteOn(mw909::kVoiceNote[static_cast<std::size_t>(n % mw909::kNumVoices)], 100, n / 2);
        host.process(n);
        finite = finite && host.allFinite(n);
        worst = std::max(worst, host.peak(0, n));
    }
    host.spy = false;
    CHECK(finite);
    CHECK(worst > 0.0f);
    CHECK(worst < 50.0f);
    CHECK(allocspy::allocations() == 0);
    CHECK(allocspy::deallocations() == 0);
}

TEST_CASE("processing: voices are routed to the enabled buses", "[plugin][buses]") {
    SECTION("main only: an individual-out assignment falls back to the main mix") {
        auto p = makeProcessor();
        REQUIRE(p->getTotalNumOutputChannels() == 2);
        REQUIRE(p->getBusCount(false) == 16);
        REQUIRE(p->getBusCount(true) == 0);
        Host host(*p);
        host.head.playing = false;
        dryMix(*p);
        setParam(*p, "bd_out", 2.0f);  // Out 3, which is disabled
        host.noteOn(36, 120);
        host.process(2048);
        CHECK(host.peak(0, 2048) > 0.01f);
        CHECK(host.peak(1, 2048) > 0.01f);
    }

    SECTION("main plus two individual outs") {
        auto p = makeProcessor(kRate, kBlock, false);
        REQUIRE(setLayout(*p, {2, 5}));
        REQUIRE(p->getTotalNumOutputChannels() == 6);
        p->prepareToPlay(kRate, kBlock);
        Host host(*p);
        host.head.playing = false;
        dryMix(*p);
        setParam(*p, "bd_out", 2.0f);   // enabled: buffer channels 2-3
        setParam(*p, "sd_out", 5.0f);   // enabled: buffer channels 4-5
        setParam(*p, "rs_out", 7.0f);   // disabled: main

        fillWithGarbage(host);
        host.noteOn(36, 120);
        host.process(2048);
        host.midi.clear();
        CHECK(host.peak(0, 2048) == 0.0f);  // the bass drum left the main mix
        CHECK(host.peak(1, 2048) == 0.0f);
        CHECK(host.peak(2, 2048) > 0.01f);
        CHECK(host.peak(3, 2048) > 0.01f);
        CHECK(host.peak(4, 2048) == 0.0f);  // garbage overwritten with silence
        CHECK(host.peak(5, 2048) == 0.0f);

        p->panic();
        host.run(1024, 512);
        host.noteOn(38, 120);
        host.process(2048);
        host.midi.clear();
        CHECK(host.peak(0, 2048) == 0.0f);
        CHECK(host.peak(2, 2048) == 0.0f);
        CHECK(host.peak(4, 2048) > 0.01f);
        CHECK(host.peak(5, 2048) > 0.01f);

        p->panic();
        host.run(1024, 512);
        host.noteOn(mw909::kVoiceNote[mw909::RS], 120);
        host.process(2048);
        CHECK(host.peak(0, 2048) > 0.01f);  // routed to a disabled bus: main
        CHECK(host.peak(2, 2048) == 0.0f);
        CHECK(host.peak(4, 2048) == 0.0f);
    }

    SECTION("all sixteen buses") {
        auto p = makeProcessor(kRate, kBlock, false);
        REQUIRE(setLayoutAll(*p));
        REQUIRE(p->getTotalNumOutputChannels() == 32);
        p->prepareToPlay(kRate, kBlock);
        Host host(*p);
        host.head.playing = false;
        dryMix(*p);

        // Voice v on bus 15 - v: every voice on an individual out except none on main.
        const auto busOf = [](int v) { return mw909::kNumBuses - 1 - v; };
        for (int v = 0; v < mw909::kNumVoices; ++v)
            setParam(*p, juce::String(std::string(mw909::kVoiceInfo[static_cast<std::size_t>(v)].id)) + "_out",
                     static_cast<float>(busOf(v)));

        for (int v = 0; v < mw909::kNumVoices; ++v) {
            p->panic();
            host.run(1024, 512);
            fillWithGarbage(host);
            host.noteOn(mw909::kVoiceNote[static_cast<std::size_t>(v)], 127);
            host.process(4096);
            host.midi.clear();
            INFO("voice " << mw909::kVoiceInfo[static_cast<std::size_t>(v)].code);
            for (int b = 0; b < mw909::kNumBuses; ++b) {
                const float left = host.peak(2 * b, 4096);
                const float right = host.peak(2 * b + 1, 4096);
                if (b == busOf(v)) {
                    CHECK(left > 1.0e-3f);
                    CHECK(right > 1.0e-3f);
                } else {
                    CHECK(left == 0.0f);
                    CHECK(right == 0.0f);
                }
            }
        }
    }

    SECTION("layouts: stereo main, and every other bus stereo or off") {
        auto p = makeProcessor(kRate, kBlock, false);
        juce::AudioProcessor::BusesLayout layout;
        for (int b = 0; b < mw909::kNumBuses; ++b) layout.outputBuses.add(juce::AudioChannelSet::disabled());

        CHECK_FALSE(p->isBusesLayoutSupported(layout));  // main disabled
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
        CHECK_FALSE(p->isBusesLayoutSupported(layout));
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
        CHECK(p->isBusesLayoutSupported(layout));
        layout.outputBuses.getReference(3) = juce::AudioChannelSet::mono();
        CHECK_FALSE(p->isBusesLayoutSupported(layout));
        layout.outputBuses.getReference(3) = juce::AudioChannelSet::stereo();
        CHECK(p->isBusesLayoutSupported(layout));
        layout.inputBuses.add(juce::AudioChannelSet::stereo());
        CHECK_FALSE(p->isBusesLayoutSupported(layout));

        for (int b = 1; b < mw909::kNumBuses; ++b) {
            CHECK(p->getBus(false, b)->getName() == "Out " + juce::String(b + 1));
            CHECK_FALSE(p->getBus(false, b)->isEnabledByDefault());
        }
        CHECK(p->getBus(false, 0)->getName() == "Main");
        CHECK(p->getBus(false, 0)->isEnabledByDefault());
        CHECK(p->getTailLengthSeconds() == 10.0);
        CHECK(p->acceptsMidi());
        CHECK(p->producesMidi());
        CHECK_FALSE(p->isMidiEffect());
        CHECK(p->getName() == "mw909");
    }
}

TEST_CASE("processing: latency is the Machine's, reported in prepareToPlay only", "[plugin][latency]") {
    auto p = makeProcessor(kRate, kBlock, false);
    CHECK(p->getLatencySamples() == 0);  // not prepared yet
    for (const double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0, 22050.0}) {
        for (const int block : {32, 512, 4096}) {
            p->prepareToPlay(rate, block);
            INFO("rate " << rate << " block " << block);
            CHECK(p->getLatencySamples() == p->machineForTests().latencySamples());
            CHECK(p->getLatencySamples() >= 0);
        }
        if (rate == 44100.0 || rate == 48000.0) CHECK(p->getLatencySamples() == 32);
    }
    // Processing and parameter changes never move it.
    p->prepareToPlay(kRate, kBlock);
    const int latency = p->getLatencySamples();
    Host host(*p);
    for (const char* id : {"quality", "variance", "revision"}) {
        setParam(*p, id, 1.0f);
        host.run(4096, 512);
        CHECK(p->getLatencySamples() == latency);
    }
}

TEST_CASE("processing: every sample rate plays every voice", "[plugin][rates]") {
    for (const double rate : {22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        auto p = makeProcessor(rate, 1024, true);
        Host host(*p);
        host.sampleRate = rate;
        host.head.playing = false;
        dryMix(*p);
        for (int v = 0; v < mw909::kNumVoices; ++v) {
            p->panic();
            host.run(2048, 1024);
            host.midi.clear();
            host.noteOn(mw909::kVoiceNote[static_cast<std::size_t>(v)], 127);
            host.process(4096);
            INFO("rate " << rate << " voice " << mw909::kVoiceInfo[static_cast<std::size_t>(v)].code);
            CHECK(host.allFinite(4096));
            CHECK(host.peak(0, 4096) + host.peak(1, 4096) > 1.0e-3f);
            CHECK(host.peak(0, 4096) < 4.0f);
        }
    }
}

TEST_CASE("processing: transport handling", "[plugin][transport]") {
    auto p = makeProcessor();
    Host host(*p);
    p->patterns().setPattern(p->patterns().selectedPattern(), busyPattern());
    const auto total = [&p] {
        std::uint32_t n = 0;
        for (int v = 0; v < mw909::kNumVoices; ++v) n += hits(*p, static_cast<mw909::VoiceId>(v));
        return n;
    };

    SECTION("host playing: Host Sync follows it") {
        host.run(48000, 512);
        CHECK(total() > 20U);
        CHECK(p->playState().playing.load());
        CHECK(p->playState().tempo.load() == 120.0f);
        CHECK(p->playState().step.load() >= 0);

        // The host tempo is used, not the Tempo parameter.
        host.head.bpm = 90.0;
        setParam(*p, "tempo", 200.0f);
        host.run(4800, 512);
        CHECK(p->playState().tempo.load() == 90.0f);
    }

    SECTION("host stopped: silent, but MIDI still plays") {
        host.head.playing = false;
        setParam(*p, "seq_run", 1.0f);  // Run is an Internal-mode control: no effect here
        host.run(48000, 512);
        CHECK(total() == 0U);
        CHECK_FALSE(p->playState().playing.load());
        CHECK(p->playState().step.load() == -1);
        host.noteOn(36, 100);
        host.process(512);
        CHECK(hits(*p, mw909::BD) == 1U);
    }

    SECTION("host looping: position jumps are followed") {
        std::uint32_t last = 0;
        for (int loop = 0; loop < 6; ++loop) {
            host.head.ppq = 4.0;  // back to the loop start every two beats
            host.run(48000, 300);
            CHECK(total() > last);
            last = total();
            CHECK(host.allFinite(300));
        }
        host.head.ppq = -3.5;  // pre-roll before bar one
        host.run(48000, 512);
        CHECK(host.allFinite(512));
    }

    SECTION("no play head: Host Sync runs on the internal clock, started by Run") {
        p->setPlayHead(nullptr);
        host.run(24000, 512);
        CHECK(total() == 0U);  // Run is off
        setParam(*p, "seq_run", 1.0f);
        setParam(*p, "tempo", 150.0f);
        host.run(48000, 512);
        CHECK(total() > 20U);
        CHECK(p->playState().playing.load());
        CHECK(p->playState().tempo.load() == 150.0f);
        setParam(*p, "seq_run", 0.0f);
        host.run(4800, 512);
        CHECK_FALSE(p->playState().playing.load());
    }

    SECTION("a play head with no position, or no musical position, counts as none") {
        setParam(*p, "seq_run", 1.0f);
        host.head.available = false;
        host.run(24000, 512);
        const auto afterUnavailable = total();
        CHECK(afterUnavailable > 5U);

        host.head.available = true;
        host.head.providePpq = false;  // what the standalone's play head gives
        host.run(24000, 512);
        CHECK(total() > afterUnavailable);

        const auto before = total();
        host.head.providePpq = true;
        host.head.ppq = std::numeric_limits<double>::quiet_NaN();
        host.run(24000, 512);
        CHECK(total() > before);  // a broken position is treated as none
        CHECK(host.allFinite(512));
    }

    SECTION("a host position without a tempo uses the Tempo parameter") {
        host.head.provideBpm = false;
        setParam(*p, "tempo", 100.0f);
        host.run(9600, 512);
        CHECK(p->playState().playing.load());
        CHECK(p->playState().tempo.load() == 100.0f);

        host.head.provideBpm = true;
        host.head.bpm = -5.0;  // nonsense tempo: same fallback
        host.run(9600, 512);
        CHECK(p->playState().tempo.load() == 100.0f);
        CHECK(host.allFinite(512));
    }

    SECTION("Internal mode ignores the host transport; Off is silent") {
        setParam(*p, "seq_mode", 2.0f);
        host.head.playing = false;
        setParam(*p, "seq_run", 1.0f);
        host.run(48000, 512);
        CHECK(total() > 20U);

        setParam(*p, "seq_mode", 0.0f);
        host.head.playing = true;
        host.run(4800, 512);
        const auto before = total();
        host.run(48000, 512);
        CHECK(total() == before);
    }
}

TEST_CASE("processing: MIDI in and out", "[plugin][midi]") {
    auto p = makeProcessor();
    Host host(*p);

    SECTION("notes on any channel play the mapped voices; other messages are ignored") {
        host.head.playing = false;
        host.noteOn(mw909::kVoiceNote[mw909::SD], 100, 10, 1);
        host.noteOn(mw909::kVoiceNote[mw909::CP], 100, 20, 16);
        host.noteOff(mw909::kVoiceNote[mw909::SD], 30, 1);
        host.midi.addEvent(juce::MidiMessage::pitchWheel(1, 9000), 5);
        host.midi.addEvent(juce::MidiMessage::programChange(1, 3), 6);
        host.midi.addEvent(juce::MidiMessage::channelPressureChange(1, 50), 7);
        const juce::uint8 sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0xF7};
        host.midi.addEvent(juce::MidiMessage(sysex, 6), 8);
        host.midi.addEvent(juce::MidiMessage::allNotesOff(1), 9);
        host.process(512);
        CHECK(hits(*p, mw909::SD) == 1U);
        CHECK(hits(*p, mw909::CP) == 1U);
        CHECK(host.midi.isEmpty());  // consumed; nothing produced with MIDI Out off
    }

    SECTION("events outside the block are clamped into it") {
        host.head.playing = false;
        host.noteOn(36, 100, 100000);  // far past the block
        host.process(64);
        CHECK(hits(*p, mw909::BD) == 1U);
    }

    SECTION("a receive channel filters the voice notes") {
        host.head.playing = false;
        setParam(*p, "midi_channel", 3.0f);  // channel 3
        host.noteOn(mw909::kVoiceNote[mw909::SD], 100, 0, 1);
        host.noteOn(mw909::kVoiceNote[mw909::CP], 100, 0, 3);
        host.process(512);
        CHECK(hits(*p, mw909::SD) == 0U);
        CHECK(hits(*p, mw909::CP) == 1U);
    }

    SECTION("the bass channel plays the bass drum chromatically") {
        host.head.playing = false;
        setParam(*p, "bass_channel", 3.0f);
        host.noteOn(60, 100, 0, 3);  // not a drum note, but on the bass channel
        host.process(512);
        CHECK(hits(*p, mw909::BD) == 1U);
    }

    SECTION("MIDI Out sends the sequencer's hits as channel-10 notes") {
        mw909::seq::Pattern pattern;
        for (int s = 0; s < 16; s += 4) pattern.at(0, mw909::BD, s).setLevel(StepLevel::Normal);
        pattern.at(0, mw909::SD, 4).setLevel(StepLevel::Accent);
        auto bank = std::make_unique<mw909::seq::PatternBank>();
        bank->patterns[0] = pattern;
        p->patterns().replaceBank(*bank);
        p->patterns().selectPattern(0, true);
        setParam(*p, "midi_out", 1.0f);

        int bdOn = 0, sdOn = 0, offs = 0, other = 0, sdVelocity = 0;
        for (int done = 0; done < 96000; done += 500) {  // exactly one measure
            host.midi.clear();
            host.process(500);
            for (const auto meta : host.midi) {
                const auto m = meta.getMessage();
                CHECK(m.getChannel() == 10);
                CHECK(meta.samplePosition >= 0);
                CHECK(meta.samplePosition < 500);
                if (m.isNoteOn() && m.getNoteNumber() == mw909::kVoiceNote[mw909::BD]) ++bdOn;
                else if (m.isNoteOn() && m.getNoteNumber() == mw909::kVoiceNote[mw909::SD]) {
                    ++sdOn;
                    sdVelocity = m.getVelocity();
                } else if (m.isNoteOff()) ++offs;
                else ++other;
            }
        }
        CHECK(bdOn == 4);
        CHECK(sdOn == 1);
        CHECK(sdVelocity == mw909::kVelocityAccent);
        CHECK(offs == 5);
        CHECK(other == 0);

        // Another output channel.
        setParam(*p, "midi_out_channel", 4.0f);  // channel 5
        int onChannel5 = 0;
        for (int done = 0; done < 96000; done += 500) {
            host.midi.clear();
            host.process(500);
            for (const auto meta : host.midi) {
                const auto m = meta.getMessage();
                if (m.isNoteOn()) {
                    CHECK(m.getChannel() == 5);
                    ++onChannel5;
                }
            }
        }
        CHECK(onChannel5 == 5);

        // Off again: nothing comes out.
        setParam(*p, "midi_out", 0.0f);
        host.midi.clear();
        host.run(48000, 512);
        CHECK(host.midi.isEmpty());
    }

    SECTION("a flood of hits is capped, never dropping a note-off") {
        mw909::seq::Pattern pattern;
        pattern.scale = mw909::seq::Scale::ThirtySecond;
        pattern.length = 64;
        for (int v = 0; v < mw909::kNumVoices; ++v)
            for (int s = 0; s < 64; ++s) {
                pattern.at(0, v, s).setLevel(StepLevel::Normal);
                pattern.at(0, v, s).setSubSteps(4);
            }
        auto bank = std::make_unique<mw909::seq::PatternBank>();
        bank->patterns[0] = pattern;
        p->patterns().replaceBank(*bank);
        p->patterns().selectPattern(0, true);
        setParam(*p, "midi_out", 1.0f);
        host.head.bpm = 300.0;

        std::array<int, 128> sounding{};
        int maxEvents = 0;
        for (int i = 0; i < 12; ++i) {
            host.midi.clear();
            host.process(8192);
            maxEvents = std::max(maxEvents, host.midi.getNumEvents());
            for (const auto meta : host.midi) {
                const auto m = meta.getMessage();
                if (m.isNoteOn()) ++sounding[static_cast<std::size_t>(m.getNoteNumber())];
                if (m.isNoteOff()) --sounding[static_cast<std::size_t>(m.getNoteNumber())];
            }
        }
        // 2048 bytes at 9 bytes per event is what a wrapper's buffer holds.
        CHECK(maxEvents * 9 <= 2048);
        // Stop and let the last note-offs out: nothing hangs.
        host.head.playing = false;
        for (int i = 0; i < 4; ++i) {
            host.midi.clear();
            host.process(8192);
            for (const auto meta : host.midi) {
                const auto m = meta.getMessage();
                if (m.isNoteOn()) ++sounding[static_cast<std::size_t>(m.getNoteNumber())];
                if (m.isNoteOff()) --sounding[static_cast<std::size_t>(m.getNoteNumber())];
            }
        }
        for (int note = 0; note < 128; ++note) {
            INFO("note " << note);
            CHECK(sounding[static_cast<std::size_t>(note)] == 0);
        }
    }
}

TEST_CASE("processing: degenerate calls are safe", "[plugin][robustness]") {
    SECTION("before prepareToPlay: silence, MIDI consumed, nothing else") {
        auto p = makeProcessor(kRate, kBlock, false);
        Host host(*p);
        fillWithGarbage(host);
        host.noteOn(36, 100);
        host.process(512);
        CHECK(host.peak(0, 512) == 0.0f);
        CHECK(host.peak(1, 512) == 0.0f);
        CHECK(host.midi.isEmpty());
        CHECK(hits(*p, mw909::BD) == 0U);
    }

    SECTION("zero samples") {
        auto p = makeProcessor();
        Host host(*p);
        host.noteOn(36, 100);
        host.process(0);
        CHECK(host.midi.isEmpty());
        host.process(0);
        host.process(512);
        CHECK(host.allFinite(512));

        juce::AudioBuffer<float> empty;  // no channels, no samples
        juce::MidiBuffer midi;
        p->processBlock(empty, midi);
        SUCCEED();
    }

    SECTION("more samples than prepared for") {
        auto p = makeProcessor(kRate, 64);
        Host host(*p, 16384);
        p->patterns().setPattern(p->patterns().selectedPattern(), busyPattern());
        fillWithGarbage(host);
        host.process(16384);
        CHECK(host.allFinite(16384));
        CHECK(host.peak(0, 16384) > 0.01f);
        CHECK(host.peak(0, 16384) < 50.0f);  // the garbage is gone
        // Hits land across the whole block, not just the first 64 samples.
        const float* left = host.storage.getReadPointer(0);
        float tailPeak = 0.0f;
        for (int i = 12000; i < 16384; ++i) tailPeak = std::max(tailPeak, std::abs(left[i]));
        CHECK(tailPeak > 1.0e-4f);

        // The same audio as a run in prepared-size blocks.
        auto q = makeProcessor(kRate, 64);
        Host reference(*q, 16384);
        q->patterns().setPattern(q->patterns().selectedPattern(), busyPattern());
        std::vector<float> expected;
        for (int done = 0; done < 16384; done += 64) {
            reference.process(64);
            expected.insert(expected.end(), reference.storage.getReadPointer(0), reference.storage.getReadPointer(0) + 64);
        }
        float worst = 0.0f;
        for (int i = 0; i < 16384; ++i) worst = std::max(worst, std::abs(expected[static_cast<std::size_t>(i)] - left[i]));
        CHECK(worst < 1.0e-4f);
    }

    SECTION("buffers with too few channels") {
        auto p = makeProcessor();
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> mono(1, 256);
        juce::FloatVectorOperations::fill(mono.getWritePointer(0), 9.0f, 256);
        midi.addEvent(juce::MidiMessage::noteOn(10, 36, juce::uint8{100}), 0);
        p->processBlock(mono, midi);
        CHECK(mono.getMagnitude(0, 0, 256) == 0.0f);

        // More channels than the layout has: the extra ones are cleared.
        juce::AudioBuffer<float> wide(6, 256);
        for (int ch = 0; ch < 6; ++ch) juce::FloatVectorOperations::fill(wide.getWritePointer(ch), 9.0f, 256);
        midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(10, 36, juce::uint8{100}), 0);
        p->processBlock(wide, midi);
        CHECK(wide.getMagnitude(0, 0, 256) > 0.0f);
        CHECK(wide.getMagnitude(0, 0, 256) < 9.0f);
        for (int ch = 2; ch < 6; ++ch) CHECK(wide.getMagnitude(ch, 0, 256) == 0.0f);
    }

    SECTION("sample-rate and block-size changes, odd prepare arguments") {
        auto p = makeProcessor(kRate, kBlock, false);
        Host host(*p, 8192);
        p->patterns().setPattern(p->patterns().selectedPattern(), busyPattern());
        for (double rate : {44100.0, 96000.0, 22050.0, 192000.0, 48000.0, 8000.0}) {
            for (int block : {1, 64, 1024, 8192}) {
                p->prepareToPlay(rate, block);
                host.sampleRate = rate;
                host.run(3 * block + 100, block);
                CHECK(host.allFinite(block));
            }
        }
        p->prepareToPlay(0.0, 0);
        host.process(256);
        CHECK(host.allFinite(256));
        p->prepareToPlay(-48000.0, -64);
        host.process(256);
        CHECK(host.allFinite(256));
        p->prepareToPlay(std::numeric_limits<double>::quiet_NaN(), 16);
        host.process(256);
        CHECK(host.allFinite(256));

        // release / prepare in any order, and processing after release.
        p->releaseResources();
        p->releaseResources();
        host.process(256);
        CHECK(host.allFinite(256));
        p->prepareToPlay(48000.0, 512);
        p->prepareToPlay(48000.0, 512);
        host.sampleRate = 48000.0;
        host.run(48000, 512);
        CHECK(host.allFinite(512));
        CHECK(host.peak(0, 512) < 20.0f);
    }

    SECTION("reset() silences the voices at the next block") {
        auto p = makeProcessor();
        Host host(*p);
        host.head.playing = false;
        host.noteOn(mw909::kVoiceNote[mw909::CR], 127);
        host.process(512);
        host.midi.clear();
        host.process(512);
        REQUIRE(host.peak(0, 512) > 0.0f);
        p->reset();
        host.process(512);
        CHECK(host.peak(0, 512) == 0.0f);
    }

    SECTION("prepareToPlay racing processBlock") {
        auto p = makeProcessor();
        std::atomic<bool> stop{false};
        std::atomic<bool> finite{true};
        std::atomic<int> blocks{0};
        std::thread audio([&] {
            juce::AudioBuffer<float> buffer(2, 256);
            juce::MidiBuffer midi;
            midi.ensureSize(2048);
            while (!stop.load()) {
                midi.clear();
                midi.addEvent(juce::MidiMessage::noteOn(10, 36, juce::uint8{100}), 0);
                p->processBlock(buffer, midi);
                for (int i = 0; i < 256; ++i)
                    if (!std::isfinite(buffer.getSample(0, i))) finite.store(false);
                blocks.fetch_add(1);
            }
        });
        for (int i = 0; i < 300; ++i) {
            p->prepareToPlay(i % 2 == 0 ? 44100.0 : 96000.0, 16 + (i * 37) % 2000);
            if (i % 10 == 0) p->reset();
            if (i % 25 == 0) p->releaseResources();
        }
        while (blocks.load() < 50) std::this_thread::yield();
        stop.store(true);
        audio.join();
        CHECK(finite.load());
    }
}

TEST_CASE("processing: 10000 random blocks with random automation and MIDI stay finite", "[plugin][fuzz]") {
    auto p = makeProcessor(kRate, 512, false);
    REQUIRE(setLayout(*p, {1, 3, 6, 11, 15}));
    p->prepareToPlay(kRate, 512);
    Host host(*p, 8192);
    Rng rng(4242U);
    const auto& params = p->getParameters();

    float worstPeak = 0.0f;
    bool finite = true;
    for (int block = 0; block < 10000 && finite; ++block) {
        // Mostly small blocks, now and then anything up to 8192.
        const int n = randInt(rng, 0, 9) == 0 ? randInt(rng, 1, 8192) : randInt(rng, 1, 512);

        host.midi.clear();
        const int events = randInt(rng, 0, 8);
        for (int e = 0; e < events; ++e) {
            const int kind = randInt(rng, 0, 9);
            const int offset = randInt(rng, 0, n - 1);
            if (kind < 6) host.noteOn(randInt(rng, 0, 127), randInt(rng, 1, 127), offset, randInt(rng, 1, 16));
            else if (kind < 8) host.noteOff(randInt(rng, 0, 127), offset, randInt(rng, 1, 16));
            else host.cc(randInt(rng, 0, 127), randInt(rng, 0, 127), offset, randInt(rng, 1, 16));
        }

        const int changes = randInt(rng, 0, 6);
        for (int c = 0; c < changes; ++c) params[randInt(rng, 0, params.size() - 1)]->setValueNotifyingHost(rand01(rng));

        // Transport: play, stop, jump, vanish, change tempo.
        switch (randInt(rng, 0, 60)) {
            case 0: host.head.playing = !host.head.playing; break;
            case 1: host.head.ppq = static_cast<double>(randInt(rng, -8, 400)) + static_cast<double>(rand01(rng)); break;
            case 2: host.head.available = !host.head.available; break;
            case 3: host.head.bpm = 20.0 + 380.0 * static_cast<double>(rand01(rng)); break;
            case 4: host.head.providePpq = !host.head.providePpq; break;
            case 5: host.head.bpm = static_cast<double>(randInt(rng, -1, 1)) * 1.0e9; break;
            default: break;
        }

        // The message thread is busy too.
        switch (randInt(rng, 0, 40)) {
            case 0: p->patterns().setPattern(randInt(rng, 0, 95), randomPattern(rng)); break;
            case 1: p->patterns().selectPattern(randInt(rng, 0, 95), randInt(rng, 0, 1) == 1); break;
            case 2: p->patterns().setChain(randomChain(rng)); break;
            case 3: p->audition(static_cast<mw909::VoiceId>(randInt(rng, 0, 10)), randInt(rng, 0, 1) == 1); break;
            case 11: p->patterns().setSongTrack(randInt(rng, 0, 7), randomSongTrack(rng)); break;
            case 12: if (randInt(rng, 0, 10) == 0) p->patterns().setUserRom(static_cast<mw909::RomSlot>(randInt(rng, 0, 2)), randomRom(rng), "x"); break;
            case 4: p->requestFill(); break;
            case 5: p->setUnitSeed(static_cast<std::uint32_t>(rng())); break;
            case 6: p->serviceMessageThread(); break;
            case 7: p->midiLearn().assign(randInt(rng, 0, 119), "master_volume"); break;
            case 8: p->reset(); break;
            case 9: if (randInt(rng, 0, 20) == 0) p->panic(); break;
            case 10: if (randInt(rng, 0, 10) == 0) loadState(*p, saveState(*p)); break;
            default: break;
        }

        host.process(n);
        finite = host.allFinite(n);
        for (int ch = 0; ch < host.storage.getNumChannels(); ++ch) worstPeak = std::max(worstPeak, host.peak(ch, n));
    }
    CHECK(finite);
    INFO("largest sample magnitude seen: " << worstPeak);
    CHECK(worstPeak > 0.0f);
    CHECK(worstPeak < 100.0f);
}

namespace {

// Every voice on every 32nd step with four sub-steps: far more hits than the
// MIDI-out cap lets through.
void loadFloodPattern(mw909Processor& p) {
    mw909::seq::Pattern pattern;
    pattern.scale = mw909::seq::Scale::ThirtySecond;
    pattern.length = 64;
    for (int v = 0; v < mw909::kNumVoices; ++v)
        for (int s = 0; s < 64; ++s) {
            pattern.at(0, v, s).setLevel(StepLevel::Normal);
            pattern.at(0, v, s).setSubSteps(4);
        }
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    bank->patterns[0] = pattern;
    p.patterns().replaceBank(*bank);
    p.patterns().selectPattern(0, true);
    setParam(p, "midi_out", 1.0f);
}

}  // namespace

TEST_CASE("processing: MIDI out into a host buffer nobody reserved never allocates", "[plugin][realtime][midi]") {
    // The Standalone's AudioProcessorPlayer hands over a MidiBuffer it never
    // reserved. Growing it event by event would allocate on the audio thread
    // again and again; the shell reserves the worst case in prepareToPlay and
    // hands that storage to the host's buffer.
    auto p = makeProcessor(kRate, 8192, true);
    loadFloodPattern(*p);
    FakePlayHead head;
    head.bpm = 300.0;
    p->setPlayHead(&head);
    juce::AudioBuffer<float> buffer(2, 8192);
    juce::MidiBuffer midi;  // capacity 0, as the Standalone's
    REQUIRE(allocspy::wrapsMalloc());
    allocspy::reset();
    int maxEvents = 0;
    for (int i = 0; i < 40; ++i) {
        midi.clear();
        {
            const allocspy::Scope audioThread;
            p->processBlock(buffer, midi);
        }
        maxEvents = std::max(maxEvents, midi.getNumEvents());
        head.ppq += 8192.0 * head.bpm / (60.0 * kRate);
    }
    p->setPlayHead(nullptr);
    CHECK(maxEvents > 100);
    CHECK(allocspy::allocations() == 0);
    CHECK(allocspy::deallocations() == 0);

    // A second unreserved buffer (a host that alternates) still works; it may
    // allocate once, but never fails or drops a note-off.
    juce::MidiBuffer other;
    for (int i = 0; i < 4; ++i) {
        other.clear();
        p->processBlock(buffer, other);
    }
    SUCCEED();
}

TEST_CASE("processing: bypass releases the notes MIDI out left sounding", "[plugin][midi]") {
    auto p = makeProcessor(kRate, kBlock, true);
    Host host(*p);
    loadFloodPattern(*p);
    host.head.bpm = 300.0;

    std::array<int, 128> sounding{};
    const auto count = [&] {
        for (const auto meta : host.midi) {
            const auto m = meta.getMessage();
            if (m.isNoteOn()) ++sounding[static_cast<std::size_t>(m.getNoteNumber())];
            if (m.isNoteOff()) --sounding[static_cast<std::size_t>(m.getNoteNumber())];
        }
    };
    host.midi.clear();
    host.process(300);  // a block ends with notes still sounding
    count();
    int open = 0;
    for (int n : sounding) open += n;
    REQUIRE(open > 0);

    // Bypassed: silence, and every open note closed in the first bypassed block.
    host.midi.clear();
    juce::AudioBuffer<float> view(host.storage.getArrayOfWritePointers(), 2, 256);
    p->processBlockBypassed(view, host.midi);
    count();
    for (int note = 0; note < 128; ++note) {
        INFO("note " << note);
        CHECK(sounding[static_cast<std::size_t>(note)] == 0);
    }
    CHECK(host.peak(0, 256) == 0.0f);

    // Back from bypass the stream stays balanced: no stray note-offs.
    for (int i = 0; i < 20; ++i) {
        host.midi.clear();
        host.process(512);
        count();
        for (int n : sounding) REQUIRE(n >= 0);
    }
}

namespace {

// Open notes per MIDI channel (1..16) and note: note-ons minus note-offs.
struct ChannelNotes {
    std::array<std::array<int, 128>, 17> open{};
    void add(const juce::MidiBuffer& midi) {
        for (const auto meta : midi) {
            const auto m = meta.getMessage();
            const auto ch = static_cast<std::size_t>(m.getChannel());
            const auto note = static_cast<std::size_t>(m.getNoteNumber());
            if (m.isNoteOn()) ++open[ch][note];
            else if (m.isNoteOff()) --open[ch][note];
        }
    }
    int openOn(int channel) const {
        int n = 0;
        for (const int x : open[static_cast<std::size_t>(channel)]) n += x;
        return n;
    }
    // Every channel: nothing open, and no note-off without a note-on.
    std::string unbalanced() const {
        std::string s;
        for (int ch = 1; ch <= 16; ++ch)
            for (int n = 0; n < 128; ++n)
                if (open[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)] != 0)
                    s += "ch" + std::to_string(ch) + " note " + std::to_string(n) + ": " +
                         std::to_string(open[static_cast<std::size_t>(ch)][static_cast<std::size_t>(n)]) + "; ";
        return s;
    }
};

}  // namespace

TEST_CASE("processing: MIDI out notes close on their own channel after a re-prepare and in bypass",
          "[plugin][midi]") {
    // midi_out_channel is a choice 0..15 for channels 1..16.
    for (const int channel : {1, 5, 16}) {
        DYNAMIC_SECTION("re-prepare, channel " << channel) {
            auto p = makeProcessor(kRate, kBlock, true);
            Host host(*p);
            loadFloodPattern(*p);
            setParam(*p, "midi_out_channel", static_cast<float>(channel - 1));
            host.head.bpm = 300.0;
            ChannelNotes notes;
            host.midi.clear();
            host.process(300);  // ends with notes open
            notes.add(host.midi);
            REQUIRE(notes.openOn(channel) > 0);

            // The host changes the buffer size while notes sound.
            p->prepareToPlay(kRate, 256);
            for (int i = 0; i < 4; ++i) {
                host.midi.clear();
                host.process(256);
                notes.add(host.midi);
                for (int ch = 1; ch <= 16; ++ch)
                    for (const int n : notes.open[static_cast<std::size_t>(ch)]) REQUIRE(n >= 0);
            }
            host.head.playing = false;
            for (int i = 0; i < 40; ++i) {
                host.midi.clear();
                host.process(256);
                notes.add(host.midi);
            }
            INFO(notes.unbalanced());
            CHECK(notes.unbalanced().empty());
        }
        DYNAMIC_SECTION("bypass, channel " << channel) {
            auto p = makeProcessor(kRate, kBlock, true);
            Host host(*p);
            loadFloodPattern(*p);
            setParam(*p, "midi_out_channel", static_cast<float>(channel - 1));
            host.head.bpm = 300.0;
            ChannelNotes notes;
            host.midi.clear();
            host.process(300);
            notes.add(host.midi);
            REQUIRE(notes.openOn(channel) > 0);
            host.midi.clear();
            juce::AudioBuffer<float> view(host.storage.getArrayOfWritePointers(), 2, 256);
            p->processBlockBypassed(view, host.midi);
            notes.add(host.midi);
            INFO(notes.unbalanced());
            CHECK(notes.unbalanced().empty());
            // Back from bypass: balanced, and no note-off nobody asked for.
            for (int i = 0; i < 20; ++i) {
                host.midi.clear();
                host.process(512);
                notes.add(host.midi);
                for (int ch = 1; ch <= 16; ++ch)
                    for (const int n : notes.open[static_cast<std::size_t>(ch)]) REQUIRE(n >= 0);
            }
        }
    }

    SECTION("notes open on two channels at once") {
        auto p = makeProcessor(kRate, kBlock, true);
        Host host(*p);
        loadFloodPattern(*p);
        setParam(*p, "midi_out_channel", 2.0f);  // channel 3
        host.head.bpm = 300.0;
        ChannelNotes notes;
        host.midi.clear();
        host.process(300);
        notes.add(host.midi);
        REQUIRE(notes.openOn(3) > 0);
        setParam(*p, "midi_out_channel", 11.0f);  // channel 12: new notes there, old ones still open on 3
        host.midi.clear();
        host.process(37);
        notes.add(host.midi);
        host.midi.clear();
        juce::AudioBuffer<float> view(host.storage.getArrayOfWritePointers(), 2, 256);
        p->processBlockBypassed(view, host.midi);
        notes.add(host.midi);
        INFO(notes.unbalanced());
        CHECK(notes.unbalanced().empty());
    }
}

TEST_CASE("processing: notes in a zero-length block play with the next block", "[plugin][midi]") {
    // The VST3 wrapper calls processBlock with no samples when the host sends
    // events without audio.
    auto p = makeProcessor();
    Host host(*p);
    host.head.playing = false;
    host.noteOn(mw909::kVoiceNote[mw909::SD], 100, 0);
    host.noteOn(mw909::kVoiceNote[mw909::BD], 100, 0);
    host.process(0);
    CHECK(hits(*p, mw909::SD) == 0U);
    host.process(0);  // a second empty block keeps them too
    host.midi.clear();
    host.noteOn(mw909::kVoiceNote[mw909::CP], 100, 10);
    host.process(512);
    CHECK(hits(*p, mw909::SD) == 1U);
    CHECK(hits(*p, mw909::BD) == 1U);
    CHECK(hits(*p, mw909::CP) == 1U);
    host.midi.clear();
    host.process(512);  // played once only
    CHECK(hits(*p, mw909::SD) == 1U);
    CHECK(hits(*p, mw909::BD) == 1U);

    // A re-prepare forgets notes still waiting.
    host.noteOn(mw909::kVoiceNote[mw909::SD], 100, 0);
    host.process(0);
    p->prepareToPlay(kRate, kBlock);
    host.midi.clear();
    host.process(512);
    CHECK(hits(*p, mw909::SD) == 1U);
}
