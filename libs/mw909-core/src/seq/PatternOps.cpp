// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Pattern editing helpers. See PatternOps.h for the contract of each call.
//
// Randomisation uses the shared dsp::hash32 mixer keyed on (seed, track,
// step), so a result depends only on its arguments: no hidden RNG state.
// OH/CH exclusion. Playback reads hat cell (i mod L) at pass-wide step i
// (architecture.md §5.3), and i runs over every integer, so OH cell a and CH
// cell b meet on some step exactly when a = b (mod gcd(L_OH, L_CH)).
// randomizePattern resolves collisions per residue class of that gcd, which
// is exact for any pair of lengths (with equal lengths a class is one step).
// randomizeTrack only clears the other hat index-wise, because it must not
// wipe a track the user did not ask to randomize; with equal lengths (the
// normal case) that is exact too.

#include "mw909/seq/PatternOps.h"

#include "mw909/dsp/Math.h"

namespace mw909::seq {

namespace {

bool validVariation(int v) noexcept { return v >= 0 && v < kNumVariations; }
bool validTrack(int t) noexcept { return t >= 0 && t < kNumTracks; }

int clampedLength(const Pattern& p, int track) noexcept {
    const int len = p.lengthOf(track);
    return len < 1 ? 1 : (len > kMaxSteps ? kMaxSteps : len);
}

int floorMod(int a, int m) noexcept {
    const int r = a % m;
    return r < 0 ? r + m : r;
}

// Uniform in [0, 1) from (seed, track, step, salt).
float unitRandom(std::uint32_t seed, int track, int step, std::uint32_t salt) noexcept {
    const std::uint32_t id = (static_cast<std::uint32_t>(track) << 16) ^ (static_cast<std::uint32_t>(step) << 4) ^ salt;
    const std::uint32_t h = dsp::hash32(seed, id);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Keep the hats exclusive after `written` (OH or CH) was filled: the other
// hat loses its steps where `written` is now on.
void enforceHatExclusion(Pattern& p, int variation, int written) noexcept {
    if (written != OH && written != CH) return;
    const int other = written == OH ? CH : OH;
    const int len = clampedLength(p, written);
    for (int s = 0; s < len; ++s)
        if (p.at(variation, written, s).on()) p.at(variation, other, s) = Step{};
}

void randomizeTrackNoHats(Pattern& p, int variation, int track, float density, std::uint32_t seed) noexcept {
    const float d = density != density ? 0.0f : (density < 0.0f ? 0.0f : (density > 1.0f ? 1.0f : density));
    const int len = clampedLength(p, track);
    for (int s = 0; s < len; ++s) {
        Step cell{};
        if (unitRandom(seed, track, s, 0x1U) < d) {
            if (track == kAccentTrack) {
                cell.setLevel(StepLevel::Normal);
            } else {
                const float r = unitRandom(seed, track, s, 0x2U);
                cell.setLevel(r < 0.15f ? StepLevel::Weak : (r < 0.85f ? StepLevel::Normal : StepLevel::Accent));
            }
        }
        p.at(variation, track, s) = cell;
    }
}

}  // namespace

void clearPattern(Pattern& p, int variation) noexcept {
    for (int v = 0; v < kNumVariations; ++v) {
        if (variation != kBothVariations && v != variation) continue;
        for (int t = 0; t < kNumTracks; ++t)
            for (int s = 0; s < kMaxSteps; ++s) p.at(v, t, s) = Step{};
    }
}

void clearTrack(Pattern& p, int variation, int track) noexcept {
    if (!validVariation(variation) || !validTrack(track)) return;
    for (int s = 0; s < kMaxSteps; ++s) p.at(variation, track, s) = Step{};
}

void copyTrack(const Pattern& src, int srcVariation, int srcTrack, Pattern& dst, int dstVariation,
               int dstTrack) noexcept {
    if (!validVariation(srcVariation) || !validVariation(dstVariation) || !validTrack(srcTrack) ||
        !validTrack(dstTrack))
        return;
    std::array<Step, kMaxSteps> tmp{};
    for (int s = 0; s < kMaxSteps; ++s) tmp[static_cast<std::size_t>(s)] = src.at(srcVariation, srcTrack, s);
    const std::uint8_t len = (srcTrack < kNumVoices) ? src.trackLength[static_cast<std::size_t>(srcTrack)] : 0;
    const std::uint8_t shuf = (srcTrack < kNumVoices) ? src.trackShuffle[static_cast<std::size_t>(srcTrack)] : 0;
    for (int s = 0; s < kMaxSteps; ++s) dst.at(dstVariation, dstTrack, s) = tmp[static_cast<std::size_t>(s)];
    if (srcTrack < kNumVoices && dstTrack < kNumVoices) {
        dst.trackLength[static_cast<std::size_t>(dstTrack)] = len;
        dst.trackShuffle[static_cast<std::size_t>(dstTrack)] = shuf;
    }
}

void copyTrack(Pattern& p, int variation, int fromTrack, int toTrack) noexcept {
    copyTrack(p, variation, fromTrack, p, variation, toTrack);
}

void shiftTrack(Pattern& p, int variation, int track, int steps) noexcept {
    if (!validVariation(variation) || !validTrack(track)) return;
    const int len = clampedLength(p, track);
    const int k = floorMod(steps, len);
    if (k == 0) return;
    std::array<Step, kMaxSteps> tmp{};
    for (int s = 0; s < len; ++s) tmp[static_cast<std::size_t>((s + k) % len)] = p.at(variation, track, s);
    for (int s = 0; s < len; ++s) p.at(variation, track, s) = tmp[static_cast<std::size_t>(s)];
}

void shiftPattern(Pattern& p, int variation, int steps) noexcept {
    if (!validVariation(variation)) return;
    for (int t = 0; t < kNumTracks; ++t) shiftTrack(p, variation, t, steps);
}

void invertTrack(Pattern& p, int variation, int track) noexcept {
    if (!validVariation(variation) || !validTrack(track)) return;
    const int len = clampedLength(p, track);
    for (int s = 0; s < len; ++s) {
        Step& cell = p.at(variation, track, s);
        Step next{};
        if (!cell.on()) next.setLevel(StepLevel::Normal);
        cell = next;
    }
}

void copyVariation(Pattern& p, int from, int to) noexcept {
    if (!validVariation(from) || !validVariation(to) || from == to) return;
    p.steps[static_cast<std::size_t>(to)] = p.steps[static_cast<std::size_t>(from)];
}

void randomizeTrack(Pattern& p, int variation, int track, float density, std::uint32_t seed) noexcept {
    if (!validVariation(variation) || !validTrack(track)) return;
    randomizeTrackNoHats(p, variation, track, density, seed);
    enforceHatExclusion(p, variation, track);
}

void randomizePattern(Pattern& p, int variation, const std::array<float, kNumTracks>& densities,
                      std::uint32_t seed) noexcept {
    if (!validVariation(variation)) return;
    for (int t = 0; t < kNumTracks; ++t)
        randomizeTrackNoHats(p, variation, t, densities[static_cast<std::size_t>(t)], seed);
    // Resolve OH+CH collisions with a coin per residue class of
    // gcd(L_OH, L_CH) (see the top of this file), so neither hat always wins
    // and no step of any measure plays both.
    const int lenOh = clampedLength(p, OH);
    const int lenCh = clampedLength(p, CH);
    int g = lenOh, b = lenCh;
    while (b != 0) {
        const int t = g % b;
        g = b;
        b = t;
    }
    for (int r = 0; r < g; ++r) {
        bool ohOn = false, chOn = false;
        for (int s = r; s < lenOh; s += g) ohOn = ohOn || p.at(variation, OH, s).on();
        for (int s = r; s < lenCh; s += g) chOn = chOn || p.at(variation, CH, s).on();
        if (!ohOn || !chOn) continue;
        const int loser = unitRandom(seed, kNumTracks, r, 0x3U) < 0.5f ? OH : CH;
        const int len = loser == OH ? lenOh : lenCh;
        for (int s = r; s < len; s += g) p.at(variation, loser, s) = Step{};
    }
}

bool expandPattern(Pattern& p) noexcept {
    const int len = p.length < 1 ? 1 : p.length;
    if (len * 2 > kMaxSteps) return false;
    for (int v = 0; v < kNumVariations; ++v) {
        for (int t = 0; t < kNumTracks; ++t) {
            if (t < kNumVoices && p.trackLength[static_cast<std::size_t>(t)] != 0) continue;
            for (int s = 0; s < len; ++s) p.at(v, t, s + len) = p.at(v, t, s);
        }
    }
    p.length = static_cast<std::uint8_t>(len * 2);
    return true;
}

void setEuclidean(Pattern& p, int variation, int track, int hits, int rotation) noexcept {
    if (!validVariation(variation) || !validTrack(track)) return;
    const int len = clampedLength(p, track);
    const int k = hits < 0 ? 0 : (hits > len ? len : hits);
    clearTrack(p, variation, track);
    const int rot = floorMod(rotation, len);  // reduce first: s - INT_MIN would overflow
    for (int s = 0; s < len; ++s) {
        const int i = floorMod(s - rot, len);
        // Bresenham form of the Euclidean (Bjorklund) rhythm: onset at 0.
        if ((i * k) % len < k) p.at(variation, track, s).setLevel(StepLevel::Normal);
    }
}

// ---------------------------------------------------------------------------

void setScale(Pattern& p, Scale scale) noexcept {
    if (static_cast<unsigned>(scale) >= static_cast<unsigned>(Scale::kNumScales)) return;
    const Scale old = static_cast<unsigned>(p.scale) < static_cast<unsigned>(Scale::kNumScales) ? p.scale
                                                                                              : Scale::Sixteenth;
    if (p.length == defaultLengthFor(old)) p.length = static_cast<std::uint8_t>(defaultLengthFor(scale));
    p.scale = scale;
}

namespace {

int songLength(const SongTrack& t) noexcept { return t.length > kMaxSongLength ? kMaxSongLength : t.length; }

}  // namespace

bool insertMeasure(SongTrack& t, int at, int pattern) noexcept {
    const int len = songLength(t);
    if (at < 0 || at > len || len >= kMaxSongLength || pattern < 0 || pattern >= kNumPatterns) return false;
    for (int i = len; i > at; --i) t.entries[static_cast<std::size_t>(i)] = t.entries[static_cast<std::size_t>(i - 1)];
    t.entries[static_cast<std::size_t>(at)] = static_cast<std::uint8_t>(pattern);
    t.length = static_cast<std::uint16_t>(len + 1);
    return true;
}

bool deleteMeasure(SongTrack& t, int at, int count) noexcept {
    const int len = songLength(t);
    if (at < 0 || at >= len || count < 1) return false;
    const int k = count > len - at ? len - at : count;
    for (int i = at; i + k < len; ++i) t.entries[static_cast<std::size_t>(i)] = t.entries[static_cast<std::size_t>(i + k)];
    for (int i = len - k; i < len; ++i) t.entries[static_cast<std::size_t>(i)] = 0;
    t.length = static_cast<std::uint16_t>(len - k);
    return true;
}

bool copyMeasures(SongTrack& t, int from, int count, int to) noexcept {
    const int len = songLength(t);
    if (from < 0 || count < 1 || from >= len || count > len - from || to < 0 || to > len) return false;
    if (to > from && to < from + count) return false;  // into the middle of the source range
    if (count > kMaxSongLength - len) return false;
    std::array<std::uint8_t, kMaxSongLength> src{};
    for (int i = 0; i < count; ++i) src[static_cast<std::size_t>(i)] = t.entries[static_cast<std::size_t>(from + i)];
    for (int i = len - 1; i >= to; --i)
        t.entries[static_cast<std::size_t>(i + count)] = t.entries[static_cast<std::size_t>(i)];
    for (int i = 0; i < count; ++i) t.entries[static_cast<std::size_t>(to + i)] = src[static_cast<std::size_t>(i)];
    t.length = static_cast<std::uint16_t>(len + count);
    return true;
}

namespace {

constexpr int kHardwareMaxSteps = 16;

std::uint8_t snapShuffle(int ticks) noexcept {
    const int t = ticks < 0 ? 0 : ticks;
    const int key = 1 + (t + 1) / 2;  // nearest even tick count; ties round up
    return shuffleTicksForKey(key > 7 ? 7 : key);
}

std::uint8_t snapFlam(int ms) noexcept {
    const int key = 1 + (ms - 20 + 2) / 4;  // nearest of 20, 24 ... 48; ties round up
    return flamMsForKey(ms < 20 ? 1 : key);
}

Step hardwareCell(Step c, int voice) noexcept {
    if (!c.on()) return Step{};
    Step out{};
    if (voice == kAccentTrack) {
        out.setLevel(StepLevel::Normal);
        return out;
    }
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voice)];
    const bool accent = c.level() == StepLevel::Accent && info.hardwareAccent;
    out.setLevel(accent ? StepLevel::Accent : StepLevel::Normal);
    out.setFlam(c.flam() && info.hardwareFlam && !accent);
    return out;
}

// Applies the rules to `p` and reports whether anything changed.
bool hardwareRules(Pattern& p) noexcept {
    const Pattern before = p;
    if (static_cast<unsigned>(p.scale) >= static_cast<unsigned>(Scale::kNumScales) || p.scale == Scale::Eighth)
        p.scale = Scale::Sixteenth;
    p.length = static_cast<std::uint8_t>(p.length < 1 ? 1 : (p.length > kHardwareMaxSteps ? kHardwareMaxSteps : p.length));
    p.trackLength.fill(0);
    p.trackShuffle.fill(0);
    p.shuffle = snapShuffle(p.shuffle);
    p.flamMs = snapFlam(p.flamMs);
    // The original's flam is two triggers at one accent level, and it has no
    // sub-steps: grace and ramp at 100 % (a quieter grace would still sound,
    // since flams on BD, SD and the toms survive).
    p.flamGrace = 100;
    p.subStart = 100;
    p.subEnd = 100;
    p.reserved = 0;
    for (int var = 0; var < kNumVariations; ++var) {
        for (int t = 0; t < kNumTracks; ++t)
            for (int s = 0; s < kMaxSteps; ++s) p.at(var, t, s) = hardwareCell(p.at(var, t, s), t);
        for (int s = 0; s < kMaxSteps; ++s)
            if (p.at(var, OH, s).on() && p.at(var, CH, s).on()) p.at(var, CH, s) = Step{};
    }
    return !(p == before);
}

}  // namespace

void applyHardwareRules(Pattern& p) noexcept { hardwareRules(p); }

bool followsHardwareRules(const Pattern& p) noexcept {
    Pattern copy = p;
    return !hardwareRules(copy);
}

}  // namespace mw909::seq
