// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Machine: Engine + Sequencer + MIDI + the lock-free links to the UI thread
// (architecture.md §4, §6). One process() call:
//   1. configure and parameterise the engine,
//   2. adopt a pending pattern bank and user ROMs, drain the command queue,
//   3. walk the block in chunks of at most 2048 samples (the sequencer's
//      exactness bound for its event buffer): MIDI in, then the sequencer for
//      the chunk, then render, split at every hit (sample-accurate),
//   4. publish the play state.

#include "mw909/Machine.h"

#include <algorithm>
#include <cmath>

#include "mw909/dsp/Math.h"

namespace mw909 {

namespace {

constexpr double kMidiNoteSeconds = 0.020;   // length of the notes sent for sequencer hits
constexpr int kMaxSequencerRange = 2048;     // see Sequencer.h: keeps every hit of any pattern

std::uint8_t velocityFor(seq::StepLevel level) noexcept {
    if (level == seq::StepLevel::Accent) return kVelocityAccent;
    if (level == seq::StepLevel::Weak) return kVelocityWeak;
    return kVelocityNormal;
}

// Mute and solo gate triggers: a muted voice never fires, and while any voice
// is soloed only the soloed (unmuted) voices fire.
bool audible(int voice, const MachineParams& params) noexcept {
    const auto v = static_cast<std::size_t>(voice);
    if (params.mute[v]) return false;
    bool anySolo = false;
    for (bool s : params.solo) anySolo = anySolo || s;
    return !anySolo || params.solo[v];
}

// Unsigned wrap-around order of bank generations.
bool older(std::uint32_t a, std::uint32_t b) noexcept { return static_cast<std::int32_t>(a - b) < 0; }

}  // namespace

Machine::~Machine() {
    delete pendingBank_.exchange(nullptr);
    BankHandoff* b = nullptr;
    while (retiredBanks_.pop(b)) delete b;
    for (int i = 0; i < numUnretiredBanks_; ++i) delete unretiredBanks_[static_cast<std::size_t>(i)];
    for (std::size_t s = 0; s < pendingRom_.size(); ++s) {
        delete pendingRom_[s].exchange(nullptr);
        delete activeRom_[s];
        activeRom_[s] = nullptr;
    }
    RomHandoff* r = nullptr;
    while (retiredRoms_.pop(r)) delete r;
    for (int i = 0; i < numUnretiredRoms_; ++i) delete unretiredRoms_[static_cast<std::size_t>(i)];
}

void Machine::prepare(double sampleRate, int maxBlockSize) {
    const bool wasPrepared = maxBlock_ > 0;
    sampleRate_ = std::isfinite(sampleRate) ? dsp::clamp(sampleRate, 8000.0, 768000.0) : 48000.0;
    maxBlock_ = std::max(1, maxBlockSize);
    engine_.prepare(sampleRate_, maxBlock_);
    sequencer_.prepare(sampleRate_);
    if (wasPrepared) {
        // A re-prepare (buffer size or rate change while playing): notes
        // already sent still get their note-offs, at the top of the next block.
        for (auto& t : noteOffAt_)
            if (t > 0) t = 0;
    } else {
        noteOffAt_.fill(-1);
    }
    noteLengthSamples_ = std::max(1, static_cast<std::int32_t>(kMidiNoteSeconds * sampleRate_));
    numLiveHits_ = 0;
    numAuditions_ = 0;
    numHeldPatternNotes_ = 0;
}

void Machine::reset() noexcept {
    engine_.reset();
    sequencer_.reset();
    numLiveHits_ = 0;
    numAuditions_ = 0;
    numHeldPatternNotes_ = 0;
    // noteOffAt_ is left alone so sounding MIDI-out notes still get their note-offs.
}

seq::Transport Machine::transportAt(const seq::Transport& t, int offset) const noexcept {
    seq::Transport out = t;
    if (t.hostValid && t.playing && offset > 0)
        out.ppq += static_cast<double>(offset) * t.bpm / (60.0 * sampleRate_);
    return out;
}

// --- handoffs ----------------------------------------------------------------

void Machine::retire(BankHandoff* h) noexcept {
    if (h == nullptr) return;
    if (retiredBanks_.push(h)) return;
    if (numUnretiredBanks_ < kMaxUnretired) unretiredBanks_[static_cast<std::size_t>(numUnretiredBanks_++)] = h;
    // The overflow array always has room here: adoption waits until it does.
}

void Machine::retire(RomHandoff* h) noexcept {
    if (h == nullptr) return;
    if (retiredRoms_.push(h)) return;
    if (numUnretiredRoms_ < kMaxUnretired) unretiredRoms_[static_cast<std::size_t>(numUnretiredRoms_++)] = h;
}

void Machine::retryRetire() noexcept {
    while (numUnretiredBanks_ > 0 && retiredBanks_.push(unretiredBanks_[static_cast<std::size_t>(numUnretiredBanks_ - 1)]))
        --numUnretiredBanks_;
    while (numUnretiredRoms_ > 0 && retiredRoms_.push(unretiredRoms_[static_cast<std::size_t>(numUnretiredRoms_ - 1)]))
        --numUnretiredRoms_;
}

void Machine::adoptPendingBank() noexcept { (void)adoptPendingBankChecked(); }

// Adopts the pending bank if there is one and its handoff can be retired.
// Returns false only when a bank is pending but must wait (retired queues full).
bool Machine::adoptPendingBankChecked() noexcept {
    if (numUnretiredBanks_ >= kMaxUnretired) return pendingBank_.load(std::memory_order_acquire) == nullptr;
    BankHandoff* handoff = pendingBank_.exchange(nullptr, std::memory_order_acq_rel);
    if (handoff == nullptr) return true;
    sequencer_.setBank(handoff->bank);
    bankGeneration_ = handoff->generation;
    retire(handoff);   // the sequencer holds its own copy
    return true;
}

void Machine::adoptPendingRoms() noexcept {
    for (int s = 0; s < kNumRomSlots; ++s) {
        // One adoption retires at most two handoffs; wait for room for both.
        if (numUnretiredRoms_ > kMaxUnretired - 2) return;
        const auto k = static_cast<std::size_t>(s);
        RomHandoff* h = pendingRom_[k].exchange(nullptr, std::memory_order_acq_rel);
        if (h == nullptr) continue;
        RomHandoff* previous = activeRom_[k];
        if (h->clear) {
            engine_.setUserRom(static_cast<RomSlot>(s), nullptr);
            activeRom_[k] = nullptr;
            retire(h);
        } else {
            engine_.setUserRom(static_cast<RomSlot>(s), &h->image);
            activeRom_[k] = h;
        }
        retire(previous);   // the engine no longer points into it
    }
}

void Machine::drainCommands() noexcept {
    if (hasDeferred_) {
        if (!applyCommand(deferred_)) return;
        hasDeferred_ = false;
    }
    Command c;
    while (commands_.pop(c)) {
        if (!applyCommand(c)) {
            deferred_ = c;   // rare: the UI has not drained the retired queue for ~80 handoffs
            hasDeferred_ = true;
            return;
        }
    }
}

// Returns false (nothing done) when the command must wait for its bank.
bool Machine::applyCommand(const Command& c) noexcept {
    const bool edit = c.type == Command::Type::SetPattern || c.type == Command::Type::SetChain ||
                      c.type == Command::Type::SetSongTrack;
    if (edit) {
        // An edit made against a newer bank than ours: that bank was posted
        // before the edit was queued, so take it first (else it would land
        // after the edit and overwrite it).
        if (older(bankGeneration_, c.generation) && !adoptPendingBankChecked()) return false;
        if (older(c.generation, bankGeneration_)) return true;   // predates the current bank
    }
    switch (c.type) {
        case Command::Type::SetPattern:
            if (c.index >= 0 && c.index < seq::kNumPatterns) sequencer_.setPattern(c.index, c.pattern);
            break;
        case Command::Type::SetChain:
            sequencer_.setChain(c.chain);
            break;
        case Command::Type::SetSongTrack:
            if (c.index >= 0 && c.index < seq::kNumSongTracks) sequencer_.setSongTrack(c.index, c.songTrack);
            break;
        case Command::Type::SelectPattern:
            if (c.index >= 0 && c.index < seq::kNumPatterns) {
                sequencer_.selectPattern(c.index, c.flag);
                // The UI asked for this one; do not echo it back as a change.
                if (sequencer_.selectedPattern() == c.index) lastSelectedPattern_ = c.index;
            }
            break;
        case Command::Type::Fill:
            sequencer_.requestFill();
            break;
        case Command::Type::Audition:
            if (c.index >= 0 && c.index < kNumVoices && numAuditions_ < kMaxAuditions) {
                TimedHit h;
                h.voice = static_cast<VoiceId>(c.index);
                h.hit.acc = std::isfinite(c.value) ? dsp::clamp(c.value, 0.0f, AccentDac::kVolts)
                                                   : AccentDac::volts(AccentDac::kNormalCode);
                h.force = true;
                auditions_[static_cast<std::size_t>(numAuditions_++)] = h;
            }
            break;
        case Command::Type::Panic:
            engine_.reset();
            sequencer_.reset();
            numHeldPatternNotes_ = 0;
            numAuditions_ = 0;
            // Close every sounding MIDI-out note at the top of the block.
            for (auto& t : noteOffAt_)
                if (t > 0) t = 0;
            break;
    }
    return true;
}

// --- HostNote held pattern notes ----------------------------------------------------

bool Machine::holdsPatternNote(int note) const noexcept {
    for (int i = 0; i < numHeldPatternNotes_; ++i)
        if (heldPatternNotes_[static_cast<std::size_t>(i)] == note) return true;
    return false;
}

void Machine::releasePatternNote(int note) noexcept {
    int j = 0;
    for (int i = 0; i < numHeldPatternNotes_; ++i)
        if (heldPatternNotes_[static_cast<std::size_t>(i)] != note)
            heldPatternNotes_[static_cast<std::size_t>(j++)] = heldPatternNotes_[static_cast<std::size_t>(i)];
    numHeldPatternNotes_ = j;
}

void Machine::releaseAllPatternNotes() noexcept {
    if (numHeldPatternNotes_ == 0) return;
    numHeldPatternNotes_ = 0;
    sequencer_.notePatternOff();
}

// --- the block ------------------------------------------------------------------

void Machine::process(const ProcessArgs& args, const MachineParams& params) noexcept {
    if (args.midiOut != nullptr) args.midiOut->clear();

    engine_.configure(params.quality, params.unitSeed, params.variance, params.revision);
    engine_.setParams(params.engine);
    retryRetire();
    adoptPendingBank();
    adoptPendingRoms();
    drainCommands();

    blockPeak_.fill(0.0f);
    if (maxBlock_ > 0 && args.numSamples > 0) {
        const int chunk = std::min(maxBlock_, kMaxSequencerRange);
        int midiIndex = 0;
        for (int start = 0; start < args.numSamples;) {
            const int len = std::min(chunk, args.numSamples - start);
            processChunk(args, params, start, len, midiIndex);
            start += len;
        }
    }
    publish(args);
}

void Machine::renderSpan(const ProcessArgs& args, int start, int n) noexcept {
    if (n <= 0) return;
    engine_.render(args.out, start, n);
    for (int v = 0; v < kNumVoices; ++v) {
        const auto k = static_cast<std::size_t>(v);
        blockPeak_[k] = std::max(blockPeak_[k], engine_.channelPeak(v));
    }
}

void Machine::processChunk(const ProcessArgs& args, const MachineParams& params, int chunkStart, int chunkLen,
                           int& midiIndex) noexcept {
    seq::Settings settings = params.seq;
    if (settings.jitterSeed == 0U) settings.jitterSeed = params.unitSeed;
    const MidiSettings& midi = params.midi;
    const bool hostNote = settings.mode == seq::Mode::HostNote;
    const bool lastChunk = chunkStart + chunkLen >= args.numSamples;

    seqEvents_.clear();
    numLiveHits_ = 0;
    int seqCursor = 0;  // samples of this chunk the sequencer has already processed
    // Leaving HostNote with keys down: the sequencer must not keep believing a
    // note is held (it would play on with no key down once HostNote is back).
    if (!hostNote) releaseAllPatternNotes();

    // Auditions from the UI sound at the top of the block.
    for (int i = 0; i < numAuditions_ && numLiveHits_ < kMaxLiveHits; ++i)
        liveHits_[static_cast<std::size_t>(numLiveHits_++)] = auditions_[static_cast<std::size_t>(i)];
    numAuditions_ = 0;

    // --- MIDI in ------------------------------------------------------------------
    while (midiIndex < args.numMidiIn && (lastChunk || args.midiIn[midiIndex].offset < chunkStart + chunkLen)) {
        const MidiMessage& m = args.midiIn[midiIndex++];
        const int type = m.status & 0xF0;
        if (type != 0x90 && type != 0x80) continue;
        const int local = dsp::clamp(m.offset - chunkStart, 0, chunkLen - 1);
        const int channel = (m.status & 0x0F) + 1;
        const int note = m.data1 & 0x7F;
        const int velocity = m.data2 & 0x7F;
        const bool noteOn = type == 0x90 && velocity > 0;
        const bool onBass = midi.bassChannel != 0 && channel == midi.bassChannel;
        const bool onReceive = midi.receiveChannel == 0 || channel == midi.receiveChannel;

        if (hostNote && !onBass) {
            if (!onReceive) continue;
            // Notes select and hold patterns; bring the sequencer up to the
            // note's position first so the change lands sample-accurately.
            // A newer note takes over; playback stops only when the last held
            // pattern note comes up (releasing the newest while an older one
            // is still down keeps the current pattern playing).
            const int index = note - kPatternNoteBase;
            const bool starts = noteOn && index >= 0 && index < seq::kNumPatterns;
            bool stops = false;
            if (!noteOn && holdsPatternNote(note)) {
                releasePatternNote(note);
                stops = numHeldPatternNotes_ == 0;
            }
            if (!starts && !stops) continue;
            if (local > seqCursor) {
                sequencer_.process(transportAt(args.transport, chunkStart + seqCursor), settings, local - seqCursor,
                                   seqEvents_, seqCursor);
                seqCursor = local;
            }
            if (starts) {
                sequencer_.notePatternOn(index);
                releasePatternNote(note);
                heldPatternNotes_[static_cast<std::size_t>(numHeldPatternNotes_++)] = static_cast<std::uint8_t>(note);
            } else {
                sequencer_.notePatternOff();
            }
            continue;
        }

        if (!noteOn || numLiveHits_ >= kMaxLiveHits) continue;

        TimedHit h;
        h.offset = local;
        h.hit = velocityToHit(velocity, settings.totalAccent, midi.velocitySensitive);
        h.level = velocity >= kAccentVelocityThreshold ? seq::StepLevel::Accent : seq::StepLevel::Normal;
        if (onBass) {
            // Chromatic bass drum: kBassRootNote plays the stock pitch.
            h.voice = BD;
            h.hit.pitchSemis = static_cast<float>(note - kBassRootNote);
        } else {
            if (!onReceive) continue;
            const std::int8_t voice = kNoteToVoice[static_cast<std::size_t>(note)];
            if (voice == kNoVoice) continue;
            h.voice = static_cast<VoiceId>(voice);
        }
        liveHits_[static_cast<std::size_t>(numLiveHits_++)] = h;

        // TAP WRITE: the played hit goes into the playing pattern. The
        // sequencer is first brought up to the hit, so a step that sounds
        // before the tap in this chunk has already fired and only steps still
        // ahead can be taken over by the tap, whatever the block partition.
        if (settings.record) {
            if (local > seqCursor) {
                sequencer_.process(transportAt(args.transport, chunkStart + seqCursor), settings, local - seqCursor,
                                   seqEvents_, seqCursor);
                seqCursor = local;
            }
            const seq::RecordResult r = sequencer_.recordHit(h.voice, velocity >= kAccentVelocityThreshold, 0);
            if (r.changed) {
                Notice n;
                n.type = Notice::Type::StepRecorded;
                n.pattern = r.pattern;
                n.variation = r.variation;
                n.track = r.track;
                n.step = r.step;
                n.cell = r.cell;
                notices_.push(n);
            }
        }
    }

    // --- sequencer ------------------------------------------------------------------
    if (chunkLen > seqCursor)
        sequencer_.process(transportAt(args.transport, chunkStart + seqCursor), settings, chunkLen - seqCursor,
                           seqEvents_, seqCursor);

    // --- render, splitting at every hit (both lists are sorted by offset) ----------
    int cursor = 0;
    int li = 0, si = 0;
    while (li < numLiveHits_ || si < seqEvents_.count) {
        const bool takeLive = si >= seqEvents_.count ||
                              (li < numLiveHits_ && liveHits_[static_cast<std::size_t>(li)].offset <=
                                                        seqEvents_.events[static_cast<std::size_t>(si)].offset);
        TimedHit h;
        if (takeLive) {
            h = liveHits_[static_cast<std::size_t>(li++)];
        } else {
            const seq::Event& e = seqEvents_.events[static_cast<std::size_t>(si++)];
            h.offset = e.offset;
            h.voice = e.voice;
            h.level = e.totalAccent ? seq::StepLevel::Accent : e.level;   // the MIDI-out velocity class
            h.hit = e.hit;
            h.fromSequencer = true;
        }
        h.offset = dsp::clamp(h.offset, cursor, chunkLen - 1);
        fire(h, args, params, chunkStart, cursor);
    }
    if (cursor < chunkLen) renderSpan(args, chunkStart + cursor, chunkLen - cursor);

    flushNoteOffs(args.midiOut, chunkStart, chunkLen);
}

void Machine::fire(const TimedHit& h, const ProcessArgs& args, const MachineParams& params, int chunkStart,
                   int& cursor) noexcept {
    if (static_cast<int>(h.voice) >= kNumVoices) return;
    if (!h.force && !audible(h.voice, params)) return;

    if (h.offset > cursor) {
        renderSpan(args, chunkStart + cursor, h.offset - cursor);
        cursor = h.offset;
    }
    engine_.trigger(h.voice, h.hit);
    state_.hits[h.voice].fetch_add(1U, std::memory_order_relaxed);

    if (h.fromSequencer && params.midi.midiOut && args.midiOut != nullptr) {
        const auto v = static_cast<std::size_t>(h.voice);
        const std::uint8_t note = kVoiceNote[v];
        const auto channel = static_cast<std::uint8_t>(dsp::clamp<int>(params.midi.outChannel, 1, 16) - 1);
        // Every note-off due up to here first, so the output stays in time order.
        flushDueNoteOffs(args.midiOut, chunkStart, h.offset);
        // The buffer always keeps room for the note-off of every sounding note:
        // at extreme density a full buffer drops note-ons (the voice still
        // sounds), never note-offs, so no note is left hanging.
        const bool sounding = noteOffAt_[v] >= 0;
        int pendingOffs = 1;   // this note's own, later
        for (int u = 0; u < kNumVoices; ++u)
            if (u != h.voice && noteOffAt_[static_cast<std::size_t>(u)] >= 0) ++pendingOffs;
        const int needed = (sounding ? 2 : 1) + pendingOffs;
        if (args.midiOut->count + needed <= MidiOutBuffer::kCapacity) {
            if (sounding) {
                // Still sounding (a retrigger inside the note length): close it here.
                args.midiOut->push(
                    {chunkStart + h.offset, static_cast<std::uint8_t>(0x80 | noteChannel_[v]), note, 0});
            }
            args.midiOut->push({chunkStart + h.offset, static_cast<std::uint8_t>(0x90 | channel), note,
                                velocityFor(h.level)});
            noteOffAt_[v] = h.offset + noteLengthSamples_;
            noteChannel_[v] = channel;
        }
    }
}

// Note-offs due at or before `upTo` (chunk-relative), in time order.
void Machine::flushDueNoteOffs(MidiOutBuffer* out, int chunkStart, int upTo) noexcept {
    for (;;) {
        int best = -1;
        for (int v = 0; v < kNumVoices; ++v) {
            const std::int32_t t = noteOffAt_[static_cast<std::size_t>(v)];
            if (t >= 0 && t <= upTo && (best < 0 || t < noteOffAt_[static_cast<std::size_t>(best)])) best = v;
        }
        if (best < 0) return;
        const auto b = static_cast<std::size_t>(best);
        if (out != nullptr &&
            !out->push({chunkStart + noteOffAt_[b], static_cast<std::uint8_t>(0x80 | noteChannel_[b]), kVoiceNote[b], 0}))
            return;   // full (only if the caller pre-filled it): kept, sent at the top of the next block
        noteOffAt_[b] = -1;
    }
}

void Machine::flushNoteOffs(MidiOutBuffer* out, int chunkStart, int chunkLen) noexcept {
    flushDueNoteOffs(out, chunkStart, chunkLen - 1);
    for (auto& t : noteOffAt_)
        if (t >= 0) t = std::max<std::int32_t>(t - chunkLen, 0);
}

void Machine::publish(const ProcessArgs& args) noexcept {
    const bool playing = sequencer_.isPlaying();
    const int step = sequencer_.currentStep();
    const std::int64_t measure = sequencer_.measureCount();
    state_.playing.store(playing, std::memory_order_relaxed);
    state_.currentPattern.store(sequencer_.currentPattern(), std::memory_order_relaxed);
    state_.selectedPattern.store(sequencer_.selectedPattern(), std::memory_order_relaxed);
    state_.queuedPattern.store(sequencer_.queuedPattern(), std::memory_order_relaxed);
    state_.variation.store(sequencer_.currentVariation(), std::memory_order_relaxed);
    state_.step.store(step, std::memory_order_relaxed);
    state_.measure.store(measure, std::memory_order_relaxed);
    // Measure and step in one store: a playhead never sees one without the other.
    const std::uint64_t position =
        playing && step >= 0
            ? (static_cast<std::uint64_t>(measure) << 8) | static_cast<std::uint64_t>((step + 1) & 0xFF)
            : 0U;
    state_.position.store(position, std::memory_order_release);
    state_.songPosition.store(sequencer_.songPosition(), std::memory_order_relaxed);
    state_.tempo.store(static_cast<float>(sequencer_.tempo()), std::memory_order_relaxed);

    if (sequencer_.selectedPattern() != lastSelectedPattern_) {
        lastSelectedPattern_ = sequencer_.selectedPattern();
        Notice n;
        n.type = Notice::Type::PatternSelected;
        n.pattern = static_cast<std::uint8_t>(lastSelectedPattern_);
        notices_.push(n);
    }

    float peakL = 0.0f, peakR = 0.0f;
    const auto& mainOut = args.out.ch[0];
    if (mainOut[0] != nullptr)
        for (int i = 0; i < args.numSamples; ++i) peakL = std::max(peakL, std::abs(mainOut[0][i]));
    if (mainOut[1] != nullptr)
        for (int i = 0; i < args.numSamples; ++i) peakR = std::max(peakR, std::abs(mainOut[1][i]));
    state_.peakLeft.store(peakL, std::memory_order_relaxed);
    state_.peakRight.store(peakR, std::memory_order_relaxed);
    for (int v = 0; v < kNumVoices; ++v)
        state_.channelPeak[static_cast<std::size_t>(v)].store(blockPeak_[static_cast<std::size_t>(v)],
                                                              std::memory_order_relaxed);
}

}  // namespace mw909
