// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// GRID view (plugin.md §7): all 12 lanes (11 voices + total accent) x every
// step of the selected pattern, with the step keys' click semantics. Each
// lane has its own playhead, computed from PlayState::position (measure and
// step, published together) and the lane's own length, so polymeter lanes
// show where they really are. One component paints the whole grid; a step
// advance repaints only the cells whose playhead moved.

#pragma once

#include "StepEdit.h"
#include "UiCommon.h"

namespace mw909::ui {

class GridView final : public juce::Component,
                       private plugin::PatternModel::Listener,
                       private EditorState::Listener {
public:
    explicit GridView(Context& ctx);
    ~GridView() override;

    void poll(const PlayState& ps);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override;

    // For the UI tool: the centre of a cell / lane header in local coordinates.
    juce::Point<int> cellCentre(int track, int step) const { return cellBounds(track, step).getCentre(); }
    juce::Point<int> laneHeaderCentre(int track) const { return laneBounds(track).getCentre(); }
    int playheadOf(int track) const noexcept { return lanePlayhead_[static_cast<std::size_t>(track)]; }

    static constexpr int kHeaderWidth = 72;
    static constexpr int kTopRuler = 16;

private:
    void patternChanged(int index) override;
    void bankChanged() override { repaint(); }
    void selectionChanged() override { repaint(); }
    void uiStateChanged() override { repaint(); }
    void visibilityChanged() override { lanePlayhead_.fill(-1); }  // poll() only runs while shown

    int selectedPattern() const { return ctx_.processor.patterns().selectedPattern(); }
    const seq::Pattern& pattern() const { return ctx_.processor.patterns().pattern(selectedPattern()); }
    int columns() const;
    juce::Rectangle<int> cellBounds(int track, int step) const;
    juce::Rectangle<int> columnBounds(int step) const;
    juce::Rectangle<int> laneBounds(int track) const;
    bool cellAt(juce::Point<int> pos, int& track, int& step) const;
    int laneAt(juce::Point<int> pos) const;  // lane header hit, -1 if none

    Context& ctx_;
    juce::Rectangle<int> headerArea_, gridArea_;
    std::array<int, seq::kNumTracks> lanePlayhead_{};
    int shownVariation_ = 0;
    int shownColumns_ = 16;
    int hoverTrack_ = -1, hoverStep_ = -1;

    bool painting_ = false;
    seq::Step brush_{};
    int paintTrack_ = -1, lastPainted_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GridView)
};

}  // namespace mw909::ui
