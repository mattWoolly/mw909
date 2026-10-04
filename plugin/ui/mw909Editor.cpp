// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "mw909Editor.h"

#include <cmath>

using namespace mw909::ui;
namespace C = mw909Colours;

namespace {
int widthForScale(float s) noexcept { return juce::roundToInt(static_cast<float>(kBaseWidth) * s); }
int heightForScale(float s) noexcept { return juce::roundToInt(static_cast<float>(kBaseHeight) * s); }
}  // namespace

mw909Editor::mw909Editor(mw909Processor& owner)
    : juce::AudioProcessorEditor(owner), processor_(owner), state_(owner), ctx_{owner, lnf_, state_, content_} {
    setLookAndFeel(&lnf_);
    setOpaque(true);

    content_.setBounds(0, 0, kBaseWidth, kBaseHeight);
    addAndMakeVisible(content_);

    header_ = std::make_unique<HeaderBar>(ctx_);
    header_->onScaleRequest = [this](float s) { setScale(s); };
    header_->onPanic = [this] { processor_.panic(); };
    content_.addAndMakeVisible(*header_);

    panel_ = std::make_unique<VoicePanel>(ctx_);
    grid_ = std::make_unique<GridView>(ctx_);
    mixer_ = std::make_unique<MixerView>(ctx_);
    fx_ = std::make_unique<FxView>(ctx_);
    for (juce::Component* v : std::initializer_list<juce::Component*>{panel_.get(), grid_.get(), mixer_.get(), fx_.get()})
        content_.addChildComponent(v);

    sequencer_ = std::make_unique<SequencerStrip>(ctx_);
    sequencer_->onSongEditToggle = [this](bool show) { showSongEditor(show); };
    content_.addAndMakeVisible(*sequencer_);

    tooltips_ = std::make_unique<juce::TooltipWindow>(this, 700);

    // Layout of the fixed face.
    header_->setBounds(0, 0, kBaseWidth, kHeaderHeight);
    const juce::Rectangle<int> viewArea(0, kViewTop, kBaseWidth, kViewHeight);
    for (juce::Component* v : std::initializer_list<juce::Component*>{panel_.get(), grid_.get(), mixer_.get(), fx_.get()})
        v->setBounds(viewArea);
    sequencer_->setBounds(0, kBaseHeight - kSequencerHeight, kBaseWidth, kSequencerHeight);
    applyView();

    state_.addListener(this);
    processor_.midiLearn().addListener(this);
    processor_.patterns().addListener(this);
    refreshLearnStates();

    // Fixed aspect, 0.6x .. 2.0x, starting at the stored scale. Read it first:
    // setResizeLimits resizes the still-empty editor to the minimum, and that
    // intermediate size must not be stored as the session's scale.
    const float stored = juce::jlimit(kMinScale, kMaxScale, state_.scale());
    {
        const juce::ScopedValueSetter<bool> guard(applyingScale_, true);
        setResizable(true, true);
        setResizeLimits(widthForScale(kMinScale), heightForScale(kMinScale), widthForScale(kMaxScale), heightForScale(kMaxScale));
        if (auto* c = getConstrainer()) c->setFixedAspectRatio(static_cast<double>(kBaseWidth) / static_cast<double>(kBaseHeight));
        setSize(widthForScale(stored), heightForScale(stored));
    }
    scale_ = stored;

    startTimerHz(30);
}

mw909Editor::~mw909Editor() {
    stopTimer();
    closeTransientWindows();
    processor_.patterns().removeListener(this);
    processor_.midiLearn().removeListener(this);
    state_.removeListener(this);
    tooltips_.reset();
    songEditor_.reset();
    sequencer_.reset();
    fx_.reset();
    mixer_.reset();
    grid_.reset();
    panel_.reset();
    header_.reset();
    setLookAndFeel(nullptr);
}

void mw909Editor::paint(juce::Graphics& g) { g.fillAll(C::band); }

void mw909Editor::resized() {
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0) return;
    const float sx = static_cast<float>(w) / static_cast<float>(kBaseWidth);
    const float sy = static_cast<float>(h) / static_cast<float>(kBaseHeight);
    const float s = juce::jmin(sx, sy);
    // Letterbox when the host hands us an off-aspect size.
    const float ox = (static_cast<float>(w) - static_cast<float>(kBaseWidth) * s) * 0.5f;
    const float oy = (static_cast<float>(h) - static_cast<float>(kBaseHeight) * s) * 0.5f;
    content_.setTransform(juce::AffineTransform::scale(s).translated(std::floor(ox), std::floor(oy)));
    scale_ = s;
    if (!applyingScale_) state_.setScale(juce::jlimit(kMinScale, kMaxScale, std::round(s * 100.0f) / 100.0f));
}

void mw909Editor::setScale(float scale) {
    scale = juce::jlimit(kMinScale, kMaxScale, scale);
    const juce::ScopedValueSetter<bool> guard(applyingScale_, true);
    setSize(widthForScale(scale), heightForScale(scale));
    state_.setScale(scale);
}

void mw909Editor::setView(View view) { state_.setView(view); }

void mw909Editor::applyView() {
    const View v = state_.view();
    panel_->setVisible(v == View::Panel);
    grid_->setVisible(v == View::Grid);
    mixer_->setVisible(v == View::Mixer);
    fx_->setVisible(v == View::Fx);
    if (songEditor_) songEditor_->toFront(false);
}

void mw909Editor::uiStateChanged() { applyView(); }

void mw909Editor::bankChanged() {
    if (!state_.syncFromProcessor()) return;
    if (std::abs(state_.scale() - scale_) > 0.005f) setScale(state_.scale());
}

// Popup menus (parented to content_), the save / serial / about AlertWindows
// (desktop windows scaled to content_) and a native file chooser may still be
// open when the host closes the editor. Close the chooser (destroying it ends
// its dialog process), dismiss the rest and detach them from lnf_, which dies
// with the editor; their callbacks run later and find their SafePointers null.
void mw909Editor::closeTransientWindows() {
    ctx_.closeFileChooser();
    juce::PopupMenu::dismissAllActiveMenus();
    auto* mcm = juce::ModalComponentManager::getInstanceWithoutCreating();
    if (mcm == nullptr) return;
    juce::Array<juce::Component::SafePointer<juce::Component>> mine;
    for (int i = 0; i < mcm->getNumModalComponents(); ++i)
        if (auto* c = mcm->getModalComponent(i))
            if (isParentOf(c) || &c->getLookAndFeel() == &lnf_) mine.add(c);
    for (auto& c : mine) {
        if (c == nullptr) continue;
        c->setLookAndFeel(nullptr);
        c->setVisible(false);
        c->exitModalState(0);
    }
}

void mw909Editor::showSongEditor(bool show) {
    sequencer_->setSongEditorOpen(show);
    if (!show) {
        songEditor_.reset();
        return;
    }
    if (songEditor_) return;
    songEditor_ = std::make_unique<SongEditor>(ctx_);
    songEditor_->onClose = [this] {
        // Deferred: the button that owns this callback may be the one being clicked.
        juce::Component::SafePointer<mw909Editor> safe(this);
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) safe->showSongEditor(false);
        });
    };
    songEditor_->setBounds(juce::Rectangle<int>(0, kViewTop, kBaseWidth, kViewHeight).reduced(110, 70));
    content_.addAndMakeVisible(*songEditor_);
}

void mw909Editor::timerCallback() {
    const auto& ps = processor_.playState();
    header_->poll(ps);
    sequencer_->poll(ps);
    if (songEditor_) songEditor_->poll(ps);
    switch (state_.view()) {
        case View::Panel: panel_->poll(ps); break;
        case View::Grid: grid_->poll(ps); break;
        case View::Mixer: mixer_->poll(ps); break;
        case View::Fx: break;
    }
}

void mw909Editor::midiLearnChanged() { refreshLearnStates(); }

void mw909Editor::refreshLearnStates() {
    auto& learn = processor_.midiLearn();
    const bool learning = learn.isLearning();
    const juce::String target = learning ? learn.learningParameter() : juce::String();
    for (auto* control : ctx_.controls) {
        const juce::String id = control->parameter().getParameterID();
        control->setLearnState(learning && id == target, learnedBinding(learn, id));
    }
}
