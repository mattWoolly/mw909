// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "MidiLearn.h"

#include <algorithm>

#include <juce_events/juce_events.h>

#include "mw909/Voices.h"

namespace mw909::plugin {

namespace {

bool validController(int controller) noexcept { return controller >= 0 && controller < MidiLearn::kNumControllers; }

}  // namespace

MidiLearn::MidiLearn(juce::AudioProcessorValueTreeState& apvts) : apvts_(apvts) {
    const std::lock_guard<std::mutex> lock(mutex_);
    setMapLocked(factoryMap());
}

const MidiLearn::Map& MidiLearn::factoryMap() {
    // plugin.md §5 / research/04 §5.3. "(drive)" on CC 22 is the bass drum's
    // channel drive, "(comp)" on CC 27 the snare's channel compressor switch.
    static const Map map = [] {
        Map m;
        const auto voice = [](int v, const char* suffix) {
            return juce::String(std::string(kVoiceInfo[static_cast<std::size_t>(v)].id)) + "_" + suffix;
        };
        m.emplace_back(9, "swing_scale");
        m.emplace_back(20, voice(BD, "tune"));
        m.emplace_back(21, voice(BD, "attack"));
        m.emplace_back(22, voice(BD, "ch_drive"));
        m.emplace_back(23, voice(BD, "decay"));
        m.emplace_back(24, voice(BD, "level"));
        m.emplace_back(25, voice(SD, "tune"));
        m.emplace_back(26, voice(SD, "snappy"));
        m.emplace_back(27, voice(SD, "comp_on"));
        m.emplace_back(28, voice(SD, "tone"));
        m.emplace_back(29, voice(SD, "level"));
        // Tune / decay / level triples.
        const std::array<std::pair<int, int>, 9> triples{{
            {46, LT}, {49, MT}, {52, HT}, {55, RS}, {58, CP}, {61, CH}, {80, OH}, {83, CR}, {86, RD}}};
        for (const auto& [cc, v] : triples) {
            m.emplace_back(cc, voice(v, "tune"));
            m.emplace_back(cc + 1, voice(v, "decay"));
            m.emplace_back(cc + 2, voice(v, "level"));
            if (cc == 61) m.emplace_back(71, "total_accent");
        }
        std::sort(m.begin(), m.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return m;
    }();
    return map;
}

void MidiLearn::resetToFactory() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        setMapLocked(factoryMap());
    }
    sessionDirty_.store(true, std::memory_order_release);
    changed();
}

void MidiLearn::setMapLocked(const Map& map) {
    for (std::size_t c = 0; c < target_.size(); ++c) {
        target_[c] = nullptr;
        bound_[c].store(false, std::memory_order_relaxed);
    }
    for (const auto& [controller, id] : map) assignLocked(controller, apvts_.getParameter(id));
    learnTarget_ = nullptr;
    learning_.store(false, std::memory_order_relaxed);
}

void MidiLearn::changed() {
    if (juce::MessageManager::existsAndIsCurrentThread()) {
        notifyPending_.store(false, std::memory_order_release);
        listeners_.call([](Listener& l) { l.midiLearnChanged(); });
    } else {
        notifyPending_.store(true, std::memory_order_release);
    }
}

bool MidiLearn::assignLocked(int controller, juce::RangedAudioParameter* parameter) {
    if (!validController(controller) || parameter == nullptr) return false;
    for (std::size_t c = 0; c < target_.size(); ++c)
        if (target_[c] == parameter) {
            target_[c] = nullptr;
            bound_[c].store(false, std::memory_order_relaxed);
        }
    target_[static_cast<std::size_t>(controller)] = parameter;
    bound_[static_cast<std::size_t>(controller)].store(true, std::memory_order_relaxed);
    return true;
}

void MidiLearn::beginLearn(const juce::String& parameterId) {
    auto* parameter = apvts_.getParameter(parameterId);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        learnTarget_ = parameter;
        // Controllers that arrived before arming must not complete the learn.
        Event stale;
        while (fifo_.pop(stale)) {}
        learning_.store(parameter != nullptr, std::memory_order_relaxed);
    }
    changed();
}

void MidiLearn::cancelLearn() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (learnTarget_ == nullptr) return;
        learnTarget_ = nullptr;
        learning_.store(false, std::memory_order_relaxed);
    }
    changed();
}

juce::String MidiLearn::learningParameter() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return learnTarget_ != nullptr ? learnTarget_->paramID : juce::String();
}

bool MidiLearn::assign(int controller, const juce::String& parameterId) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!assignLocked(controller, apvts_.getParameter(parameterId))) return false;
    }
    sessionDirty_.store(true, std::memory_order_release);
    changed();
    return true;
}

void MidiLearn::forget(const juce::String& parameterId) {
    bool any = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t c = 0; c < target_.size(); ++c)
            if (target_[c] != nullptr && target_[c]->paramID == parameterId) {
                target_[c] = nullptr;
                bound_[c].store(false, std::memory_order_relaxed);
                any = true;
            }
    }
    if (!any) return;
    sessionDirty_.store(true, std::memory_order_release);
    changed();
}

void MidiLearn::forgetController(int controller) {
    if (!validController(controller)) return;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto c = static_cast<std::size_t>(controller);
        if (target_[c] == nullptr) return;
        target_[c] = nullptr;
        bound_[c].store(false, std::memory_order_relaxed);
    }
    sessionDirty_.store(true, std::memory_order_release);
    changed();
}

void MidiLearn::clear() {
    bool any = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t c = 0; c < target_.size(); ++c) {
            any = any || target_[c] != nullptr;
            target_[c] = nullptr;
            bound_[c].store(false, std::memory_order_relaxed);
        }
    }
    if (!any) return;
    sessionDirty_.store(true, std::memory_order_release);
    changed();
}

int MidiLearn::controllerFor(const juce::String& parameterId) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t c = 0; c < target_.size(); ++c)
        if (target_[c] != nullptr && target_[c]->paramID == parameterId) return static_cast<int>(c);
    return -1;
}

juce::String MidiLearn::parameterFor(int controller) const {
    if (!validController(controller)) return {};
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto* p = target_[static_cast<std::size_t>(controller)];
    return p != nullptr ? p->paramID : juce::String();
}

MidiLearn::Map MidiLearn::getMap() const {
    Map map;
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t c = 0; c < target_.size(); ++c)
        if (target_[c] != nullptr) map.emplace_back(static_cast<int>(c), target_[c]->paramID);
    return map;
}

void MidiLearn::setMap(const Map& map) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        setMapLocked(map);
    }
    sessionDirty_.store(false, std::memory_order_release);
    changed();
}

void MidiLearn::service() {
    // Latest value per controller, in arrival order of first appearance.
    std::array<int, kNumControllers> latest;
    latest.fill(-1);
    int firstController = -1;

    struct Pending {
        juce::RangedAudioParameter* parameter = nullptr;
        float value = 0.0f;
    };
    std::array<Pending, kNumControllers> pending{};
    int numPending = 0;
    bool learned = false;

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Event e;
        while (fifo_.pop(e)) {
            if (e.controller >= kNumControllers) continue;
            if (firstController < 0) firstController = e.controller;
            latest[e.controller] = e.value;
        }

        if (learnTarget_ != nullptr && firstController >= 0) {
            assignLocked(firstController, learnTarget_);
            learnTarget_ = nullptr;
            learning_.store(false, std::memory_order_relaxed);
            learned = true;
        }

        for (std::size_t c = 0; c < target_.size(); ++c)
            if (latest[c] >= 0 && target_[c] != nullptr)
                // Snapped to a value the parameter can hold (switches, choices, stepped ranges).
                pending[static_cast<std::size_t>(numPending++)] = {
                    target_[c],
                    target_[c]->convertTo0to1(target_[c]->convertFrom0to1(static_cast<float>(latest[c]) / 127.0f))};
    }

    // Host callbacks happen outside the lock.
    for (int i = 0; i < numPending; ++i) {
        auto* p = pending[static_cast<std::size_t>(i)].parameter;
        const float value = pending[static_cast<std::size_t>(i)].value;
        if (juce::approximatelyEqual(p->getValue(), value)) continue;
        p->beginChangeGesture();
        p->setValueNotifyingHost(value);
        p->endChangeGesture();
    }

    if (learned) {
        sessionDirty_.store(true, std::memory_order_release);
        notifyPending_.store(true, std::memory_order_release);
    }
    if (notifyPending_.exchange(false, std::memory_order_acq_rel))
        listeners_.call([](Listener& l) { l.midiLearnChanged(); });
}

}  // namespace mw909::plugin
