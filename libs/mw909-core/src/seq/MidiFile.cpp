// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Standard MIDI File export/import of one pattern measure. See MidiFile.h.
// Message thread only (allocates). Timing math mirrors Sequencer.cpp: every
// note's time is computed exactly in quarter notes and rounded to a tick once.

#include "mw909/seq/MidiFile.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "SeqTiming.h"
#include "mw909/seq/PatternOps.h"

namespace mw909::seq {

namespace {

constexpr std::uint8_t kMetaVersion = 1;
constexpr std::array<std::uint8_t, 5> kMetaTag{0x7D, 'M', 'W', '9', '0'};  // 0x7D = non-commercial id
// length, scale, shuffle, flamMs, flamGrace, subStart, subEnd
constexpr std::size_t kMetaHeader = 7;
constexpr std::size_t kMetaPayload = kMetaTag.size() + 1 + kMetaHeader + 2 * static_cast<std::size_t>(kNumVoices) +
                                     static_cast<std::size_t>(kNumTracks) * kMaxSteps;
// "The velocities are the writer's three classes, and flams are this many
// ticks long" (no embedded pattern): tag, version, flam spacing (u16, 0 = none).
constexpr std::array<std::uint8_t, 5> kClassTag{0x7D, 'M', 'W', '9', 'C'};
constexpr std::size_t kClassPayload = kClassTag.size() + 1 + 2;

int clampLength(int len) noexcept { return len < 1 ? 1 : (len > kMaxSteps ? kMaxSteps : len); }

int stepTicksOf(Scale s) noexcept {
    return static_cast<int>(std::lround(stepLengthQn(detail::validScale(s)) * kMidiFilePpq));
}

std::uint8_t velocityOf(StepLevel l) noexcept {
    switch (l) {
        case StepLevel::Weak: return kMidiVelocityWeak;
        case StepLevel::Accent: return kMidiVelocityAccent;
        default: return kMidiVelocityNormal;
    }
}

StepLevel levelOfVelocity(int vel, bool classes) noexcept {
    if (!classes) return vel >= kAccentVelocityThreshold ? StepLevel::Accent : StepLevel::Normal;
    return vel < 72 ? StepLevel::Weak : (vel < 112 ? StepLevel::Normal : StepLevel::Accent);
}

void putU16(std::vector<std::uint8_t>& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void putU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int sh = 24; sh >= 0; sh -= 8) b.push_back(static_cast<std::uint8_t>((v >> sh) & 0xFF));
}

void putVarLen(std::vector<std::uint8_t>& b, std::uint32_t v) {
    std::uint8_t tmp[5];
    int n = 0;
    tmp[n++] = static_cast<std::uint8_t>(v & 0x7F);
    while ((v >>= 7) != 0) tmp[n++] = static_cast<std::uint8_t>(0x80 | (v & 0x7F));
    while (n > 0) b.push_back(tmp[--n]);
}

struct NoteOut {
    std::int64_t tick;
    int order;
    std::uint8_t note;
    std::uint8_t velocity;
};

struct TrackEvent {
    std::int64_t tick;
    int kind;  // 0 = note off, 1 = note on (offs sort first at equal ticks)
    int order;
    std::uint8_t note;
    std::uint8_t velocity;
};

}  // namespace

std::vector<std::uint8_t> writeMidiFile(const Pattern& pattern, int variation,
                                        const std::array<std::uint8_t, kNumVoices>& noteMap,
                                        const MidiFileOptions& options) {
    const int var = (variation == 1) ? 1 : 0;
    const int len = clampLength(pattern.length);
    const Scale scale = detail::validScale(pattern.scale);
    const double stepQn = stepLengthQn(scale);
    const double tpq = kMidiFilePpq;
    const std::int64_t stepTicks = stepTicksOf(scale);
    const std::int64_t measureTicks = stepTicks * len;
    const double bpm = std::isfinite(options.bpm) && options.bpm > 0.0 ? std::clamp(options.bpm, 20.0, 400.0) : 120.0;
    const double swing = std::isfinite(options.swingScale) ? std::clamp(static_cast<double>(options.swingScale), 0.0, 1.0) : 1.0;
    const auto swingF = static_cast<float>(swing);
    const double flamMs = std::clamp<int>(pattern.flamMs, 0, kMaxFlamMs);
    const double flamQn = flamMs * 0.001 * bpm / 60.0;

    // --- collect notes (same arithmetic as the sequencer, measure 0) ---------
    std::vector<NoteOut> notes;
    int order = 0;
    auto add = [&](double qn, std::uint8_t note, std::uint8_t vel) {
        std::int64_t tick = std::llround(qn * tpq);
        tick = ((tick % measureTicks) + measureTicks) % measureTicks;  // a loop wraps late hits to the top
        notes.push_back({tick, order++, static_cast<std::uint8_t>(note & 0x7F), vel});
    };
    for (int s = 0; s < len; ++s) {
        const bool accentOn = pattern.at(var, kAccentTrack, s).on();
        const double ts = s * stepQn;
        if (accentOn && options.accentNote >= 0 && options.accentNote <= 127)
            add(ts + detail::shuffleQn(scale, pattern.shuffleOf(kAccentTrack), swingF, s, len),
                static_cast<std::uint8_t>(options.accentNote), kMidiVelocityAccent);
        for (int v = 0; v < kNumVoices; ++v) {
            const int tl = clampLength(pattern.lengthOf(v));
            const Step cell = pattern.at(var, v, s % tl);
            if (!cell.on()) continue;
            const double t0 = ts + detail::shuffleQn(scale, pattern.shuffleOf(v), swingF, s, len);
            const std::uint8_t vel = velocityOf(promotedLevel(cell.level(), accentOn));
            const std::uint8_t note = noteMap[static_cast<std::size_t>(v)];
            const int subs = cell.subSteps();
            for (int k = 0; k < subs; ++k) {
                const double t = t0 + k * stepQn / subs;
                if (k == 0 && cell.flam() && flamMs > 0.0) {
                    const auto grace = static_cast<std::uint8_t>(std::max(1L, std::lround(0.6 * vel)));
                    add(t, note, grace);
                    add(t + flamQn, note, vel);
                } else {
                    add(t, note, vel);
                }
            }
        }
    }

    // --- note lengths: never overlap the next hit of the same key -----------
    std::sort(notes.begin(), notes.end(), [](const NoteOut& a, const NoteOut& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    const std::int64_t nominal = options.oneStepNotes ? stepTicks : kMidiFilePpq / 4;
    std::vector<TrackEvent> events;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const NoteOut& n = notes[i];
        std::int64_t gap = measureTicks - n.tick;
        bool duplicate = false;
        for (std::size_t j = i + 1; j < notes.size(); ++j) {
            if (notes[j].note != n.note) continue;
            gap = std::min(gap, notes[j].tick - n.tick);
            duplicate = notes[j].tick == n.tick;
            break;
        }
        if (duplicate) continue;  // two hits of one key on one tick: keep the later (main) one
        const std::int64_t length = std::max<std::int64_t>(1, std::min(nominal, gap));
        events.push_back({n.tick, 1, n.order, n.note, n.velocity});
        events.push_back({n.tick + length, 0, n.order, n.note, 0});
    }
    std::sort(events.begin(), events.end(), [](const TrackEvent& a, const TrackEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.order < b.order;
    });

    // --- track chunk ----------------------------------------------------------
    std::vector<std::uint8_t> trk;
    if (options.includeTempo) {
        const auto usPerQn = static_cast<std::uint32_t>(std::lround(60.0e6 / bpm));
        trk.insert(trk.end(), {0x00, 0xFF, 0x51, 0x03});
        trk.push_back(static_cast<std::uint8_t>((usPerQn >> 16) & 0xFF));
        trk.push_back(static_cast<std::uint8_t>((usPerQn >> 8) & 0xFF));
        trk.push_back(static_cast<std::uint8_t>(usPerQn & 0xFF));
    }
    if (options.embedPattern) {
        trk.insert(trk.end(), {0x00, 0xFF, 0x7F});
        putVarLen(trk, static_cast<std::uint32_t>(kMetaPayload));
        trk.insert(trk.end(), kMetaTag.begin(), kMetaTag.end());
        trk.push_back(kMetaVersion);
        trk.push_back(pattern.length);
        trk.push_back(static_cast<std::uint8_t>(pattern.scale));
        trk.push_back(pattern.shuffle);
        trk.push_back(pattern.flamMs);
        trk.push_back(pattern.flamGrace);
        trk.push_back(pattern.subStart);
        trk.push_back(pattern.subEnd);
        for (std::uint8_t tl : pattern.trackLength) trk.push_back(tl);
        for (std::uint8_t ts : pattern.trackShuffle) trk.push_back(ts);
        for (int t = 0; t < kNumTracks; ++t)
            for (int s = 0; s < kMaxSteps; ++s) trk.push_back(pattern.at(var, t, s).bits);
    } else {
        trk.insert(trk.end(), {0x00, 0xFF, 0x7F});
        putVarLen(trk, static_cast<std::uint32_t>(kClassPayload));
        trk.insert(trk.end(), kClassTag.begin(), kClassTag.end());
        trk.push_back(kMetaVersion);
        const auto flamTicks = static_cast<unsigned>(
            flamMs > 0.0 ? std::clamp<long long>(std::llround(flamQn * tpq), 0, 0xFFFF) : 0);
        putU16(trk, flamTicks);
    }
    std::int64_t last = 0;
    for (const TrackEvent& e : events) {
        putVarLen(trk, static_cast<std::uint32_t>(e.tick - last));
        last = e.tick;
        trk.push_back(static_cast<std::uint8_t>((e.kind == 1 ? 0x90 : 0x80) | 0x09));  // channel 10
        trk.push_back(e.note);
        trk.push_back(e.kind == 1 ? e.velocity : 0x40);
    }
    putVarLen(trk, static_cast<std::uint32_t>(std::max<std::int64_t>(0, measureTicks - last)));
    trk.insert(trk.end(), {0xFF, 0x2F, 0x00});

    std::vector<std::uint8_t> file;
    file.reserve(22 + trk.size());
    file.insert(file.end(), {'M', 'T', 'h', 'd'});
    putU32(file, 6);
    putU16(file, 0);  // format 0
    putU16(file, 1);  // one track
    putU16(file, kMidiFilePpq);
    file.insert(file.end(), {'M', 'T', 'r', 'k'});
    putU32(file, static_cast<std::uint32_t>(trk.size()));
    file.insert(file.end(), trk.begin(), trk.end());
    return file;
}

// ---------------------------------------------------------------------------

namespace {

class ByteReader {
public:
    ByteReader(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}
    bool ok() const { return ok_; }
    std::size_t pos() const { return pos_; }
    bool atEnd(std::size_t end) const { return pos_ >= end; }
    std::uint8_t u8() {
        if (pos_ >= n_) {
            ok_ = false;
            return 0;
        }
        return d_[pos_++];
    }
    std::uint8_t peek() const { return pos_ < n_ ? d_[pos_] : 0; }
    std::uint32_t u16() {
        const std::uint32_t hi = u8();
        const std::uint32_t lo = u8();
        return (hi << 8) | lo;
    }
    std::uint32_t u32() {
        const std::uint32_t hi = u16();
        const std::uint32_t lo = u16();
        return (hi << 16) | lo;
    }
    std::uint32_t varLen() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = u8();
            v = (v << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) return v;
        }
        ok_ = false;  // longer than the 4 bytes the standard allows
        return 0;
    }
    bool skip(std::size_t k) {
        if (k > n_ - pos_) {
            ok_ = false;
            return false;
        }
        pos_ += k;
        return true;
    }
    const std::uint8_t* here() const { return d_ + pos_; }

private:
    const std::uint8_t* d_;
    std::size_t n_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

struct NoteIn {
    std::int64_t tick;  // in kMidiFilePpq units
    int order;
    int voice;
    int velocity;
};

}  // namespace

bool readMidiFile(const std::vector<std::uint8_t>& bytes, const std::array<std::uint8_t, kNumVoices>& noteMap,
                  Pattern& out, int variation) {
    const int var = (variation == 1) ? 1 : 0;
    ByteReader r(bytes.data(), bytes.size());
    if (bytes.size() < 14 || r.u8() != 'M' || r.u8() != 'T' || r.u8() != 'h' || r.u8() != 'd') return false;
    const std::uint32_t headerLen = r.u32();
    if (headerLen < 6) return false;
    const std::uint32_t format = r.u16();
    r.u16();  // number of tracks: we read whatever MTrk chunks are present
    const std::uint32_t division = r.u16();
    if (!r.ok() || format > 2 || division == 0 || (division & 0x8000U) != 0) return false;
    if (!r.skip(headerLen - 6)) return false;

    std::array<int, 128> noteToVoice{};
    noteToVoice.fill(-1);
    for (int v = 0; v < kNumVoices; ++v) {
        const int note = noteMap[static_cast<std::size_t>(v)] & 0x7F;
        if (noteToVoice[static_cast<std::size_t>(note)] < 0) noteToVoice[static_cast<std::size_t>(note)] = v;
    }
    // The original machine's receive map fills the notes `noteMap` leaves free.
    for (std::size_t note = 0; note < noteToVoice.size(); ++note)
        if (noteToVoice[note] < 0 && kNoteToVoice[note] != kNoVoice) noteToVoice[note] = kNoteToVoice[note];

    std::vector<NoteIn> notes;
    std::int64_t endTick = 0;
    int tracks = 0;
    const std::uint8_t* embedded = nullptr;
    bool classes = false;  // velocities are the writer's three classes
    std::int64_t markerFlamTicks = -1;  // the writer's flam spacing, when it left one
    int order = 0;
    std::uint32_t usPerQn = 0;  // first tempo event; 120 BPM when there is none

    while (bytes.size() - r.pos() >= 8) {
        const std::uint8_t id[4] = {r.u8(), r.u8(), r.u8(), r.u8()};
        const std::uint32_t chunkLen = r.u32();
        if (!r.ok() || chunkLen > bytes.size() - r.pos()) return false;
        const std::size_t end = r.pos() + chunkLen;
        if (!(id[0] == 'M' && id[1] == 'T' && id[2] == 'r' && id[3] == 'k')) {
            r.skip(chunkLen);
            continue;
        }
        ++tracks;
        std::int64_t tick = 0;
        std::uint8_t running = 0;
        bool sawEnd = false;
        while (!r.atEnd(end)) {
            tick += r.varLen();
            std::uint8_t status = r.peek();
            if ((status & 0x80) != 0) {
                r.u8();
                if (status < 0xF0) running = status;
            } else {
                if (running == 0) return false;
                status = running;
            }
            if (status == 0xFF) {
                const std::uint8_t type = r.u8();
                const std::uint32_t len = r.varLen();
                if (!r.ok() || len > end - r.pos()) return false;
                if (type == 0x7F && len >= kMetaTag.size() &&
                    std::equal(kMetaTag.begin(), kMetaTag.end(), r.here())) {
                    classes = true;
                    if (len == kMetaPayload && r.here()[kMetaTag.size()] == kMetaVersion)
                        embedded = r.here() + kMetaTag.size() + 1;
                }
                if (type == 0x7F && len >= kClassTag.size() && std::equal(kClassTag.begin(), kClassTag.end(), r.here())) {
                    classes = true;
                    if (len == kClassPayload && r.here()[kClassTag.size()] == kMetaVersion)
                        markerFlamTicks = (static_cast<std::int64_t>(r.here()[kClassTag.size() + 1]) << 8) |
                                          r.here()[kClassTag.size() + 2];
                }
                if (type == 0x51 && len == 3 && usPerQn == 0)
                    usPerQn = (static_cast<std::uint32_t>(r.here()[0]) << 16) |
                              (static_cast<std::uint32_t>(r.here()[1]) << 8) | r.here()[2];
                r.skip(len);
                running = 0;  // meta events cancel running status
                if (type == 0x2F) {
                    sawEnd = true;
                    break;
                }
            } else if (status == 0xF0 || status == 0xF7) {
                const std::uint32_t len = r.varLen();
                if (!r.ok() || len > end - r.pos()) return false;
                r.skip(len);
                running = 0;
            } else if (status >= 0xF1) {
                return false;  // system common/realtime bytes do not belong in a file
            } else {
                const int hi = status & 0xF0;
                const std::uint8_t d1 = r.u8();
                const std::uint8_t d2 = (hi == 0xC0 || hi == 0xD0) ? 0 : r.u8();
                if ((d1 & 0x80) != 0 || (d2 & 0x80) != 0) return false;
                if (hi == 0x90 && d2 > 0) {
                    const int voice = noteToVoice[d1];
                    if (voice >= 0) {
                        const std::int64_t t480 = (tick * kMidiFilePpq + division / 2) / division;
                        notes.push_back({t480, order++, voice, d2});
                    }
                }
            }
            if (!r.ok() || r.pos() > end) return false;
        }
        if (!sawEnd || !r.ok() || r.pos() > end) return false;  // every track must end with FF 2F 00
        r.skip(end - r.pos());                                  // tolerate padding after it
        endTick = std::max(endTick, (tick * kMidiFilePpq + division / 2) / division);
    }
    if (tracks == 0 || !r.ok()) return false;

    Pattern res = out;
    if (embedded != nullptr) {
        const std::uint8_t* d = embedded;
        const int len = d[0];
        const int scale = d[1];
        const int shuffle = d[2];
        const std::uint8_t* tl = d + kMetaHeader;
        const std::uint8_t* ts = tl + kNumVoices;
        bool valid = len >= 1 && len <= kMaxSteps && scale < static_cast<int>(Scale::kNumScales) &&
                     shuffle <= kMaxShuffleTicks && d[3] <= kMaxFlamMs && d[4] <= 100 && d[5] <= 100 && d[6] <= 100;
        for (int v = 0; v < kNumVoices; ++v) valid = valid && tl[v] <= kMaxSteps && ts[v] <= kMaxShuffleTicks + 1;
        if (valid) {
            res.length = static_cast<std::uint8_t>(len);
            res.scale = static_cast<Scale>(scale);
            res.shuffle = static_cast<std::uint8_t>(shuffle);
            res.flamMs = d[3];
            res.flamGrace = d[4];
            res.subStart = d[5];
            res.subEnd = d[6];
            for (int v = 0; v < kNumVoices; ++v) {
                res.trackLength[static_cast<std::size_t>(v)] = tl[v];
                res.trackShuffle[static_cast<std::size_t>(v)] = ts[v];
            }
            const std::uint8_t* cells = ts + kNumVoices;
            for (int t = 0; t < kNumTracks; ++t)
                for (int s = 0; s < kMaxSteps; ++s) res.at(var, t, s).bits = cells[t * kMaxSteps + s];
            out = res;
            return true;
        }
    }

    // --- quantise notes to res.scale --------------------------------------------
    // The writer's tempo range (20..400 BPM); also bounds the flam search below.
    usPerQn = usPerQn == 0 ? 500000U : std::clamp<std::uint32_t>(usPerQn, 150000U, 3000000U);
    const std::int64_t T = stepTicksOf(res.scale);
    for (int t = 0; t < kNumTracks; ++t)
        for (int s = 0; s < kMaxSteps; ++s) res.at(var, t, s) = Step{};
    res.trackLength.fill(0);
    res.trackShuffle.fill(0);
    if (endTick > 0)
        res.length = static_cast<std::uint8_t>(std::clamp<std::int64_t>((endTick + T / 2) / T, 1, kMaxSteps));
    const int len = clampLength(res.length);

    std::stable_sort(notes.begin(), notes.end(), [](const NoteIn& a, const NoteIn& b) {
        if (a.voice != b.voice) return a.voice < b.voice;
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    // Only notes that can land in the measure, and a bounded number per voice
    // (a measure holds at most 64 x 5 per voice), so the grouping below stays
    // cheap even for a huge or hostile file.
    {
        constexpr std::size_t kMaxNotesPerVoice = 1024;
        const std::int64_t limit = static_cast<std::int64_t>(len) * T + T / 2;
        std::size_t w = 0, perVoice = 0;
        for (std::size_t k = 0; k < notes.size(); ++k) {
            if (k > 0 && notes[k].voice != notes[k - 1].voice) perVoice = 0;
            if (notes[k].tick >= limit || perVoice >= kMaxNotesPerVoice) continue;
            ++perVoice;
            notes[w++] = notes[k];
        }
        notes.resize(w);
    }

    // Group each voice's notes back into steps. A step of the writer produces
    // an onset on the grid (odd steps up to 2T/3 late from shuffle), sub-step
    // notes k*T/n after it, and for a flam a grace note (0.6 x velocity) with
    // the main note up to 100 ms later. Those later notes may fall nearer the
    // NEXT step than their own (a 24 ms flam on 32nds at 200 BPM, a shuffled
    // sub-step) or wrap past the measure end to the top, so quantising every
    // note to its nearest step would invent hits. Instead: take the onsets
    // step by step, claim each onset's sub-step and flam notes (cyclically
    // within the measure), and only then quantise whatever is left.
    const std::int64_t M = static_cast<std::int64_t>(len) * T;
    const std::int64_t flamMaxTicks =
        static_cast<std::int64_t>(std::ceil(100000.0 / static_cast<double>(usPerQn) * kMidiFilePpq)) + 2;
    auto cyc = [M](std::int64_t d) { return ((d % M) + M) % M; };
    auto graceOf = [](int grace, int main) {
        return main > grace && std::abs(grace - static_cast<int>(std::lround(0.6 * main))) <= 1;
    };
    // The flam spacing is one value for the whole file (one flamMs, one
    // tempo): take the most common grace-to-main distance, so a main note is
    // never confused with a neighbouring step's note.
    // Our own notes-only files state the spacing (0: no flams), which beats
    // any estimate where sub-step spacings happen to look alike.
    std::int64_t flamTicks = markerFlamTicks;
    if (markerFlamTicks >= 0) {
        flamTicks = markerFlamTicks * kMidiFilePpq / static_cast<std::int64_t>(division);
    } else {
        std::vector<int> hist(static_cast<std::size_t>(flamMaxTicks) + 2, 0);
        for (std::size_t a = 0; a < notes.size(); ++a)
            for (std::size_t c = a + 1; c < notes.size() && notes[c].voice == notes[a].voice; ++c) {
                const std::int64_t d = notes[c].tick - notes[a].tick;
                if (d > flamMaxTicks) break;
                if (d >= 1 && graceOf(notes[a].velocity, notes[c].velocity)) ++hist[static_cast<std::size_t>(d)];
            }
        int bestCount = 0;
        for (std::size_t d = 1; d + 1 < hist.size(); ++d) {
            const int c = hist[d - 1] + hist[d] + hist[d + 1];
            if (c > bestCount) {
                bestCount = c;
                flamTicks = static_cast<std::int64_t>(d);
            }
        }
        if (flamTicks > 0) {  // the peak itself, not the edge of the window
            const auto k = static_cast<std::size_t>(flamTicks);
            if (hist[k - 1] > hist[k] && hist[k - 1] >= hist[k + 1]) --flamTicks;
            else if (hist[k + 1] > hist[k]) ++flamTicks;
        }
    }

    std::size_t i = 0;
    while (i < notes.size()) {
        std::size_t e = i;
        while (e < notes.size() && notes[e].voice == notes[i].voice) ++e;
        const std::size_t count = e - i;
        std::vector<char> used(count, 0);
        const NoteIn* vn = &notes[i];

        // Visit, in index order, the notes whose tick is within [lo, lo + w]
        // cyclically (w < M); `fn` returns true to stop. Ticks lie in
        // [0, M + T/2), so three shifted windows cover every match. The
        // searches below use it instead of scanning every note of the voice,
        // so a dense or hostile file stays O(n log n) per voice; visiting in
        // index order keeps the first-best choice of a full scan.
        auto forCyclic = [&](std::int64_t lo, std::int64_t w, auto&& fn) {
            const std::int64_t base = cyc(lo);
            for (const std::int64_t shift : {-M, std::int64_t{0}, M}) {
                const std::int64_t a0 = base + shift;
                const auto* it = std::lower_bound(vn, vn + count, a0,
                                                  [](const NoteIn& n, std::int64_t t) { return n.tick < t; });
                for (; it != vn + count && it->tick <= a0 + w; ++it)
                    if (fn(static_cast<std::size_t>(it - vn))) return true;
            }
            return false;
        };
        // An unclaimed note near `tick` (cyclically, within one tick), other than `skip`.
        const std::int64_t limitTick = static_cast<std::int64_t>(len) * T + T / 2;
        auto nearUnused = [&](std::int64_t tick, std::size_t skip) {
            for (std::int64_t t0 : {cyc(tick), cyc(tick) + M}) {
                if (t0 - 1 >= limitTick) continue;
                const auto* it = std::lower_bound(vn, vn + count, t0 - 1,
                                                  [](const NoteIn& n, std::int64_t t) { return n.tick < t; });
                for (; it != vn + count && it->tick <= t0 + 1; ++it) {
                    const auto c = static_cast<std::size_t>(it - vn);
                    if (used[c] == 0 && c != skip) return c;
                }
            }
            return count;
        };
        // True when the notes k*T/n after note `g` (k = 1..n-1) are all there,
        // unclaimed, with velocity `vel`: g could be the onset of an xn step.
        auto chainFrom = [&](std::size_t g, int n, int vel) {
            for (int q = 1; q < n; ++q) {
                const std::size_t h = nearUnused(vn[g].tick + std::llround(static_cast<double>(q * T) / n), g);
                if (h == count || vn[h].velocity != vel) return false;
            }
            return true;
        };
        // A note near the top of the measure may be a sub-step note that a
        // shuffled last step wrapped there: it belongs to that step, whose
        // onset and every other sub-step note (same velocity; the onset may
        // be a flam's grace) are present at the end, and the note does not
        // start an xn step of its own. Only x3 and x4 are claimed this way: a
        // lone note half a step after a shuffled last step is far more often
        // the next downbeat than a wrapped x2.
        auto wrappedSub = [&](std::size_t c) {
            if (vn[c].tick > T) return false;
            for (int n = 4; n >= 3; --n)
                for (int k = 1; k < n; ++k) {
                    const std::int64_t off = std::llround(static_cast<double>(k * T) / n);
                    const std::size_t g = nearUnused(vn[c].tick + M - off, c);
                    if (g == count || vn[g].tick + off < M - 1) continue;
                    if (vn[g].velocity != vn[c].velocity && !graceOf(vn[g].velocity, vn[c].velocity)) continue;
                    // The onset must sit in a step window.
                    const std::int64_t sg = (vn[g].tick + 1) / T;
                    const bool inWindow = vn[g].tick <= sg * T + 1 ||
                                          (((sg & 1) != 0) && vn[g].tick <= sg * T + 1 + 2 * T / 3);
                    if (!inWindow) continue;
                    // ...unless the note starts an xn step of its own there.
                    if (chainFrom(g, n, vn[c].velocity) && !chainFrom(c, n, vn[c].velocity)) return true;
                }
            return false;
        };

        auto place = [&](std::size_t a, std::int64_t step) {
            used[a] = 1;
            // Sub-steps: n evenly spaced notes from the onset (largest n first).
            std::array<std::size_t, 3> subIdx{};
            int subs = 1;
            for (int cand = 4; cand >= 2 && subs == 1; --cand) {
                bool all = true;
                for (int k = 1; k < cand && all; ++k) {
                    const std::int64_t target = vn[a].tick + std::llround(static_cast<double>(k * T) / cand);
                    // The closest free note within one tick (a flam's main
                    // note may sit one tick from a sub-step note).
                    all = false;
                    std::int64_t bestErr = 2;
                    forCyclic(target - 1, 2, [&](std::size_t c) {  // err <= 1 lies in this window
                        if (used[c] != 0 || c == a) return false;
                        bool taken = false;
                        for (int q = 1; q < k; ++q) taken = taken || subIdx[static_cast<std::size_t>(q - 1)] == c;
                        const std::int64_t d = cyc(vn[c].tick - target);
                        const std::int64_t err = std::min(d, M - d);
                        // Off by a tick and exactly one flam spacing late: the main note.
                        const bool flamMain = err != 0 && flamTicks > 0 &&
                                              std::llabs(cyc(vn[c].tick - vn[a].tick) - flamTicks) <= 1 &&
                                              graceOf(vn[a].velocity, vn[c].velocity);
                        // Sub-step notes repeat the main velocity. A note on
                        // the step grid is the next step's onset (a shuffled
                        // x2 lands exactly there), unless it is a wrapped
                        // x3/x4 note of the last step.
                        const bool sameHit = vn[c].velocity == vn[a].velocity || graceOf(vn[a].velocity, vn[c].velocity);
                        const std::int64_t gm = cyc(vn[c].tick) % T;
                        const bool wraps = cand >= 3 && target >= M - 1 && !chainFrom(c, cand, vn[c].velocity);
                        // Nor is another step's flam main note (its grace is
                        // one flam spacing earlier and still unclaimed).
                        bool otherMain = false;
                        if (flamTicks > 0) {
                            const std::size_t g = nearUnused(vn[c].tick - flamTicks, a);
                            otherMain = g != count && graceOf(vn[g].velocity, vn[c].velocity);
                        }
                        const bool onGrid = (gm <= 1 || gm >= T - 1) && !wraps;
                        if (!taken && !flamMain && !otherMain && sameHit && !onGrid && err < bestErr) {
                            subIdx[static_cast<std::size_t>(k - 1)] = c;
                            bestErr = err;
                            all = true;
                        }
                        return false;
                    });
                }
                if (all) subs = cand;
            }
            for (int k = 1; k < subs; ++k) used[subIdx[static_cast<std::size_t>(k - 1)]] = 1;
            // Flam: a louder main note whose grace is 0.6 x its velocity, one
            // flam spacing later. In files with no such pairs (foreign files):
            // any clearly louder note within half a step.
            std::size_t best = count;
            std::int64_t bestErr = 2;
            if (flamTicks > 0) {
                // err < 2 lies in this window (a superset; the test is exact).
                forCyclic(vn[a].tick + flamTicks - 1, 2, [&](std::size_t c) {
                    if (used[c] != 0) return false;
                    const std::int64_t err = std::llabs(cyc(vn[c].tick - vn[a].tick) - flamTicks);
                    if (err >= bestErr || !graceOf(vn[a].velocity, vn[c].velocity)) return false;
                    if (subs > 1 && vn[c].velocity != vn[subIdx[0]].velocity) return false;
                    best = c;
                    bestErr = err;
                    return false;
                });
            }
            if (best == count && flamTicks < 0 && T / 2 >= 2) {
                forCyclic(vn[a].tick + 1, T / 2 - 2, [&](std::size_t c) {  // 1 <= d < T/2
                    const std::int64_t d = cyc(vn[c].tick - vn[a].tick);
                    if (used[c] == 0 && d >= 1 && d < T / 2 && vn[c].velocity * 5 > vn[a].velocity * 6) {
                        best = c;
                        return true;
                    }
                    return false;
                });
            }
            bool flam = best != count;
            int mainVel = vn[a].velocity;
            if (flam) {
                used[best] = 1;
                mainVel = vn[best].velocity;
            } else if (subs > 1 && graceOf(vn[a].velocity, vn[subIdx[0]].velocity)) {
                flam = true;  // the main note fell on a sub-step note (one note on that tick)
                mainVel = vn[subIdx[0]].velocity;
            }
            if (step < 0 || step >= len) return;
            Step& cell = res.at(var, vn[a].voice, static_cast<int>(step));
            const StepLevel level = levelOfVelocity(mainVel, classes);
            if (!cell.on() || static_cast<int>(level) > static_cast<int>(cell.level())) {
                Step next{};
                next.setLevel(level);
                next.setFlam(flam);
                next.setSubSteps(subs);
                cell = next;
            }
        };

        // 1) Onsets: the first unclaimed note in each step's window, in step order.
        for (int s = 0; s < len; ++s) {
            const std::int64_t lo = static_cast<std::int64_t>(s) * T - 1;
            const std::int64_t hi = static_cast<std::int64_t>(s) * T + 1 + ((s & 1) != 0 ? 2 * T / 3 : 0);
            const auto* first = std::lower_bound(vn, vn + count, lo,
                                                 [](const NoteIn& n, std::int64_t t) { return n.tick < t; });
            for (auto c = static_cast<std::size_t>(first - vn); c < count && vn[c].tick <= hi; ++c) {
                if (used[c] != 0) continue;
                // A flam's main note that happens to sit in this window (it may
                // have wrapped from the measure end) is left for its grace.
                const bool isMain =
                    flamTicks > 0 && forCyclic(vn[c].tick - flamTicks - 1, 2, [&](std::size_t g) {
                        return g != c && std::llabs(cyc(vn[c].tick - vn[g].tick) - flamTicks) <= 1 &&
                               graceOf(vn[g].velocity, vn[c].velocity);
                    });
                if (isMain || wrappedSub(c)) continue;
                place(c, s);
                break;
            }
        }
        // 2) Anything left (off-grid playing in foreign files): nearest step.
        for (std::size_t c = 0; c < count; ++c)
            if (used[c] == 0) place(c, (vn[c].tick + T / 2) / T);
        i = e;
    }
    out = res;
    return true;
}

}  // namespace mw909::seq
