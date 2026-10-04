// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Second independent reference for the sequencer (review pass 2). It shares no
// code with Sequencer.cpp or with seq_oracle.cpp and covers what that oracle
// does not: tempo changes on every tempo block, relocations (forward and
// backward jumps, loops), stop/start gaps, zero-length calls and probability,
// together with chains, song tracks and timing jitter.
//
// Model, straight from architecture.md §5: the measure structure is a pure
// function of PPQ from 0 (chain/track entry, auto fill from the playing
// pattern's group, variation by measure index); every hit has a musical time
// th (step + shuffle + sub-step) and a delay in samples (flam + jitter). The host reports one tempo per tempo block; inside such a block
// the sample of a hit is blockStart + round((th - ppq0) * spp + extra). A hit
// belongs to the first tempo block in which that offset is below the block
// length (offset clamped at 0). A step is decided in the first block that
// contains its own (unshuffled) sample. After a start or a relocation, steps
// whose sample lies before the range start are skipped, and hits still waiting
// at a relocation or stop are dropped.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

#include "mw909/seq/MidiFile.h"
#include "mw909/MidiMap.h"
#include "mw909/seq/PatternOps.h"
#include "mw909/seq/SeqHash.h"
#include "mw909/seq/Sequencer.h"

using namespace mw909;
using namespace mw909::seq;

namespace {

struct RHit {
    std::int64_t sample = 0;
    int voice = 0;
    float volts = 0.0f;
    int level = 0;
    bool ta = false;
    bool optional = false;  // probability < 1: may be absent
    bool tie = false;       // within 1e-5 samples of a rounding tie: may move by one
};

struct Got {
    std::int64_t sample;
    int voice;
    float volts;
    int level;
    bool ta;
    bool operator==(const Got&) const = default;
};

// One tempo block of the host timeline.
struct TBlock {
    int n = 0;
    bool playing = true;
    double ppq0 = 0.0;
    double bpm = 120.0;
    bool jump = false;  // the transport was relocated at the start of this block
};

struct World {
    double sr = 48000.0;
    bool internal = false;
    PatternBank bank{};
    int selected = 0;
    Settings st{};
};

long double qnOf(Scale s) {
    switch (s) {
        case Scale::EighthTriplet: return 1.0L / 3.0L;
        case Scale::SixteenthTriplet: return 1.0L / 6.0L;
        case Scale::Sixteenth: return 0.25L;
        case Scale::ThirtySecond: return 0.125L;
        case Scale::Eighth: return 0.5L;
        default: return 0.25L;
    }
}

std::int64_t fmod64(std::int64_t a, std::int64_t m) { return ((a % m) + m) % m; }

int clampLen(int l) { return l < 1 ? 1 : (l > kMaxSteps ? kMaxSteps : l); }

int patternOfMeasure(const World& w, std::int64_t m) {
    int base = w.selected;
    if (w.st.play == PlayMode::Chain && w.bank.chain.length > 0) {
        const int len = std::min<int>(w.bank.chain.length, kMaxChainLength);
        base = std::min<int>(w.bank.chain.entries[static_cast<std::size_t>(fmod64(m, len))], kNumPatterns - 1);
    }
    const SongTrack& t = w.bank.tracks[w.st.songTrack];
    if (w.st.play == PlayMode::Track && t.length > 0) {
        const int len = std::min<int>(t.length, kMaxSongLength);
        base = std::min<int>(t.entries[static_cast<std::size_t>(fmod64(m, len))], kNumPatterns - 1);
    }
    const int n = autoFillMeasures(w.st.autoFill);
    if (n > 0 && fmod64(m, n) == n - 1) return (base / 16) * 16 + w.st.fillSlot;  // same group, slot fillSlot
    return base;
}

long double measureQn(const World& w, std::int64_t m) {
    const Pattern& p = w.bank.patterns[static_cast<std::size_t>(patternOfMeasure(w, m))];
    return clampLen(p.length) * qnOf(p.scale);
}

struct RawHit {
    long double th;
    long double extra;
    long double stepT;
    int voice;
    float volts;
    int level;
    bool ta;
    bool optional;
};

// All hits of measures overlapping [from, to) in PPQ (walking from PPQ 0).
void hitsBetween(const World& w, long double from, long double to, std::vector<RawHit>& out) {
    std::int64_t m = 0;
    long double start = 0.0L;
    if (from >= 0.0L) {
        while (start + measureQn(w, m) <= from) start += measureQn(w, m++);
    } else {
        while (start > from) start -= measureQn(w, --m);
    }
    const int taCodes = static_cast<int>(39.0f * std::clamp(w.st.totalAccent, 0.0f, 1.0f) + 0.5f);
    const long double swing = std::clamp(w.st.swingScale, 0.0f, 1.0f);
    for (; start < to; start += measureQn(w, m++)) {
        const Pattern& p = w.bank.patterns[static_cast<std::size_t>(patternOfMeasure(w, m))];
        const int L = clampLen(p.length);
        const long double q = qnOf(p.scale);
        const int var = w.st.variation == VariationMode::A ? 0 : (w.st.variation == VariationMode::B ? 1 : static_cast<int>(fmod64(m, 2)));
        const long double flamSamples = std::min<int>(p.flamMs, 100) * 0.001L * w.sr;
        const float grace = static_cast<float>(std::min<int>(p.flamGrace, 100)) / 100.0f;
        const auto ra = static_cast<float>(std::min<int>(p.subStart, 100));
        const auto rb = static_cast<float>(std::min<int>(p.subEnd, 100));
        for (int s = 0; s < L; ++s) {
            const long double t = start + s * q;
            const bool ac = p.at(var, kAccentTrack, s).on();
            const long double jit = w.st.jitter ? jitterMs(w.st.jitterSeed, m, s) * 0.001L * w.sr : 0.0L;
            for (int v = 0; v < kNumVoices; ++v) {
                // Shuffle: odd steps, in 1/96 beat, by scale (§5.3).
                const int ticks = std::min(p.trackShuffle[static_cast<std::size_t>(v)] != 0
                                               ? p.trackShuffle[static_cast<std::size_t>(v)] - 1
                                               : static_cast<int>(p.shuffle),
                                           16);
                const long double tk = ticks * swing;
                long double sh = 0.0L;
                if (s % 2 == 1 && !(L % 2 == 1 && s == L - 1)) {
                    if (p.scale == Scale::Sixteenth) sh = tk / 96.0L;
                    if (p.scale == Scale::ThirtySecond) sh = 2.0L * std::floor(tk / 4.0L) / 96.0L;
                    if (p.scale == Scale::Eighth) sh = 2.0L * tk / 96.0L;
                }
                const int tl = p.trackLength[static_cast<std::size_t>(v)] == 0 ? L : clampLen(p.trackLength[static_cast<std::size_t>(v)]);
                const Step c = p.at(var, v, static_cast<int>(fmod64(m * L + s, tl)));
                if (c.level() == StepLevel::Off) continue;
                const int lv = static_cast<int>(c.level());
                const float volts = AccentDac::volts((lv == 1 ? 12 : 24) + (lv == 3 ? 16 : 0) + (ac ? taCodes : 0));
                const bool opt = c.probabilityCode() != 0;
                const int subs = c.subSteps();
                for (int k = 0; k < subs; ++k) {
                    const long double th = t + sh + k * (q / subs);
                    const float vk =
                        subs > 1 ? volts * (ra + (rb - ra) * static_cast<float>(k) / static_cast<float>(subs - 1)) / 100.0f
                                 : volts;
                    if (k == 0 && c.flam() && flamSamples >= 1.0L) {
                        const bool light = p.flamGrace < 100;
                        out.push_back({th, jit, t, v, vk * grace, light ? 1 : lv, light ? false : ac, opt});
                        out.push_back({th, jit + flamSamples, t, v, vk, lv, ac, opt});
                    } else {
                        out.push_back({th, jit, t, v, vk, lv, ac, opt});
                    }
                }
            }
        }
    }
}

double clampedRate(const World& w, double hostBpm) {
    return w.internal ? std::clamp(hostBpm, 20.0, 400.0) : std::clamp(hostBpm, 1.0, 1000.0);
}

// Expected hits for a list of tempo blocks (absolute sample positions).
std::vector<RHit> reference(const World& w, const std::vector<TBlock>& tb) {
    std::vector<RHit> out;
    std::size_t b = 0;
    std::int64_t pos = 0;
    std::vector<std::int64_t> starts;
    for (const TBlock& x : tb) {
        starts.push_back(pos);
        pos += x.n;
    }
    while (b < tb.size()) {
        if (!tb[b].playing) {
            ++b;
            continue;
        }
        // A segment: [b, e) of playing blocks with no jump after b.
        std::size_t e = b + 1;
        while (e < tb.size() && tb[e].playing && !tb[e].jump) ++e;
        const long double segFrom = tb[b].ppq0 - 1.0L;
        const long double segTo = tb[e - 1].ppq0 + tb[e - 1].n * tb[e - 1].bpm / (60.0L * w.sr) + 1.0L;
        std::vector<RawHit> raw;
        hitsBetween(w, segFrom, segTo, raw);
        auto offsetIn = [&](std::size_t k, long double t, long double extra, bool& tie) {
            const long double spp = w.sr * 60.0L / clampedRate(w, tb[k].bpm);
            const long double x = (t - tb[k].ppq0) * spp + extra + 0.5L;
            const long double fl = std::floor(x);
            tie = tie || (x - fl) < 1e-5L || (fl + 1.0L - x) < 1e-5L;
            return static_cast<std::int64_t>(std::floor(x + 1e-6L));
        };
        for (const RawHit& h : raw) {
            bool tie = false;
            // The step: decided in the first block that holds its own sample.
            std::size_t k = b;
            std::int64_t so = offsetIn(k, h.stepT, 0.0L, tie);
            if (so < 0) continue;  // before the start / relocation point: skipped
            while (k < e && offsetIn(k, h.stepT, 0.0L, tie) >= tb[k].n) ++k;
            if (k == e) continue;  // step not reached in this segment
            // The hit: first block from k on that holds it.
            while (k < e) {
                const std::int64_t o = offsetIn(k, h.th, h.extra, tie);
                if (o < tb[k].n) {
                    out.push_back({starts[k] + std::max<std::int64_t>(0, o), h.voice, h.volts, h.level, h.ta, h.optional,
                                   tie});
                    break;
                }
                ++k;
            }
        }
        b = e;
    }
    return out;
}

// Runs the sequencer over the tempo blocks, each split into random process()
// calls (sub-range PPQ advanced at that block's tempo, as Machine does), with
// random zero-length calls in between. Returns false on event-buffer overflow.
bool runImpl(Sequencer& s, const World& w, const std::vector<TBlock>& tb, std::mt19937& rng, int maxSplit,
             std::vector<Got>& got) {
    auto buf = std::make_unique<EventBuffer>();
    std::int64_t pos = 0;
    for (const TBlock& x : tb) {
        Settings st = w.st;
        Transport tr;
        if (w.internal) {
            st.run = x.playing;
            st.internalBpm = x.bpm;
        } else {
            tr.hostValid = true;
            tr.playing = x.playing;
            tr.bpm = x.bpm;
        }
        int done = 0;
        while (done < x.n) {
            const int len = std::min(x.n - done, 1 + static_cast<int>(rng() % static_cast<unsigned>(maxSplit)));
            tr.ppq = x.ppq0 + static_cast<double>(done) * x.bpm / (60.0 * w.sr);
            buf->clear();
            const int base = static_cast<int>(rng() % 3);
            if (rng() % 8 == 0) s.process(tr, st, 0, *buf, base);
            REQUIRE(buf->count == 0);
            s.process(tr, st, len, *buf, base);
            if (buf->count >= EventBuffer::kCapacity) return false;
            int prev = base;
            for (int i = 0; i < buf->count; ++i) {
                const Event& ev = buf->events[static_cast<std::size_t>(i)];
                REQUIRE(ev.offset >= prev);
                REQUIRE(ev.offset < base + len);
                prev = ev.offset;
                got.push_back({pos + done + ev.offset - base, static_cast<int>(ev.voice), ev.hit.acc,
                               static_cast<int>(ev.level), ev.totalAccent});
            }
            if (!x.playing) REQUIRE(s.currentStep() == -1);
            done += len;
        }
        pos += x.n;
    }
    return true;
}

// Matches got against want; returns the number of unexplained differences.
int compare(const std::vector<RHit>& want, const std::vector<Got>& got, std::string& why) {
    std::multimap<std::tuple<std::int64_t, int, float, int, bool>, bool> pool;
    for (const Got& g : got) pool.insert({{g.sample, g.voice, g.volts, g.level, g.ta}, false});
    int bad = 0;
    for (const RHit& h : want) {
        auto it = pool.find({h.sample, h.voice, h.volts, h.level, h.ta});
        if (it == pool.end() && h.tie) {
            it = pool.find({h.sample - 1, h.voice, h.volts, h.level, h.ta});
            if (it == pool.end()) it = pool.find({h.sample + 1, h.voice, h.volts, h.level, h.ta});
        }
        if (it != pool.end()) {
            pool.erase(it);
            continue;
        }
        if (h.optional) continue;
        if (bad < 4)
            why += "missing {" + std::to_string(h.sample) + " v" + std::to_string(h.voice) + " " +
                   std::to_string(h.volts) + " L" + std::to_string(h.level) + "} ";
        ++bad;
    }
    for (const auto& kv : pool) {
        if (bad < 8)
            why += "extra {" + std::to_string(std::get<0>(kv.first)) + " v" + std::to_string(std::get<1>(kv.first)) +
                   " " + std::to_string(std::get<2>(kv.first)) + "} ";
        ++bad;
    }
    return bad;
}

Pattern randomPat(std::mt19937& rng, float density, bool prob) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Pattern p;
    p.length = static_cast<std::uint8_t>(rng() % 4 == 0 ? 1 + rng() % 64 : (rng() % 2 ? 16 : 1 + rng() % 24));
    p.scale = static_cast<Scale>(rng() % 5);
    p.shuffle = static_cast<std::uint8_t>(rng() % 2 ? 0 : rng() % 17);
    p.flamMs = static_cast<std::uint8_t>(rng() % 8 == 0 ? 0 : rng() % 101);
    p.flamGrace = static_cast<std::uint8_t>(rng() % 2 ? 100 : rng() % 101);
    p.subStart = static_cast<std::uint8_t>(rng() % 2 ? 100 : rng() % 101);
    p.subEnd = static_cast<std::uint8_t>(rng() % 2 ? 100 : rng() % 101);
    for (int v = 0; v < kNumVoices; ++v) {
        if (rng() % 6 == 0) p.trackLength[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(1 + rng() % 64);
        if (rng() % 6 == 0) p.trackShuffle[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(1 + rng() % 17);
    }
    for (int var = 0; var < 2; ++var)
        for (int t = 0; t < kNumTracks; ++t)
            for (int s = 0; s < kMaxSteps; ++s) {
                if (u(rng) >= density) continue;
                Step& c = p.at(var, t, s);
                c.setLevel(static_cast<StepLevel>(1 + rng() % 3));
                if (t == kAccentTrack) continue;
                if (rng() % 6 == 0) c.setFlam(true);
                if (rng() % 6 == 0) c.setSubSteps(static_cast<int>(1 + rng() % 4));
                if (prob && rng() % 6 == 0) c.setProbabilityCode(static_cast<int>(rng() % 8));
            }
    return p;
}

void randomWorld(World& w, std::mt19937& rng) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double rates[] = {44100.0, 48000.0, 96000.0, 192000.0, 22050.0};
    w.sr = rates[rng() % 5];
    w.internal = rng() % 3 == 0;
    const int groupBase = 16 * static_cast<int>(rng() % 6);
    for (int i = 0; i < 16; ++i) {
        const float density = 0.05f + 0.25f * static_cast<float>(u(rng));
        const bool prob = rng() % 2 == 0;
        w.bank.patterns[static_cast<std::size_t>(groupBase + i)] = randomPat(rng, density, prob);
    }
    // A second group (another bank half the time) for chains and tracks that cross groups.
    const int other = (groupBase + 16 * (1 + static_cast<int>(rng() % 5))) % kNumPatterns;
    for (int i = 0; i < 16; ++i)
        w.bank.patterns[static_cast<std::size_t>(other + i)] = randomPat(rng, 0.05f, false);
    w.selected = groupBase + static_cast<int>(rng() % 16);
    const auto play = static_cast<PlayMode>(rng() % 3);
    w.bank.chain.length = static_cast<std::uint8_t>(play == PlayMode::Chain ? 1 + rng() % 7 : 0);
    for (int i = 0; i < kMaxChainLength; ++i) {
        const int b = rng() % 2 ? groupBase : other;  // separate statements: a fixed rng() order
        w.bank.chain.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b + static_cast<int>(rng() % 16));
    }
    const auto songTrack = static_cast<std::uint8_t>(rng() % kNumSongTracks);
    SongTrack& t = w.bank.tracks[songTrack];
    t.length = static_cast<std::uint16_t>(play == PlayMode::Track ? 1 + rng() % 20 : 0);
    for (int i = 0; i < t.length; ++i) {
        const int b = rng() % 2 ? groupBase : other;
        t.entries[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(b + static_cast<int>(rng() % 16));
    }
    Settings& st = w.st;
    st.mode = w.internal ? Mode::Internal : Mode::HostSync;
    st.totalAccent = static_cast<float>(u(rng));
    st.swingScale = rng() % 2 ? 1.0f : static_cast<float>(u(rng));
    st.variation = static_cast<VariationMode>(rng() % 3);
    st.autoFill = static_cast<AutoFill>(rng() % 6);
    st.fillSlot = static_cast<std::uint8_t>(rng() % 16);
    st.play = play;
    st.songTrack = songTrack;
    st.jitter = rng() % 3 == 0;
    st.jitterSeed = static_cast<std::uint32_t>(rng());
}

std::vector<TBlock> randomTimeline(const World& w, std::mt19937& rng, std::int64_t total) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<TBlock> tb;
    double ppq = w.internal ? 0.0 : (rng() % 3 == 0 ? std::round(u(rng) * 8.0) : (u(rng) - 0.25) * 30.0);
    double bpm = 20.0 + u(rng) * 380.0;
    bool playing = true;
    const bool tempoEveryBlock = rng() % 2 == 0;
    std::int64_t pos = 0;
    while (pos < total) {
        TBlock x;
        const unsigned maxN = rng() % 4 == 0 ? 8192U : 1500U;
        x.n = static_cast<int>(1 + rng() % maxN);
        if (tempoEveryBlock) bpm = std::clamp(bpm * (0.8 + 0.4 * u(rng)), 20.0, 400.0);
        if (!w.internal && rng() % 40 == 0) bpm = 1.0 + u(rng) * 999.0;  // hosts outside 20..400
        x.bpm = bpm;
        const auto ev = static_cast<unsigned>(rng() % 30);
        if (ev == 0) {
            playing = !playing;  // stop or start
            if (playing && w.internal) ppq = 0.0;
            x.jump = true;
        } else if (ev == 1 && !w.internal && playing) {
            // Relocation: a jump of at least 5 ms in either direction, or a loop back.
            const double spp = w.sr * 60.0 / std::clamp(bpm, 1.0, 1000.0);
            const double minQn = std::max(0.005 * w.sr, 3.0) / spp;
            const double d = minQn + u(rng) * 6.0;
            ppq += rng() % 2 ? d : -d;
            x.jump = true;
        }
        x.playing = playing;
        x.ppq0 = ppq;
        if (w.internal && !playing) x.ppq0 = 0.0;
        tb.push_back(x);
        if (playing) ppq += x.n * std::clamp(bpm, w.internal ? 20.0 : 1.0, w.internal ? 400.0 : 1000.0) / (60.0 * w.sr);
        pos += x.n;
    }
    return tb;
}

}  // namespace

TEST_CASE("reference: chains, tempo changes, jumps, stop/start and probability over random partitions",
          "[seq][reference]") {
    std::mt19937 rng(0x5EC0DE2U);
    int compared = 0, hits = 0;
    for (int it = 0; it < 800; ++it) {
        auto w = std::make_unique<World>();
        randomWorld(*w, rng);
        const auto total = static_cast<std::int64_t>(w->sr * (0.5 + 4.0 * static_cast<double>(rng() % 1000) / 1000.0));
        auto tb = randomTimeline(*w, rng, total);
        // Internal blocks carry their PPQ from the clamped tempo; recompute so the
        // reference sees the internal clock exactly.
        if (w->internal) {
            double ppq = 0.0;
            bool was = false;
            for (TBlock& x : tb) {
                if (x.playing && !was) ppq = 0.0;
                x.ppq0 = ppq;
                if (x.playing) ppq += x.n * std::clamp(x.bpm, 20.0, 400.0) / (60.0 * w->sr);
                x.jump = x.playing && !was;
                was = x.playing;
            }
        }
        const auto want = reference(*w, tb);
        std::vector<std::vector<Got>> runs;
        for (int maxSplit : {1 << 30, 37, 700}) {
            if (maxSplit == 37 && it % 4 != 0) continue;
            auto s = std::make_unique<Sequencer>();
            s->prepare(w->sr);
            if (it % 2 == 0) {
                s->setBank(w->bank);
            } else {
                for (int i = 0; i < kNumPatterns; ++i) s->setPattern(i, w->bank.patterns[static_cast<std::size_t>(i)]);
                s->setChain(w->bank.chain);
                for (int i = 0; i < kNumSongTracks; ++i) s->setSongTrack(i, w->bank.tracks[static_cast<std::size_t>(i)]);
            }
            s->selectPattern(w->selected);
            std::vector<Got> got;
            std::mt19937 split(static_cast<std::uint32_t>(it * 7 + maxSplit));
            if (!runImpl(*s, *w, tb, split, maxSplit, got)) continue;
            std::string why;
            INFO("scenario " << it << " sr " << w->sr << (w->internal ? " internal" : " host") << " play "
                             << static_cast<int>(w->st.play) << " jitter " << w->st.jitter
                             << " split " << maxSplit << " blocks " << tb.size());
            const int bad = compare(want, got, why);
            INFO(why);
            REQUIRE(bad == 0);
            runs.push_back(got);
            ++compared;
            hits += static_cast<int>(got.size());
        }
        // Probability decisions (and everything else) must not depend on the partition.
        for (std::size_t r = 1; r < runs.size(); ++r) {
            REQUIRE(runs[r] == runs[0]);
        }
    }
    WARN("reference: " << compared << " runs compared, " << hits << " hits");
    CHECK(compared > 1500);
    CHECK(hits > 80000);
}

TEST_CASE("hostile inputs: extreme values every call, 1-sample blocks, NaN/Inf, huge and backward PPQ",
          "[seq][reference][robust]") {
    std::mt19937 rng(0xBADF00DU);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double weirdD[] = {nan, inf, -inf, 0.0, -0.0, 1e-300, -1e-300, 4.9e-324, 1e300, -1e300, 19.999, 400.0001,
                             1e7, -1e7, 1e9, 9.99e8, -9.99e8, 1.0, 1000.0, 1000.0001};
    const float weirdF[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), -1.0f, 0.0f, 1e-45f, 1.0f, 2.0f, 100.0f,
                            100.0001f, 1e30f};
    auto pickD = [&](double normal) { return rng() % 6 == 0 ? weirdD[rng() % std::size(weirdD)] : normal; };
    auto pickF = [&](float normal) { return rng() % 6 == 0 ? weirdF[rng() % std::size(weirdF)] : normal; };
    for (double sr : {44100.0, 1000.0, 1.0e6, 0.0, -48000.0, nan, inf, 3.0e6}) {
        auto s = std::make_unique<Sequencer>();
        s->prepare(sr);
        // Out-of-contract pattern data the sequencer must clamp on read.
        for (int i = 0; i < kNumPatterns; ++i) {
            Pattern p;
            p.length = static_cast<std::uint8_t>(rng() % 256);
            p.scale = static_cast<Scale>(rng() % 256);
            p.shuffle = static_cast<std::uint8_t>(rng() % 256);
            p.flamMs = static_cast<std::uint8_t>(rng() % 256);
            p.flamGrace = static_cast<std::uint8_t>(rng() % 256);
            p.subStart = static_cast<std::uint8_t>(rng() % 256);
            p.subEnd = static_cast<std::uint8_t>(rng() % 256);
            p.reserved = static_cast<std::uint8_t>(rng() % 256);
            for (auto& tl : p.trackLength) tl = static_cast<std::uint8_t>(rng() % 4 == 0 ? rng() % 256 : 0);
            for (auto& ts : p.trackShuffle) ts = static_cast<std::uint8_t>(rng() % 4 == 0 ? rng() % 256 : 0);
            for (auto& var : p.steps)
                for (auto& tr : var)
                    for (auto& c : tr) c.bits = static_cast<std::uint8_t>(rng() % 3 == 0 ? rng() % 256 : 0);
            s->setPattern(i, p);
        }
        Chain ch;
        ch.length = static_cast<std::uint8_t>(rng() % 256);
        for (auto& e : ch.entries) e = static_cast<std::uint8_t>(rng() % 256);
        s->setChain(ch);
        for (int k = 0; k < kNumSongTracks; ++k) {
            SongTrack t;
            t.length = static_cast<std::uint16_t>(rng() % 70000);
            for (auto& e : t.entries) e = static_cast<std::uint8_t>(rng() % 256);
            s->setSongTrack(k, t);
        }
        auto buf = std::make_unique<EventBuffer>();
        double ppq = (u(rng) - 0.5) * 2e7;
        for (int call = 0; call < 40000; ++call) {
            Settings st;
            st.mode = static_cast<Mode>(rng() % 6);
            st.run = rng() % 50 != 0;
            st.internalBpm = pickD(20.0 + u(rng) * 380.0);
            st.totalAccent = pickF(static_cast<float>(u(rng)));
            st.swingScale = pickF(static_cast<float>(u(rng)));
            st.variation = static_cast<VariationMode>(rng() % 5);
            st.autoFill = static_cast<AutoFill>(rng() % 8);
            st.fillSlot = static_cast<std::uint8_t>(rng() % 256);
            st.play = static_cast<PlayMode>(rng() % 5);
            st.songTrack = static_cast<std::uint8_t>(rng() % 256);
            st.jitter = rng() % 2 == 0;
            st.jitterSeed = static_cast<std::uint32_t>(rng());
            st.record = rng() % 2 == 0;
            Transport tr;
            tr.hostValid = rng() % 20 != 0;
            tr.playing = rng() % 100 != 0;
            tr.bpm = pickD(20.0 + u(rng) * 380.0);
            const int n = rng() % 10 == 0 ? 0 : (rng() % 3 == 0 ? 1 : static_cast<int>(1 + rng() % 4096));
            // Forward mostly, sometimes backwards, sometimes frozen, sometimes far away.
            const auto mv = static_cast<unsigned>(rng() % 20);
            if (mv == 0) ppq -= u(rng) * 8.0;
            else if (mv == 1) ppq = (u(rng) - 0.5) * 2e7;
            else if (mv != 2) ppq += n * 3.0 / 48000.0;
            tr.ppq = pickD(ppq);
            switch (rng() % 40) {
                case 0: {
                    const int idx = static_cast<int>(rng() % 120) - 8;
                    s->selectPattern(idx, rng() % 2 == 0);
                    break;
                }
                case 1: s->requestFill(); break;
                case 2: s->notePatternOn(static_cast<int>(rng() % 120) - 8); break;
                case 3: s->notePatternOff(); break;
                case 4: {
                    const auto v = static_cast<VoiceId>(rng() % 20);
                    const bool acc = rng() % 2 == 0;
                    const int off = rng() % 2 ? static_cast<int>(rng() % 100000) - 1000 : std::numeric_limits<int>::max();
                    s->recordHit(v, acc, off);
                    break;
                }
                case 5: s->reset(); break;
                default: break;
            }
            buf->clear();
            if (rng() % 50 == 0) buf->count = static_cast<int>(rng() % EventBuffer::kCapacity);  // prefilled
            const int before = buf->count;
            const int base = static_cast<int>(rng() % 64);
            s->process(tr, st, n, *buf, base);
            REQUIRE(buf->count >= before);
            REQUIRE(buf->count <= EventBuffer::kCapacity);
            int prev = base;
            for (int i = before; i < buf->count; ++i) {
                const Event& e = buf->events[static_cast<std::size_t>(i)];
                REQUIRE(e.offset >= prev);
                REQUIRE(e.offset < base + n);
                prev = e.offset;
                REQUIRE(static_cast<int>(e.voice) < kNumVoices);
                REQUIRE(e.level != StepLevel::Off);
                REQUIRE(static_cast<int>(e.level) <= 3);
                REQUIRE(std::isfinite(e.hit.acc));
                REQUIRE(e.hit.acc >= 0.0f);
                REQUIRE(e.hit.acc <= AccentDac::kVolts);
            }
            if (!s->isPlaying()) {
                REQUIRE(s->currentStep() == -1);
                REQUIRE(buf->count == before);
            }
            REQUIRE(s->currentStep() >= -1);
            REQUIRE(s->currentStep() < kMaxSteps);
            REQUIRE(s->currentPattern() >= 0);
            REQUIRE(s->currentPattern() < kNumPatterns);
            REQUIRE(s->selectedPattern() >= 0);
            REQUIRE(s->selectedPattern() < kNumPatterns);
            REQUIRE(s->queuedPattern() >= -1);
            REQUIRE(s->queuedPattern() < kNumPatterns);
            REQUIRE((s->currentVariation() == 0 || s->currentVariation() == 1));
            REQUIRE(s->tempo() >= 20.0);
            REQUIRE(s->tempo() <= 400.0);
            REQUIRE(s->songPosition() >= -1);
            REQUIRE(s->songPosition() < kMaxSongLength);
        }
    }
}

TEST_CASE("notes-only MIDI import reads back the writer's flams, sub-steps and shuffle at default settings",
          "[seq][reference][midi]") {
    // Regression: the reader quantised every note to its nearest step and only
    // knew flams shorter than half a step, so a default 24 ms flam at 120 BPM
    // on 32nds with sub-steps, any shuffled sub-step, or a late hit wrapped to
    // the top of the measure came back as invented hits on other steps (a third
    // of random patterns at 120 BPM / 24 ms; nearly all with shuffle).
    const std::array<std::uint8_t, kNumVoices> map = kVoiceNote;
    struct Case {
        double bpm;
        int flam;
        int shuffle;  // ticks
    };
    const Case cases[] = {{60, 24, 0},   {60, 48, 8},   {120, 10, 0},  {120, 20, 0}, {120, 24, 6},
                          {120, 36, 12}, {120, 10, 16}, {200, 10, 0},  {290, 20, 12}};
    std::mt19937 rng(3);
    int checked = 0;
    for (const Case& k : cases) {
        for (int c = 0; c < 400; ++c) {
            Pattern p;
            p.length = static_cast<std::uint8_t>(1 + rng() % 64);
            p.scale = static_cast<Scale>(rng() % 5);
            p.shuffle = static_cast<std::uint8_t>(k.shuffle);
            p.flamMs = static_cast<std::uint8_t>(k.flam);
            int notes = 0;
            for (int v = 0; v < kNumVoices; ++v)
                for (int s = 0; s < p.length; ++s) {
                    if (rng() % 5 != 0) continue;
                    Step& x = p.at(0, v, s);
                    x.setLevel(static_cast<StepLevel>(1 + rng() % 3));
                    if (rng() % 3 == 0) x.setFlam(true);
                    if (rng() % 3 == 0) x.setSubSteps(static_cast<int>(1 + rng() % 4));
                    notes += x.subSteps() + (x.flam() ? 1 : 0);
                }
            MidiFileOptions o;
            o.embedPattern = false;
            o.bpm = k.bpm;
            const auto bytes = writeMidiFile(p, 0, map, o);
            // Skip the files where two hits of one key share a tick (the writer
            // keeps one; nothing can recover the other).
            int ons = 0;
            for (std::size_t i = 0; i + 2 < bytes.size(); ++i)
                if (bytes[i] == 0x99 && bytes[i + 2] != 0) ++ons;  // our files: no running status
            if (ons != notes) continue;
            // Skip the documented ambiguity: a sub-step note that shuffle puts
            // exactly on the step grid reads as the next step's onset (unless
            // it is an x3/x4 note wrapped past the measure end).
            {
                const auto T = std::llround(stepLengthQn(p.scale) * 480.0);
                const auto M = T * p.length;
                long double sh = 0.0L;
                if (p.scale == Scale::Sixteenth) sh = k.shuffle / 96.0L;
                if (p.scale == Scale::ThirtySecond) sh = 2.0L * (k.shuffle / 4) / 96.0L;
                if (p.scale == Scale::Eighth) sh = 2.0L * k.shuffle / 96.0L;
                bool ambiguous = false;
                for (int v = 0; v < kNumVoices; ++v)
                    for (int st = 1; st < p.length; st += 2) {
                        const Step x = p.at(0, v, st);
                        const int n = x.subSteps();
                        if (!x.on() || n < 2 || (p.length % 2 == 1 && st == p.length - 1)) continue;
                        for (int q = 1; q < n; ++q) {
                            const auto r = std::llround((st * stepLengthQn(p.scale) + sh +
                                                         q * stepLengthQn(p.scale) / n) * 480.0L);
                            const auto gm = (r % M) % T;
                            if ((gm <= 1 || gm >= T - 1) && !(n >= 3 && r >= M - 1)) ambiguous = true;
                        }
                    }
                if (ambiguous) continue;
            }
            Pattern q;
            q.scale = p.scale;
            REQUIRE(readMidiFile(bytes, map, q, 0));
            INFO("bpm " << k.bpm << " flam " << k.flam << " shuffle " << k.shuffle << " case " << c);
            REQUIRE(q.length == p.length);
            for (int v = 0; v < kNumVoices; ++v)
                for (int s = 0; s < kMaxSteps; ++s) {
                    INFO("voice " << v << " step " << s << " len " << int(p.length) << " scale " << int(p.scale)
                                   << " got " << int(q.at(0, v, s).bits) << " want " << int(p.at(0, v, s).bits)
                                   << " row " << [&] {
                                          std::string r;
                                          for (int z = 0; z < p.length; ++z) r += std::to_string(p.at(0, v, z).bits) + ",";
                                          r += " / ";
                                          for (int z = 0; z < p.length; ++z) r += std::to_string(q.at(0, v, z).bits) + ",";
                                          return r;
                                      }());
                    REQUIRE(q.at(0, v, s) == p.at(0, v, s));
                }
            ++checked;
        }
    }
    WARN("notes-only round trips checked: " << checked);
    CHECK(checked > 2500);
}

TEST_CASE("PatternOps: extreme rotations and an endless MIDI file stay defined", "[seq][reference][ops]") {
    // Regression: setEuclidean computed s - rotation before reducing it, which
    // overflowed (UB) for rotation = INT_MIN.
    Pattern a, b;
    setEuclidean(a, 0, BD, 3, std::numeric_limits<int>::min());
    setEuclidean(b, 0, BD, 3, std::numeric_limits<int>::min() % 16);
    CHECK(a == b);
    setEuclidean(a, 0, BD, 5, std::numeric_limits<int>::max());
    setEuclidean(b, 0, BD, 5, std::numeric_limits<int>::max() % 16);
    CHECK(a == b);
    // Regression: the length came from static_cast<int>(endTick / step), which
    // wraps for files longer than 2^31 steps and gave a 1-step pattern.
    std::vector<std::uint8_t> f{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 0};
    const std::size_t lenAt = f.size() - 4;
    for (int i = 0; i < 1200; ++i) f.insert(f.end(), {0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0x01, 0x00});  // 2^28-1 ticks each
    f.insert(f.end(), {0x00, 0x99, 36, 100, 0x00, 0xFF, 0x2F, 0x00});
    const auto trk = static_cast<std::uint32_t>(f.size() - lenAt - 4);
    for (int k = 0; k < 4; ++k) f[lenAt + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(trk >> (24 - 8 * k));
    Pattern q;
    REQUIRE(readMidiFile(f, kVoiceNote, q, 0));
    CHECK(q.length == kMaxSteps);
}

TEST_CASE("notes-only import of random note soup: no crash, a valid pattern", "[seq][reference][midi]") {
    const std::array<std::uint8_t, kNumVoices> map = kVoiceNote;
    std::mt19937 rng(77);
    for (int c = 0; c < 400; ++c) {
        std::vector<std::uint8_t> f{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 0, 'M', 'T', 'r', 'k', 0, 0, 0, 0};
        const auto div = static_cast<unsigned>(24 + rng() % 960);
        f[12] = static_cast<std::uint8_t>(div >> 8);
        f[13] = static_cast<std::uint8_t>(div & 0xFF);
        if (rng() % 2 != 0) {  // a tempo event, sometimes absurd
            const auto us = static_cast<std::uint32_t>(rng() % 4 == 0 ? 1 + rng() % 100 : 100000 + rng() % 3000000);
            f.insert(f.end(), {0x00, 0xFF, 0x51, 0x03, static_cast<std::uint8_t>(us >> 16), static_cast<std::uint8_t>(us >> 8),
                               static_cast<std::uint8_t>(us)});
        }
        const int n = static_cast<int>(rng() % 600);
        for (int k = 0; k < n; ++k) {
            const auto dt = static_cast<unsigned>(rng() % 3 == 0 ? 0 : rng() % 100);
            f.push_back(static_cast<std::uint8_t>(dt & 0x7F));
            const auto note = static_cast<std::uint8_t>(rng() % 3 == 0 ? 30 + rng() % 40 : map[rng() % kNumVoices]);
            f.insert(f.end(), {0x99, note, static_cast<std::uint8_t>(1 + rng() % 127)});
        }
        f.insert(f.end(), {0x00, 0xFF, 0x2F, 0x00});
        const auto len = static_cast<std::uint32_t>(f.size() - 22);
        for (int k = 0; k < 4; ++k) f[18 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(len >> (24 - 8 * k));
        Pattern q;
        q.scale = static_cast<Scale>(rng() % 5);
        REQUIRE(readMidiFile(f, map, q, static_cast<int>(rng() % 2)));
        REQUIRE(q.length >= 1);
        REQUIRE(q.length <= kMaxSteps);
        for (int var = 0; var < 2; ++var)
            for (int v = 0; v < kNumVoices; ++v)
                for (int s = q.length; s < kMaxSteps; ++s) REQUIRE_FALSE(q.at(var, v, s).on());
    }
}
