// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Private to the factory library (src/Factory*.cpp): the step notation the
// factory pattern memories are written in, its parser, and the compile-time
// checker every memory passes through a static_assert.
//
// ---------------------------------------------------------------------------
// Step notation
// ---------------------------------------------------------------------------
// A variation is one string of tracks separated by ';'. Each track is a
// two-letter voice code (kVoiceInfo codes: BD SD LT MT HT RS CP CH OH CR RD)
// or "AC" for the TOTAL ACCENT row, followed by its steps:
//
//     "BD X...|X...|X...|X...;"
//
//   .   off                       '|' and ' ' are ignored (they only group
//   o   weak hit (extension)          beats for the eye)
//   x   normal hit
//   X   per-instrument accent (the AC row accents every voice on its step)
//
// Modifiers follow the hit they change:
//
//   ~       flam (grace hit, then the main hit the pattern's flam time later)
//   2 3 4   sub-steps: that many evenly spaced hits inside the step (extension)
//   ?       probability: each '?' takes 25 % off (x? = 75 %, x?? = 50 %) (extension)
//
// Track headers (extension, variation A only), between the code and the steps:
//
//   @       the number of steps written is this track's own loop length (polymeter)
//   +N      this track's own shuffle, N = 0..16 ticks of 1/96 beat
//
//     "RS@+8 ..x.|..x.|.x;"   a 10-step rim-shot loop with its own heavy swing
//
// On the AC row any hit character means "accent on"; modifiers and headers are
// not allowed there. A track has exactly `length` steps unless it has '@'.
// Tracks not written are empty.
//
// Variation B is written as the tracks that CHANGE from A: B starts as a copy
// of A and each listed track replaces A's track completely (same loop length).
// An empty B string makes B a plain copy of A, so the AB mode plays A twice.
//
// Song tracks and chains are written as measure lists: "1.1*4 1.2*3 1.16"
// where each entry is GROUP.SLOT[*REPEATS]. GROUP 1..3 are pattern groups 1..3
// of bank I and 4..6 those of bank II; SLOT is 1..16.
//
// What the checker refuses (the static_assert value names memory slot and
// reason, see Error): bad characters, wrong step counts, a voice track and the
// AC row written empty in A, B equal to A, CH and OH on one step (following
// each track's own loop), a group with patterns but no fill in slot 16, a
// name without data or data without a name, and, in a hardware memory,
// anything the original could not store (research/03 §0, §2; the same rules as
// seq::applyHardwareRules): more than 16 steps, the 8th scale, weak steps,
// sub-steps, probability, per-track length or shuffle, a variation B, accent
// on any voice but BD SD LT MT HT CH, flam on any voice but BD SD LT MT HT or
// together with an accent, shuffle other than 0, 2, ... 12 ticks (and any
// shuffle in the two triplet scales, which have none) and flam times other
// than 20, 24, ... 48 ms; its song tracks must stay inside their own bank
// (tracks 1..4 bank I, 5..8 bank II) and its chain must be one run of
// adjoining patterns of one group, as the original enters it (first and last
// key, research/03 §2.8): no repeats, no jumps.
//
// Phrasing: song tracks and chains are arrangements in 4-bar phrases. A
// measure counts as its length in 4/4 bars (a 32-step 16th pattern is two
// bars, a 16-step 32nd pattern half a bar); a measure in an odd meter (3/4,
// 7/8, 5/4) counts as one bar of its own meter. Every fill (a group's slot
// 16) must END a phrase, i.e. finish on bar 4, 8, 12 ..., and the whole list
// must be a whole number of phrases, so it loops on the grid too.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "mw909/Factory.h"

namespace mw909::factory::detail {

using seq::Scale;
inline constexpr Scale S16 = Scale::Sixteenth;         // 4 steps per beat (16 per 4/4 bar)
inline constexpr Scale S32 = Scale::ThirtySecond;      // 8 steps per beat (16 = half a bar)
inline constexpr Scale T8 = Scale::EighthTriplet;      // 3 steps per beat (12 per bar)
inline constexpr Scale T16 = Scale::SixteenthTriplet;  // 6 steps per beat (12 = half a bar)
inline constexpr Scale S8 = Scale::Eighth;             // 2 steps per beat (16 = two bars), extension

// Hardware SHUFFLE key n (1..7) in ticks, and FLAM key n (1..8) in ms.
constexpr int sh(int key) noexcept { return seq::shuffleTicksForKey(key); }
constexpr int fl(int key) noexcept { return seq::flamMsForKey(key); }

// Written as {name, length, scale, shuffle, A, B, flamMs, flamGrace, subStart,
// subEnd}; everything after A is optional. (A constructor rather than an
// aggregate so the trailing fields can be left out without a warning.)
struct PatternDef {
    std::string_view name;
    int length = 16;
    Scale scale = S16;
    int shuffle = 0;          // ticks, 0..kMaxShuffleTicks
    std::string_view a;
    std::string_view b;
    int flamMs = 24;          // Pattern default (hardware flam key 2)
    int flamGrace = 100;      // % of the main hit's accent line
    int subStart = 100;       // sub-step ramp, %
    int subEnd = 100;

    constexpr PatternDef() noexcept = default;
    constexpr PatternDef(std::string_view name_, int length_, Scale scale_, int shuffle_, std::string_view a_,
                         std::string_view b_ = {}, int flamMs_ = 24, int flamGrace_ = 100, int subStart_ = 100,
                         int subEnd_ = 100) noexcept
        : name(name_), length(length_), scale(scale_), shuffle(shuffle_), a(a_), b(b_), flamMs(flamMs_),
          flamGrace(flamGrace_), subStart(subStart_), subEnd(subEnd_) {}
};

using Group = std::array<PatternDef, seq::kPatternsPerGroup>;
using BankDefs = std::array<PatternDef, seq::kNumPatterns>;

inline constexpr int kGroups = seq::kNumBanks * seq::kGroupsPerBank;  // 6
inline constexpr int kFillSlot = seq::kPatternsPerGroup - 1;           // slot 16, the default fill slot

// A group: the basic patterns from slot 1 up, the fill in slot 16.
template <std::size_t N>
constexpr Group group(const std::array<PatternDef, N>& basics, const PatternDef& fill) noexcept {
    static_assert(N <= static_cast<std::size_t>(kFillSlot), "at most 15 basic patterns per group");
    Group g{};
    for (std::size_t i = 0; i < N; ++i) g[i] = basics[i];
    g[static_cast<std::size_t>(kFillSlot)] = fill;
    return g;
}
inline constexpr Group kNoGroup{};

constexpr BankDefs memory(const Group& g1, const Group& g2, const Group& g3, const Group& g4,
                          const Group& g5, const Group& g6) noexcept {
    BankDefs d{};
    const std::array<const Group*, kGroups> gs{&g1, &g2, &g3, &g4, &g5, &g6};
    for (std::size_t g = 0; g < gs.size(); ++g)
        for (std::size_t s = 0; s < static_cast<std::size_t>(seq::kPatternsPerGroup); ++s)
            d[g * static_cast<std::size_t>(seq::kPatternsPerGroup) + s] = (*gs[g])[s];
    return d;
}

struct MemoryDef {
    BankInfo info;
    bool hardware = false;                                  // obeys the original's rules
    const BankDefs* patterns = nullptr;
    std::array<std::string_view, seq::kNumSongTracks> songs{};  // TRACK 1..8 measure lists
    std::string_view chain;                                 // live chain measure list
};

enum Error : int {
    kOk = 0,
    kUnknownTrack,       //  1
    kDuplicateTrack,     //  2
    kBadChar,            //  3
    kStrayModifier,      //  4
    kTooManySteps,       //  5
    kWrongStepCount,     //  6
    kAccentModifier,     //  7
    kBadTrackHeader,     //  8
    kBadHeader,          //  9
    kHatClash,           // 10
    kSameAsA,            // 11
    kNameWithoutData,    // 12
    kDataWithoutName,    // 13
    kEmptyTrack,         // 14
    kHardwareRule,       // 15
    kMissingFill,        // 16
    kBadSong,            // 17
    kBadChain,           // 18
    kBadName,            // 19
    kBadPhrase,          // 20  a fill off the 4-bar grid, or a list that is not whole phrases
    kHardwareChain       // 21  a hardware chain that is not one run of adjoining patterns
};

constexpr int trackIndex(std::string_view code) noexcept {
    if (code == "AC") return seq::kAccentTrack;
    for (int v = 0; v < kNumVoices; ++v)
        if (kVoiceInfo[static_cast<std::size_t>(v)].code == code) return v;
    return -1;
}

constexpr std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

// Parse one variation's text into `p`. For B (`isB`), `p` already holds A in
// both variations; listed tracks replace B's copy.
constexpr Error parseVariation(std::string_view text, bool isB, seq::Pattern& p) noexcept {
    const int variation = isB ? 1 : 0;
    std::array<bool, seq::kNumTracks> seen{};
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find(';', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view tok = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (tok.empty()) continue;
        if (tok.size() < 2) return kUnknownTrack;
        const int track = trackIndex(tok.substr(0, 2));
        if (track < 0) return kUnknownTrack;
        if (seen[static_cast<std::size_t>(track)]) return kDuplicateTrack;
        seen[static_cast<std::size_t>(track)] = true;
        const bool accentRow = track == seq::kAccentTrack;

        std::size_t i = 2;
        bool ownLength = false;
        int ownShuffle = -1;
        while (i < tok.size() && (tok[i] == '@' || tok[i] == '+')) {
            if (accentRow || isB) return kBadTrackHeader;
            if (tok[i] == '@') {
                if (ownLength) return kBadTrackHeader;
                ownLength = true;
                ++i;
                continue;
            }
            if (ownShuffle >= 0) return kBadTrackHeader;
            ++i;
            int n = 0;
            int digits = 0;
            while (i < tok.size() && isDigit(tok[i])) {
                n = n * 10 + (tok[i] - '0');
                ++digits;
                ++i;
            }
            if (digits == 0 || n > seq::kMaxShuffleTicks) return kBadTrackHeader;
            ownShuffle = n;
        }

        std::array<seq::Step, seq::kMaxSteps> cells{};
        int count = 0;
        bool lastOn = false;
        bool anyOn = false;
        for (; i < tok.size(); ++i) {
            const char c = tok[i];
            if (c == ' ' || c == '|' || c == '\n') continue;
            if (c == '.' || c == 'o' || c == 'x' || c == 'X') {
                if (count >= seq::kMaxSteps) return kTooManySteps;
                seq::Step& s = cells[static_cast<std::size_t>(count++)];
                if (c == '.') {
                    lastOn = false;
                    continue;
                }
                if (accentRow && c == 'o') return kBadChar;
                s.setLevel(accentRow ? seq::StepLevel::Normal
                                     : (c == 'o' ? seq::StepLevel::Weak
                                                 : (c == 'x' ? seq::StepLevel::Normal : seq::StepLevel::Accent)));
                lastOn = true;
                anyOn = true;
                continue;
            }
            if (c == '~' || c == '2' || c == '3' || c == '4' || c == '?') {
                if (accentRow) return kAccentModifier;
                if (!lastOn) return kStrayModifier;
                seq::Step& s = cells[static_cast<std::size_t>(count - 1)];
                if (c == '~') {
                    if (s.flam()) return kStrayModifier;
                    s.setFlam(true);
                } else if (c == '?') {
                    if (s.probabilityCode() >= 6) return kStrayModifier;
                    s.setProbabilityCode(s.probabilityCode() + 2);
                } else {
                    if (s.subSteps() != 1) return kStrayModifier;
                    s.setSubSteps(c - '0');
                }
                continue;
            }
            return kBadChar;
        }

        int expected = p.length;
        if (isB) {
            expected = p.lengthOf(track);
        } else if (ownLength) {
            if (count < 1) return kWrongStepCount;
            p.trackLength[static_cast<std::size_t>(track)] = static_cast<std::uint8_t>(count);
            expected = count;
        }
        if (!isB && ownShuffle >= 0) p.trackShuffle[static_cast<std::size_t>(track)] = static_cast<std::uint8_t>(ownShuffle + 1);
        if (count != expected) return kWrongStepCount;
        // A track written in A must hold something; in B an all-off track is
        // a legitimate "drop this part".
        if (!isB && !anyOn) return kEmptyTrack;

        p.steps[static_cast<std::size_t>(variation)][static_cast<std::size_t>(track)] = cells;
    }
    return kOk;
}

constexpr int gcd(int a, int b) noexcept {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// True if OH and CH ever sound on the same step of a variation, following
// each track's own loop (global step t reads index t mod L).
constexpr bool hatsClash(const seq::Pattern& p, int variation) noexcept {
    const int lo = p.lengthOf(OH), lc = p.lengthOf(CH);
    const int period = lo / gcd(lo, lc) * lc;
    for (int t = 0; t < period; ++t)
        if (p.at(variation, OH, t % lo).on() && p.at(variation, CH, t % lc).on()) return true;
    return false;
}

// The original machine's storage rules (see the header comment).
constexpr bool hardwareLegal(const seq::Pattern& p) noexcept {
    if (p.length < 1 || p.length > 16) return false;
    if (static_cast<int>(p.scale) >= seq::kNumHardwareScales) return false;
    if (p.shuffle > 12 || (p.shuffle % 2) != 0) return false;
    if ((p.scale == Scale::EighthTriplet || p.scale == Scale::SixteenthTriplet) && p.shuffle != 0) return false;
    if (p.flamMs < 20 || p.flamMs > 48 || ((p.flamMs - 20) % 4) != 0) return false;
    for (int v = 0; v < kNumVoices; ++v)
        if (p.trackLength[static_cast<std::size_t>(v)] != 0 || p.trackShuffle[static_cast<std::size_t>(v)] != 0)
            return false;
    for (int var = 0; var < seq::kNumVariations; ++var) {
        for (int t = 0; t < seq::kNumTracks; ++t) {
            for (int s = 0; s < seq::kMaxSteps; ++s) {
                const seq::Step c = p.at(var, t, s);
                if (!c.on()) {
                    if (c.bits != 0) return false;
                    continue;
                }
                if (c.subSteps() != 1 || c.probabilityCode() != 0 || c.level() == seq::StepLevel::Weak) return false;
                if (t == seq::kAccentTrack) {
                    if (c.level() != seq::StepLevel::Normal || c.flam()) return false;
                    continue;
                }
                const VoiceInfo& info = kVoiceInfo[static_cast<std::size_t>(t)];
                const bool accent = c.level() == seq::StepLevel::Accent;
                if (accent && !info.hardwareAccent) return false;
                if (c.flam() && (!info.hardwareFlam || accent)) return false;
            }
        }
    }
    return true;
}

constexpr bool nameOk(std::string_view n) noexcept {
    if (n.empty() || n.size() > 24 || n.front() == ' ' || n.back() == ' ') return false;
    for (const char c : n) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                        c == '-' || c == '\'' || c == '&' || c == '/' || c == '.';
        if (!ok) return false;
    }
    return true;
}

constexpr Error parsePattern(const PatternDef& d, bool hardware, seq::Pattern& p) noexcept {
    p = seq::Pattern{};
    if (d.a.empty()) return d.name.empty() && d.b.empty() ? kOk : kNameWithoutData;
    if (d.name.empty()) return kDataWithoutName;
    if (!nameOk(d.name)) return kBadName;
    if (d.length < 1 || d.length > seq::kMaxSteps || d.shuffle < 0 || d.shuffle > seq::kMaxShuffleTicks ||
        static_cast<int>(d.scale) >= static_cast<int>(Scale::kNumScales) || d.flamMs < seq::kMinFlamMs ||
        d.flamMs > seq::kMaxFlamMs || d.flamGrace < 0 || d.flamGrace > 100 || d.subStart < 0 || d.subStart > 100 ||
        d.subEnd < 0 || d.subEnd > 100)
        return kBadHeader;
    p.length = static_cast<std::uint8_t>(d.length);
    p.scale = d.scale;
    p.shuffle = static_cast<std::uint8_t>(d.shuffle);
    p.flamMs = static_cast<std::uint8_t>(d.flamMs);
    p.flamGrace = static_cast<std::uint8_t>(d.flamGrace);
    p.subStart = static_cast<std::uint8_t>(d.subStart);
    p.subEnd = static_cast<std::uint8_t>(d.subEnd);
    if (const Error e = parseVariation(d.a, false, p); e != kOk) return e;
    p.steps[1] = p.steps[0];
    if (const Error e = parseVariation(d.b, true, p); e != kOk) return e;
    if (!d.b.empty() && p.steps[0] == p.steps[1]) return kSameAsA;
    if (hatsClash(p, 0) || hatsClash(p, 1)) return kHatClash;
    if (hardware && (!d.b.empty() || !hardwareLegal(p))) return kHardwareRule;
    return kOk;
}

constexpr bool slotUsed(const BankDefs& defs, int index) noexcept {
    return !defs[static_cast<std::size_t>(index)].a.empty();
}

// Parses a measure list ("1.1*4 1.16 4.2*2") into `entries` (global pattern
// indices). Returns the number of measures, or -1 when the list is malformed,
// too long, or names an empty slot. `bankOnly` >= 0 restricts the entries to
// that bank (the hardware's tracks cannot cross banks).
template <std::size_t N>
constexpr int parseMeasures(std::string_view text, const BankDefs& defs, int bankOnly,
                            std::array<std::uint8_t, N>& entries) noexcept {
    int count = 0;
    std::size_t i = 0;
    auto number = [&](int& out) {
        int n = 0;
        int digits = 0;
        while (i < text.size() && isDigit(text[i])) {
            n = n * 10 + (text[i] - '0');
            ++digits;
            ++i;
        }
        out = n;
        return digits > 0 && digits <= 3;
    };
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '|' || text[i] == '\n') {
            ++i;
            continue;
        }
        int g = 0, s = 0, r = 1;
        if (!number(g) || g < 1 || g > kGroups) return -1;
        if (i >= text.size() || text[i] != '.') return -1;
        ++i;
        if (!number(s) || s < 1 || s > seq::kPatternsPerGroup) return -1;
        if (i < text.size() && text[i] == '*') {
            ++i;
            if (!number(r) || r < 1) return -1;
        }
        if (i < text.size() && text[i] != ' ' && text[i] != '|' && text[i] != '\n') return -1;
        const int index = (g - 1) * seq::kPatternsPerGroup + (s - 1);
        if (!slotUsed(defs, index)) return -1;
        if (bankOnly >= 0 && seq::bankOf(index) != bankOnly) return -1;
        for (int k = 0; k < r; ++k) {
            if (count >= static_cast<int>(N)) return -1;
            entries[static_cast<std::size_t>(count++)] = static_cast<std::uint8_t>(index);
        }
    }
    return count;
}

// Length of one measure of a pattern in 96ths of a 4/4 bar (see "Phrasing"
// above): whole bars and whole fractions of a bar count as they are, an odd
// meter counts as one bar.
inline constexpr int kBarUnits = 96;
inline constexpr int kPhraseUnits = 4 * kBarUnits;

constexpr int stepUnits(Scale s) noexcept {
    switch (s) {
        case Scale::Sixteenth: return 6;
        case Scale::ThirtySecond: return 3;
        case Scale::EighthTriplet: return 8;
        case Scale::SixteenthTriplet: return 4;
        case Scale::Eighth: return 12;
        default: return 6;
    }
}

constexpr int measureUnits(const PatternDef& d) noexcept {
    const int u = d.length * stepUnits(d.scale);
    if (u <= 0) return kBarUnits;
    if (u % kBarUnits == 0 || kBarUnits % u == 0) return u;
    return kBarUnits;  // an odd meter: one bar of its own
}

constexpr bool isFillSlot(int index) noexcept { return index % seq::kPatternsPerGroup == kFillSlot; }

// True when every fill in the list ends a 4-bar phrase and the list is whole
// phrases.
template <std::size_t N>
constexpr bool phrased(const std::array<std::uint8_t, N>& entries, int count, const BankDefs& defs) noexcept {
    int units = 0;
    for (int i = 0; i < count; ++i) {
        const int index = entries[static_cast<std::size_t>(i)];
        units += measureUnits(defs[static_cast<std::size_t>(index)]);
        if (isFillSlot(index) && units % kPhraseUnits != 0) return false;
    }
    return units % kPhraseUnits == 0;
}

// The original's chain: one run of adjoining patterns inside one group.
template <std::size_t N>
constexpr bool hardwareChain(const std::array<std::uint8_t, N>& entries, int count) noexcept {
    for (int i = 1; i < count; ++i) {
        const int prev = entries[static_cast<std::size_t>(i - 1)];
        const int cur = entries[static_cast<std::size_t>(i)];
        if (cur != prev + 1 || cur / seq::kPatternsPerGroup != prev / seq::kPatternsPerGroup) return false;
    }
    return true;
}

// 0 if the memory is valid, else (slot + 1) * 100 + error for a pattern, or
// 10000 + song track * 100 + error / 20000 + error for a chain.
constexpr int checkMemory(const MemoryDef& m) noexcept {
    const BankDefs& defs = *m.patterns;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        seq::Pattern p{};
        if (const Error e = parsePattern(defs[i], m.hardware, p); e != kOk) return static_cast<int>(i + 1) * 100 + e;
    }
    for (int g = 0; g < kGroups; ++g) {
        bool any = false;
        for (int s = 0; s < seq::kPatternsPerGroup; ++s) any = any || slotUsed(defs, g * seq::kPatternsPerGroup + s);
        if (any && !slotUsed(defs, g * seq::kPatternsPerGroup + kFillSlot))
            return (g * seq::kPatternsPerGroup + kFillSlot + 1) * 100 + kMissingFill;
    }
    for (int t = 0; t < seq::kNumSongTracks; ++t) {
        const std::string_view text = m.songs[static_cast<std::size_t>(t)];
        if (text.empty()) continue;
        std::array<std::uint8_t, seq::kMaxSongLength> entries{};
        const int bankOnly = m.hardware ? t / seq::kSongTracksPerBank : -1;
        const int n = parseMeasures(text, defs, bankOnly, entries);
        if (n < 1) return 10000 + t * 100 + kBadSong;
        if (!phrased(entries, n, defs)) return 10000 + t * 100 + kBadPhrase;
    }
    if (!m.chain.empty()) {
        std::array<std::uint8_t, seq::kMaxChainLength> entries{};
        const int n = parseMeasures(m.chain, defs, -1, entries);
        if (n < 1) return 20000 + kBadChain;
        if (!phrased(entries, n, defs)) return 20000 + kBadPhrase;
        if (m.hardware && !hardwareChain(entries, n)) return 20000 + kHardwareChain;
    }
    if (m.info.name.empty() || m.info.category.empty() || m.info.tempo < 20.0f || m.info.tempo > 400.0f)
        return 30000 + kBadName;
    return 0;
}

// The memories (one translation unit each).
const MemoryDef& classicMemory() noexcept;
const MemoryDef& houseMemory() noexcept;
const MemoryDef& technoMemory() noexcept;
const MemoryDef& acidElectroMemory() noexcept;
const MemoryDef& shuffleBreaksMemory() noexcept;
const MemoryDef& hardMemory() noexcept;
const MemoryDef& tranceDiscoMemory() noexcept;
const MemoryDef& labMemory() noexcept;

// Defined in FactoryPatterns.cpp, used by Factory.cpp.
int bankCount() noexcept;
const BankInfo& bankInfoAt(int index) noexcept;
void buildBankAt(int index, seq::PatternBank& out) noexcept;
std::string_view patternNameAt(int bankIndex, int patternIndex) noexcept;

}  // namespace mw909::factory::detail
