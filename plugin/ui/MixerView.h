// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// MIXER view (plugin.md §7): one channel per voice — output and route, send
// bus, amount and pre, pan, spread, drive, the channel strip as a compact
// summary that expands the channel into a full column (EQ on, low / high cut,
// resonance, two bands, compressor on / pre and its five controls), mute,
// solo, a level fader with a post-fader meter (PlayState::channelPeak) and the
// name plate — plus a master column (FX returns, master volume, output meter).

#pragma once

#include "UiCommon.h"
#include "VoicePanel.h"

namespace mw909::ui {

// A vertical peak meter with a short hold, fed linear peaks by the timer.
class LevelMeter : public juce::Component {
public:
    explicit LevelMeter(int channels = 1);
    void setPeak(int channel, float linear);
    void paint(juce::Graphics& g) override;

private:
    static float toDisplay(float linear) noexcept;  // 0..1 across -60..+6 dB
    int channels_;
    std::array<float, 2> shown_{}, held_{};
    std::array<int, 2> holdAge_{};
};

// The compact strip: EQ and COMP lamps; click expands the channel.
class StripSummary : public juce::Component, public juce::SettableTooltipClient {
public:
    StripSummary(Context& ctx, int voice);
    ~StripSummary() override;
    void setExpanded(bool e) { if (e != expanded_) { expanded_ = e; repaint(); } }
    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }
    std::function<void()> onToggle;

private:
    Context& ctx_;
    bool eqOn_ = false, compOn_ = false, expanded_ = false, hover_ = false;
    std::unique_ptr<juce::ParameterAttachment> eqAttachment_, compAttachment_;
};

// Every strip control of one channel (shown while its channel is expanded).
class StripPanel : public CachedBackground {
public:
    StripPanel(Context& ctx, int voice);
    void resized() override;

private:
    void paintStatic(juce::Graphics& g) override;
    Context& ctx_;
    int voice_;
    std::unique_ptr<ParamButton> eqOn_, band1On_, band2On_, compOn_, compPre_;
    std::unique_ptr<ParamControl> lowCut_, highCut_, reso_;
    std::array<std::unique_ptr<ParamControl>, 3> band1_, band2_;
    std::unique_ptr<ParamControl> thresh_, ratio_, attack_, release_, makeup_;
    juce::Rectangle<int> eqBand_, compBand_, band1Label_, band2Label_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StripPanel)
};

class MixerChannel : public juce::Component {
public:
    MixerChannel(Context& ctx, int voice);
    void setSelected(bool selected) { plate_.setSelected(selected); }
    void setImpliedMute(bool muted) { plate_.setImpliedMute(muted); }
    void setExpanded(bool expanded);
    bool expanded() const noexcept { return expanded_; }
    void flash() { lamp_.flash(); }
    void tick() { lamp_.tick(); }
    void setPeak(float linear) { meter_.setPeak(0, linear); }
    void resized() override;
    std::function<void()> onStripToggle;

    static constexpr int kBandH = 20;
    static constexpr int kStripWidth = 250;

private:
    Context& ctx_;
    int voice_;
    bool expanded_ = false;
    std::unique_ptr<ParamValueBox> out_, route_, sendBus_;
    std::unique_ptr<ParamButton> sendPre_, mute_, solo_;
    std::unique_ptr<ParamControl> send_, pan_, spread_, drive_, level_;
    StripSummary summary_;
    std::unique_ptr<StripPanel> strip_;
    TriggerLamp lamp_;
    LevelMeter meter_;
    NamePlate plate_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerChannel)
};

class MasterColumn : public juce::Component {
public:
    explicit MasterColumn(Context& ctx);
    void resized() override;
    void setPeaks(float l, float r) { meter_.setPeak(0, l); meter_.setPeak(1, r); }

private:
    Context& ctx_;
    std::unique_ptr<ParamControl> fx1_, fx2_, volume_;
    LevelMeter meter_{2};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterColumn)
};

class MixerView final : public CachedBackground, private EditorState::Listener {
public:
    explicit MixerView(Context& ctx);
    ~MixerView() override;

    void poll(const PlayState& ps);
    void resized() override;
    void setExpanded(int voice);  // -1 collapses
    int expanded() const noexcept { return expanded_; }

    static constexpr int kMasterWidth = 104;

private:
    void paintStatic(juce::Graphics& g) override;
    void uiStateChanged() override;
    // poll() only runs while this view is shown: re-prime the hit counters on
    // return so triggers that happened meanwhile do not flash all at once.
    void visibilityChanged() override { hitsPrimed_ = false; }

    Context& ctx_;
    juce::OwnedArray<MixerChannel> channels_;
    MasterColumn master_;
    int expanded_ = -1;
    std::array<std::uint32_t, kNumVoices> lastHits_{};
    bool hitsPrimed_ = false;
    SoloWatch solos_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerView)
};

}  // namespace mw909::ui
