// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The message-thread owner of everything that is state but not a parameter
// (docs/design/plugin.md §3): the authoritative pattern memory (96 patterns,
// the chain, the 8 song tracks), the selected pattern, the unit seed, the user
// ROM images and the editor's UI state. (The MIDI-learn map lives in
// MidiLearn, which has its own lock.) It is also the ONLY user of the UI side
// of mw909::Machine's cross-thread links, so their single-producer /
// single-consumer contracts hold however many threads call in.
//
// Threads
//   message thread  everything, unless noted. Listener callbacks happen here.
//   any non-audio   capture() / restore() / postCommand() / collectRetired():
//                   hosts call get/setStateInformation from worker threads.
//   audio thread    unitSeedForAudio() only (one relaxed atomic load).
//
// How a state restore from a foreign thread stays race-free: the model's own
// data is written by the message thread only. A foreign restore() hands the
// new bank and ROMs straight to the audio thread (so an offline bounce right
// after the load plays them) and parks a copy in `staged_`; the message
// thread adopts it on its next service() or mutator call, inside the same
// critical section as that mutator's own change, and then tells the
// listeners. Until then capture() serves the staged copy, so a save right
// after a load is exact.
//
// Delivery guarantees
//   * A whole bank goes through Machine::postBank with a fresh generation, and
//     each user ROM through Machine::postUserRom: the very next audio block
//     uses them, with no timer or message loop involved.
//   * Single edits (pattern, chain, song track) go through the command queue
//     stamped with the current generation. When the queue is full the item is
//     marked dirty and re-posted from service(); nothing is lost.
//   * Notices from the audio thread (recorded steps, pattern changes) are
//     merged into the model in service(). The cases where a notice and a
//     message-thread change cross are resolved in favour of the model, which
//     is then re-sent, so both sides converge on the same data.

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>

#include <juce_core/juce_core.h>

#include "mw909/Machine.h"
#include "mw909/dsp/Rom.h"
#include "mw909/seq/Pattern.h"

namespace mw909::plugin {

// Editor state kept in the session. Plain values; the model never interprets
// them beyond clamping to the documented ranges.
struct UiState {
    float scale = 1.0f;      // editor scale factor, 0.6 .. 2.0
    int view = 0;            // 0 PANEL, 1 GRID, 2 MIXER, 3 FX
    int selectedTrack = 0;   // 0..10 voices, 11 total accent
    int stepEditMode = 0;    // 0 LEVEL, 1 FLAM, 2 SUB, 3 PROB
    int page = 0;            // 0..3
    bool authentic = false;  // edits follow the original machine's rules (seq::applyHardwareRules)
    std::array<juce::String, 3> presetNames{};  // last loaded/saved preset per kind (kit, patterns, full)

    bool operator==(const UiState&) const = default;
};

inline constexpr int kNumViews = 4;
inline constexpr float kMinUiScale = 0.6f;
inline constexpr float kMaxUiScale = 2.0f;
inline constexpr std::uint32_t kDefaultUnitSeed = 909;
inline constexpr int kMaxRomNameLength = 128;

// One user ROM slot. `image` is immutable once made (shared between the
// model, staged restores and saved snapshots); null = the slot is empty.
struct UserRom {
    std::shared_ptr<const RomImage> image;
    juce::String name;  // where it came from (the imported file's name)

    bool empty() const noexcept { return image == nullptr; }
    bool operator==(const UserRom& other) const noexcept;
};
using UserRoms = std::array<UserRom, kNumRomSlots>;

// Force every field into its legal range (patterns reach the audio thread
// unchecked by the core). A valid value is returned unchanged, bit for bit.
void sanitise(seq::Pattern& p) noexcept;
void sanitise(seq::Chain& c) noexcept;
void sanitise(seq::SongTrack& t) noexcept;
void sanitise(UiState& ui) noexcept;
// True when every word of the image is a 6-bit value.
bool validRom(const RomImage& image) noexcept;

class PatternModel {
public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void patternChanged(int /*index*/) {}
        virtual void bankChanged() {}        // every pattern, the chain and the tracks may have changed
        virtual void selectionChanged() {}
        virtual void chainChanged() {}
        virtual void songTrackChanged(int /*index*/) {}
        virtual void userRomChanged(int /*slot*/) {}
    };

    // Everything the model holds, for state save / restore.
    struct Snapshot {
        seq::PatternBank bank{};
        int selectedPattern = 0;
        std::uint32_t unitSeed = kDefaultUnitSeed;
        UserRoms roms{};
        UiState ui{};
    };

    explicit PatternModel(Machine& machine);
    ~PatternModel();
    PatternModel(const PatternModel&) = delete;
    PatternModel& operator=(const PatternModel&) = delete;

    // Set the starting content. Only for the owner's constructor, before any
    // other thread can reach the model: applies directly on whatever thread
    // this is and calls no listeners.
    void initialise(std::unique_ptr<Snapshot> snapshot);

    // --- message thread ----------------------------------------------------
    const seq::Pattern& pattern(int index) const noexcept;  // index clamped
    const seq::PatternBank& bank() const noexcept { return bank_; }
    const seq::Chain& chain() const noexcept { return bank_.chain; }
    const seq::SongTrack& songTrack(int index) const noexcept;  // index clamped

    // A copy of a pattern as the session holds it now: a restore another
    // thread staged and the message thread has not adopted yet included. Any
    // non-audio thread.
    seq::Pattern patternCopy(int index) const;  // index clamped

    void setPattern(int index, const seq::Pattern& p);  // model + Command::SetPattern to audio
    void setChain(const seq::Chain& c);
    void setSongTrack(int index, const seq::SongTrack& t);  // model + Command::SetSongTrack

    // Read-modify-write edits: fn(Pattern&) / fn(Chain&) / fn(SongTrack&) on a
    // copy, then the matching set*. Use these rather than copying pattern()
    // and calling set* yourself. A restore staged by another thread is
    // adopted BEFORE the copy is taken, so the edit applies to the restored
    // data; if another one is staged while fn runs, fn runs again on the new
    // data. So fn must only transform its argument (it may run more than
    // once). fn may return bool: false abandons the edit. Returns true when
    // the model changed.
    template <class Fn>
    bool edit(int index, Fn&& fn) {
        if (index < 0 || index >= seq::kNumPatterns) return false;
        return editLoop(fn, [this, index] { return bank_.patterns[static_cast<std::size_t>(index)]; },
                        [this, index](const seq::Pattern& p, std::uint64_t epoch) { return commitPattern(index, p, epoch); });
    }
    template <class Fn>
    bool editChain(Fn&& fn) {
        return editLoop(fn, [this] { return bank_.chain; },
                        [this](const seq::Chain& c, std::uint64_t epoch) { return commitChain(c, epoch); });
    }
    template <class Fn>
    bool editSongTrack(int index, Fn&& fn) {
        if (index < 0 || index >= seq::kNumSongTracks) return false;
        return editLoop(fn, [this, index] { return bank_.tracks[static_cast<std::size_t>(index)]; },
                        [this, index](const seq::SongTrack& t, std::uint64_t epoch) { return commitSongTrack(index, t, epoch); });
    }
    void replaceBank(const seq::PatternBank& bank);     // model + Machine::postBank (generation++)

    int selectedPattern() const noexcept { return selected_; }
    void selectPattern(int index, bool immediate = false);

    std::uint32_t unitSeed() const noexcept { return seed_.load(std::memory_order_relaxed); }
    void setUnitSeed(std::uint32_t seed);

    // User ROM slots (Machine::postUserRom). setUserRom refuses an image with
    // values above 63.
    UserRom userRom(RomSlot slot) const;
    bool setUserRom(RomSlot slot, const RomImage& image, const juce::String& name);
    void clearUserRom(RomSlot slot);

    UiState uiState() const;
    void setUiState(const UiState& ui);

    void addListener(Listener* l) { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

    // Runs what the processor's 30 Hz timer runs: adopt a staged restore, merge
    // notices, re-post dirty items, free retired bank and ROM handoffs.
    void service();

    // True once after pattern data, the chain, a track, a ROM or the unit seed
    // changed through an edit, a preset load or live recording (not through
    // restore()): the host should be told the session is dirty.
    bool takeSessionDirty() noexcept { return sessionDirty_.exchange(false, std::memory_order_acq_rel); }

    // --- any non-audio thread ------------------------------------------------
    void capture(Snapshot& out) const;
    // Replace everything. On the message thread the model and the listeners
    // are updated before this returns; elsewhere see the header comment.
    // `keepUi` leaves the current UI state in place (preset loads).
    void restore(std::unique_ptr<Snapshot> snapshot, bool keepUi = false);
    // Audition / Fill / Panic / anything that is not model data. False when the
    // command queue is full (the command is dropped).
    bool postCommand(const Command& command);
    // Frees bank and ROM handoffs the audio thread has finished with.
    void collectRetired();

    // --- audio thread ----------------------------------------------------------
    std::uint32_t unitSeedForAudio() const noexcept { return seed_.load(std::memory_order_relaxed); }

    // --- diagnostics (tests) -------------------------------------------------
    int numDirty() const;                    // edits (+chain, +tracks, +selection) waiting for a re-post
    std::uint32_t generation() const;        // generation of the bank last handed to the audio thread
    bool hasStagedRestore() const noexcept { return hasStaged_.load(std::memory_order_acquire); }

private:
    enum class Commit { Unchanged, Changed, Stale };
    static constexpr std::uint64_t kAnyEpoch = ~std::uint64_t{0};
    static constexpr int kMaxEditAttempts = 4;  // then the edit is applied to whatever is current

    // Adopts a staged restore (and tells the listeners), then returns the
    // restore epoch the model's data now belongs to. Message thread.
    std::uint64_t beginEdit();
    // set*, unless a restore newer than `epoch` arrived in between (Stale:
    // nothing changed but that restore's adoption). kAnyEpoch never refuses.
    Commit commitPattern(int index, const seq::Pattern& p, std::uint64_t epoch);
    Commit commitChain(const seq::Chain& c, std::uint64_t epoch);
    Commit commitSongTrack(int index, const seq::SongTrack& t, std::uint64_t epoch);

    template <class Fn, class Read, class Commit_>
    bool editLoop(Fn& fn, Read&& read, Commit_&& commit) {
        for (int attempt = 0;; ++attempt) {
            const std::uint64_t epoch = beginEdit();
            auto value = read();  // the model data, read on the message thread (its only writer)
            if constexpr (std::is_same_v<std::invoke_result_t<Fn&, decltype(value)&>, bool>) {
                if (!fn(value)) return false;
            } else {
                fn(value);
            }
            const Commit result = commit(value, attempt + 1 < kMaxEditAttempts ? epoch : kAnyEpoch);
            if (result != Commit::Stale) return result == Commit::Changed;
        }
    }

    // kRomSlot << slot marks one user ROM slot; kRoms all of them.
    enum Change : unsigned { kBank = 1U, kSelection = 2U, kChain = 4U, kRomSlot = 8U, kRoms = 8U | 16U | 32U };
    static unsigned romChanges(const UserRoms& a, const UserRoms& b) noexcept;

    // All *Locked functions expect mutex_ to be held.
    void postBankLocked(const seq::PatternBank& bank);
    void postPatternLocked(int index);
    void postChainLocked();
    void postSongTrackLocked(int index);
    void postSelectLocked(int index, bool immediate);
    void postRomLocked(int slot, const UserRom& rom);
    void collectRetiredLocked();
    unsigned adoptStagedLocked();
    void applyLocked(const Snapshot& snapshot);
    void notify(unsigned changes, const std::bitset<seq::kNumPatterns>& patterns);

    Machine& machine_;
    mutable std::mutex mutex_;

    // Model data: written on the message thread only (under mutex_), read on
    // the message thread without it and elsewhere with it.
    seq::PatternBank bank_{};
    int selected_ = 0;
    UserRoms roms_{};
    UiState ui_{};
    std::atomic<std::uint32_t> seed_{kDefaultUnitSeed};

    // A restore that arrived on another thread, waiting for the message thread.
    std::unique_ptr<Snapshot> staged_;
    std::atomic<bool> hasStaged_{false};
    // Counts restores (staged or applied), under mutex_: an edit whose copy was
    // taken before the latest one is redone on the restored data.
    std::uint64_t restoreEpoch_ = 0;

    // Link bookkeeping (under mutex_).
    std::uint32_t generation_ = 0;
    std::bitset<seq::kNumPatterns> dirty_{};
    std::bitset<seq::kNumSongTracks> tracksDirty_{};
    bool chainDirty_ = false;
    bool selectDirty_ = false;
    bool selectDirtyImmediate_ = false;
    bool awaitingBank_ = false;        // a posted bank has not been seen adopted yet
    bool adoptionObserved_ = false;    // ...its handoff came back retired
    int awaitingSelect_ = -1;          // target of a select command the audio side has not shown yet
    std::uint32_t tick_ = 0;           // service() counter
    std::array<std::uint32_t, seq::kNumPatterns> postedAtTick_{};
    std::bitset<seq::kNumPatterns> postedRecently_{};
    std::atomic<bool> sessionDirty_{false};

    juce::ListenerList<Listener> listeners_;
};

}  // namespace mw909::plugin
