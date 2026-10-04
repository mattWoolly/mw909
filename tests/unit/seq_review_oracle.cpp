// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Independent review oracle for the sequencer (written by the area's reviewer,
// sharing no code with Sequencer.cpp or SeqTestUtil.h). It recomputes every hit
// from architecture.md §5 and covers what the area's own oracle leaves out:
// sample rates 22.05 and 384 kHz, song positions far from PPQ 0 (1e5..5e7
// quarter notes, i.e. days of transport), garbage pattern fields (length 0 or
// > 64, invalid scale, shuffle/flam/grace/ramp values out of range, per-track
// length and shuffle out of range, garbage chain/track entries) and block
// partitions from 1 to 8192 samples. Measures are walked in exact integer
// units of 1/24 quarter note so the oracle itself never drifts. The only
// shared pieces are the deterministic hashes of SeqHash.h (the probability
// and jitter values are hash values, not arithmetic the spec defines) and the
// accent-DAC constants of Voices.h.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <tuple>
#include <vector>

#include "mw909/seq/SeqHash.h"
#include "mw909/seq/Sequencer.h"

using namespace mw909;
using namespace mw909::seq;

namespace {

struct RHit {
    std::int64_t sample = 0;
    int voice = 0;
    float acc = 0.0f;
    bool tie = false;  // within the double-precision rounding band of a half sample
};

struct RScen {
    double sr = 48000.0;
    double bpm = 120.0;
    double ppq0 = 0.0;
    bool internal = false;
    std::int64_t total = 0;
    Settings st{};
    int selected = 0;
    PatternBank bank{};
};

Scale validScaleR(Scale s) { return static_cast<unsigned>(s) < 5U ? s : Scale::Sixteenth; }

// Step length in 1/24 quarter note.
std::int64_t stepUnits(Scale s) {
    switch (validScaleR(s)) {
        case Scale::ThirtySecond: return 3;
        case Scale::EighthTriplet: return 8;
        case Scale::SixteenthTriplet: return 4;
        case Scale::Eighth: return 12;
        default: return 6;
    }
}

int patLen(const Pattern& p) { return std::clamp<int>(p.length, 1, kMaxSteps); }

int trackLen(const Pattern& p, int v) {
    const auto k = static_cast<std::size_t>(v);
    const int l = p.trackLength[k] != 0 ? p.trackLength[k] : p.length;
    return std::clamp(l, 1, kMaxSteps);
}

int trackShuffle(const Pattern& p, int v) {
    const auto k = static_cast<std::size_t>(v);
    const int t = p.trackShuffle[k] != 0 ? p.trackShuffle[k] - 1 : p.shuffle;
    return std::clamp(t, 0, kMaxShuffleTicks);
}

// §5.3 shuffle, literally: off-beat (0-based odd) steps; t = ticks x swingScale.
long double shuffleQnR(Scale sc, int ticks, float swing, int s, int len) {
    if (s % 2 == 0) return 0.0L;
    if (len % 2 == 1 && s == len - 1) return 0.0L;
    const long double t = static_cast<long double>(ticks) * static_cast<long double>(swing);
    switch (validScaleR(sc)) {
        case Scale::Sixteenth: return t / 96.0L;
        case Scale::ThirtySecond: return 2.0L * std::floor(t / 4.0L) / 96.0L;
        case Scale::Eighth: return 2.0L * t / 96.0L;
        default: return 0.0L;
    }
}

float voltsR(int code) {
    return AccentDac::kVolts * static_cast<float>(std::clamp(code, 0, AccentDac::kMaxCode)) /
           static_cast<float>(AccentDac::kMaxCode);
}

std::int64_t floorModR(std::int64_t a, std::int64_t m) { return ((a % m) + m) % m; }

std::vector<RHit> oracleHits(const RScen& sc, long double tieBand) {
    std::vector<RHit> out;
    const long double spp = static_cast<long double>(sc.sr) * 60.0L / static_cast<long double>(sc.bpm);
    const Settings& st = sc.st;
    int listLen = 0;
    if (st.play == PlayMode::Chain) listLen = std::min<int>(sc.bank.chain.length, kMaxChainLength);
    if (st.play == PlayMode::Track)
        listLen = std::min<int>(sc.bank.tracks[st.songTrack].length, kMaxSongLength);
    auto base = [&](std::int64_t m) -> int {
        if (listLen == 0) return sc.selected;
        const auto i = static_cast<std::size_t>(floorModR(m, listLen));
        const int e = st.play == PlayMode::Chain ? sc.bank.chain.entries[i] : sc.bank.tracks[st.songTrack].entries[i];
        return std::min(e, kNumPatterns - 1);
    };
    auto pat = [&](std::int64_t m) -> int {
        const int b = base(m);
        const int n = autoFillMeasures(st.autoFill);
        if (n > 0 && floorModR(m, n) == n - 1) return patternIndex(bankOf(b), groupOf(b), st.fillSlot);
        return b;
    };
    auto mUnits = [&](int pi) {
        const Pattern& p = sc.bank.patterns[static_cast<std::size_t>(pi)];
        return static_cast<std::int64_t>(patLen(p)) * stepUnits(p.scale);
    };
    std::int64_t period = 1;
    {
        std::int64_t a = std::max(1, listLen), b = std::max(1, autoFillMeasures(st.autoFill));
        const std::int64_t prod = a * b;
        while (b != 0) {
            const std::int64_t t = a % b;
            a = b;
            b = t;
        }
        period = prod / a;
    }
    std::int64_t cycU = 0;
    for (std::int64_t k = 0; k < period; ++k) cycU += mUnits(pat(k));
    const long double q0 = sc.ppq0;
    const auto target = static_cast<std::int64_t>(std::floor(q0 * 24.0L));
    const std::int64_t cyc = target >= 0 ? target / cycU : -((-target + cycU - 1) / cycU);
    std::int64_t m = cyc * period;
    std::int64_t su = cyc * cycU;
    while (su + mUnits(pat(m)) <= target) su += mUnits(pat(m++));
    while (su > target) su -= mUnits(pat(--m));

    const long double endQ = q0 + static_cast<long double>(sc.total) / spp + 1.0L;
    const int totalCodes =
        static_cast<int>(static_cast<float>(AccentDac::kTotalAccentCodes) * st.totalAccent + 0.5f);
    for (; static_cast<long double>(su) / 24.0L < endQ; su += mUnits(pat(m)), ++m) {
        const long double start = static_cast<long double>(su) / 24.0L;
        const int pi = pat(m);
        const Pattern& p = sc.bank.patterns[static_cast<std::size_t>(pi)];
        const int len = patLen(p);
        const Scale scl = validScaleR(p.scale);
        const long double sq = static_cast<long double>(stepUnits(scl)) / 24.0L;
        const int var = st.variation == VariationMode::B ? 1
                        : st.variation == VariationMode::AB ? static_cast<int>(floorModR(m, 2))
                                                             : 0;
        for (int s = 0; s < len; ++s) {
            const long double tq = start + s * sq;
            const long double baseX = (tq - q0) * spp + 0.5L;
            if (baseX < -tieBand) continue;  // before the start: not fired (§5.1)
            if (std::floor(baseX) >= static_cast<long double>(sc.total)) continue;
            const bool accentOn = p.at(var, kAccentTrack, s).on();
            const long double jit =
                st.jitter ? static_cast<long double>(jitterMs(st.jitterSeed, m, s)) * 0.001L * sc.sr : 0.0L;
            for (int v = 0; v < kNumVoices; ++v) {
                const int tl = trackLen(p, v);
                const auto idx = static_cast<int>(floorModR(m * len + s, tl));
                const Step c = p.at(var, v, idx);
                if (!c.on()) continue;
                if (!probabilityPasses(c.probabilityCode(), pi, m, s, v)) continue;
                const int code = (c.level() == StepLevel::Weak ? AccentDac::kWeakCode : AccentDac::kNormalCode) +
                                 (c.level() == StepLevel::Accent ? AccentDac::kVoiceAccentCodes : 0) +
                                 (accentOn ? totalCodes : 0);
                const float a = voltsR(code);
                const long double t0 = tq + shuffleQnR(scl, trackShuffle(p, v), st.swingScale, s, len);
                const int n = c.subSteps();
                const long double flamS = static_cast<long double>(std::min<int>(p.flamMs, kMaxFlamMs)) * 0.001L * sc.sr;
                for (int k = 0; k < n; ++k) {
                    float ak = a;
                    if (n > 1) {
                        const auto s0 = static_cast<float>(std::min<int>(p.subStart, 100));
                        const auto s1 = static_cast<float>(std::min<int>(p.subEnd, 100));
                        ak = a * (s0 + (s1 - s0) * static_cast<float>(k) / static_cast<float>(n - 1)) / 100.0f;
                    }
                    const long double x = (t0 + k * sq / n - q0) * spp + jit + 0.5L;
                    auto push = [&](long double xx, float aa) {
                        auto smp = static_cast<std::int64_t>(std::floor(xx));
                        const long double fr = xx - std::floor(xx);
                        const bool tie = fr < tieBand || fr > 1.0L - tieBand;
                        if (smp < 0) smp = 0;
                        if (smp < sc.total) out.push_back({smp, v, aa, tie});
                    };
                    if (k == 0 && c.flam() && flamS >= 1.0L) {
                        push(x, ak * static_cast<float>(std::min<int>(p.flamGrace, 100)) / 100.0f);
                        push(x + flamS, ak);
                    } else {
                        push(x, ak);
                    }
                }
            }
        }
    }
    return out;
}

std::vector<RHit> runSeq(const RScen& sc, std::mt19937_64& rng, int maxBlock) {
    static Sequencer seq;  // large object: keep it off the stack
    seq.prepare(sc.sr);
    seq.setBank(sc.bank);
    seq.selectPattern(sc.selected);
    std::vector<RHit> out;
    EventBuffer eb;
    Transport tr;
    tr.hostValid = !sc.internal;
    tr.playing = true;
    tr.bpm = sc.bpm;
    Settings st = sc.st;
    st.mode = sc.internal ? Mode::Internal : Mode::HostSync;
    st.run = true;
    st.internalBpm = sc.bpm;
    std::uniform_int_distribution<int> blocks(1, maxBlock);
    const long double spq = static_cast<long double>(sc.sr) * 60.0L / static_cast<long double>(sc.bpm);
    std::int64_t pos = 0;
    while (pos < sc.total) {
        const auto n = static_cast<int>(std::min<std::int64_t>(blocks(rng), sc.total - pos));
        tr.ppq = static_cast<double>(static_cast<long double>(sc.ppq0) + static_cast<long double>(pos) / spq);
        eb.clear();
        seq.process(tr, st, n, eb, 0);
        for (int i = 0; i < eb.count; ++i) {
            const Event& e = eb.events[static_cast<std::size_t>(i)];
            REQUIRE(e.offset >= 0);
            REQUIRE(e.offset < n);
            out.push_back({pos + e.offset, static_cast<int>(e.voice), e.hit.acc, false});
        }
        pos += n;
    }
    return out;
}

Pattern randomPattern(std::mt19937_64& r, bool garbage) {
    auto u = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(r); };
    auto byte = [&](int a, int b) { return static_cast<std::uint8_t>(u(a, b)); };
    Pattern p;
    p.length = garbage ? byte(0, 255) : byte(1, u(0, 3) == 0 ? 64 : 16);
    p.scale = static_cast<Scale>(garbage ? u(0, 7) : u(0, 4));
    p.shuffle = garbage ? byte(0, 255) : byte(0, kMaxShuffleTicks);
    p.flamMs = garbage ? byte(0, 255) : byte(kMinFlamMs, kMaxFlamMs);
    p.flamGrace = garbage ? byte(0, 255) : byte(0, 100);
    p.subStart = garbage ? byte(0, 255) : byte(0, 100);
    p.subEnd = garbage ? byte(0, 255) : byte(0, 100);
    for (int v = 0; v < kNumVoices; ++v) {
        const auto k = static_cast<std::size_t>(v);
        if (u(0, 3) == 0) p.trackLength[k] = garbage ? byte(0, 255) : byte(1, 64);
        if (u(0, 3) == 0) p.trackShuffle[k] = garbage ? byte(0, 255) : byte(1, kMaxShuffleTicks + 1);
    }
    const int density = u(1, 6);
    for (int var = 0; var < kNumVariations; ++var)
        for (int t = 0; t < kNumTracks; ++t)
            for (int s = 0; s < kMaxSteps; ++s)
                if (u(0, density) == 0) {
                    Step c;
                    c.bits = byte(0, 255);
                    if (u(0, 2) != 0) c.bits = static_cast<std::uint8_t>(c.bits & 0x1FU);
                    p.at(var, t, s) = c;
                }
    return p;
}

struct Totals {
    long scenarios = 0, partitions = 0, hits = 0, mismatches = 0;
};

void runScenarios(int count, std::uint64_t seed, bool far, Totals& tot) {
    std::mt19937_64 rng(seed);
    auto u = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
    auto ud = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    static const double kRates[] = {22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0};
    // Far song positions: the double PPQ the host passes is only good to
    // ~1e-8 qn there, so accept a one-sample move inside a wider band.
    const long double tieBand = far ? 3.0e-3L : 1.0e-4L;
    for (int i = 0; i < count; ++i) {
        RScen sc;
        sc.sr = kRates[u(0, 5)];
        sc.internal = !far && u(0, 3) == 0;
        sc.bpm = sc.internal ? ud(20.0, 400.0) : (u(0, 4) == 0 ? ud(1.0, 1000.0) : ud(40.0, 250.0));
        if (u(0, 3) == 0) sc.bpm = std::round(sc.bpm);
        if (far)
            sc.ppq0 = ud(1.0e5, 5.0e7);
        else
            sc.ppq0 = sc.internal ? 0.0 : (u(0, 2) == 0 ? 0.0 : ud(-8.0, 200.0));
        if (!sc.internal && u(0, 4) == 0) sc.ppq0 = std::round(sc.ppq0 * 4.0) / 4.0;
        const double secs = sc.bpm < 20.0 ? 12.0 : 3.0;
        sc.total = static_cast<std::int64_t>(secs * std::min(sc.sr, 96000.0));
        const bool garbage = u(0, 4) == 0;
        Settings& st = sc.st;
        st.totalAccent = static_cast<float>(ud(0.0, 1.0));
        st.swingScale = u(0, 2) == 0 ? 1.0f : static_cast<float>(ud(0.0, 1.0));
        st.variation = static_cast<VariationMode>(u(0, 2));
        st.autoFill = static_cast<AutoFill>(u(0, 5));
        st.fillSlot = static_cast<std::uint8_t>(u(0, 15));
        st.play = static_cast<PlayMode>(u(0, 2));
        st.songTrack = static_cast<std::uint8_t>(u(0, kNumSongTracks - 1));
        st.jitter = u(0, 2) == 0;
        st.jitterSeed = static_cast<std::uint32_t>(rng());
        for (int k = 0; k < 6; ++k) sc.bank.patterns[static_cast<std::size_t>(u(0, 95))] = randomPattern(rng, garbage);
        sc.selected = u(0, 95);
        sc.bank.patterns[static_cast<std::size_t>(sc.selected)] = randomPattern(rng, garbage);
        sc.bank.chain.length = static_cast<std::uint8_t>(u(0, 8));
        for (auto& e : sc.bank.chain.entries) e = static_cast<std::uint8_t>(u(0, 95));
        auto& track = sc.bank.tracks[st.songTrack];
        track.length = static_cast<std::uint16_t>(u(0, 12));
        for (auto& e : track.entries) e = static_cast<std::uint8_t>(u(0, garbage ? 255 : 95));
        for (int k = 0; k < 8; ++k)
            sc.bank.patterns[sc.bank.chain.entries[static_cast<std::size_t>(k)]] = randomPattern(rng, garbage);
        for (int k = 0; k < 12; ++k)
            sc.bank.patterns[std::min<std::size_t>(track.entries[static_cast<std::size_t>(k)], 95)] =
                randomPattern(rng, garbage);

        const auto ref = oracleHits(sc, tieBand);
        tot.hits += static_cast<long>(ref.size());
        ++tot.scenarios;
        for (int part = 0; part < 3; ++part) {
            const int maxBlock = part == 0 ? 3 : (part == 1 ? u(2, 512) : 8192);
            const auto got = runSeq(sc, rng, maxBlock);
            ++tot.partitions;
            // Match per voice: same acc, same sample (one sample either way at a tie).
            std::multimap<int, RHit> left;
            for (const auto& h : ref) left.insert({h.voice, h});
            long extra = 0;
            for (const auto& g : got) {
                auto [lo, hi] = left.equal_range(g.voice);
                bool found = false;
                for (auto it = lo; it != hi; ++it) {
                    const RHit& h = it->second;
                    if (std::llabs(h.sample - g.sample) <= (h.tie ? 1 : 0) && std::fabs(h.acc - g.acc) < 1.0e-5f) {
                        left.erase(it);
                        found = true;
                        break;
                    }
                }
                if (!found) ++extra;
            }
            if (extra != 0 || !left.empty()) {
                ++tot.mismatches;
                UNSCOPED_INFO("scenario " << i << " part " << part << " sr " << sc.sr << " bpm " << sc.bpm << " ppq0 "
                                          << sc.ppq0 << " internal " << sc.internal << " garbage " << garbage
                                          << " missing " << left.size() << " extra " << extra);
            }
        }
    }
}

}  // namespace

TEST_CASE("review oracle: 22.05..384 kHz, garbage patterns, partitions 1..8192", "[seq][review]") {
    Totals tot;
    runScenarios(160, 0x5EED0001ULL, false, tot);
    INFO("scenarios " << tot.scenarios << " partitions " << tot.partitions << " hits " << tot.hits);
    CHECK(tot.hits > 20000);
    CHECK(tot.mismatches == 0);
}

TEST_CASE("review oracle: far song positions (1e5..5e7 quarter notes)", "[seq][review]") {
    Totals tot;
    runScenarios(60, 0x5EED0002ULL, true, tot);
    INFO("scenarios " << tot.scenarios << " partitions " << tot.partitions << " hits " << tot.hits);
    CHECK(tot.hits > 5000);
    CHECK(tot.mismatches == 0);
}
