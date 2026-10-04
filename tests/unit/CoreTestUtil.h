// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Helpers for the Engine / Machine tests: owned bus storage, parameter sets
// with the plugin's defaults, block-wise rendering and pattern builders.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "mw909/Machine.h"

namespace coretest {

// Stereo storage for every bus, with a BusBuffers view onto it.
struct Buses {
    explicit Buses(int numSamples, int connected = mw909::kNumBuses) : length(numSamples) {
        for (int b = 0; b < mw909::kNumBuses; ++b)
            for (int c = 0; c < 2; ++c)
                data[b][c].assign(static_cast<std::size_t>(b < connected ? numSamples : 0), 0.0f);
    }

    // View starting `offset` samples in.
    mw909::BusBuffers view(int offset = 0) {
        mw909::BusBuffers v;
        for (int b = 0; b < mw909::kNumBuses; ++b)
            for (int c = 0; c < 2; ++c)
                v.ch[static_cast<std::size_t>(b)][static_cast<std::size_t>(c)] =
                    data[b][c].empty() ? nullptr : data[b][c].data() + offset;
        return v;
    }

    std::vector<float>& left(int bus = 0) { return data[bus][0]; }
    std::vector<float>& right(int bus = 0) { return data[bus][1]; }
    bool connected(int bus) const { return !data[bus][0].empty(); }

    int length;
    std::vector<float> data[mw909::kNumBuses][2];
};

inline float peak(const std::vector<float>& x, int from = 0, int to = -1) {
    const int end = to < 0 ? static_cast<int>(x.size()) : to;
    float p = 0.0f;
    for (int i = from; i < end; ++i) p = std::max(p, std::abs(x[static_cast<std::size_t>(i)]));
    return p;
}

inline double energy(const std::vector<float>& x, int from = 0, int to = -1) {
    const int end = to < 0 ? static_cast<int>(x.size()) : to;
    double e = 0.0;
    for (int i = from; i < end; ++i) e += static_cast<double>(x[static_cast<std::size_t>(i)]) * x[static_cast<std::size_t>(i)];
    return e;
}

inline bool allFinite(const std::vector<float>& x) {
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

// Bit-identical buffers (kept out of REQUIRE so a failure does not print them).
inline bool same(const std::vector<float>& a, const std::vector<float>& b) { return a == b; }

// First sample whose magnitude exceeds `threshold`, or -1.
inline int firstAbove(const std::vector<float>& x, float threshold, int from = 0) {
    for (int i = from; i < static_cast<int>(x.size()); ++i)
        if (std::abs(x[static_cast<std::size_t>(i)]) > threshold) return i;
    return -1;
}

// The plugin's defaults (plugin.md §2): every knob at its default, channels at
// unity, the sequencer on its internal clock but stopped.
inline mw909::EngineParams defaultEngineParams() {
    mw909::EngineParams p;
    for (int v = 0; v < mw909::kNumVoices; ++v) p.voice[static_cast<std::size_t>(v)] = mw909::defaultParams(v);
    return p;
}

inline mw909::MachineParams defaultMachineParams() {
    mw909::MachineParams p;
    p.engine = defaultEngineParams();
    p.seq.mode = mw909::seq::Mode::Internal;
    p.seq.run = false;
    return p;
}

// A pattern with the given voices on the given steps of variation A.
inline mw909::seq::Pattern patternWith(std::initializer_list<mw909::VoiceId> voices, std::initializer_list<int> steps,
                                       mw909::seq::StepLevel level = mw909::seq::StepLevel::Normal) {
    mw909::seq::Pattern p;
    for (auto v : voices)
        for (int s : steps) p.at(0, v, s).setLevel(level);
    return p;
}

// Every voice on every step, with accents, total accents, flams and sub-steps
// sprinkled in: the CPU and partition stress pattern.
inline mw909::seq::Pattern densePattern() {
    using namespace mw909;
    seq::Pattern p;
    p.length = 16;
    p.shuffle = 4;
    for (int v = 0; v < kNumVoices; ++v)
        for (int s = 0; s < 16; ++s) {
            auto& c = p.at(0, v, s);
            c.setLevel(((s + v) % 4 == 0) ? seq::StepLevel::Accent
                                          : (((s + v) % 5 == 0) ? seq::StepLevel::Weak : seq::StepLevel::Normal));
            if (v < CH && (s % 8) == 3) c.setFlam(true);
            if ((s % 4) == 2) c.setSubSteps(2);
        }
    for (int s = 0; s < 16; s += 4) p.at(0, seq::kAccentTrack, s).setLevel(seq::StepLevel::Normal);
    return p;
}

// Drive a Machine over `total` samples in blocks of `blockSize` (or the given
// partition), with a rolling host transport and optional MIDI (absolute
// sample offsets, sorted).
struct Runner {
    mw909::Machine& machine;
    mw909::MachineParams params = defaultMachineParams();
    double sampleRate = 48000.0;
    double bpm = 120.0;
    bool hostValid = true;
    bool playing = true;
    double startPpq = 0.0;
    std::vector<mw909::MidiMessage> midi{};      // absolute offsets
    std::vector<mw909::MidiMessage> midiOut{};   // collected, absolute offsets

    void block(Buses& buses, int pos, int n) {
        blockMidi.clear();
        for (const auto& m : midi)
            if (m.offset >= pos && m.offset < pos + n) {
                auto local = m;
                local.offset -= pos;
                blockMidi.push_back(local);
            }
        mw909::Machine::ProcessArgs args;
        args.transport.hostValid = hostValid;
        args.transport.playing = playing;
        args.transport.bpm = bpm;
        args.transport.ppq = startPpq + static_cast<double>(pos) * bpm / (60.0 * sampleRate);
        args.midiIn = blockMidi.data();
        args.numMidiIn = static_cast<int>(blockMidi.size());
        args.midiOut = &out;
        args.out = buses.view(pos);
        args.numSamples = n;
        machine.process(args, params);
        for (int i = 0; i < out.count; ++i) {
            auto m = out.messages[static_cast<std::size_t>(i)];
            m.offset += pos;
            midiOut.push_back(m);
        }
    }

    void run(Buses& buses, int total, int blockSize, int startSample = 0) {
        for (int pos = startSample; pos < startSample + total;) {
            const int n = std::min(blockSize, startSample + total - pos);
            block(buses, pos, n);
            pos += n;
        }
    }

    // Blocks of the listed sizes, cycled.
    void runPartition(Buses& buses, int total, const std::vector<int>& sizes, int startSample = 0) {
        std::size_t k = 0;
        for (int pos = startSample; pos < startSample + total;) {
            const int n = std::min(sizes[k++ % sizes.size()], startSample + total - pos);
            block(buses, pos, n);
            pos += n;
        }
    }

    std::vector<mw909::MidiMessage> blockMidi{};
    mw909::MidiOutBuffer out{};
};

inline mw909::MidiMessage noteOn(int offset, int note, int velocity = 96, int channel = 10) {
    return {offset, static_cast<std::uint8_t>(0x90 | (channel - 1)), static_cast<std::uint8_t>(note),
            static_cast<std::uint8_t>(velocity)};
}
inline mw909::MidiMessage noteOff(int offset, int note, int channel = 10) {
    return {offset, static_cast<std::uint8_t>(0x80 | (channel - 1)), static_cast<std::uint8_t>(note), 0};
}

inline void postPattern(mw909::Machine& m, int index, const mw909::seq::Pattern& p, std::uint32_t generation = 0) {
    mw909::Command c;
    c.type = mw909::Command::Type::SetPattern;
    c.index = index;
    c.pattern = p;
    c.generation = generation;
    m.post(c);
}

// Render an Engine directly into `buses`, triggering (voice, hit) pairs at
// absolute sample positions, in blocks of `block` samples.
struct EngineHit {
    int at = 0;
    mw909::VoiceId voice = mw909::BD;
    mw909::Hit hit = mw909::Hit::normal();
};

inline void renderEngine(mw909::Engine& e, Buses& buses, std::vector<EngineHit> hits, int block) {
    std::sort(hits.begin(), hits.end(), [](const EngineHit& a, const EngineHit& b) { return a.at < b.at; });
    std::size_t h = 0;
    const int total = buses.length;
    for (int pos = 0; pos < total;) {
        int n = std::min(block, total - pos);
        while (h < hits.size() && hits[h].at <= pos) {
            e.trigger(hits[h].voice, hits[h].hit);
            ++h;
        }
        if (h < hits.size() && hits[h].at < pos + n) n = hits[h].at - pos;
        e.render(buses.view(), pos, n);
        pos += n;
    }
}

}  // namespace coretest
