// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The mw909 editor (docs/design/plugin.md §7). A 1280 x 760 content component
// is drawn through a scale transform, so the whole face resizes with a fixed
// aspect ratio between 0.6x and 2.0x; the scale is kept in the processor's
// UI state. Static layers are cached per physical scale (CachedBackground).
// A 30 Hz timer polls the processor's PlayState and each part repaints only
// what changed.
//
// Lifetime: every attachment, listener and timer is owned by the component
// that uses it and removed in its destructor, before anything it references;
// popup menus and dialogs are dismissed and knob gestures ended when the
// editor closes. The editor can be opened, closed and resized repeatedly
// while audio runs. Nothing here is reachable from the audio thread, and the
// editor never registers an AudioProcessor listener (plugin.md §7.1).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "FxView.h"
#include "GridView.h"
#include "HeaderBar.h"
#include "MixerView.h"
#include "SequencerStrip.h"
#include "UiCommon.h"
#include "VoicePanel.h"
#include "mw909LookAndFeel.h"
#include "mw909Processor.h"

class mw909Editor final : public juce::AudioProcessorEditor,
                          public juce::DragAndDropContainer,
                          private juce::Timer,
                          private mw909::plugin::MidiLearn::Listener,
                          private mw909::plugin::PatternModel::Listener,
                          private mw909::ui::EditorState::Listener {
public:
    explicit mw909Editor(mw909Processor& owner);
    ~mw909Editor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // --- for tests and tools ------------------------------------------------------
    void setView(mw909::ui::View view);
    mw909::ui::View view() const noexcept { return state_.view(); }
    // Resize the window to a scale factor of the 1280 x 760 base.
    void setScale(float scale);
    float scale() const noexcept { return scale_; }
    static constexpr int baseWidth() noexcept { return mw909::ui::kBaseWidth; }
    static constexpr int baseHeight() noexcept { return mw909::ui::kBaseHeight; }
    // Run one timer tick (what the 30 Hz timer does).
    void pollNow() { timerCallback(); }
    void showSongEditor(bool show);
    mw909::ui::SongEditor* songEditor() noexcept { return songEditor_.get(); }

private:
    void timerCallback() override;
    void midiLearnChanged() override;
    void uiStateChanged() override;
    void bankChanged() override;  // PatternModel: a state restore may carry a new UI state
    void applyView();
    void refreshLearnStates();
    void closeTransientWindows();

    mw909Processor& processor_;
    mw909LookAndFeel lnf_;
    mw909::ui::EditorState state_;

    // The fixed-size face, scaled by a transform. Menus and dialogs are
    // parented to it (Context::menuParent) so they scale with the UI.
    class Content : public juce::Component {
    public:
        Content() { setOpaque(true); }
        void paint(juce::Graphics& g) override { g.fillAll(mw909Colours::panel); }
    };
    Content content_;
    mw909::ui::Context ctx_;

    std::unique_ptr<mw909::ui::HeaderBar> header_;
    std::unique_ptr<mw909::ui::VoicePanel> panel_;
    std::unique_ptr<mw909::ui::GridView> grid_;
    std::unique_ptr<mw909::ui::MixerView> mixer_;
    std::unique_ptr<mw909::ui::FxView> fx_;
    std::unique_ptr<mw909::ui::SequencerStrip> sequencer_;
    std::unique_ptr<mw909::ui::SongEditor> songEditor_;
    std::unique_ptr<juce::TooltipWindow> tooltips_;

    float scale_ = 1.0f;
    bool applyingScale_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(mw909Editor)
};
