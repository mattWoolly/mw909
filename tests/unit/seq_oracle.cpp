// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// An independent brute-force timing oracle for the sequencer, plus review
// regressions. The oracle shares no code with Sequencer.cpp: it walks
// measures from PPQ 0 in long double, applies architecture.md §5 literally
// (play mode Pattern/Chain/Track, variation, auto fill from the playing
// pattern's group, polymeter, per-scale and per-track shuffle with
// swingScale, sub-steps with the level ramp, flam in ms with the grace level,
// accent-DAC dynamics, timing jitter) and rounds each hit to the nearest
// sample. Every block partition must match it exactly; only hits within
// 2e-6 samples of a rounding tie may move by one. The only shared pieces are
// the deterministic hashes of SeqHash.h (the jitter amount is a hash value,
// not arithmetic the spec defines) and AccentDac::volts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <random>
#include <tuple>
#include <vector>

#include "SeqTestUtil.h"
#include "mw909/seq/SeqHash.h"

using namespace seqtest;

namespace {

struct OHit {
    std::int64_t sample = 0;
    int voice = 0;
    float volts = 0.0f;
    int level = 0;
    bool ta = false;
    bool tie = false;  // within 2e-6 samples of a rounding tie
};

enum class Clock { Host, Internal, HostSyncNoHost };

struct OScenario {
    double sr = 48000.0, bpm = 120.0, ppqStart = 0.0;
    std::int64_t total = 0;
    float knob = 0.7f, swing = 1.0f;
    VariationMode variation = VariationMode::A;
    AutoFill autoFill = AutoFill::Manual;
    int fillSlot = 15;
    std::vector<int> list;  // chain/track entries; empty = Pattern mode
    bool jitter = false;
    std::uint32_t jitterSeed = 0;
};

long double stepQnOracle(Scale s) {
    switch (s) {
        case Scale::ThirtySecond: return 0.125L;
        case Scale::EighthTriplet: return 1.0L / 3.0L;
        case Scale::SixteenthTriplet: return 1.0L / 6.0L;
        case Scale::Eighth: return 0.5L;
        default: return 0.25L;
    }
}

// The off-beat delay in quarter notes, straight from §5.3.
long double shuffleOracle(const Pattern& p, int voice, int s, int L, float swing) {
    if (s % 2 == 0) return 0.0L;
    if (L % 2 == 1 && s == L - 1) return 0.0L;
    int ticks = p.shuffle;
    if (p.trackShuffle[static_cast<std::size_t>(voice)] != 0) ticks = p.trackShuffle[static_cast<std::size_t>(voice)] - 1;
    ticks = std::min(ticks, kMaxShuffleTicks);
    const long double t = static_cast<long double>(ticks) * static_cast<long double>(swing);
    switch (p.scale) {
        case Scale::Sixteenth: return t / 96.0L;
        case Scale::ThirtySecond: return 2.0L * std::floor(t / 4.0L) / 96.0L;
        case Scale::Eighth: return 2.0L * t / 96.0L;
        default: return 0.0L;
    }
}

std::vector<OHit> oracle(const PatternBank& bank, int base, const OScenario& sc) {
    std::vector<OHit> out;
    const long double spp = static_cast<long double>(sc.sr) * 60.0L / static_cast<long double>(sc.bpm);
    const long double p0 = sc.ppqStart;
    const int fillN = autoFillMeasures(sc.autoFill);
    const auto listLen = static_cast<std::int64_t>(sc.list.size());
    auto patternAt = [&](std::int64_t m) {
        const int b = listLen == 0 ? base : sc.list[static_cast<std::size_t>(((m % listLen) + listLen) % listLen)];
        const bool fill = fillN > 0 && ((m % fillN) + fillN) % fillN == fillN - 1;
        // Slot `fillSlot` of the group the playing pattern belongs to.
        return fill ? (b / 48) * 48 + ((b % 48) / 16) * 16 + sc.fillSlot : b;
    };
    auto lenOf = [&](int pi) { return std::clamp<int>(bank.patterns[static_cast<std::size_t>(pi)].length, 1, kMaxSteps); };
    auto measureQn = [&](std::int64_t m) {
        const int pi = patternAt(m);
        return lenOf(pi) * stepQnOracle(bank.patterns[static_cast<std::size_t>(pi)].scale);
    };
    auto round = [&](long double t, long double extra, bool& tie) {
        const long double x = (t - p0) * spp + extra + 0.5L;
        const long double fl = std::floor(x);
        tie = (x - fl) < 2e-6L || (fl + 1.0L - x) < 2e-6L;
        return static_cast<std::int64_t>(std::floor(x + 1e-6L));
    };

    std::int64_t m = 0;
    long double start = 0.0L;
    if (p0 >= 0.0L) {
        while (start + measureQn(m) <= p0) start += measureQn(m++);
    } else {
        while (start > p0) start -= measureQn(--m);
    }
    const long double endQn = p0 + static_cast<long double>(sc.total) / spp + 2.0L;
    const int ta = static_cast<int>(39.0f * sc.knob + 0.5f);
    for (; start < endQn; start += measureQn(m++)) {
        const int pi = patternAt(m);
        const Pattern& p = bank.patterns[static_cast<std::size_t>(pi)];
        const int L = lenOf(pi);
        const long double stepQn = stepQnOracle(p.scale);
        const int var = sc.variation == VariationMode::B    ? 1
                        : sc.variation == VariationMode::AB ? static_cast<int>(((m % 2) + 2) % 2)
                                                            : 0;
        const long double flamSamples =
            static_cast<long double>(std::min<int>(p.flamMs, 100)) * static_cast<long double>(sc.sr) / 1000.0L;
        const float grace = static_cast<float>(std::min<int>(p.flamGrace, 100)) / 100.0f;
        const auto a = static_cast<float>(std::min<int>(p.subStart, 100));
        const auto b = static_cast<float>(std::min<int>(p.subEnd, 100));
        for (int s = 0; s < L; ++s) {
            const long double t = start + s * stepQn;
            bool stepTie = false;
            const std::int64_t at = round(t, 0.0L, stepTie);
            if (at < -1 || (at == -1 && !stepTie) || at >= sc.total) continue;
            const bool accentOn = p.at(var, kAccentTrack, s).on();
            const long double jitter =
                sc.jitter ? static_cast<long double>(jitterMs(sc.jitterSeed, m, s)) * static_cast<long double>(sc.sr) /
                                1000.0L
                          : 0.0L;
            for (int v = 0; v < kNumVoices; ++v) {
                const int tl = p.trackLength[static_cast<std::size_t>(v)] != 0
                                   ? std::clamp<int>(p.trackLength[static_cast<std::size_t>(v)], 1, kMaxSteps)
                                   : L;
                const Step c = p.at(var, v, static_cast<int>(((m * L + s) % tl + tl) % tl));
                if (!c.on()) continue;
                const int level = static_cast<int>(c.level());
                const int code = (level == 1 ? 12 : 24) + (level == 3 ? 16 : 0) + (accentOn ? ta : 0);
                const float volts = AccentDac::volts(code);
                const long double sh = shuffleOracle(p, v, s, L, sc.swing);
                const int n = c.subSteps();
                for (int k = 0; k < n; ++k) {
                    const long double th = t + sh + k * stepQn / n;
                    const float vk =
                        n > 1 ? volts * (a + (b - a) * static_cast<float>(k) / static_cast<float>(n - 1)) / 100.0f
                              : volts;
                    bool tie = false;
                    auto push = [&](std::int64_t smp, float vv, int lv, bool t2) {
                        if (smp < sc.total)
                            out.push_back({std::max<std::int64_t>(0, smp), v, vv, lv, t2, tie || stepTie});
                    };
                    if (k == 0 && c.flam() && flamSamples >= 1.0L) {
                        const bool light = p.flamGrace < 100;
                        push(round(th, jitter, tie), vk * grace, light ? 1 : level, light ? false : accentOn);
                        push(round(th, jitter + flamSamples, tie), vk, level, accentOn);
                    } else {
                        push(round(th, jitter, tie), vk, level, accentOn);
                    }
                }
            }
        }
    }
    return out;
}

// Mismatch count: exact multiset match; a tie hit may match one sample away.
int mismatches(const std::vector<OHit>& want, const std::vector<Ev>& got) {
    using Key = std::tuple<std::int64_t, int, float, int, bool>;
    std::multimap<Key, int> pool;
    for (const Ev& e : got) pool.insert({{e.sample, e.voice, e.volts, static_cast<int>(e.level), e.ta}, 0});
    int bad = 0;
    for (const OHit& w : want) {
        auto it = pool.find({w.sample, w.voice, w.volts, w.level, w.ta});
        if (it == pool.end() && w.tie) {
            it = pool.find({w.sample - 1, w.voice, w.volts, w.level, w.ta});
            if (it == pool.end()) it = pool.find({w.sample + 1, w.voice, w.volts, w.level, w.ta});
        }
        if (it == pool.end()) {
            ++bad;
            continue;
        }
        pool.erase(it);
    }
    return bad + static_cast<int>(pool.size());
}

}  // namespace

TEST_CASE("independent oracle: thousands of random scenarios and block partitions", "[seq][oracle]") {
    std::mt19937 rng(0x0EAC1EU);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double rates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
    int scenarios = 0, checked = 0, withJitter = 0, chains = 0, tracks = 0;
    std::int64_t hits = 0;
    for (int it = 0; it < 3000; ++it) {
        OScenario sc;
        sc.sr = rates[rng() % 5];
        sc.bpm = (rng() % 3 == 0) ? std::round(20.0 + u(rng) * 380.0) : 20.0 + u(rng) * 380.0;
        const auto clock = static_cast<Clock>(rng() % 3);
        sc.autoFill = static_cast<AutoFill>(rng() % 6);
        sc.ppqStart = clock != Clock::Host ? 0.0
                                           : (rng() % 4 == 0 ? std::round(u(rng) * 16.0) : (u(rng) - 0.2) * 40.0);
        sc.total = static_cast<std::int64_t>(sc.sr * (0.5 + u(rng) * 3.5));
        sc.knob = static_cast<float>(u(rng));
        sc.swing = rng() % 2 != 0 ? 1.0f : static_cast<float>(u(rng));
        sc.variation = static_cast<VariationMode>(rng() % 3);
        sc.fillSlot = static_cast<int>(rng() % 16);
        sc.jitter = rng() % 4 == 0;
        sc.jitterSeed = static_cast<std::uint32_t>(rng());
        const int base = static_cast<int>(rng() % kNumPatterns);
        const auto playMode = static_cast<PlayMode>(rng() % 4 == 0 ? 1 : (rng() % 3 == 0 ? 2 : 0));

        auto s = makeSeq(sc.sr);
        auto bank = std::make_unique<PatternBank>();
        std::vector<int> used{base};
        if (playMode != PlayMode::Pattern) {
            const int n = 1 + static_cast<int>(rng() % (playMode == PlayMode::Chain ? 6 : 9));
            for (int k = 0; k < n; ++k) {
                // Mostly patterns of one group, sometimes any pattern.
                const int e = rng() % 3 == 0 ? static_cast<int>(rng() % kNumPatterns)
                                             : (base / 16) * 16 + static_cast<int>(rng() % 16);
                sc.list.push_back(e);
                used.push_back(e);
            }
            if (playMode == PlayMode::Chain) {
                Chain c;
                c.length = static_cast<std::uint8_t>(n);
                for (int k = 0; k < n; ++k) c.entries[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(sc.list[static_cast<std::size_t>(k)]);
                bank->chain = c;
                ++chains;
            } else {
                SongTrack t;
                t.length = static_cast<std::uint16_t>(n);
                for (int k = 0; k < n; ++k) t.entries[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(sc.list[static_cast<std::size_t>(k)]);
                bank->tracks[3] = t;
                ++tracks;
            }
        }
        for (int pi : std::vector<int>(used)) used.push_back((pi / 16) * 16 + sc.fillSlot);
        for (int pi : used) {
            std::mt19937 pr(static_cast<std::uint32_t>(rng()));
            const bool fill = pi % 16 == sc.fillSlot;
            bank->patterns[static_cast<std::size_t>(pi)] =
                randomPattern(pr, fill ? 0.25f : static_cast<float>(0.05 + u(rng) * 0.45), false);
        }
        s->setBank(*bank);

        Settings st = clock == Clock::Host ? hostSettings() : internalSettings(sc.bpm);
        if (clock == Clock::HostSyncNoHost) st.mode = Mode::HostSync;  // no host transport: runs as Internal
        st.totalAccent = sc.knob;
        st.swingScale = sc.swing;
        st.variation = sc.variation;
        st.autoFill = sc.autoFill;
        st.fillSlot = static_cast<std::uint8_t>(sc.fillSlot);
        st.play = playMode;
        st.songTrack = 3;
        st.jitter = sc.jitter;
        st.jitterSeed = sc.jitterSeed;

        const auto want = oracle(*bank, base, sc);
        std::uniform_int_distribution<int> sizes(1, 4096);
        std::vector<std::vector<int>> parts{fixedBlocks(sc.total, sizes(rng)), fixedBlocks(sc.total, 4096),
                                            randomBlocks(sc.total, rng, 4096), randomBlocks(sc.total, rng, 64)};
        if (it % 40 == 0) parts.push_back(fixedBlocks(sc.total, 1));
        ++scenarios;
        withJitter += sc.jitter ? 1 : 0;
        for (const auto& blocks : parts) {
            s->reset();
            s->selectPattern(base);
            bool overflow = false;
            const auto got = clock == Clock::Host ? runHost(*s, st, sc.sr, sc.bpm, sc.ppqStart, blocks, &overflow)
                                                  : runInternal(*s, st, blocks, &overflow);
            if (overflow) continue;
            INFO("scenario " << it << " sr " << sc.sr << " bpm " << sc.bpm << " ppq0 " << sc.ppqStart << " blocks "
                             << blocks.size() << " mode " << static_cast<int>(playMode) << " jitter " << sc.jitter);
            REQUIRE(mismatches(want, got) == 0);
            ++checked;
            hits += static_cast<std::int64_t>(got.size());
        }
    }
    WARN("oracle: " << scenarios << " scenarios (" << chains << " chain, " << tracks << " track, " << withJitter
                    << " with jitter), " << checked << " partitions checked, " << hits << " hits compared");
    CHECK(checked > 11000);
    CHECK(hits > 1500000);
}

TEST_CASE("HostSync follows host tempos outside 20..400: no double or missed steps", "[seq][oracle][review]") {
    // Regression: the PPQ-to-sample mapping used the clamped tempo, so every
    // block of a 10 BPM host looked like a backward jump (each step fired
    // twice) and a 600 BPM host like a forward jump (a third of the steps lost).
    for (double bpm : {1.0, 5.0, 10.0, 19.0, 401.0, 600.0, 999.0, 1000.0}) {
        for (int block : {64, 512, 4096}) {
            auto s = makeSeq(48000.0);
            s->setPattern(0, everyStep(BD));
            const double spp = 48000.0 * 60.0 / bpm;
            const double qn = bpm < 20.0 ? 0.6 : 16.0;
            const auto total = static_cast<std::int64_t>(qn * spp);
            const auto ev = runHost(*s, hostSettings(), 48000.0, bpm, 0.0, fixedBlocks(total, block));
            std::vector<std::int64_t> want;
            for (int k = 0;; ++k) {
                const auto smp = static_cast<std::int64_t>(std::floor(k * 0.25L * spp + 0.5L + 1e-6L));
                if (smp >= total) break;
                want.push_back(smp);
            }
            INFO("host " << bpm << " BPM, block " << block);
            REQUIRE(samplesOf(ev) == want);
            CHECK(s->tempo() == std::clamp(bpm, 20.0, 400.0));  // the reported tempo keeps the display clamp
        }
    }
}

TEST_CASE("HostSync tempo ramp in large blocks is not a relocation", "[seq][oracle][review]") {
    // Regression: the host integrates a ramp inside each block but reports one
    // BPM per block. With 8192-sample blocks of a 60 -> 400 BPM ramp the PPQ
    // landed > 1 ms from the old-tempo continuation and steps were skipped.
    for (double target : {240.0, 400.0, 1000.0}) {
        for (int block : {512, 4096, 8192}) {
            auto s = makeSeq(48000.0);
            s->setPattern(0, everyStep(BD));
            auto buf = std::make_unique<EventBuffer>();
            double ppq = 0.0;
            std::vector<Ev> ev;
            std::int64_t pos = 0;
            while (ppq < 24.0) {
                const auto bpmAt = [&](double q) { return q < 16.0 ? 60.0 + (target - 60.0) * q / 16.0 : target; };
                Transport t{true, true, ppq, bpmAt(ppq)};
                buf->clear();
                s->process(t, hostSettings(), block, *buf);
                collect(*buf, block, pos, ev);
                for (int i = 0; i < block; ++i) ppq += bpmAt(ppq) / (60.0 * 48000.0);
                pos += block;
            }
            INFO("ramp to " << target << ", block " << block);
            // Every sixteenth up to the final position fires exactly once, in order.
            const auto expected = static_cast<std::size_t>(std::floor(ppq * 4.0)) + 1;
            CHECK((ev.size() == expected || ev.size() + 1 == expected));
            for (std::size_t i = 1; i < ev.size(); ++i) CHECK(ev[i].sample > ev[i - 1].sample);
        }
    }
}

TEST_CASE("recording two live hits of one voice inside one range fires neither step again", "[seq][oracle][review]") {
    // Regression: one suppression slot per voice; the second recordHit
    // overwrote the first, so the first recorded step sounded twice (live hit
    // plus sequencer) when a large block covered several steps.
    auto s = makeSeq(48000.0);
    Settings st = internalSettings(120.0);
    st.record = true;
    Driver d(*s);
    d.block(Transport{}, st, 100);  // steps every 6000 samples; next range starts at 100
    const RecordResult r1 = s->recordHit(SD, false, 5850);   // sample 5950  -> step 1
    const RecordResult r2 = s->recordHit(SD, true, 11850);   // sample 11950 -> step 2
    const RecordResult r3 = s->recordHit(SD, false, 17900);  // sample 18000 -> step 3
    CHECK(r1.step == 1);
    CHECK(r2.step == 2);
    CHECK(r3.step == 3);
    const auto first = d.block(Transport{}, st, 20000);
    CHECK(onlyVoice(first, SD).empty());
    // The written steps play on the next pass.
    const auto next = d.runUntil(st, 96000 + 20000, 512);
    const auto sd = onlyVoice(next, SD);
    REQUIRE(sd.size() == 3);
    CHECK(sd[0].sample == 96000 + 6000);
    CHECK(sd[1].sample == 96000 + 12000);
    CHECK(sd[2].sample == 96000 + 18000);
    CHECK(sd[1].volts == kAccentV);
}

TEST_CASE("a flam shorter than one sample plays the main hit only", "[seq][oracle][review]") {
    // flamMs = 0 would put the grace and the main hit on one sample (two
    // triggers and two MIDI note-ons at once): the main hit plays alone.
    for (double sr : {44100.0, 192000.0}) {
        auto s = makeSeq(sr);
        Pattern p;
        p.flamMs = 0;
        p.flamGrace = 50;
        p.at(0, SD, 0).setLevel(StepLevel::Normal);
        p.at(0, SD, 0).setFlam(true);
        s->setPattern(0, p);
        Driver d(*s);
        const auto ev = d.runUntil(internalSettings(120.0), 1000, 64);
        INFO("sr " << sr);
        REQUIRE(ev.size() == 1);
        CHECK(ev[0] == Ev{0, SD, kNormalV, StepLevel::Normal, false});
    }
    // 1 ms and up: grace then main.
    auto s = makeSeq(48000.0);
    Pattern p;
    p.flamMs = 1;
    p.at(0, SD, 0).setLevel(StepLevel::Normal);
    p.at(0, SD, 0).setFlam(true);
    s->setPattern(0, p);
    Driver d(*s);
    const auto ev = d.runUntil(internalSettings(120.0), 1000, 64);
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].sample == 0);
    CHECK(ev[1].sample == 48);
}

TEST_CASE("pending queue holds the worst case at a 1000 BPM host", "[seq][oracle][review]") {
    // HostSync follows host tempos up to 1000 BPM: the densest legal pattern
    // with 100 ms flams and timing jitter must lose nothing through the queue.
    for (Scale sc : {Scale::ThirtySecond, Scale::SixteenthTriplet, Scale::Eighth}) {
        Pattern p;
        p.length = 64;
        p.scale = sc;
        p.shuffle = kMaxShuffleTicks;
        p.flamMs = kMaxFlamMs;
        for (int t = 0; t < kNumTracks; ++t)
            for (int k = 0; k < kMaxSteps; ++k) {
                Step& c = p.at(0, t, k);
                c.setLevel(StepLevel::Accent);
                if (t < kNumVoices) {
                    c.setSubSteps(4);
                    c.setFlam(true);
                }
            }
        p.steps[1] = p.steps[0];
        auto s = makeSeq(48000.0);
        s->setPattern(0, p);
        Settings st = hostSettings();
        st.jitter = true;
        const std::int64_t total = 48000;
        bool overflow = false;
        const auto big = runHost(*s, st, 48000.0, 1000.0, 0.0, fixedBlocks(total, 256), &overflow);
        REQUIRE_FALSE(overflow);
        s->reset();
        const auto tiny = runHost(*s, st, 48000.0, 1000.0, 0.0, fixedBlocks(total, 1));
        CHECK(big.size() > 1500);
        REQUIRE(tiny == big);
    }
}
