// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The always-visible sequencer strip (plugin.md §7): START/STOP, TAP WRITE,
// FILL, variation A / AB / B, play mode PATTERN / CHAIN / TRACK with the
// chain / track editor; BANK I / II, GROUP 1-3 and pattern keys 1-16 (the
// queued pattern blinks); the 16 step keys with their LEDs (the running step
// lit); the track selector (11 voices + AC) and pages 1-4; the step-edit mode
// LEVEL / FLAM / SUB / PROB and the tools (copy, paste, clear, shift, random
// with a seed, euclid, authentic); LAST STEP, SCALE, track length, FLAM
// interval, SHUFFLE keys 1-7 + fine, auto fill and the MIDI drag handle.
//
// Pattern data is read from PatternModel and written through
// Context::editPattern on the message thread; the play position comes from
// PlayState through poll().

#pragma once

#include <optional>

#include "StepEdit.h"
#include "UiCommon.h"

namespace mw909::ui {

// One step key with its LED.
class StepKey : public juce::Component {
public:
    explicit StepKey(int indexInPage);

    struct State {
        seq::Step step{};
        bool inRange = true;      // within the track's loop length
        bool running = false;
        int absoluteStep = 0;     // 0..63
        StepMode mode = StepMode::Level;
        int track = 0;
        bool operator==(const State&) const = default;
    };
    void setState(const State& s);
    const State& state() const noexcept { return state_; }
    int index() const noexcept { return index_; }

    std::function<void(int index, const juce::MouseEvent&)> onPress;
    std::function<void(int index)> onDragOver;
    std::function<void()> onRelease;
    std::function<void(int index, juce::Component&)> onMenu;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }

    static constexpr int kLedHeight = 9;

private:
    int index_;
    State state_;
    bool hover_ = false, down_ = false;
};

// A pattern key that can blink (queued pattern) and mark the sounding one.
class PatternButton : public juce::TextButton {
public:
    using juce::TextButton::TextButton;
    void setBlink(bool blinking, bool phase);
    void setPlaying(bool playing);
    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    bool blinking_ = false, phase_ = false, playing_ = false;
};

// Drag the selected pattern out as a .mid file; drop a .mid file to import it.
class MidiDragHandle : public juce::Component, public juce::FileDragAndDropTarget, public juce::SettableTooltipClient {
public:
    explicit MidiDragHandle(Context& ctx);
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override { dragStarted_ = false; }
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override { dragStarted_ = false; }
    void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dropHover_ = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dropHover_ = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    Context& ctx_;
    bool hover_ = false, dropHover_ = false, dragStarted_ = false;
};

// The chain / song-track editor overlay (shown by the editor over the view
// area). A long track (up to 256 measures) scrolls by rows; the playing entry
// is kept in view.
class SongEditor : public juce::Component, private plugin::PatternModel::Listener, private juce::ScrollBar::Listener {
public:
    explicit SongEditor(Context& ctx);
    ~SongEditor() override;
    std::function<void()> onClose;
    void poll(const PlayState& ps);
    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool editingTrack() const noexcept { return trackTab_; }
    void setTab(bool track);

    // For the UI tool: the entry a chip shows (-1 when off screen) and scrolling.
    juce::Rectangle<int> chipBounds(int index) const;
    bool entryVisible(int index) const;
    int firstRow() const noexcept { return firstRow_; }
    void scrollToRow(int row);
    int numEntries() const;

private:
    void chainChanged() override { updateScroll(); repaint(); }
    void songTrackChanged(int) override { updateScroll(); repaint(); }
    void bankChanged() override { updateScroll(); repaint(); }
    void scrollBarMoved(juce::ScrollBar*, double newRangeStart) override;
    int perRow() const;
    int visibleRows() const;
    int totalRows() const;
    void updateScroll();
    int entry(int index) const;
    void addSelected();
    void removeAt(int index);
    void removeLast();
    void clearAll();
    int songTrack() const;

    Context& ctx_;
    bool trackTab_ = false;
    int playingEntry_ = -1;
    juce::TextButton chainTab_{"CHAIN"}, trackTabButton_{"TRACK"};
    std::unique_ptr<ChoiceButtons> trackSelect_;
    juce::TextButton addButton_{"ADD SELECTED"}, removeButton_{"REMOVE LAST"}, clearButton_{"CLEAR"}, closeButton_{"CLOSE"};
    juce::Rectangle<int> titleArea_, chipArea_;
    juce::ScrollBar scroll_{true};
    int firstRow_ = 0;
    std::unique_ptr<juce::ParameterAttachment> trackAttachment_, modeAttachment_;
};

class SequencerStrip final : public CachedBackground,
                             private plugin::PatternModel::Listener,
                             private EditorState::Listener {
public:
    explicit SequencerStrip(Context& ctx);
    ~SequencerStrip() override;

    void poll(const PlayState& ps);
    std::function<void(bool)> onSongEditToggle;
    // The editor tells the strip when the song editor is shown or hidden, so
    // the EDIT button follows however the overlay was closed.
    void setSongEditorOpen(bool open) { songEditButton_.setToggleState(open, juce::dontSendNotification); }

    void resized() override;

    // Exposed for the UI tool.
    juce::TextButton* bankButton(int b) const noexcept { return bankButtons_[b]; }
    juce::TextButton* groupButton(int g) const noexcept { return groupButtons_[g]; }
    PatternButton* patternButton(int i) const noexcept { return patternButtons_[i]; }
    PatternButton* pageButton(int p) const noexcept { return pageButtons_[p]; }
    juce::TextButton* shuffleKey(int k) const noexcept { return shuffleKeys_[k]; }
    ValueBox* lastStepBox() const noexcept { return lastStep_.get(); }
    ValueBox* scaleBox() const noexcept { return scale_.get(); }
    ValueBox* flamBox() const noexcept { return flam_.get(); }
    ValueBox* shuffleFineBox() const noexcept { return shuffleFine_.get(); }
    ValueBox* trackLengthBox() const noexcept { return trackLength_.get(); }
    ValueBox* seedBox() const noexcept { return seed_.get(); }
    juce::TextButton& authenticButton() noexcept { return authenticButton_; }
    std::uint32_t seed() const noexcept { return seedValue_; }

    // The hint line under the tools (depends on the step-edit mode, the track
    // and AUTHENTIC); exposed for the UI tool.
    juce::String hintText() const;

private:
    void paintStatic(juce::Graphics& g) override;
    void paintDynamic(juce::Graphics& g) override;
    void refreshHint();

    // PatternModel::Listener
    void patternChanged(int index) override;
    void bankChanged() override;
    void selectionChanged() override;
    // EditorState::Listener
    void uiStateChanged() override;

    void refreshAll();
    void refreshKeys();
    void refreshPatternButtons();
    void refreshTrackButtons();
    void refreshPageButtons();
    void refreshModeButtons();
    void refreshValueBoxes();
    void refreshShuffleKeys();

    void stepKeyPressed(int index, const juce::MouseEvent& e);
    void keyDragged(int index);
    void editStep(int absoluteStep, std::function<seq::Step(seq::Step)> fn);
    void editSelected(std::function<void(seq::Pattern&)> fn);
    void selectPatternSlot(int slot);
    int selectedPattern() const { return ctx_.processor.patterns().selectedPattern(); }
    const seq::Pattern& pattern() const { return ctx_.processor.patterns().pattern(selectedPattern()); }
    int editVariation() const { return ctx_.editVariation(); }

    // Tools.
    void copy(bool trackOnly);
    void paste(bool trackOnly);
    void clear(bool trackOnly);
    void shift(int steps, bool trackOnly);
    void randomise(std::uint32_t seed, bool trackOnly);
    void showEuclidMenu();
    void setAuthentic(bool on);
    void showAuthenticMenu(bool turningOn);
    void conformSelected();

    Context& ctx_;

    // Left block.
    std::unique_ptr<ParamButton> run_, record_;
    Lamp playLamp_{mw909Colours::ledOn, mw909Colours::ledOff};
    juce::TextButton fillButton_{"FILL"}, songEditButton_{"EDIT CHAIN / TRACK"};
    std::unique_ptr<ChoiceButtons> variation_, playMode_;

    // Middle.
    juce::OwnedArray<juce::TextButton> bankButtons_, groupButtons_;
    juce::OwnedArray<PatternButton> patternButtons_;
    juce::OwnedArray<StepKey> keys_;
    juce::OwnedArray<juce::TextButton> trackButtons_, modeButtons_;
    juce::OwnedArray<PatternButton> pageButtons_;
    juce::TextButton copyButton_{"COPY"}, pasteButton_{"PASTE"}, clearButton_{"CLEAR"}, shiftLeftButton_{"< SHIFT"},
        shiftRightButton_{"SHIFT >"}, randomButton_{"RANDOM"}, seedPrevButton_{"<"}, seedNextButton_{">"}, euclidButton_{"EUCLID"};
    MenuButton authenticButton_{"AUTHENTIC"};  // right-click: conform / restore
    std::unique_ptr<ValueBox> seed_;

    // Right block.
    std::unique_ptr<ValueBox> lastStep_, scale_, trackLength_, flam_, shuffleFine_, grace_, subStart_, subEnd_;
    std::unique_ptr<ParamValueBox> autoFill_, fillSlot_;
    juce::OwnedArray<juce::TextButton> shuffleKeys_;
    MidiDragHandle midiHandle_;

    // Legend rectangles for the cached layer.
    juce::Rectangle<int> leftBlock_, rightBlock_, transportBand_, patternBand_;
    juce::Array<std::pair<juce::Rectangle<int>, juce::String>> legends_;
    juce::Rectangle<int> hintArea_;
    juce::String shownHint_;
    // The pattern as it was before AUTHENTIC conformed it (one level of undo),
    // and what conforming made of it: the undo is offered while it still
    // holds that.
    struct ConformUndo {
        int pattern = -1;
        seq::Pattern before{}, after{};
    };
    std::optional<ConformUndo> conformUndo_;

    int shownBank_ = 0, shownGroup_ = 0;
    int runningStep_ = -1;
    int shownVariation_ = 0;
    int blinkTick_ = 0;
    bool blinkPhase_ = false;
    int lastQueued_ = -1;
    int lastCurrent_ = -1;
    bool lastPlaying_ = false;
    int runLampState_ = -1;

    // Drag painting.
    bool painting_ = false;
    seq::Step brush_{};
    int paintTrack_ = 0;
    int lastPainted_ = -1;

    // Clipboard.
    std::optional<seq::Pattern> patternClip_;
    std::optional<std::array<seq::Step, seq::kMaxSteps>> trackClip_;
    std::uint8_t trackClipLength_ = 0;
    std::uint32_t seedValue_ = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequencerStrip)
};

}  // namespace mw909::ui
