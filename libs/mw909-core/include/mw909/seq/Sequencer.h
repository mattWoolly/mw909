// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Sample-accurate step sequencer. Pure logic: it turns a transport description
// into timed voice hits and never touches audio. Every method here is called
// on the audio thread and must not allocate, lock or block.
//
// The PUBLIC interface below is a contract with mw909::Machine and the tests;
// private members are free to change. Full semantics: docs/design/architecture.md §5.

#pragma once

#include <array>
#include <cstdint>

#include "mw909/Voices.h"
#include "mw909/seq/Pattern.h"

namespace mw909::seq {

enum class Mode : std::uint8_t {
    Off = 0,    // sequencer silent; voices are played from MIDI only
    HostSync,   // runs while the host transport rolls, locked to host PPQ
    Internal,   // free-running on its own clock, started by Settings::run
    HostNote    // a held MIDI "pattern note" plays that pattern from its top
};

// Host transport at the FIRST sample of the range passed to process().
struct Transport {
    bool hostValid = false;  // host supplied tempo/position
    bool playing = false;    // host transport rolling
    double ppq = 0.0;        // position in quarter notes
    double bpm = 120.0;      // host tempo (followed over 1..1000 BPM in HostSync)
};

// Per-block settings (from plugin parameters).
struct Settings {
    Mode mode = Mode::HostSync;
    bool run = false;                  // Internal mode start/stop
    double internalBpm = 120.0;        // used when the host gives no tempo (37..290 on the hardware; 20..400 here)
    float totalAccent = 0.7f;          // TOTAL ACCENT knob, 0..1
    float swingScale = 1.0f;           // global multiplier on every shuffle, 0..1 (1 = as stored)
    VariationMode variation = VariationMode::A;
    AutoFill autoFill = AutoFill::Manual;
    std::uint8_t fillSlot = 15;        // 0..15: fill pattern = this slot of the playing pattern's group
    PlayMode play = PlayMode::Pattern;
    std::uint8_t songTrack = 0;        // 0..kNumSongTracks-1, used when play == Track
    bool record = false;               // recordHit() writes into the playing pattern (TAP WRITE)
    bool jitter = false;               // the hardware's interrupt-clock timing jitter (§5.3)
    std::uint32_t jitterSeed = 0;      // unit seed; jitter is a deterministic hash of (seed, measure, step)
};

struct Event {
    std::int32_t offset = 0;   // sample offset inside the processed range
    VoiceId voice = BD;
    StepLevel level = StepLevel::Normal;  // the step's own class (for MIDI out)
    bool totalAccent = false;             // the total-accent row was set on this step (for MIDI out)
    Hit hit{};                            // the ACC level the voice receives (§5.3)
};

struct EventBuffer {
    static constexpr int kCapacity = 1024;
    std::array<Event, kCapacity> events{};
    int count = 0;

    void clear() noexcept { count = 0; }
    bool push(const Event& e) noexcept {
        if (count >= kCapacity) return false;
        events[static_cast<std::size_t>(count++)] = e;
        return true;
    }
};

struct RecordResult {
    bool changed = false;
    std::uint8_t pattern = 0, variation = 0, track = 0, step = 0;
    Step cell{};
};

class Sequencer {
public:
    Sequencer() noexcept;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;  // stop, clear pending hits, rewind; keeps the bank

    // --- pattern memory (audio-thread copy) --------------------------------
    const PatternBank& bank() const noexcept { return bank_; }
    void setBank(const PatternBank& b) noexcept;   // whole memory (state restore); keeps the play position
    void setPattern(int index, const Pattern& p) noexcept;
    void setChain(const Chain& c) noexcept;
    void setSongTrack(int index, const SongTrack& t) noexcept;

    // --- performance controls ----------------------------------------------
    // Queue a pattern; it takes over at the end of the current measure
    // (immediately when stopped, or when `immediate`, keeping the step position).
    void selectPattern(int index, bool immediate = false) noexcept;
    // Play the fill pattern for the next measure, once.
    void requestFill() noexcept;
    // HostNote mode: a pattern note went down / the last one came up.
    void notePatternOn(int index) noexcept;
    void notePatternOff() noexcept;
    // TAP WRITE: quantise a played hit, `sampleOffset` samples into the NEXT
    // process() range, to the nearest step of the playing pattern. Has no
    // effect unless playing with Settings::record set on the previous process().
    RecordResult recordHit(VoiceId voice, bool accent, int sampleOffset) noexcept;

    // --- the clock ----------------------------------------------------------
    // Advance by numSamples and append the hits that fall inside the range to
    // `out` (sorted by offset; offsets are relative to the range start plus
    // `offsetBase`). May be called on consecutive sub-ranges of a host block.
    void process(const Transport& transport, const Settings& settings, int numSamples,
                 EventBuffer& out, int offsetBase = 0) noexcept;

    // --- state for the UI (read on the audio thread, published by Machine) ---
    bool isPlaying() const noexcept { return playing_; }
    int currentPattern() const noexcept { return currentPattern_; }   // the one sounding now (may be a fill)
    int selectedPattern() const noexcept { return selectedPattern_; } // the basic pattern looped/edited
    int queuedPattern() const noexcept { return queuedPattern_; }     // -1 if none
    int currentVariation() const noexcept { return currentVariation_; }
    int currentStep() const noexcept { return currentStep_; }         // 0-based within the measure, -1 when stopped
    std::int64_t measureCount() const noexcept { return measure_; }   // pass count used for polymeter playheads (§5.3)
    int songPosition() const noexcept { return songPosition_; }       // entry index in the chain/track, -1 if none
    double tempo() const noexcept { return tempo_; }                  // effective BPM, display-clamped to 20..400

private:
    // The implementation owns everything below and may restructure it freely.
    //
    // Timing model (Sequencer.cpp has the full story): every hit carries an
    // exact musical time in quarter notes (step + shuffle + sub-step) plus a
    // tempo-independent delay in samples (flam, timing jitter). Inside a
    // process() range starting at ppq0 the hit lands on sample
    // floor((ppq - ppq0) * spp + extra + 0.5 + eps). Steps are walked with a
    // cursor (measure start + step index), so each step is decided exactly
    // once whatever the block partition.
    struct Pending {
        double ppq = 0.0;            // musical time (step + shuffle + sub-step)
        double extraSamples = 0.0;   // flam + jitter delay on top, in samples
        std::uint64_t order = 0;     // generation order: tie-break at equal offsets
        Event event{};
    };
    struct Scratch {
        std::int64_t offset = 0;
        std::uint64_t order = 0;
        Event event{};
    };
    // One process() call can hand out at most EventBuffer::kCapacity hits;
    // more are dropped, and which ones depends on the partition. A step holds
    // at most 11 voices x 5 hits (4 sub-steps + a flam grace), so a range of
    // n samples carries at most 55 x (n / stepSamples + 2) hits: callers keep
    // ranges at or below 2048 samples (and flush the buffer between ranges)
    // to stay exact at 1000 BPM on the 32nd scale down to 22.05 kHz.
    static constexpr int kMaxPending = 1024;
    static constexpr int kMaxScratch = EventBuffer::kCapacity;

    void stopPlayback() noexcept;
    void clearSuppress() noexcept;
    bool takeSuppressed(int voice, std::int64_t measure, int step, int pattern, int variation) noexcept;
    void locate(double ppq, double spp) noexcept;
    void startMeasure(std::int64_t measure, double startPpq, int pattern) noexcept;
    void advanceMeasure(double startPpq) noexcept;
    void fireStep(double stepPpq, double ppq0, double spp, int numSamples) noexcept;
    void emit(double ppq, double extraSamples, const Event& e, double ppq0, double spp,
              int numSamples) noexcept;
    void pushScratch(std::int64_t offset, std::uint64_t order, const Event& e) noexcept;
    int listLength() const noexcept;               // active chain/track length, 0 = none
    int listEntry(int index) const noexcept;
    int basePatternFor(std::int64_t measure) const noexcept;
    int fillPatternFor(int basePattern) const noexcept;
    bool autoFillAt(std::int64_t measure) const noexcept;
    int patternAt(std::int64_t measure) const noexcept;  // base or auto fill, no requests
    int variationFor(std::int64_t measure) const noexcept;
    double measureLengthQn(int pattern) const noexcept;

    PatternBank bank_{};
    double sampleRate_ = 48000.0;

    // --- UI-visible state ---
    bool playing_ = false;
    int currentPattern_ = 0;
    int selectedPattern_ = 0;
    int queuedPattern_ = -1;
    int currentVariation_ = 0;
    int currentStep_ = -1;
    std::int64_t measure_ = 0;
    int songPosition_ = -1;
    double tempo_ = 120.0;

    // --- settings latched from the latest process() call ---
    Settings settings_{};
    Mode activeMode_ = Mode::Off;     // the mode playback was started in

    // --- clock ---
    double prevPpq0_ = 0.0;           // range start of the previous process() call
    double prevSpp_ = 24000.0;        // samples per quarter note in that call
    bool prevFollowHost_ = false;     // that call mapped host PPQ (HostSync with a host transport)
    int prevN_ = 0;                   // its length in samples
    bool prevRecord_ = false;         // Settings::record in that call
    double segPpq_ = 0.0;             // internal clock: PPQ at the last tempo change
    std::int64_t segSamples_ = 0;     //   samples since then
    double segSpp_ = 24000.0;         //   samples per quarter note since then

    // --- measure cursor ---
    double measureStart_ = 0.0;       // PPQ of step 0 of the current measure
    double stepQn_ = 0.25;            // step length of the current measure (latched)
    int measureLen_ = 16;             // steps in the current measure (latched)
    int nextStep_ = 0;                // next step to fire; == measureLen_ at the measure end

    // --- performance requests ---
    bool fillRequested_ = false;
    bool noteHeld_ = false;
    bool noteRestart_ = false;
    int notePattern_ = 0;

    // Recorded hits that already sounded live: skip that step once, but only
    // when the measure it falls in plays the pattern and variation the hit
    // was written into (the next measure may be a chain entry, a fill, a
    // queued pattern or the other AB variation). Several per voice, because
    // one process() range can hold many upcoming steps of a voice: 8192
    // samples at 1000 BPM on the 32nd scale are 25 steps at 44.1 kHz and 50 at
    // 22.05 kHz, so 64 slots cover a live roll into any contract-sized block.
    struct Suppress {
        std::int64_t measure = 0;
        std::int16_t step = -1;       // -1: free
        std::uint8_t pattern = 0;
        std::uint8_t variation = 0;
    };
    static constexpr int kSuppressSlots = 64;
    std::array<std::array<Suppress, kSuppressSlots>, kNumVoices> suppress_{};
    std::array<int, kNumVoices> numSuppress_{};   // live slots per voice (fast path: 0)

    std::uint64_t order_ = 0;

    std::array<Pending, kMaxPending> pending_{};
    int numPending_ = 0;
    std::array<Scratch, kMaxScratch> scratch_{};
    int numScratch_ = 0;
};

}  // namespace mw909::seq
