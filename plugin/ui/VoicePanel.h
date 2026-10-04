// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// PANEL view (plugin.md §7): the global block (TEMPO, TOTAL ACCENT, master
// VOLUME, SHUFFLE depth, sequencer MODE) and the 11 voices in hardware order.
// Each voice column has its LEVEL and original panel knobs (skirted caps) on
// top, its extended knobs (flat caps) below, the sample voices' Source
// selector (ROM / Synth / User) with a user-ROM load button, a trigger LED,
// MUTE / SOLO and a name plate that auditions the voice and selects its
// track. CH and OH share one HI HAT band with the choke switch.

#pragma once

#include "UiCommon.h"

namespace mw909::ui {

// The voice's key: auditions on press (shift: accented) and selects its track.
class NamePlate : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit NamePlate(int voice);
    void setSelected(bool selected);
    // Silenced because another voice is soloed: the plate is drawn dimmed.
    void setImpliedMute(bool muted);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }
    int voice() const noexcept { return voice_; }

    std::function<void(bool accent)> onAudition;
    std::function<void()> onSelect;

private:
    int voice_;
    bool selected_ = false, hover_ = false, down_ = false, impliedMute_ = false;
};

// Reads the 11 SOLO switches every timer tick: which voices a solo silences.
class SoloWatch {
public:
    explicit SoloWatch(Context& ctx);
    // True for voice v when any voice is soloed and v is not.
    std::array<bool, kNumVoices> impliedMutes() const;

private:
    std::array<std::atomic<float>*, kNumVoices> solo_{};
};

// Vertical positions shared by every voice column (so rows line up across
// the panel like a printed front plate).
struct PanelRows {
    int hwTop = 0, hwRowH = 0;      // two rows of original-panel knobs
    int extTop = 0, extRowH = 0;    // two rows of extended knobs
    int sourceTop = 0;              // Source selector + user load button (sample voices)
    int switchTop = 0;              // LED, MUTE, SOLO
    int plateTop = 0, plateH = 0;   // name plate
};

class VoiceColumn : public juce::Component {
public:
    VoiceColumn(Context& ctx, int voice);
    ~VoiceColumn() override;

    void setRows(const PanelRows& rows) { rows_ = rows; resized(); }
    void setSelected(bool selected) { plate_.setSelected(selected); }
    void setImpliedMute(bool muted) { plate_.setImpliedMute(muted); }
    void flash() { lamp_.flash(); }
    void tick() { lamp_.tick(); }
    void refreshUserRom();

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void chooseUserRom();
    void showUserRomMenu();

    Context& ctx_;
    int voice_;
    PanelRows rows_;
    juce::OwnedArray<ParamControl> hardware_;  // LEVEL first, then the panel knobs
    juce::OwnedArray<ParamControl> extended_;
    std::unique_ptr<ChoiceButtons> source_;
    MenuButton loadButton_{"LOAD"};
    juce::String userRomName_;  // shown under LOAD while a user ROM is loaded
    TriggerLamp lamp_;
    std::unique_ptr<ParamButton> mute_, solo_;
    NamePlate plate_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceColumn)
};

class GlobalBlock : public CachedBackground {
public:
    explicit GlobalBlock(Context& ctx);
    void resized() override;

private:
    void paintStatic(juce::Graphics& g) override;

    Context& ctx_;
    std::unique_ptr<ParamControl> tempo_, accent_, volume_, shuffle_;
    std::unique_ptr<ChoiceButtons> mode_;
    juce::Rectangle<int> globalBand_, seqBand_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GlobalBlock)
};

class VoicePanel final : public CachedBackground,
                         private EditorState::Listener,
                         private plugin::PatternModel::Listener {
public:
    explicit VoicePanel(Context& ctx);
    ~VoicePanel() override;

    void poll(const PlayState& ps);
    void resized() override;

private:
    void paintStatic(juce::Graphics& g) override;
    void uiStateChanged() override;
    void userRomChanged(int) override;
    void bankChanged() override;
    // poll() only runs while this view is shown: re-prime the hit counters on
    // return so triggers that happened meanwhile do not flash all at once.
    void visibilityChanged() override { hitsPrimed_ = false; }

    Context& ctx_;
    GlobalBlock global_;
    juce::OwnedArray<VoiceColumn> columns_;
    std::unique_ptr<ParamButton> choke_;
    juce::Rectangle<int> voiceArea_;
    PanelRows rows_;
    std::array<std::uint32_t, kNumVoices> lastHits_{};
    bool hitsPrimed_ = false;
    SoloWatch solos_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoicePanel)
};

}  // namespace mw909::ui
