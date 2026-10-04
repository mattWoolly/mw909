// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// FX view (plugin.md §7): the two FX buses as slot rows showing each bus's
// effects in running order (drag a slot to reorder it or move it to the
// other bus; its lamp switches it on and off), the bus returns (level,
// output, FX 1 -> FX 2 send), the selected effect's full controls, the
// master chain (output stage, drive, 3-band compressor, limiter, soft clip,
// volume) and the three trigger outs.
//
// Placement is three parameters per effect (fx_<fx>_on / _bus / _order):
// the rack reads them through attachments and writes them as host gestures.

#pragma once

#include "UiCommon.h"

namespace mw909::ui {

// Both bus rows in one component, so a slot can be dragged between them.
class FxRack : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit FxRack(Context& ctx);
    ~FxRack() override;

    std::function<void(int instance)> onSelect;
    void setSelected(int instance);
    int selected() const noexcept { return selected_; }
    // Effects of a bus in running order (for the UI tool too).
    juce::Array<int> busOrder(int bus) const;
    juce::Rectangle<int> slotBounds(int bus, int position) const;
    juce::Rectangle<int> lampBounds(int bus, int position) const;
    bool dragging() const noexcept { return dragInstance_ >= 0 && dragMoved_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    static juce::String shortName(int instance);
    static constexpr int kLabelWidth = 46;

private:
    bool hitSlot(juce::Point<int> pos, int& bus, int& position) const;
    void dropTarget(juce::Point<int> pos, int& bus, int& index) const;
    void move(int instance, int bus, int index);

    Context& ctx_;
    std::vector<std::unique_ptr<juce::ParameterAttachment>> attachments_;
    int selected_ = 0;
    int dragInstance_ = -1;
    bool dragMoved_ = false;
    juce::Point<int> dragPos_;
    std::array<juce::Rectangle<int>, 2> rows_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxRack)
};

// The full controls of one effect instance.
class FxEditor : public juce::Component {
public:
    explicit FxEditor(Context& ctx);
    void setInstance(int instance);
    int instance() const noexcept { return instance_; }
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    Context& ctx_;
    int instance_ = -1;
    std::unique_ptr<ParamButton> on_;
    juce::OwnedArray<juce::Component> controls_;
    std::unique_ptr<juce::ParameterAttachment> busAttachment_, orderAttachment_;
    juce::Rectangle<int> band_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxEditor)
};

class FxView final : public CachedBackground {
public:
    explicit FxView(Context& ctx);
    ~FxView() override;
    void resized() override;
    FxRack& rack() noexcept { return rack_; }
    FxEditor& effectEditor() noexcept { return editor_; }

private:
    void paintStatic(juce::Graphics& g) override;

    Context& ctx_;
    FxRack rack_;
    FxEditor editor_;
    // Returns.
    std::unique_ptr<ParamControl> fx1Level_, fx2Level_, fx1To2_;
    std::unique_ptr<ParamValueBox> fx1Out_, fx2Out_;
    std::unique_ptr<ParamButton> fx1To2Pre_;
    // Master chain.
    std::unique_ptr<ChoiceButtons> stage_;
    std::unique_ptr<ParamControl> drive_, volume_, limThresh_, limAttack_, limRelease_, xLow_, xHigh_;
    std::unique_ptr<ParamButton> softClip_, limiter_, mcompOn_;
    struct BandRow {
        std::unique_ptr<ParamButton> on, solo;
        std::array<std::unique_ptr<ParamControl>, 5> knobs;
    };
    std::array<BandRow, 3> bands_;
    // Trigger outs.
    struct TrigRow {
        std::unique_ptr<ParamButton> on;
        std::unique_ptr<ParamValueBox> source, route, out;
        std::unique_ptr<ParamControl> pan, level;
    };
    std::array<TrigRow, 3> trigs_;

    juce::Rectangle<int> leftArea_, rightArea_, busBand_, returnsArea_, masterBand_, trigBand_;
    juce::Array<std::pair<juce::Rectangle<int>, juce::String>> legends_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxView)
};

}  // namespace mw909::ui
