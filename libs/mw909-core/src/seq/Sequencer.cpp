// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Sample-accurate step sequencer (docs/design/architecture.md §5).
//
// What the original machine does (research/03 §0, §2): a crystal-clocked CPU
// steps through 1..16 steps in one of four scales, delays the off-beat steps
// on a 1/48-beat grid for SHUFFLE (16th and 32nd scales only), fires a flam's
// second trigger a fixed number of ms after the first, and writes a 6-bit
// accent-DAC code per voice next to every trigger (normal + per-instrument
// accent + TOTAL ACCENT row x knob). A pattern switch waits for the end of the
// measure. Patterns live in 2 banks x 3 groups x 16; TRACK mode plays a list
// of pattern numbers one per measure. Per-track length, per-track shuffle,
// weak steps, sub-steps, probability, A/B variations, fills and flam/accent
// on every voice are extensions carried over from mw808 (research/03 §8).
//
// Timing model (research/05; carried from mw808, where it was proven):
//  * Every hit has an exact time in quarter notes (step + shuffle + sub-step)
//    plus a tempo-independent delay in samples (the flam in ms, and the
//    optional timing jitter in ms).
//  * In a range that starts at ppq0, a hit lands on sample
//        floor((ppq - ppq0) * spp + extra + 0.5 + kSnapEps)
//    (round to nearest; kSnapEps keeps exact half-sample positions, common at
//    44.1 kHz, from flipping under ~1e-12 qn of host PPQ noise). It belongs to
//    this range when that offset is < numSamples; otherwise it waits in the
//    pending queue, which keeps the musical time, so the decision is the same
//    for any block partition.
//  * Steps are walked with a cursor (measure start + next step index): a step
//    is decided once, when its own sample enters a range. Pattern switches,
//    fills and chain/track moves happen lazily when the next measure's
//    downbeat enters a range, so a request made any time before then applies.
//  * HostSync derives position from host PPQ on every call; Internal/HostNote
//    use a clock of (PPQ at last tempo change + samples since / spp), so a
//    constant tempo gives the same PPQ for any partition.
//  * HostSync converts PPQ to samples at the host's own tempo (1..1000 BPM);
//    the 20..400 clamp applies to tempo() and to the internal clock.
//  * Relocation: host PPQ more than max(2 samples, 1 ms) outside the
//    continuation of the previous call, taken at any tempo between the
//    previous and the current host tempo (step change or ramp). Smaller
//    differences are host rounding and are absorbed (a hit that the noise
//    pushes before the range start is emitted at offset 0, never twice).
//    A host that holds PPQ still while rolling (within that tolerance of the
//    previous range start) is a stall, not a relocation: nothing re-fires.
//    A host PPQ with no usable tempo is mapped at the rate it advanced over
//    the previous range, so its blocks stay continuous.
//  * Tap write marks the recorded step (measure, step, pattern, variation):
//    it is skipped once, and only when that measure plays the same pattern
//    and variation.
//  * Relocation and start find the measure by walking measures from PPQ 0
//    (chain/track entries and auto fills may differ in length and scale), so
//    playback is a pure function of host PPQ.

#include "mw909/seq/Sequencer.h"

#include <algorithm>
#include <cmath>

#include "mw909/seq/SeqHash.h"
#include "SeqTiming.h"

namespace mw909::seq {

namespace {

constexpr double kMinBpm = 20.0;
constexpr double kMaxBpm = 400.0;
// HostSync only: the range of host tempos the PPQ-to-sample mapping follows
// (hosts offer roughly 1..999 BPM). tempo() still reports kMinBpm..kMaxBpm.
constexpr double kMinHostBpm = 1.0;
constexpr double kMaxHostBpm = 1000.0;
constexpr double kSnapEps = 1.0e-6;          // samples
constexpr double kMaxAbsPpq = 1.0e9;         // beyond this a host position is treated as invalid

template <typename T>
constexpr T clampv(T x, T lo, T hi) noexcept {
    return x < lo ? lo : (x > hi ? hi : x);
}

template <typename T>
T finiteOr(T x, T fallback) noexcept {
    return std::isfinite(x) ? x : fallback;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t m) noexcept {
    std::int64_t q = a / m;
    if ((a % m != 0) && ((a < 0) != (m < 0))) --q;
    return q;
}

std::int64_t floorMod(std::int64_t a, std::int64_t m) noexcept {
    const std::int64_t r = a % m;
    return r < 0 ? r + m : r;
}

int gcd(int a, int b) noexcept {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

Scale scaleOf(const Pattern& p) noexcept { return detail::validScale(p.scale); }

int patternLength(const Pattern& p) noexcept { return clampv<int>(p.length, 1, kMaxSteps); }

int trackLength(const Pattern& p, int track) noexcept { return clampv(p.lengthOf(track), 1, kMaxSteps); }

double stepQnOf(const Pattern& p) noexcept { return stepLengthQn(scaleOf(p)); }

Settings sanitize(const Settings& in) noexcept {
    Settings s = in;
    if (static_cast<unsigned>(s.mode) > static_cast<unsigned>(Mode::HostNote)) s.mode = Mode::Off;
    s.internalBpm = finiteOr(s.internalBpm, 120.0);
    s.totalAccent = clampv(finiteOr(s.totalAccent, 0.7f), 0.0f, 1.0f);
    s.swingScale = clampv(finiteOr(s.swingScale, 1.0f), 0.0f, 1.0f);
    if (static_cast<unsigned>(s.variation) > static_cast<unsigned>(VariationMode::B)) s.variation = VariationMode::A;
    if (static_cast<unsigned>(s.autoFill) > static_cast<unsigned>(AutoFill::Every2)) s.autoFill = AutoFill::Manual;
    if (s.fillSlot >= kPatternsPerGroup) s.fillSlot = kPatternsPerGroup - 1;
    if (static_cast<unsigned>(s.play) > static_cast<unsigned>(PlayMode::Track)) s.play = PlayMode::Pattern;
    if (s.songTrack >= kNumSongTracks) s.songTrack = kNumSongTracks - 1;
    return s;
}

std::int64_t offsetOf(double ppq, double extraSamples, double ppq0, double spp) noexcept {
    double x = (ppq - ppq0) * spp + extraSamples + 0.5 + kSnapEps;
    if (!(x < 4.0e18)) x = 4.0e18;  // also catches NaN
    if (x < -4.0e18) x = -4.0e18;
    return static_cast<std::int64_t>(std::floor(x));
}

}  // namespace

Sequencer::Sequencer() noexcept { clearSuppress(); }

void Sequencer::prepare(double sampleRate) noexcept {
    sampleRate_ = (std::isfinite(sampleRate) && sampleRate > 0.0) ? clampv(sampleRate, 1000.0, 1.0e6) : 48000.0;
    reset();
}

void Sequencer::reset() noexcept {
    stopPlayback();
    activeMode_ = Mode::Off;
    measure_ = 0;
    measureStart_ = 0.0;
    nextStep_ = 0;
    fillRequested_ = false;
    noteHeld_ = false;
    noteRestart_ = false;
    prevPpq0_ = 0.0;
    prevN_ = 0;
    prevSpp_ = sampleRate_ * 60.0 / tempo_;
    prevFollowHost_ = false;
    prevRecord_ = false;
    segPpq_ = 0.0;
    segSamples_ = 0;
    segSpp_ = prevSpp_;
    order_ = 0;
    numScratch_ = 0;
}

void Sequencer::setBank(const PatternBank& b) noexcept {
    // The play position is kept; the playing measure's length and scale are
    // latched, so new content applies to the steps not yet fired.
    bank_ = b;
}

void Sequencer::setPattern(int index, const Pattern& p) noexcept {
    if (index < 0 || index >= kNumPatterns) return;
    bank_.patterns[static_cast<std::size_t>(index)] = p;
}

void Sequencer::setChain(const Chain& c) noexcept { bank_.chain = c; }

void Sequencer::setSongTrack(int index, const SongTrack& t) noexcept {
    if (index < 0 || index >= kNumSongTracks) return;
    bank_.tracks[static_cast<std::size_t>(index)] = t;
}

void Sequencer::selectPattern(int index, bool immediate) noexcept {
    if (index < 0 || index >= kNumPatterns) return;
    if (!playing_) {
        selectedPattern_ = index;
        currentPattern_ = index;
        queuedPattern_ = -1;
        return;
    }
    if (!immediate) {
        // Re-selecting the pattern already selected cancels a queued change.
        queuedPattern_ = (index == selectedPattern_) ? -1 : index;
        return;
    }
    selectedPattern_ = index;
    queuedPattern_ = -1;
    if (listLength() > 0) return;  // the chain/track decides what sounds
    // Take over now, keeping the step position: the next step keeps its time.
    const Pattern& p = bank_.patterns[static_cast<std::size_t>(index)];
    const double nextTime = measureStart_ + nextStep_ * stepQn_;
    const int newLen = patternLength(p);
    const double newStepQn = stepQnOf(p);
    const int ns = std::min(nextStep_, newLen);
    currentPattern_ = index;
    measureLen_ = newLen;
    stepQn_ = newStepQn;
    nextStep_ = ns;
    measureStart_ = nextTime - ns * newStepQn;
    if (currentStep_ >= newLen) currentStep_ = newLen - 1;
}

void Sequencer::requestFill() noexcept { fillRequested_ = true; }

void Sequencer::notePatternOn(int index) noexcept {
    if (index < 0 || index >= kNumPatterns) return;
    notePattern_ = index;
    noteHeld_ = true;
    noteRestart_ = true;
}

void Sequencer::notePatternOff() noexcept {
    noteHeld_ = false;
    noteRestart_ = false;
}

RecordResult Sequencer::recordHit(VoiceId voice, bool accent, int sampleOffset) noexcept {
    RecordResult r;
    if (!playing_ || !prevRecord_ || static_cast<int>(voice) >= kNumVoices) return r;
    const int v = static_cast<int>(voice);
    const double t = prevPpq0_ + (static_cast<double>(prevN_) + std::max(0, sampleOffset)) / prevSpp_;
    // Nearest step of the playing measure's grid, ignoring shuffle (research/03
    // §2.7: TAP WRITE quantises to the scale step; rounding to nearest).
    const double rel = std::floor((t - measureStart_) / stepQn_ + 0.5);
    const auto stepAbs = static_cast<std::int64_t>(clampv(rel, -1.0e15, 1.0e15));
    const std::int64_t ahead = floorDiv(stepAbs, measureLen_);
    const int s = static_cast<int>(floorMod(stepAbs, measureLen_));
    const std::int64_t m = measure_ + ahead;

    Pattern& p = bank_.patterns[static_cast<std::size_t>(currentPattern_)];
    const int var = currentVariation_;
    const int idx = static_cast<int>(floorMod(m * measureLen_ + s, trackLength(p, v)));
    Step& cell = p.at(var, v, idx);
    Step next = cell.on() ? cell : Step{};
    next.setLevel(accent ? StepLevel::Accent : StepLevel::Normal);
    r.changed = !(next == cell);
    cell = next;
    r.pattern = static_cast<std::uint8_t>(currentPattern_);
    r.variation = static_cast<std::uint8_t>(var);
    r.track = static_cast<std::uint8_t>(v);
    r.step = static_cast<std::uint8_t>(idx);
    r.cell = next;

    // The live hit already sounded: if that step has not fired yet on this
    // pass, it must not fire again.
    std::int64_t upM = measure_;
    int upS = nextStep_;
    if (upS >= measureLen_) {
        ++upM;
        upS = 0;
    }
    if (m > upM || (m == upM && s >= upS)) {
        // Reuse a slot that is free, already holds this step, or names a step
        // that is now behind the cursor (it can never match again); if all
        // are live, replace the earliest.
        auto& slots = suppress_[static_cast<std::size_t>(v)];
        int slot = -1;
        int earliest = 0;
        for (int i = 0; i < kSuppressSlots; ++i) {
            const Suppress& q = slots[static_cast<std::size_t>(i)];
            const bool stale = q.step < 0 || q.measure < upM || (q.measure == upM && q.step < upS);
            if (stale || (q.measure == m && q.step == s && q.pattern == currentPattern_ && q.variation == var)) {
                slot = i;
                break;
            }
            const Suppress& e = slots[static_cast<std::size_t>(earliest)];
            if (q.measure < e.measure || (q.measure == e.measure && q.step < e.step)) earliest = i;
        }
        if (slot < 0) slot = earliest;
        Suppress& q = slots[static_cast<std::size_t>(slot)];
        if (q.step < 0) ++numSuppress_[static_cast<std::size_t>(v)];
        q.measure = m;
        q.step = static_cast<std::int16_t>(s);
        q.pattern = static_cast<std::uint8_t>(currentPattern_);
        q.variation = static_cast<std::uint8_t>(var);
    }
    return r;
}

// ---------------------------------------------------------------------------

void Sequencer::clearSuppress() noexcept {
    for (auto& a : suppress_) a.fill(Suppress{});
    numSuppress_.fill(0);
}

bool Sequencer::takeSuppressed(int voice, std::int64_t measure, int step, int pattern, int variation) noexcept {
    int& live = numSuppress_[static_cast<std::size_t>(voice)];
    if (live <= 0) return false;
    for (Suppress& q : suppress_[static_cast<std::size_t>(voice)]) {
        if (q.step == step && q.measure == measure && q.pattern == pattern && q.variation == variation) {
            q.step = -1;
            --live;
            return true;
        }
    }
    return false;
}

void Sequencer::stopPlayback() noexcept {
    playing_ = false;
    currentStep_ = -1;
    songPosition_ = -1;
    numPending_ = 0;
    clearSuppress();
    if (queuedPattern_ >= 0) selectedPattern_ = queuedPattern_;  // stopped: takes over at once
    queuedPattern_ = -1;
    currentPattern_ = selectedPattern_;
    nextStep_ = 0;
}

int Sequencer::listLength() const noexcept {
    if (settings_.mode == Mode::HostNote) return 0;  // a held pattern note plays that pattern
    switch (settings_.play) {
        case PlayMode::Chain: return std::min<int>(bank_.chain.length, kMaxChainLength);
        case PlayMode::Track:
            return std::min<int>(bank_.tracks[static_cast<std::size_t>(settings_.songTrack)].length, kMaxSongLength);
        default: return 0;
    }
}

int Sequencer::listEntry(int index) const noexcept {
    const int e = settings_.play == PlayMode::Chain
                      ? bank_.chain.entries[static_cast<std::size_t>(index)]
                      : bank_.tracks[static_cast<std::size_t>(settings_.songTrack)]
                            .entries[static_cast<std::size_t>(index)];
    return e < kNumPatterns ? e : kNumPatterns - 1;
}

int Sequencer::basePatternFor(std::int64_t measure) const noexcept {
    const int len = listLength();
    if (len <= 0) return selectedPattern_;
    return listEntry(static_cast<int>(floorMod(measure, len)));
}

int Sequencer::fillPatternFor(int basePattern) const noexcept {
    return patternIndex(bankOf(basePattern), groupOf(basePattern), settings_.fillSlot);
}

bool Sequencer::autoFillAt(std::int64_t measure) const noexcept {
    const int n = autoFillMeasures(settings_.autoFill);
    return n > 0 && floorMod(measure, n) == n - 1;
}

int Sequencer::patternAt(std::int64_t measure) const noexcept {
    const int base = basePatternFor(measure);
    return autoFillAt(measure) ? fillPatternFor(base) : base;
}

int Sequencer::variationFor(std::int64_t measure) const noexcept {
    switch (settings_.variation) {
        case VariationMode::B: return 1;
        case VariationMode::AB: return static_cast<int>(floorMod(measure, 2));
        default: return 0;
    }
}

double Sequencer::measureLengthQn(int pattern) const noexcept {
    const Pattern& p = bank_.patterns[static_cast<std::size_t>(pattern)];
    return patternLength(p) * stepQnOf(p);
}

void Sequencer::startMeasure(std::int64_t measure, double startPpq, int pattern) noexcept {
    const Pattern& p = bank_.patterns[static_cast<std::size_t>(pattern)];
    measure_ = measure;
    measureStart_ = startPpq;
    currentPattern_ = pattern;
    measureLen_ = patternLength(p);
    stepQn_ = stepQnOf(p);
    currentVariation_ = variationFor(measure);
    const int len = listLength();
    songPosition_ = len > 0 ? static_cast<int>(floorMod(measure, len)) : -1;
    nextStep_ = 0;
}

void Sequencer::advanceMeasure(double startPpq) noexcept {
    const std::int64_t m = measure_ + 1;
    if (queuedPattern_ >= 0) {
        selectedPattern_ = queuedPattern_;
        queuedPattern_ = -1;
    }
    int pattern = patternAt(m);
    if (fillRequested_) {
        pattern = fillPatternFor(basePatternFor(m));
        fillRequested_ = false;
    }
    startMeasure(m, startPpq, pattern);
}

// Put the cursor on the first step whose sample is at or after `ppq`, with
// the measure structure a pure function of PPQ from 0 (chain/track entries
// and auto fill included), so a relocation lands where uninterrupted playback
// would be. A fill requested while stopped plays when the start lands on a
// downbeat (otherwise it waits for the next one).
void Sequencer::locate(double ppq, double spp) noexcept {
    const int listLen = std::max(1, listLength());
    const int fillN = std::max(1, autoFillMeasures(settings_.autoFill));
    const int period = listLen / gcd(listLen, fillN) * fillN;  // <= 256 * 16
    double cycle = 0.0;
    for (int k = 0; k < period; ++k) cycle += measureLengthQn(patternAt(k));

    const double c = std::floor(ppq / cycle);
    double start = c * cycle;
    double rest = ppq - start;
    int k = 0;
    while (k < period - 1) {
        const double len = measureLengthQn(patternAt(k));
        if (rest < len) break;
        rest -= len;
        start += len;
        ++k;
    }
    const std::int64_t measure = static_cast<std::int64_t>(c) * period + k;
    startMeasure(measure, start, patternAt(measure));

    int s = static_cast<int>(clampv(std::floor((ppq - start) / stepQn_), 0.0, static_cast<double>(measureLen_)));
    if (s < measureLen_ && offsetOf(start + s * stepQn_, 0.0, ppq, spp) < 0) ++s;
    if (s == 0 && fillRequested_) {
        fillRequested_ = false;
        startMeasure(measure, start, fillPatternFor(basePatternFor(measure)));
    }
    nextStep_ = s;
    currentStep_ = s > 0 ? s - 1 : -1;
}

void Sequencer::pushScratch(std::int64_t offset, std::uint64_t order, const Event& e) noexcept {
    if (numScratch_ >= kMaxScratch) return;  // more hits than one block can carry: drop
    Scratch& s = scratch_[static_cast<std::size_t>(numScratch_++)];
    s.offset = offset < 0 ? 0 : offset;
    s.order = order;
    s.event = e;
}

void Sequencer::emit(double ppq, double extraSamples, const Event& e, double ppq0, double spp,
                     int numSamples) noexcept {
    const std::int64_t off = offsetOf(ppq, extraSamples, ppq0, spp);
    const std::uint64_t order = order_++;
    if (off < numSamples) {
        pushScratch(off, order, e);
    } else if (numPending_ < kMaxPending) {
        Pending& p = pending_[static_cast<std::size_t>(numPending_++)];
        p.ppq = ppq;
        p.extraSamples = extraSamples;
        p.order = order;
        p.event = e;
    }
    // else: pending queue full, the hit is dropped (bounded by design).
}

void Sequencer::fireStep(double stepPpq, double ppq0, double spp, int numSamples) noexcept {
    const int patIndex = currentPattern_;
    const Pattern& p = bank_.patterns[static_cast<std::size_t>(patIndex)];
    const int s = nextStep_;
    const int var = currentVariation_;
    const Scale scale = scaleOf(p);
    const bool accentOn = p.at(var, kAccentTrack, s).on();
    const int totalCodes =
        accentOn ? static_cast<int>(static_cast<float>(AccentDac::kTotalAccentCodes) * settings_.totalAccent + 0.5f)
                 : 0;
    // Flam: fixed in ms, tempo independent. A flam shorter than one sample
    // would put both triggers on one sample: play the main hit alone then.
    const double flamSamples =
        static_cast<double>(clampv<int>(p.flamMs, 0, kMaxFlamMs)) * 0.001 * sampleRate_;
    const bool flamAudible = flamSamples >= 1.0;
    const float grace = static_cast<float>(clampv<int>(p.flamGrace, 0, 100)) / 100.0f;
    const auto subStart = static_cast<float>(clampv<int>(p.subStart, 0, 100));
    const auto subEnd = static_cast<float>(clampv<int>(p.subEnd, 0, 100));
    // Timing jitter: one delay for every hit of this step.
    const double jitter =
        settings_.jitter ? jitterMs(settings_.jitterSeed, measure_, s) * 0.001 * sampleRate_ : 0.0;
    const std::int64_t polyIndex = measure_ * measureLen_ + s;

    for (int v = 0; v < kNumVoices; ++v) {
        const Step cell = p.at(var, v, static_cast<int>(floorMod(polyIndex, trackLength(p, v))));
        if (!cell.on()) continue;
        if (takeSuppressed(v, measure_, s, patIndex, var)) continue;
        if (!probabilityPasses(cell.probabilityCode(), patIndex, measure_, s, v)) continue;

        const StepLevel level = cell.level();
        const int code = (level == StepLevel::Weak ? AccentDac::kWeakCode : AccentDac::kNormalCode) +
                         (level == StepLevel::Accent ? AccentDac::kVoiceAccentCodes : 0) + totalCodes;
        Event e;
        e.voice = static_cast<VoiceId>(v);
        e.level = level;
        e.totalAccent = accentOn;
        e.hit = Hit::fromCode(code);
        const float acc = e.hit.acc;

        const double t0 = stepPpq + detail::shuffleQn(scale, p.shuffleOf(v), settings_.swingScale, s, measureLen_);
        const int subs = cell.subSteps();
        const double subQn = stepQn_ / subs;
        for (int k = 0; k < subs; ++k) {
            const double t = t0 + k * subQn;
            Event hit = e;
            if (subs > 1)
                hit.hit.acc = acc * (subStart + (subEnd - subStart) * static_cast<float>(k) /
                                                    static_cast<float>(subs - 1)) / 100.0f;
            if (k == 0 && cell.flam() && flamAudible) {
                Event g = hit;
                g.hit.acc = hit.hit.acc * grace;
                if (p.flamGrace < 100) {  // a lighter grace note reads as a weak hit on MIDI out
                    g.level = StepLevel::Weak;
                    g.totalAccent = false;
                }
                emit(t, jitter, g, ppq0, spp, numSamples);
                emit(t, jitter + flamSamples, hit, ppq0, spp, numSamples);
            } else {
                emit(t, jitter, hit, ppq0, spp, numSamples);
            }
        }
    }
}

void Sequencer::process(const Transport& transport, const Settings& settingsIn, int numSamples, EventBuffer& out,
                        int offsetBase) noexcept {
    settings_ = sanitize(settingsIn);
    const Settings& st = settings_;

    const bool hostTempo = transport.hostValid && std::isfinite(transport.bpm) && transport.bpm > 0.0;
    double bpm = hostTempo ? transport.bpm : st.internalBpm;
    bpm = clampv(bpm, kMinBpm, kMaxBpm);
    tempo_ = bpm;
    if (numSamples <= 0) return;
    const int n = numSamples;
    // In HostSync the host owns the position: its PPQ advances at the host's
    // real tempo whatever we clamp. Converting PPQ to samples at the clamped
    // tempo would put every block outside the relocation tolerance (a 600 BPM
    // host lost a third of its steps; a 10 BPM host double-fired every step),
    // so the mapping follows the host rate within a wider sanity range.
    const bool hostSync = st.mode == Mode::HostSync && transport.hostValid;
    double rateBpm = (hostSync && hostTempo) ? clampv(transport.bpm, kMinHostBpm, kMaxHostBpm) : bpm;
    if (hostSync && !hostTempo && playing_ && prevFollowHost_ && prevN_ > 0 && std::isfinite(transport.ppq)) {
        // A host position without a tempo: the PPQ still advances at the
        // host's real rate, so mapping it at internalBpm would make every
        // block a relocation and drop the steps in between. Follow the rate
        // the host actually advanced at over the previous range; a stall
        // keeps the previous rate, and a jump outside 1..1000 BPM keeps it
        // too, so the relocation test below still sees it.
        const double est = (transport.ppq - prevPpq0_) * 60.0 * sampleRate_ / static_cast<double>(prevN_);
        rateBpm = (est >= kMinHostBpm && est <= kMaxHostBpm) ? est : sampleRate_ * 60.0 / prevSpp_;
        tempo_ = clampv(rateBpm, kMinBpm, kMaxBpm);
    }
    const double spp = sampleRate_ * 60.0 / rateBpm;

    // --- what should be playing ---------------------------------------------
    // HostSync without a host transport behaves as Internal (architecture.md §5.1).
    bool want = false;
    switch (st.mode) {
        case Mode::HostSync:
            want = hostSync ? (transport.playing && std::isfinite(transport.ppq) &&
                               std::abs(transport.ppq) < kMaxAbsPpq)
                            : st.run;
            break;
        case Mode::Internal: want = st.run; break;
        case Mode::HostNote: want = noteHeld_; break;
        default: want = false; break;
    }
    const bool followHost = hostSync;
    const Mode clockMode = (st.mode == Mode::HostSync && !followHost) ? Mode::Internal : st.mode;
    if (playing_ && (clockMode != activeMode_ || (st.mode == Mode::HostNote && noteRestart_))) stopPlayback();
    if (!want) {
        if (playing_) stopPlayback();
        if (st.mode != Mode::HostNote) noteRestart_ = false;
        currentVariation_ = st.variation == VariationMode::B ? 1 : 0;
        prevRecord_ = false;
        return;
    }

    // --- position -------------------------------------------------------------
    const bool starting = !playing_;
    bool relocate = false;
    double ppq0 = 0.0;
    if (followHost) {
        ppq0 = transport.ppq;
        if (!starting) {
            // The host advanced the previous range at some tempo between the
            // one it reported then and the one it reports now (a step change
            // lands on either end, a ramp in between), so the continuation is
            // a window, widened by the tolerance.
            const double eOld = prevPpq0_ + static_cast<double>(prevN_) / prevSpp_;
            const double eNew = prevPpq0_ + static_cast<double>(prevN_) / spp;
            const double tolQn = std::max(2.0, 0.001 * sampleRate_) / spp;
            const double lo = std::min(eOld, eNew) - tolQn;
            const double hi = std::max(eOld, eNew) + tolQn;
            // A host that holds its position while rolling (count-in,
            // buffering) repeats the previous range start: that range has
            // already fired, so keep the cursor instead of re-firing the step
            // there on every block. A real jump back lands elsewhere.
            const bool stalled = std::abs(ppq0 - prevPpq0_) <= tolQn;
            if (!(ppq0 >= lo && ppq0 <= hi) && !stalled) relocate = true;
        }
    } else {
        if (starting) {
            segPpq_ = 0.0;
            segSamples_ = 0;
            segSpp_ = spp;
        } else if (spp != segSpp_) {  // tempo change: takes effect from this call
            segPpq_ += static_cast<double>(segSamples_) / segSpp_;
            segSamples_ = 0;
            segSpp_ = spp;
        }
        ppq0 = segPpq_ + static_cast<double>(segSamples_) / segSpp_;
    }

    if (starting) {
        playing_ = true;
        activeMode_ = clockMode;
        numPending_ = 0;
        clearSuppress();
        if (st.mode == Mode::HostNote) {
            selectedPattern_ = notePattern_;
            queuedPattern_ = -1;
            noteRestart_ = false;
        }
        locate(ppq0, spp);
    } else if (relocate) {
        numPending_ = 0;
        clearSuppress();
        locate(ppq0, spp);
    }

    // --- hits ---------------------------------------------------------------
    numScratch_ = 0;
    int keep = 0;
    for (int i = 0; i < numPending_; ++i) {
        const Pending& p = pending_[static_cast<std::size_t>(i)];
        const std::int64_t off = offsetOf(p.ppq, p.extraSamples, ppq0, spp);
        if (off < n)
            pushScratch(off, p.order, p.event);
        else
            pending_[static_cast<std::size_t>(keep++)] = p;
    }
    numPending_ = keep;

    for (;;) {
        const double t = measureStart_ + nextStep_ * stepQn_;
        if (offsetOf(t, 0.0, ppq0, spp) >= n) break;
        if (nextStep_ >= measureLen_) {
            advanceMeasure(t);
            continue;
        }
        fireStep(t, ppq0, spp, n);
        currentStep_ = nextStep_;
        ++nextStep_;
    }

    std::sort(scratch_.begin(), scratch_.begin() + numScratch_, [](const Scratch& a, const Scratch& b) {
        return a.offset != b.offset ? a.offset < b.offset : a.order < b.order;
    });
    for (int i = 0; i < numScratch_; ++i) {
        Event e = scratch_[static_cast<std::size_t>(i)].event;
        e.offset = static_cast<std::int32_t>(scratch_[static_cast<std::size_t>(i)].offset + offsetBase);
        if (!out.push(e)) break;
    }
    numScratch_ = 0;

    prevPpq0_ = ppq0;
    prevN_ = n;
    prevSpp_ = spp;
    prevFollowHost_ = followHost;
    prevRecord_ = st.record;
    if (!followHost) segSamples_ += n;
}

}  // namespace mw909::seq
