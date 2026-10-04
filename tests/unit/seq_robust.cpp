// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Sequencer robustness: buffer and queue overflow, a long run with random
// transport abuse (NaN/Inf/extreme values everywhere), no allocation in
// process(), and the cost of process().

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "SeqTestUtil.h"

// --- allocation counting -----------------------------------------------------
// Replaces the global operator new/delete for this test executable. The counter
// only runs while `g_countAllocs` is set, around the calls under test.
namespace {
std::atomic<bool> g_countAllocs{false};
std::atomic<long> g_allocs{0};

void* countedAlloc(std::size_t n) {
    if (g_countAllocs.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(n);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(n);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace seqtest;

namespace {

Pattern densePattern() {
    Pattern p;
    p.length = 64;
    p.scale = Scale::ThirtySecond;
    p.shuffle = kMaxShuffleTicks;
    p.flamMs = kMaxFlamMs;
    p.subStart = 30;
    p.subEnd = 100;
    p.flamGrace = 70;
    for (int var = 0; var < kNumVariations; ++var)
        for (int t = 0; t < kNumTracks; ++t)
            for (int s = 0; s < kMaxSteps; ++s) {
                Step& c = p.at(var, t, s);
                c.setLevel(StepLevel::Accent);
                if (t < kNumVoices) {
                    c.setSubSteps(4);
                    c.setFlam(true);
                }
            }
    return p;
}

void checkGetters(const Sequencer& s) {
    REQUIRE(s.currentStep() >= -1);
    REQUIRE(s.currentStep() < kMaxSteps);
    REQUIRE(s.currentPattern() >= 0);
    REQUIRE(s.currentPattern() < kNumPatterns);
    REQUIRE(s.selectedPattern() >= 0);
    REQUIRE(s.selectedPattern() < kNumPatterns);
    REQUIRE(s.queuedPattern() >= -1);
    REQUIRE(s.queuedPattern() < kNumPatterns);
    REQUIRE((s.currentVariation() == 0 || s.currentVariation() == 1));
    REQUIRE(s.tempo() >= 20.0);
    REQUIRE(s.tempo() <= 400.0);
    if (!s.isPlaying()) REQUIRE(s.currentStep() == -1);
    if (!s.isPlaying()) REQUIRE(s.songPosition() == -1);
    REQUIRE(s.songPosition() >= -1);
    REQUIRE(s.songPosition() < kMaxSongLength);
    REQUIRE(std::isfinite(s.tempo()));
}

}  // namespace

TEST_CASE("event buffer overflow: a huge block of a dense pattern stays bounded and sorted", "[seq][robust]") {
    auto s = makeSeq(44100.0);
    s->setPattern(0, densePattern());
    Settings st = internalSettings(400.0);
    st.jitter = true;
    auto buf = std::make_unique<EventBuffer>();
    // One 4-second block: ~1200 steps' worth of 80 hits each, far beyond 1024.
    s->process(Transport{}, st, 44100 * 4, *buf);
    REQUIRE(buf->count == EventBuffer::kCapacity);
    for (int i = 1; i < buf->count; ++i)
        REQUIRE(buf->events[static_cast<std::size_t>(i)].offset >= buf->events[static_cast<std::size_t>(i - 1)].offset);
    // The clock kept going: the next block starts where it should.
    buf->clear();
    s->process(Transport{}, st, 512, *buf);
    for (int i = 0; i < buf->count; ++i) REQUIRE(buf->events[static_cast<std::size_t>(i)].offset < 512);
    CHECK(s->isPlaying());
    checkGetters(*s);

    // A partially filled buffer from an earlier sub-range plus offsetBase.
    buf->clear();
    for (int i = 0; i < 1000; ++i) buf->push(Event{});
    s->process(Transport{}, st, 4096, *buf, 300);
    REQUIRE(buf->count == EventBuffer::kCapacity);
    for (int i = 1000; i < buf->count; ++i) {
        REQUIRE(buf->events[static_cast<std::size_t>(i)].offset >= 300);
        REQUIRE(buf->events[static_cast<std::size_t>(i)].offset < 300 + 4096);
    }
}

TEST_CASE("pending queue: worst legal load never drops a hit, and it stays bounded", "[seq][robust]") {
    // Worst legal case: all 11 voices, x4 sub-steps + flam on every 32nd at
    // 400 BPM, maximum shuffle, flam at its 100 ms maximum, timing jitter on.
    // Hits in flight peak near 11 x (104 ms / 18.75 ms + ~2 steps of
    // sub-hits) ~ 100, well under the 1024 slots, so per-sample blocks
    // (everything goes through the queue) must give exactly what large
    // blocks give.
    for (double sr : {44100.0, 192000.0}) {
        auto s = makeSeq(sr);
        s->setPattern(0, densePattern());
        Settings st = internalSettings(400.0);
        st.jitter = true;
        const auto total = static_cast<std::int64_t>(sr * 0.5);
        const auto big = runInternal(*s, st, fixedBlocks(total, 512));
        s->reset();
        const auto tiny = runInternal(*s, st, fixedBlocks(total, 1));
        INFO("sr " << sr);
        CHECK(big.size() > 1200);
        REQUIRE(tiny == big);
        checkGetters(*s);
    }
    // Stop drops the queue: a sparse pattern then plays exactly.
    auto s = makeSeq(192000.0);
    s->setPattern(0, densePattern());
    Settings st = internalSettings(400.0);
    (void)runInternal(*s, st, fixedBlocks(20000, 7));
    st.run = false;
    (void)runInternal(*s, st, fixedBlocks(10, 10));
    s->setPattern(0, everyStep(BD));
    st.run = true;
    const auto clean = runInternal(*s, st, fixedBlocks(192000, 1));
    CHECK(clean.size() == 27);  // a sixteenth is 7200 samples at 400 BPM / 192 kHz
    for (const Ev& e : clean) CHECK(e.voice == BD);
}

TEST_CASE("ten minutes of random transport abuse keeps every invariant", "[seq][robust][long]") {
    std::mt19937 rng(0xA11CE5U);
    auto s = makeSeq(48000.0);
    for (int i = 0; i < kNumPatterns; ++i) {
        std::mt19937 prng(static_cast<std::uint32_t>(i * 7919));
        s->setPattern(i, randomPattern(prng, 0.2f));
    }
    // A few pathological patterns.
    Pattern zeroLen = everyStep(BD);
    zeroLen.length = 0;
    zeroLen.trackLength[3] = 200;
    zeroLen.scale = static_cast<Scale>(17);
    zeroLen.shuffle = 255;
    zeroLen.flamMs = 255;
    zeroLen.flamGrace = 255;
    zeroLen.subStart = 255;
    zeroLen.subEnd = 200;
    zeroLen.trackShuffle[2] = 255;
    zeroLen.reserved = 77;
    zeroLen.at(0, BD, 3).bits = 0xFF;
    s->setPattern(5, zeroLen);
    s->setPattern(6, densePattern());
    Chain c;
    c.length = 200;  // out of range on purpose
    for (int i = 0; i < kMaxChainLength; ++i) c.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(rng() % 256);
    s->setChain(c);
    for (int k = 0; k < kNumSongTracks; ++k) {
        SongTrack t;
        t.length = static_cast<std::uint16_t>(k == 7 ? 60000 : rng() % 300);  // some out of range on purpose
        for (auto& e : t.entries) e = static_cast<std::uint8_t>(rng() % 256);
        s->setSongTrack(k, t);
    }

    std::uniform_int_distribution<int> pct(0, 99), blockD(1, 2048), patD(0, kNumPatterns - 1), voiceD(0, kNumVoices - 1);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const std::int64_t total = 48000LL * 600;
    std::int64_t pos = 0;
    Settings st = hostSettings();
    Transport t{true, true, 0.0, 120.0};
    auto buf = std::make_unique<EventBuffer>();
    std::int64_t hits = 0, calls = 0;
    while (pos < total) {
        const int n = pct(rng) < 3 ? (pct(rng) < 50 ? 0 : 8192) : blockD(rng);
        // --- abuse -------------------------------------------------------
        const int r = pct(rng);
        if (r < 2) st.mode = static_cast<Mode>(rng() % 5);  // includes an invalid value
        else if (r < 5) t.playing = !t.playing;
        else if (r < 7) {
            const double weird[] = {std::nan(""), HUGE_VAL, -HUGE_VAL, 1.0e300, -1.0e12, 4.0e9};
            t.ppq = pct(rng) < 15 ? weird[rng() % 6] : (u(rng) - 0.2) * 500.0;  // relocation, incl. negative
        } else if (r < 9) {
            const double weird[] = {std::nan(""), HUGE_VAL, -5.0, 0.0, 1.0e9, 1.0e-9};
            t.bpm = pct(rng) < 20 ? weird[rng() % 6] : 0.5 + u(rng) * 1200.0;
        }
        else if (r < 10) t.hostValid = !t.hostValid;
        else if (r < 12) st.run = !st.run;
        else if (r < 13) st.internalBpm = pct(rng) < 10 ? std::nan("") : u(rng) * 600.0;
        else if (r < 15) s->selectPattern(patD(rng), pct(rng) < 30);
        else if (r < 16) s->requestFill();
        else if (r < 17) s->notePatternOn(patD(rng));
        else if (r < 18) s->notePatternOff();
        else if (r < 19) st.variation = static_cast<VariationMode>(rng() % 4);
        else if (r < 20) st.autoFill = static_cast<AutoFill>(rng() % 7);
        else if (r < 21) {
            st.play = static_cast<PlayMode>(rng() % 4);  // includes an invalid value
            st.songTrack = static_cast<std::uint8_t>(rng() % 12);
        }
        else if (r < 22) st.record = !st.record;
        else if (r < 23) (void)s->recordHit(static_cast<VoiceId>(voiceD(rng)), pct(rng) < 50, blockD(rng));
        else if (r < 24) {
            std::mt19937 prng(static_cast<std::uint32_t>(rng()));
            s->setPattern(patD(rng), randomPattern(prng, static_cast<float>(u(rng))));
        } else if (r < 25) {
            const float weird[] = {std::nanf(""), HUGE_VALF, -HUGE_VALF, -3.0f, 7.0f};
            st.totalAccent = pct(rng) < 20 ? weird[rng() % 5] : static_cast<float>(u(rng) * 3.0 - 1.0);
            st.swingScale = pct(rng) < 20 ? weird[rng() % 5] : static_cast<float>(u(rng) * 2.0);
            st.fillSlot = static_cast<std::uint8_t>(rng() % 40);
            st.jitter = pct(rng) < 50;
            st.jitterSeed = static_cast<std::uint32_t>(rng());
        } else if (r < 26) {
            s->prepare(u(rng) < 0.5 ? 44100.0 : 96000.0);
        } else if (r < 27) {
            s->reset();
        }
        const bool wasPlaying = s->isPlaying();
        buf->clear();
        const int base = static_cast<int>(rng() % 64);
        s->process(t, st, n, *buf, base);
        ++calls;
        // --- invariants ----------------------------------------------------
        for (int i = 0; i < buf->count; ++i) {
            const Event& e = buf->events[static_cast<std::size_t>(i)];
            REQUIRE(e.offset >= base);
            REQUIRE(e.offset < base + n);
            if (i > 0) REQUIRE(e.offset >= buf->events[static_cast<std::size_t>(i - 1)].offset);
            REQUIRE(static_cast<int>(e.voice) < kNumVoices);
            REQUIRE(std::isfinite(e.hit.acc));
            REQUIRE(e.hit.acc >= 0.0f);
            REQUIRE(e.hit.acc <= AccentDac::kVolts);
            REQUIRE(e.hit.pitchSemis == 0.0f);
            REQUIRE(e.level != StepLevel::Off);
        }
        if (!s->isPlaying()) REQUIRE(buf->count == 0);
        (void)wasPlaying;
        checkGetters(*s);
        hits += buf->count;
        // Advance the host position like a host would (when it is playing).
        if (t.playing && std::isfinite(t.bpm) && t.bpm > 0.0 && std::isfinite(t.ppq))
            t.ppq += n * t.bpm / (60.0 * 48000.0);
        pos += n;
    }
    CHECK(hits > 1000);
    CHECK(calls > 10000);
}

TEST_CASE("process() cost for a dense pattern", "[seq][robust][bench]") {
    auto s = makeSeq(48000.0);
    Pattern p = densePattern();
    for (int t = 0; t < kNumVoices; ++t)
        for (int st = 0; st < kMaxSteps; ++st) {
            p.at(0, t, st).setSubSteps(1);
            p.at(0, t, st).setFlam(false);
            p.at(0, t, st).setProbabilityCode(st % 4);
        }
    p.scale = Scale::Sixteenth;
    s->setPattern(0, p);
    const Settings st = internalSettings(174.0);
    auto buf = std::make_unique<EventBuffer>();
    const int calls = 20000;
    std::int64_t hits = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) {
        buf->clear();
        s->process(Transport{}, st, 512, *buf);
        hits += buf->count;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / calls;
    std::printf("[bench] dense pattern (11 voices every 16th, 174 BPM, 512-sample blocks @ 48 kHz): "
                "%.3f us per process(), %.1f hits per block\n",
                us, static_cast<double>(hits) / calls);

    // Worst case: all voices x4 sub-steps + flam on every 32nd at 400 BPM.
    s->setPattern(0, densePattern());
    Settings worst = internalSettings(400.0);
    worst.jitter = true;
    s->reset();
    hits = 0;
    const auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) {
        buf->clear();
        s->process(Transport{}, worst, 512, *buf);
        hits += buf->count;
    }
    const auto t3 = std::chrono::steady_clock::now();
    const double us2 = std::chrono::duration<double, std::micro>(t3 - t2).count() / calls;
    std::printf("[bench] worst case (11 voices x4 sub-steps + flam, 32nds at 400 BPM): %.3f us per process(), "
                "%.1f hits per block\n",
                us2, static_cast<double>(hits) / calls);
    CHECK(us < 200.0);
    CHECK(us2 < 500.0);
}

TEST_CASE("process(), recordHit() and the controls never allocate", "[seq][robust][alloc]") {
    std::mt19937 rng(0xA110CU);
    auto s = makeSeq(48000.0);
    auto bank = std::make_unique<PatternBank>();
    for (int i = 0; i < kNumPatterns; ++i) {
        std::mt19937 prng(static_cast<std::uint32_t>(i + 1));
        bank->patterns[static_cast<std::size_t>(i)] = randomPattern(prng, 0.3f);
    }
    bank->patterns[3] = densePattern();
    bank->chain.length = 5;
    for (int i = 0; i < 5; ++i) bank->chain.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i * 7);
    bank->tracks[2].length = 40;
    for (int i = 0; i < 40; ++i) bank->tracks[2].entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    s->setBank(*bank);
    auto buf = std::make_unique<EventBuffer>();
    auto pattern = std::make_unique<Pattern>(densePattern());
    const Chain chain = bank->chain;
    const SongTrack track = bank->tracks[2];
    std::uniform_int_distribution<int> sizes(1, 8192), pct(0, 99);
    Transport t{true, true, -1.0, 133.0};
    Settings st = hostSettings();
    st.jitter = true;
    st.record = true;
    long processCalls = 0;

    g_allocs = 0;
    g_countAllocs = true;
    for (int i = 0; i < 6000; ++i) {
        const int n = sizes(rng);
        const int r = pct(rng);
        if (r < 3) st.play = static_cast<PlayMode>(rng() % 3);
        else if (r < 5) st.mode = static_cast<Mode>(rng() % 4);
        else if (r < 7) s->selectPattern(static_cast<int>(rng() % kNumPatterns), r == 6);
        else if (r < 8) s->requestFill();
        else if (r < 9) s->notePatternOn(static_cast<int>(rng() % kNumPatterns));
        else if (r < 10) s->notePatternOff();
        else if (r < 12) (void)s->recordHit(static_cast<VoiceId>(rng() % kNumVoices), r == 11, n / 2);
        else if (r < 13) s->setPattern(3, *pattern);
        else if (r < 14) s->setChain(chain);
        else if (r < 15) s->setSongTrack(2, track);
        else if (r < 16) t.ppq = -3.0 + static_cast<double>(rng() % 1000) * 0.37;
        else if (r < 17) st.autoFill = static_cast<AutoFill>(rng() % 6);
        else if (r < 18) st.run = !st.run;
        buf->clear();
        s->process(t, st, n, *buf, static_cast<int>(rng() % 16));
        ++processCalls;
        t.ppq += n * t.bpm / (60.0 * 48000.0);
    }
    s->setBank(*bank);
    s->reset();
    s->prepare(96000.0);
    g_countAllocs = false;
    CHECK(processCalls == 6000);
    CHECK(g_allocs.load() == 0);

    // The counter works: a vector allocation is seen.
    g_countAllocs = true;
    { std::vector<int> v(10); (void)v; }
    g_countAllocs = false;
    CHECK(g_allocs.load() > 0);
}
