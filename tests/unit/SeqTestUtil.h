// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Shared helpers for the sequencer tests: a scripted host transport, block
// partitions, random patterns and an event record that compares exactly.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <ostream>
#include <random>
#include <vector>

#include "mw909/seq/Sequencer.h"

namespace seqtest {

using namespace mw909;
using namespace mw909::seq;

// One hit at an absolute sample position. `volts` is the accent-line
// voltage (Event::hit.acc); `level` and `ta` the classes for MIDI out.
struct Ev {
    std::int64_t sample = 0;
    int voice = 0;
    float volts = 0.0f;
    StepLevel level = StepLevel::Off;
    bool ta = false;
    bool operator==(const Ev&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const Ev& e) {
    return os << "{" << e.sample << " v" << e.voice << " " << e.volts << "V L" << static_cast<int>(e.level)
              << (e.ta ? " TA" : "") << "}";
}

// Accent-line volts of a DAC code, and of the standard step classes.
inline float voltsOf(int code) { return AccentDac::volts(code); }
inline constexpr float kWeakV = AccentDac::volts(AccentDac::kWeakCode);
inline constexpr float kNormalV = AccentDac::volts(AccentDac::kNormalCode);
inline constexpr float kAccentV = AccentDac::volts(AccentDac::kNormalCode + AccentDac::kVoiceAccentCodes);
// Codes the TOTAL ACCENT row adds at knob k (same rounding as Hit::accented).
inline int totalCodes(float knob) {
    return static_cast<int>(static_cast<float>(AccentDac::kTotalAccentCodes) * knob + 0.5f);
}

// The sequencer is large (the 96-pattern bank); keep it off the stack.
inline std::unique_ptr<Sequencer> makeSeq(double sampleRate) {
    auto s = std::make_unique<Sequencer>();
    s->prepare(sampleRate);
    return s;
}

inline Settings hostSettings() {
    Settings st;
    st.mode = Mode::HostSync;
    return st;
}

inline Settings internalSettings(double bpm) {
    Settings st;
    st.mode = Mode::Internal;
    st.run = true;
    st.internalBpm = bpm;
    return st;
}

// Checks every block's output contract; appends events with absolute sample positions.
inline void collect(const EventBuffer& buf, int numSamples, std::int64_t blockStart, std::vector<Ev>& out,
                    int offsetBase = 0) {
    int prev = offsetBase;
    for (int i = 0; i < buf.count; ++i) {
        const Event& e = buf.events[static_cast<std::size_t>(i)];
        REQUIRE(e.offset >= offsetBase);
        REQUIRE(e.offset < offsetBase + numSamples);
        REQUIRE(e.offset >= prev);
        prev = e.offset;
        REQUIRE(static_cast<int>(e.voice) < kNumVoices);
        REQUIRE(std::isfinite(e.hit.acc));
        REQUIRE(e.hit.acc >= 0.0f);
        REQUIRE(e.hit.acc <= AccentDac::kVolts);
        REQUIRE(e.hit.pitchSemis == 0.0f);
        REQUIRE(e.level != StepLevel::Off);
        out.push_back({blockStart + (e.offset - offsetBase), static_cast<int>(e.voice), e.hit.acc, e.level,
                       e.totalAccent});
    }
}

// Host transport rolling from ppqStart at a constant tempo, delivered in the
// given blocks. Block PPQ is computed like a host does: start + samples / spp.
// `overflowed` is set if any block filled the event buffer.
inline std::vector<Ev> runHost(Sequencer& s, const Settings& st, double sampleRate, double bpm, double ppqStart,
                               const std::vector<int>& blocks, bool* overflowed = nullptr, double ppqNoise = 0.0,
                               std::uint32_t noiseSeed = 1) {
    std::vector<Ev> evs;
    std::int64_t pos = 0;
    const double spp = sampleRate * 60.0 / bpm;
    auto buf = std::make_unique<EventBuffer>();
    std::mt19937 rng(noiseSeed);
    std::uniform_real_distribution<double> noise(-ppqNoise, ppqNoise);
    for (int n : blocks) {
        Transport t;
        t.hostValid = true;
        t.playing = true;
        t.bpm = bpm;
        t.ppq = ppqStart + static_cast<double>(pos) / spp + (ppqNoise > 0.0 ? noise(rng) : 0.0);
        buf->clear();
        s.process(t, st, n, *buf);
        if (overflowed != nullptr && buf->count >= EventBuffer::kCapacity) *overflowed = true;
        collect(*buf, n, pos, evs);
        pos += n;
    }
    return evs;
}

// Internal clock (Settings::run) delivered in the given blocks.
inline std::vector<Ev> runInternal(Sequencer& s, const Settings& st, const std::vector<int>& blocks,
                                   bool* overflowed = nullptr) {
    std::vector<Ev> evs;
    std::int64_t pos = 0;
    auto buf = std::make_unique<EventBuffer>();
    Transport t;  // no host
    for (int n : blocks) {
        buf->clear();
        s.process(t, st, n, *buf);
        if (overflowed != nullptr && buf->count >= EventBuffer::kCapacity) *overflowed = true;
        collect(*buf, n, pos, evs);
        pos += n;
    }
    return evs;
}

// Step-by-step driver for scripted scenarios: keeps the absolute sample
// position and appends to `events`.
struct Driver {
    Sequencer& s;
    std::int64_t pos = 0;
    std::vector<Ev> events;
    std::unique_ptr<EventBuffer> buf = std::make_unique<EventBuffer>();

    explicit Driver(Sequencer& seq) : s(seq) {}

    // Returns the events of this call only.
    std::vector<Ev> block(const Transport& t, const Settings& st, int n) {
        buf->clear();
        s.process(t, st, n, *buf);
        std::vector<Ev> mine;
        collect(*buf, n, pos, mine);
        events.insert(events.end(), mine.begin(), mine.end());
        pos += n;
        return mine;
    }
    std::vector<Ev> host(double ppq, double bpm, const Settings& st, int n, bool playing = true) {
        Transport t;
        t.hostValid = true;
        t.playing = playing;
        t.ppq = ppq;
        t.bpm = bpm;
        return block(t, st, n);
    }
    // Internal-clock blocks until `untilSample` (absolute).
    std::vector<Ev> runUntil(const Settings& st, std::int64_t untilSample, int blockSize) {
        std::vector<Ev> all;
        while (pos < untilSample) {
            const int n = static_cast<int>(std::min<std::int64_t>(blockSize, untilSample - pos));
            const auto e = block(Transport{}, st, n);
            all.insert(all.end(), e.begin(), e.end());
        }
        return all;
    }
};

inline std::vector<Ev> onlyVoice(const std::vector<Ev>& ev, int voice) {
    std::vector<Ev> r;
    for (const Ev& e : ev)
        if (e.voice == voice) r.push_back(e);
    return r;
}

inline std::vector<std::int64_t> samplesOf(const std::vector<Ev>& ev) {
    std::vector<std::int64_t> r;
    for (const Ev& e : ev) r.push_back(e.sample);
    return r;
}

inline std::vector<int> fixedBlocks(std::int64_t total, int size) {
    std::vector<int> b;
    while (total > 0) {
        const int n = static_cast<int>(std::min<std::int64_t>(total, size));
        b.push_back(n);
        total -= n;
    }
    return b;
}

inline std::vector<int> randomBlocks(std::int64_t total, std::mt19937& rng, int maxSize) {
    std::uniform_int_distribution<int> d(1, maxSize);
    std::vector<int> b;
    while (total > 0) {
        const int n = static_cast<int>(std::min<std::int64_t>(total, d(rng)));
        b.push_back(n);
        total -= n;
    }
    return b;
}

// A pattern with the given voice set Normal on the listed steps.
inline Pattern patternWith(int voice, std::initializer_list<int> steps, int length = 16,
                           Scale scale = Scale::Sixteenth) {
    Pattern p;
    p.length = static_cast<std::uint8_t>(length);
    p.scale = scale;
    for (int s : steps) p.at(0, voice, s).setLevel(StepLevel::Normal);
    p.steps[1] = p.steps[0];
    return p;
}

inline Pattern everyStep(int voice, int length = 16, Scale scale = Scale::Sixteenth) {
    Pattern p;
    p.length = static_cast<std::uint8_t>(length);
    p.scale = scale;
    for (int s = 0; s < length; ++s) {
        p.at(0, voice, s).setLevel(StepLevel::Normal);
        p.at(1, voice, s).setLevel(StepLevel::Normal);
    }
    return p;
}

// Random pattern with every feature switched on at random.
inline Pattern randomPattern(std::mt19937& rng, float density, bool withProbability = true) {
    std::uniform_int_distribution<int> len(1, kMaxSteps), scale(0, 4), shuffle(0, kMaxShuffleTicks), coin(0, 99),
        lvl(1, 3), subs(1, 4), prob(0, 7), tl(1, kMaxSteps), flam(0, kMaxFlamMs), pct(0, 100);
    Pattern p;
    p.length = static_cast<std::uint8_t>(coin(rng) < 30 ? 16 : len(rng));
    p.scale = static_cast<Scale>(scale(rng));
    p.shuffle = static_cast<std::uint8_t>(coin(rng) < 30 ? 0 : shuffle(rng));
    p.flamMs = static_cast<std::uint8_t>(coin(rng) < 10 ? 0 : flam(rng));
    p.flamGrace = static_cast<std::uint8_t>(coin(rng) < 50 ? 100 : pct(rng));
    p.subStart = static_cast<std::uint8_t>(coin(rng) < 50 ? 100 : pct(rng));
    p.subEnd = static_cast<std::uint8_t>(coin(rng) < 50 ? 100 : pct(rng));
    for (int v = 0; v < kNumVoices; ++v) {
        p.trackLength[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(coin(rng) < 20 ? tl(rng) : 0);
        p.trackShuffle[static_cast<std::size_t>(v)] =
            static_cast<std::uint8_t>(coin(rng) < 20 ? 1 + shuffle(rng) : 0);
    }
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int var = 0; var < kNumVariations; ++var) {
        for (int t = 0; t < kNumTracks; ++t) {
            for (int s = 0; s < kMaxSteps; ++s) {
                if (u(rng) >= density) continue;
                Step& c = p.at(var, t, s);
                c.setLevel(static_cast<StepLevel>(lvl(rng)));
                if (t == kAccentTrack) continue;
                if (coin(rng) < 15) c.setFlam(true);
                if (coin(rng) < 15) c.setSubSteps(subs(rng));
                if (withProbability && coin(rng) < 15) c.setProbabilityCode(prob(rng));
            }
        }
    }
    return p;
}

}  // namespace seqtest
