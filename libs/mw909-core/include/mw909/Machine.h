// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The whole instrument behind one realtime entry point: the sound engine, the
// step sequencer, MIDI in/out handling and the lock-free links to a UI thread.
// The plugin shell and the CLI renderer both drive a Machine and nothing below it.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "mw909/Engine.h"
#include "mw909/MidiMap.h"
#include "mw909/Voices.h"
#include "mw909/seq/Pattern.h"
#include "mw909/seq/Sequencer.h"
#include "mw909/util/SpscQueue.h"

namespace mw909 {

// A channel-voice MIDI message with its sample offset in the block.
struct MidiMessage {
    std::int32_t offset = 0;
    std::uint8_t status = 0, data1 = 0, data2 = 0;
};

struct MidiOutBuffer {
    static constexpr int kCapacity = 4096;   // worst case: 22.05 kHz, 300 BPM, 32nd scale, 8192-sample blocks
    std::array<MidiMessage, kCapacity> messages{};
    int count = 0;

    void clear() noexcept { count = 0; }
    bool push(const MidiMessage& m) noexcept {
        if (count >= kCapacity) return false;
        messages[static_cast<std::size_t>(count++)] = m;
        return true;
    }
};

struct MidiSettings {
    bool velocitySensitive = true;  // continuous velocity vs three fixed classes
    std::uint8_t receiveChannel = 0;// 0 = omni; 1..16: voice notes only on this channel
    std::uint8_t bassChannel = 0;   // 0 = off; 1..16: notes on this channel play BD chromatically
    bool midiOut = false;           // send sequencer hits as notes
    std::uint8_t outChannel = 10;   // 1..16
};

// Everything the audio thread needs for one block, as plain values.
struct MachineParams {
    EngineParams engine{};
    seq::Settings seq{};
    MidiSettings midi{};
    std::array<bool, kNumVoices> mute{};
    std::array<bool, kNumVoices> solo{};
    Quality quality = Quality::Standard;
    std::uint32_t unitSeed = 0;
    float variance = 0.0f;
    Revision revision = Revision::Late;
};

// UI -> audio.
struct Command {
    enum class Type : std::uint8_t {
        SetPattern,     // index, pattern
        SetChain,       // chain
        SetSongTrack,   // index (0..kNumSongTracks-1), songTrack
        SelectPattern,  // index; flag = take over immediately
        Fill,           // play the fill pattern for the next measure
        Audition,       // index = voice, value = trigger volts
        Panic           // silence all voices, drop pending hits
    };
    Type type = Type::Panic;
    bool flag = false;
    std::int32_t index = 0;
    float value = 0.0f;
    std::uint32_t generation = 0;  // SetPattern / SetChain / SetSongTrack: BankHandoff generation the edit was made against
    seq::Chain chain{};
    seq::SongTrack songTrack{};
    seq::Pattern pattern{};
};

// A complete pattern memory handed to the audio thread in one piece (state
// restore, preset load). `generation` orders it against queued single edits:
// SetPattern / SetChain commands stamped with an older generation are stale
// and ignored once this bank is in place.
struct BankHandoff {
    seq::PatternBank bank{};
    std::uint32_t generation = 0;
};

// A user ROM image handed to the audio thread (same ownership protocol as
// BankHandoff: postUserRom / takeRetiredRom). image == nullptr clears the slot.
struct RomHandoff {
    RomSlot slot = RomSlot::Hat;
    bool clear = false;
    RomImage image{};
};

// Audio -> UI.
struct Notice {
    enum class Type : std::uint8_t {
        StepRecorded,    // pattern / variation / track / step now hold `cell`
        PatternSelected  // pattern = newly selected basic pattern (changed on the audio side)
    };
    Type type = Type::StepRecorded;
    std::uint8_t pattern = 0, variation = 0, track = 0, step = 0;
    seq::Step cell{};
};

// Play position and activity, published every block for the UI to poll.
struct PlayState {
    std::atomic<bool> playing{false};
    std::atomic<int> currentPattern{0};
    std::atomic<int> selectedPattern{0};
    std::atomic<int> queuedPattern{-1};
    std::atomic<int> variation{0};
    std::atomic<int> step{-1};
    std::atomic<std::int64_t> measure{0};                        // pass count of the sounding pattern
    // measure and step published together, so a per-track playhead
    // (measure * length + step) mod trackLength is never torn:
    // bits 63..8 = measure (wraps), bits 7..0 = step + 1 (0 = stopped).
    std::atomic<std::uint64_t> position{0};
    std::atomic<int> songPosition{-1};                           // chain/track entry index, -1 if none
    std::atomic<float> tempo{120.0f};
    std::atomic<float> peakLeft{0.0f}, peakRight{0.0f};          // main-bus block peak
    std::array<std::atomic<std::uint32_t>, kNumVoices> hits{};   // bumps on every trigger
    std::array<std::atomic<float>, kNumVoices> channelPeak{};    // post-fader block peak per voice
};

class Machine {
public:
    Machine() = default;
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    struct ProcessArgs {
        seq::Transport transport{};         // at the first sample of the block
        const MidiMessage* midiIn = nullptr;  // sorted by offset
        int numMidiIn = 0;
        MidiOutBuffer* midiOut = nullptr;   // optional
        BusBuffers out{};
        int numSamples = 0;
    };

    // Non-realtime: allocates the engine's scratch buffers.
    void prepare(double sampleRate, int maxBlockSize);
    // Realtime-safe: silence and rewind, keeps pattern memory.
    void reset() noexcept;

    // The realtime entry point.
    void process(const ProcessArgs& args, const MachineParams& params) noexcept;

    // --- UI thread (single producer / single consumer each way) -------------
    bool post(const Command& command) noexcept { return commands_.push(command); }
    bool poll(Notice& notice) noexcept { return notices_.pop(notice); }
    const PlayState& playState() const noexcept { return state_; }

    // Replace the whole pattern memory. Takes ownership of a heap-allocated
    // handoff; the very next process() call sees it, however many are posted.
    // Returns a handoff the caller must delete (a superseded one), or nullptr.
    [[nodiscard]] BankHandoff* postBank(BankHandoff* handoff) noexcept {
        return pendingBank_.exchange(handoff, std::memory_order_acq_rel);
    }
    // Handoffs the audio thread has finished with. Call until it returns
    // nullptr and delete each one (message thread).
    [[nodiscard]] BankHandoff* takeRetiredBank() noexcept {
        BankHandoff* h = nullptr;
        return retiredBanks_.pop(h) ? h : nullptr;
    }

    // Same protocol for the user ROM slots (one pending handoff per slot).
    [[nodiscard]] RomHandoff* postUserRom(RomHandoff* handoff) noexcept {
        return pendingRom_[static_cast<std::size_t>(handoff->slot)].exchange(handoff, std::memory_order_acq_rel);
    }
    [[nodiscard]] RomHandoff* takeRetiredRom() noexcept {
        RomHandoff* h = nullptr;
        return retiredRoms_.pop(h) ? h : nullptr;
    }

    // Constant per prepared sample rate (Engine::latencySamples).
    int latencySamples() const noexcept { return engine_.latencySamples(); }

    // --- direct access for offline use (renderer, tests): not thread-safe ----
    seq::Sequencer& sequencer() noexcept { return sequencer_; }
    Engine& engine() noexcept { return engine_; }

private:
    struct TimedHit {
        std::int32_t offset = 0;  // within the current chunk
        VoiceId voice = BD;
        seq::StepLevel level = seq::StepLevel::Normal;
        Hit hit{};
        bool fromSequencer = false;
        bool force = false;       // ignores mute/solo (auditions)
    };
    static constexpr int kMaxLiveHits = 512;
    static constexpr int kMaxAuditions = 32;

    void adoptPendingBank() noexcept;
    void drainCommands() noexcept;
    void processChunk(const ProcessArgs& args, const MachineParams& params, int chunkStart, int chunkLen,
                      int& midiIndex) noexcept;
    void fire(const TimedHit& h, const ProcessArgs& args, const MachineParams& params, int chunkStart,
              int& cursor) noexcept;
    void flushNoteOffs(MidiOutBuffer* out, int chunkStart, int chunkLen) noexcept;
    void publish(const ProcessArgs& args) noexcept;
    seq::Transport transportAt(const seq::Transport& t, int offset) const noexcept;

    Engine engine_;
    seq::Sequencer sequencer_;
    double sampleRate_ = 48000.0;
    int maxBlock_ = 0;

    seq::EventBuffer seqEvents_{};
    std::array<TimedHit, kMaxLiveHits> liveHits_{};  // MIDI-played hits of the current chunk, sorted
    int numLiveHits_ = 0;
    std::array<TimedHit, kMaxAuditions> auditions_{};
    int numAuditions_ = 0;

    // HostNote: the pattern notes held down, oldest first (playback stops
    // only when the last one comes up).
    std::array<std::uint8_t, seq::kNumPatterns> heldPatternNotes_{};
    int numHeldPatternNotes_ = 0;
    int lastSelectedPattern_ = 0;

    // MIDI out: samples from the current chunk start until each voice's
    // note-off is due (-1 = no note sounding).
    std::array<std::int32_t, kNumVoices> noteOffAt_{};
    std::int32_t noteLengthSamples_ = 960;

    std::atomic<BankHandoff*> pendingBank_{nullptr};
    std::array<std::atomic<RomHandoff*>, kNumRomSlots> pendingRom_{};
    std::array<RomHandoff*, kNumRomSlots> activeRom_{};          // owned by the audio thread until retired
    util::SpscQueue<RomHandoff*, 64> retiredRoms_;
    util::SpscQueue<BankHandoff*, 64> retiredBanks_;
    std::uint32_t bankGeneration_ = 0;

    util::SpscQueue<Command, 128> commands_;
    util::SpscQueue<Notice, 512> notices_;
    PlayState state_;

    // --- implementation details (Machine.cpp) ---------------------------------
    void adoptPendingRoms() noexcept;
    void retire(BankHandoff* h) noexcept;
    void retire(RomHandoff* h) noexcept;
    void retryRetire() noexcept;
    void renderSpan(const ProcessArgs& args, int start, int n) noexcept;
    void flushDueNoteOffs(MidiOutBuffer* out, int chunkStart, int upTo) noexcept;

    std::array<std::uint8_t, kNumVoices> noteChannel_{};   // MIDI channel (0-based) each sounding note went out on
    std::array<float, kNumVoices> blockPeak_{};             // channel peaks over the current block
    // Handoffs the retired queues had no room for; retried every block (never
    // freed here). A handoff is adopted only while there is room here, so none
    // is ever dropped.
    static constexpr int kMaxUnretired = 16;
    std::array<BankHandoff*, kMaxUnretired> unretiredBanks_{};
    int numUnretiredBanks_ = 0;
    std::array<RomHandoff*, kMaxUnretired> unretiredRoms_{};
    int numUnretiredRoms_ = 0;
    // An edit waiting for its bank, which could not be adopted yet because
    // the retired queues are full (the UI stopped draining). Applied first
    // next block, so the command order is kept and no handoff is ever leaked.
    Command deferred_{};
    bool hasDeferred_ = false;
    bool applyCommand(const Command& c) noexcept;
    bool adoptPendingBankChecked() noexcept;
    bool holdsPatternNote(int note) const noexcept;
    void releasePatternNote(int note) noexcept;
    void releaseAllPatternNotes() noexcept;
};

}  // namespace mw909
