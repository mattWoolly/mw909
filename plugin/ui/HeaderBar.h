// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The header (plugin.md §7): wordmark, preset browser (kind, previous / next,
// name, save, menu), PANEL / GRID / MIXER / FX switch, tempo readout, output
// meter and the settings menu (quality, variance, serial number, unit
// revision, UI scale, MIDI options, about).

#pragma once

#include <functional>

#include "UiCommon.h"

namespace mw909::ui {

// Stereo peak meter fed from PlayState::peakLeft / peakRight by the editor's timer.
class OutputMeter : public juce::Component {
public:
    OutputMeter();
    void setPeaks(float left, float right);  // linear block peaks
    void paint(juce::Graphics& g) override;

private:
    static float toDisplay(float linear) noexcept;  // 0..1 across -60..+6 dB
    float shownL_ = 0.0f, shownR_ = 0.0f;
    float holdL_ = 0.0f, holdR_ = 0.0f;
    int holdAgeL_ = 0, holdAgeR_ = 0;
    bool clipL_ = false, clipR_ = false;
};

class TempoReadout : public juce::Component, public juce::SettableTooltipClient {
public:
    void set(float bpm, bool playing, const juce::String& source, const juce::String& position);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    std::function<void(juce::Component&)> onClick;

private:
    float bpm_ = 120.0f;
    bool playing_ = false;
    juce::String source_ = "INT", position_;
};

class HeaderBar final : public CachedBackground,
                        private plugin::PresetManager::Listener,
                        private plugin::PatternModel::Listener,
                        private EditorState::Listener {
public:
    explicit HeaderBar(Context& ctx);
    ~HeaderBar() override;

    // Called by the editor's 30 Hz timer with the current play state.
    void poll(const PlayState& ps);

    // The editor supplies these: scale changes resize the window.
    std::function<void(float)> onScaleRequest;
    std::function<void()> onPanic;

    void resized() override;

private:
    void paintStatic(juce::Graphics& g) override;
    void presetListChanged(plugin::PresetKind) override;
    void presetLoaded(plugin::PresetKind) override;
    void bankChanged() override { refreshPresetName(); }  // session restore: names live in the UI state
    void uiStateChanged() override;

    void setKind(plugin::PresetKind kind);
    void refreshPresetName();
    void showPresetList();
    void showPresetMenu();
    void showSettingsMenu();
    void askSavePreset();
    void askSerialNumber();
    void showAbout();

    Context& ctx_;
    plugin::PresetKind kind_ = plugin::PresetKind::Full;

    juce::OwnedArray<juce::TextButton> kindButtons_;
    juce::TextButton prevButton_{"<"}, nextButton_{">"}, nameButton_, saveButton_{"SAVE"}, presetMenuButton_{"..."};
    juce::OwnedArray<juce::TextButton> viewButtons_;
    TempoReadout tempo_;
    OutputMeter meter_;
    juce::TextButton settingsButton_;

    juce::Rectangle<int> wordmarkArea_, presetArea_, viewArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HeaderBar)
};

}  // namespace mw909::ui
