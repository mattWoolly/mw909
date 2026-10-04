// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// MIDI learn (docs/design/plugin.md §5): a map from controller number to one
// parameter. The audio thread never touches a parameter: it forwards the
// controller messages it sees through a lock-free FIFO, and service() (the
// processor's 30 Hz message-thread timer) applies them with
// setValueNotifyingHost inside a gesture.
//
// Rules: one parameter per controller and one controller per parameter (a new
// binding replaces both older ones). Controllers 0..119 on any channel; the
// channel-mode messages 120..127 cannot be learned.
//
// A new instance starts with the factory CC map (plugin.md §5, research/04
// §5.3), the receive map of the original machine's family, so a controller
// templated for it works with no learning. Learn / forget edit that map per
// controller; resetToFactory() brings it back.
//
// Threads: the map is guarded by a mutex because hosts read and write state
// from worker threads. Listener callbacks always happen on the message thread.

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "mw909/util/SpscQueue.h"

namespace mw909::plugin {

class MidiLearn {
public:
    static constexpr int kNumControllers = 120;

    struct Listener {
        virtual ~Listener() = default;
        virtual void midiLearnChanged() {}  // the map or the learn target changed
    };

    // (controller, parameter id), sorted by controller.
    using Map = std::vector<std::pair<int, juce::String>>;

    explicit MidiLearn(juce::AudioProcessorValueTreeState& apvts);

    // --- message thread ----------------------------------------------------
    // Arm: the next controller that arrives is bound to this parameter.
    void beginLearn(const juce::String& parameterId);
    void cancelLearn();
    bool isLearning() const noexcept { return learning_.load(std::memory_order_relaxed); }
    juce::String learningParameter() const;

    bool assign(int controller, const juce::String& parameterId);  // false: unknown id or controller
    void forget(const juce::String& parameterId);
    void forgetController(int controller);
    void clear();
    void resetToFactory();

    // The factory CC map, sorted by controller.
    static const Map& factoryMap();

    int controllerFor(const juce::String& parameterId) const;      // -1 when unbound
    juce::String parameterFor(int controller) const;               // empty when unbound

    void addListener(Listener* l) { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

    // Apply queued controller values and finish a pending learn. Called by the
    // processor's timer.
    void service();

    // True once after the map changed through learn / assign / forget (not setMap).
    bool takeSessionDirty() noexcept { return sessionDirty_.exchange(false, std::memory_order_acq_rel); }

    // --- any non-audio thread ------------------------------------------------
    Map getMap() const;
    void setMap(const Map& map);  // unknown ids and controllers are skipped

    // --- audio thread ----------------------------------------------------------
    void handleController(int controller, int value) noexcept {
        if (controller < 0 || controller >= kNumControllers) return;
        if (!learning_.load(std::memory_order_relaxed) &&
            !bound_[static_cast<std::size_t>(controller)].load(std::memory_order_relaxed))
            return;
        fifo_.push({static_cast<std::uint8_t>(controller), static_cast<std::uint8_t>(value & 0x7F)});
    }

private:
    struct Event {
        std::uint8_t controller = 0;
        std::uint8_t value = 0;
    };

    bool assignLocked(int controller, juce::RangedAudioParameter* parameter);
    void setMapLocked(const Map& map);
    void changed();

    juce::AudioProcessorValueTreeState& apvts_;

    mutable std::mutex mutex_;
    std::array<juce::RangedAudioParameter*, kNumControllers> target_{};  // under mutex_
    juce::RangedAudioParameter* learnTarget_ = nullptr;                  // under mutex_

    std::array<std::atomic<bool>, kNumControllers> bound_{};
    std::atomic<bool> learning_{false};
    std::atomic<bool> notifyPending_{false};
    std::atomic<bool> sessionDirty_{false};

    util::SpscQueue<Event, 1024> fifo_;
    juce::ListenerList<Listener> listeners_;
};

}  // namespace mw909::plugin
