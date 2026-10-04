// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "SequencerStrip.h"

namespace mw909::ui {

namespace C = mw909Colours;
namespace paramid = plugin::paramid;

namespace {
constexpr int kStepsPerPage = 16;
constexpr int kNumPages = seq::kMaxSteps / kStepsPerPage;
constexpr int kLeftWidth = 140;
constexpr int kRightWidth = 300;
constexpr int kPad = 8;
constexpr int kBlinkTicks = 8;  // 30 Hz ticks per half period
constexpr int kLegendH = 11;
constexpr int kBandH = 18;
constexpr int kNumShuffleKeys = 7;
}  // namespace

// --- stepedit::showStepMenu --------------------------------------------------------------------

namespace stepedit {

void showStepMenu(Context& ctx, int patternIndex, int variation, int track, int step, juce::Component& target) {
    const seq::Step s = ctx.processor.patterns().pattern(patternIndex).at(variation, track, step);
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx.lnf);
    menu.addSectionHeader(voiceCode(track) + "  step " + juce::String(step + 1));
    if (isAccentTrack(track)) {
        menu.addItem(1, "Off", true, !s.on());
        menu.addItem(4, "Accent", true, s.on());
    } else {
        menu.addItem(1, "Off", true, s.level() == seq::StepLevel::Off);
        menu.addItem(2, "Weak", true, s.level() == seq::StepLevel::Weak);
        menu.addItem(3, "Normal", true, s.level() == seq::StepLevel::Normal);
        menu.addItem(4, "Accent", true, s.level() == seq::StepLevel::Accent);
        menu.addSeparator();
        menu.addItem(10, "Flam", s.on(), s.flam());
        juce::PopupMenu sub;
        for (int n = 1; n <= 4; ++n) sub.addItem(20 + n, "x" + juce::String(n), s.on(), s.subSteps() == n);
        menu.addSubMenu("Sub-steps", sub, s.on());
        juce::PopupMenu prob;
        for (int c = 0; c <= 7; ++c)
            prob.addItem(30 + c, juce::String(juce::roundToInt(static_cast<float>(8 - c) / 8.0f * 100.0f)) + " %", s.on(),
                         s.probabilityCode() == c);
        menu.addSubMenu("Probability", prob, s.on());
    }
    juce::Component::SafePointer<juce::Component> safe(&target);
    Context* context = &ctx;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target).withParentComponent(&ctx.menuParent),
                       [safe, context, patternIndex, variation, track, step](int result) {
                           if (safe == nullptr || result <= 0) return;
                           context->editPattern(patternIndex, [&](seq::Pattern& p) {
                               seq::Step& st = p.at(variation, track, step);
                               if (result >= 1 && result <= 4) {
                                   st.setLevel(static_cast<seq::StepLevel>(result - 1));
                                   if (!st.on()) clearModifiers(st);
                               } else if (result == 10) st.setFlam(!st.flam());
                               else if (result >= 21 && result <= 24) st.setSubSteps(result - 20);
                               else if (result >= 30 && result <= 37) st.setProbabilityCode(result - 30);
                           });
                       });
}

}  // namespace stepedit

// --- StepKey ------------------------------------------------------------------------------------

StepKey::StepKey(int indexInPage) : index_(indexInPage) { setRepaintsOnMouseActivity(false); }

void StepKey::setState(const State& s) {
    if (s == state_) return;
    state_ = s;
    repaint();
}

void StepKey::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat();
    // LED above the key: the step's level, white-hot on the running step.
    auto ledRow = r.removeFromTop(static_cast<float>(kLedHeight));
    const auto led = ledRow.withSizeKeepingCentre(juce::jmin(ledRow.getWidth() * 0.5f, 22.0f), 6.0f);
    float lit = 0.0f;
    if (state_.inRange && state_.step.on()) {
        const auto level = state_.step.level();
        lit = level == seq::StepLevel::Weak ? 0.45f : level == seq::StepLevel::Normal ? 0.85f : 1.0f;
    }
    if (state_.running) mw909LookAndFeel::drawLedBar(g, led, 1.0f, C::ledRun, C::ledOff);
    else mw909LookAndFeel::drawLedBar(g, led, lit);
    r.removeFromTop(3.0f);

    mw909LookAndFeel::KeyState ks;
    ks.indexInPage = index_;
    ks.level = state_.inRange ? static_cast<int>(state_.step.level()) : 0;
    ks.inRange = state_.inRange;
    ks.running = state_.running;
    ks.hover = hover_;
    ks.down = down_;
    ks.caption = juce::String(state_.absoluteStep + 1);
    ks.badge = state_.inRange ? stepedit::badge(state_.step, state_.mode, state_.track) : juce::String();
    mw909LookAndFeel::drawStepKey(g, r, ks);
}

void StepKey::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        if (onMenu) onMenu(index_, *this);
        return;
    }
    down_ = true;
    repaint();
    if (onPress) onPress(index_, e);
}

void StepKey::mouseDrag(const juce::MouseEvent& e) {
    if (!down_ || onDragOver == nullptr) return;
    auto* parent = getParentComponent();
    if (parent == nullptr) return;
    const auto pos = e.getEventRelativeTo(parent).getPosition();
    for (auto* sibling : parent->getChildren())
        if (auto* key = dynamic_cast<StepKey*>(sibling))
            if (key->getBounds().contains(pos)) {
                onDragOver(key->index());
                return;
            }
}

void StepKey::mouseUp(const juce::MouseEvent&) {
    if (!down_) return;
    down_ = false;
    repaint();
    if (onRelease) onRelease();
}

// --- PatternButton ---------------------------------------------------------------------------------

void PatternButton::setBlink(bool blinking, bool phase) {
    if (blinking == blinking_ && (phase == phase_ || !blinking)) return;
    blinking_ = blinking;
    phase_ = phase;
    repaint();
}

void PatternButton::setPlaying(bool playing) {
    if (playing == playing_) return;
    playing_ = playing;
    repaint();
}

void PatternButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) {
    const auto r = getLocalBounds().toFloat().reduced(0.5f).withTrimmedBottom(1.0f);
    if (blinking_) {
        // The queued key blinks its whole face: lit and unlit alternate, so
        // the blink reads whether or not the key is also the selected one.
        const bool lit = getToggleState() != phase_;
        mw909LookAndFeel::drawButtonFace(g, r, lit, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, C::orange, false, isEnabled());
        g.setColour(C::ink);
        g.setFont(mw909LookAndFeel::legible(g, getLookAndFeel().getTextButtonFont(*this, getHeight()), r.getHeight() - 2.0f));
        g.drawFittedText(getButtonText(), getLocalBounds().reduced(3, 1).withTrimmedBottom(1), juce::Justification::centred, 1, 0.7f);
    } else {
        juce::TextButton::paintButton(g, shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
    }
    if (playing_) mw909LookAndFeel::drawLamp(g, juce::Rectangle<float>(5.0f, 5.0f).withCentre({r.getRight() - 5.0f, r.getY() + 5.0f}), 1.0f);
}

// --- MidiDragHandle ---------------------------------------------------------------------------------

MidiDragHandle::MidiDragHandle(Context& ctx) : ctx_(ctx) {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    setTooltip("Drag the selected pattern out as a MIDI file. Drop a .mid file here to import it.");
}

void MidiDragHandle::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    mw909LookAndFeel::drawButtonFace(g, r.withTrimmedBottom(1.0f), dropHover_, hover_, false, C::orangeSoft);
    auto inner = getLocalBounds().reduced(8, 3);
    // Grip dots.
    auto grip = inner.removeFromLeft(12).toFloat();
    g.setColour(C::inkDim);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            g.fillEllipse(grip.getX() + 1.0f + static_cast<float>(col) * 5.0f, grip.getCentreY() - 6.5f + static_cast<float>(row) * 5.0f,
                          2.5f, 2.5f);
    inner.removeFromLeft(6);
    g.setColour(C::ink);
    g.setFont(mw909LookAndFeel::legend(10.0f));
    g.drawFittedText("MIDI", inner.removeFromLeft(34), juce::Justification::centredLeft, 1);
    g.setColour(C::inkDim);
    g.setFont(mw909LookAndFeel::text(10.5f));
    g.drawFittedText(dropHover_ ? "drop to import into the pattern" : "drag the pattern out, or drop a .mid in", inner,
                     juce::Justification::centredLeft, 1, 0.8f);
}

void MidiDragHandle::mouseDrag(const juce::MouseEvent& e) {
    if (dragStarted_ || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 6) return;
    dragStarted_ = true;
    auto& p = ctx_.processor;
    const juce::File file = p.exportPatternToMidiFile(p.patterns().selectedPattern(), ctx_.editVariation());
    if (!file.existsAsFile()) return;
    juce::DragAndDropContainer::performExternalDragDropOfFiles({file.getFullPathName()}, false, this);
}

bool MidiDragHandle::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi")) return true;
    return false;
}

void MidiDragHandle::filesDropped(const juce::StringArray& files, int, int) {
    dropHover_ = false;
    repaint();
    for (const auto& f : files) {
        if (!(f.endsWithIgnoreCase(".mid") || f.endsWithIgnoreCase(".midi"))) continue;
        auto& p = ctx_.processor;
        p.importMidiFile(juce::File(f), p.patterns().selectedPattern(), ctx_.editVariation());
        break;
    }
}

// --- SongEditor -------------------------------------------------------------------------------------

SongEditor::SongEditor(Context& ctx) : ctx_(ctx) {
    for (auto* b : {&chainTab_, &trackTabButton_}) {
        styleButton(*b, C::orange, false, 10.0f);
        addAndMakeVisible(b);
    }
    chainTab_.onClick = [this] { setTab(false); };
    trackTabButton_.onClick = [this] { setTab(true); };
    if (auto* p = ctx_.param(paramid::songTrack)) {
        trackSelect_ = std::make_unique<ChoiceButtons>(ctx_, *p, juce::StringArray{"I-1", "I-2", "I-3", "I-4", "II-1", "II-2", "II-3", "II-4"},
                                                       false);
        trackSelect_->setFontHeight(9.5f);
        addChildComponent(*trackSelect_);
        trackAttachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float) { repaint(); });
    }
    if (auto* p = ctx_.param(paramid::playMode))
        modeAttachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float) { repaint(); });
    for (auto* b : {&addButton_, &removeButton_, &clearButton_, &closeButton_}) {
        styleButton(*b, C::orange, false, 10.0f);
        addAndMakeVisible(b);
    }
    addButton_.onClick = [this] { addSelected(); };
    removeButton_.onClick = [this] { removeLast(); };
    clearButton_.onClick = [this] { clearAll(); };
    closeButton_.onClick = [this] { if (onClose) onClose(); };
    addButton_.setTooltip("Append the selected pattern");
    scroll_.setAutoHide(false);
    scroll_.setSingleStepSize(1.0);
    scroll_.addListener(this);
    addChildComponent(scroll_);
    ctx_.processor.patterns().addListener(this);
    setTab(ctx_.choice(paramid::playMode) == static_cast<int>(seq::PlayMode::Track));
}

SongEditor::~SongEditor() {
    scroll_.removeListener(this);
    modeAttachment_.reset();
    trackAttachment_.reset();
    ctx_.processor.patterns().removeListener(this);
}

void SongEditor::setTab(bool track) {
    trackTab_ = track;
    chainTab_.setToggleState(!track, juce::dontSendNotification);
    trackTabButton_.setToggleState(track, juce::dontSendNotification);
    if (trackSelect_) trackSelect_->setVisible(track);
    playingEntry_ = -1;
    firstRow_ = 0;
    updateScroll();
    repaint();
}

int SongEditor::perRow() const {
    constexpr int chipW = 54, gap = 4;
    return juce::jmax(1, (chipArea_.getWidth() + gap) / (chipW + gap));
}

int SongEditor::visibleRows() const {
    constexpr int chipH = 20, gap = 4;
    return juce::jmax(1, (chipArea_.getHeight() + gap) / (chipH + gap));
}

int SongEditor::totalRows() const { return (numEntries() + perRow() - 1) / perRow(); }

void SongEditor::updateScroll() {
    const int total = totalRows(), visible = visibleRows();
    firstRow_ = juce::jlimit(0, juce::jmax(0, total - visible), firstRow_);
    scroll_.setVisible(total > visible);
    scroll_.setRangeLimits(0.0, static_cast<double>(juce::jmax(total, visible)), juce::dontSendNotification);
    scroll_.setCurrentRange(static_cast<double>(firstRow_), static_cast<double>(visible), juce::dontSendNotification);
}

void SongEditor::scrollToRow(int row) {
    const int maxFirst = juce::jmax(0, totalRows() - visibleRows());
    row = juce::jlimit(0, maxFirst, row);
    if (row == firstRow_) return;
    firstRow_ = row;
    updateScroll();
    repaint();
}

void SongEditor::scrollBarMoved(juce::ScrollBar*, double newRangeStart) { scrollToRow(juce::roundToInt(newRangeStart)); }

void SongEditor::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    const float delta = !juce::approximatelyEqual(wheel.deltaY, 0.0f) ? wheel.deltaY : -wheel.deltaX;
    if (juce::approximatelyEqual(delta, 0.0f)) return;
    scrollToRow(firstRow_ + ((delta > 0.0f) != wheel.isReversed ? -1 : 1));
}

bool SongEditor::entryVisible(int index) const {
    const int row = index / perRow();
    return index >= 0 && index < numEntries() && row >= firstRow_ && row < firstRow_ + visibleRows();
}

int SongEditor::songTrack() const { return juce::jlimit(0, seq::kNumSongTracks - 1, ctx_.choice(paramid::songTrack)); }

int SongEditor::numEntries() const {
    auto& model = ctx_.processor.patterns();
    return trackTab_ ? static_cast<int>(model.songTrack(songTrack()).length) : static_cast<int>(model.chain().length);
}

int SongEditor::entry(int index) const {
    auto& model = ctx_.processor.patterns();
    return trackTab_ ? model.songTrack(songTrack()).entries[static_cast<std::size_t>(index)]
                     : model.chain().entries[static_cast<std::size_t>(index)];
}

void SongEditor::addSelected() {
    auto& model = ctx_.processor.patterns();
    const int sel = model.selectedPattern();
    if (trackTab_) {
        model.editSongTrack(songTrack(), [sel](seq::SongTrack& t) { return seq::insertMeasure(t, t.length, sel); });
    } else {
        model.editChain([sel](seq::Chain& c) {
            if (c.length >= seq::kMaxChainLength) return false;
            c.entries[c.length++] = static_cast<std::uint8_t>(sel);
            return true;
        });
    }
    scrollToRow(totalRows());  // show what was appended
}

void SongEditor::removeAt(int index) {
    auto& model = ctx_.processor.patterns();
    if (trackTab_) {
        model.editSongTrack(songTrack(), [index](seq::SongTrack& t) { return seq::deleteMeasure(t, index, 1); });
    } else {
        model.editChain([index](seq::Chain& c) {
            if (index < 0 || index >= c.length) return false;
            for (int k = index; k + 1 < c.length; ++k) c.entries[static_cast<std::size_t>(k)] = c.entries[static_cast<std::size_t>(k + 1)];
            --c.length;
            return true;
        });
    }
}

void SongEditor::removeLast() {
    const int n = numEntries();
    if (n > 0) removeAt(n - 1);
}

void SongEditor::clearAll() {
    auto& model = ctx_.processor.patterns();
    if (trackTab_) model.editSongTrack(songTrack(), [](seq::SongTrack& t) { t = seq::SongTrack{}; });
    else model.editChain([](seq::Chain& c) { c = seq::Chain{}; });
}

void SongEditor::resized() {
    auto r = getLocalBounds().reduced(12, 8);
    titleArea_ = r.removeFromTop(24);
    {
        auto t = titleArea_;
        chainTab_.setBounds(t.removeFromLeft(70));
        t.removeFromLeft(3);
        trackTabButton_.setBounds(t.removeFromLeft(70));
        t.removeFromLeft(16);
        if (trackSelect_) trackSelect_->setBounds(t.removeFromLeft(8 * 46 + 7 * 2));
    }
    r.removeFromTop(8);
    auto buttons = r.removeFromBottom(24);
    closeButton_.setBounds(buttons.removeFromRight(70));
    addButton_.setBounds(buttons.removeFromLeft(116));
    buttons.removeFromLeft(4);
    removeButton_.setBounds(buttons.removeFromLeft(110));
    buttons.removeFromLeft(4);
    clearButton_.setBounds(buttons.removeFromLeft(70));
    r.removeFromBottom(8);
    chipArea_ = r.withTrimmedTop(16);
    scroll_.setBounds(chipArea_.removeFromRight(10));
    chipArea_.removeFromRight(6);
    updateScroll();
}

juce::Rectangle<int> SongEditor::chipBounds(int index) const {
    constexpr int chipW = 54, chipH = 20, gap = 4;
    const int n = perRow();
    const int row = index / n - firstRow_, col = index % n;
    return {chipArea_.getX() + col * (chipW + gap), chipArea_.getY() + row * (chipH + gap), chipW, chipH};
}

void SongEditor::poll(const PlayState& ps) {
    const bool modeMatches = ctx_.choice(paramid::playMode) == (trackTab_ ? 2 : 1);
    const int pos = ps.playing.load(std::memory_order_relaxed) && modeMatches ? ps.songPosition.load(std::memory_order_relaxed) : -1;
    if (pos == playingEntry_) return;
    if (playingEntry_ >= 0 && entryVisible(playingEntry_)) repaint(chipBounds(playingEntry_).expanded(2));
    playingEntry_ = pos;
    if (playingEntry_ >= 0) {
        // Follow the song: keep the playing entry on screen.
        if (!entryVisible(playingEntry_)) scrollToRow(playingEntry_ / perRow() - (playingEntry_ / perRow() > 0 ? 1 : 0));
        else repaint(chipBounds(playingEntry_).expanded(2));
    }
}

void SongEditor::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.3f));
    g.fillRoundedRectangle(r.translated(0.0f, 3.0f), 4.0f);
    mw909LookAndFeel::drawPlate(g, r, C::panelLight, C::band, 4.0f);
    g.setColour(C::band);
    g.fillRect(r.withHeight(4.0f));

    const int n = numEntries();
    const int capacity = trackTab_ ? seq::kMaxSongLength : seq::kMaxChainLength;
    const int mode = ctx_.choice(paramid::playMode);
    juce::String info = trackTab_ ? "Track " + juce::String(songTrack() < 4 ? "I-" : "II-") + juce::String(songTrack() % 4 + 1) + ":  "
                                  : juce::String("Chain:  ");
    info << n << " / " << capacity << (trackTab_ ? " measures" : " patterns") << ".  Click an entry to remove it.";
    if (totalRows() > visibleRows()) {
        const int first = firstRow_ * perRow() + 1;
        const int last = juce::jmin(n, (firstRow_ + visibleRows()) * perRow());
        info << "  Showing " << first << "-" << last << " (scroll for more).";
    }
    g.setColour(C::inkDim);
    g.setFont(mw909LookAndFeel::text(11.5f));
    g.drawText(info, chipArea_.withHeight(14).translated(0, -16), juce::Justification::centredLeft, false);
    const bool active = mode == (trackTab_ ? 2 : 1);
    g.setColour(active ? C::orangeDeep : C::inkDim);
    g.setFont(mw909LookAndFeel::legend(9.5f));
    g.drawText(active ? (trackTab_ ? "PLAY MODE: TRACK" : "PLAY MODE: CHAIN")
                      : (trackTab_ ? "SET PLAY MODE TO TRACK TO PLAY IT" : "SET PLAY MODE TO CHAIN TO PLAY IT"),
               chipArea_.withHeight(14).translated(0, -16), juce::Justification::centredRight, false);
    const int first = firstRow_ * perRow();
    const int last = juce::jmin(n, (firstRow_ + visibleRows()) * perRow());
    for (int i = first; i < last; ++i) {
        const auto chip = chipBounds(i);
        const int pat = entry(i);
        mw909LookAndFeel::drawButtonFace(g, chip.toFloat(), i == playingEntry_, false, false, C::orange);
        g.setColour(C::ink);
        g.setFont(mw909LookAndFeel::bold(10.5f));
        g.drawFittedText(patternLabel(pat), chip.reduced(2, 0), juce::Justification::centred, 1, 0.8f);
    }
    if (n == 0) {
        g.setColour(C::inkDim);
        g.setFont(mw909LookAndFeel::text(12.5f));
        g.drawText("Empty. Select a pattern with the pattern keys below and press ADD SELECTED.", chipArea_,
                   juce::Justification::centred, false);
    }
}

void SongEditor::mouseDown(const juce::MouseEvent& e) {
    const int n = numEntries();
    const int last = juce::jmin(n, (firstRow_ + visibleRows()) * perRow());
    for (int i = firstRow_ * perRow(); i < last; ++i) {
        const auto chip = chipBounds(i);
        if (chip.contains(e.getPosition())) {
            removeAt(i);
            return;
        }
    }
}

// --- SequencerStrip -----------------------------------------------------------------------------------

SequencerStrip::SequencerStrip(Context& ctx) : ctx_(ctx), midiHandle_(ctx) {
    setOpaque(true);
    auto& model = ctx_.processor.patterns();

    // Left block.
    if (auto* p = ctx_.param(paramid::seqRun)) {
        run_ = std::make_unique<ParamButton>(ctx_, *p, "START / STOP", C::orange, false);
        run_->getProperties().set("fontHeight", 11.0);
        run_->setTooltip("Run the internal clock (in Host Sync the host transport plays the sequencer)");
        addAndMakeVisible(*run_);
    }
    addAndMakeVisible(playLamp_);
    if (auto* p = ctx_.param(paramid::record)) {
        record_ = std::make_unique<ParamButton>(ctx_, *p, "TAP WRITE", C::record, true);
        record_->getProperties().set("fontHeight", 9.0);
        record_->setTooltip("Tap write: notes and name-plate hits are recorded into the running pattern");
        addAndMakeVisible(*record_);
    }
    styleButton(fillButton_, C::orange, false, 9.5f);
    fillButton_.onClick = [this] { ctx_.processor.requestFill(); };
    fillButton_.setTooltip("Play the fill pattern for the next measure");
    addAndMakeVisible(fillButton_);
    if (auto* p = ctx_.param(paramid::variation)) {
        variation_ = std::make_unique<ChoiceButtons>(ctx_, *p, juce::StringArray{"A", "AB", "B"}, false);
        variation_->setFontHeight(10.0f);
        addAndMakeVisible(*variation_);
    }
    if (auto* p = ctx_.param(paramid::playMode)) {
        playMode_ = std::make_unique<ChoiceButtons>(ctx_, *p, juce::StringArray{"PATTERN", "CHAIN", "TRACK"}, true);
        playMode_->setFontHeight(9.0f);
        playMode_->setLamp(true);
        playMode_->setGap(2);
        addAndMakeVisible(*playMode_);
    }
    styleButton(songEditButton_, C::orange, false, 9.0f);
    songEditButton_.setClickingTogglesState(true);
    songEditButton_.onClick = [this] { if (onSongEditToggle) onSongEditToggle(songEditButton_.getToggleState()); };
    songEditButton_.setTooltip("Edit the pattern chain and the song tracks");
    addAndMakeVisible(songEditButton_);

    // Banks, groups, patterns, pages.
    for (int b = 0; b < seq::kNumBanks; ++b) {
        auto* btn = bankButtons_.add(new juce::TextButton(b == 0 ? "I" : "II"));
        styleButton(*btn, C::orange, false, 10.0f);
        btn->onClick = [this, b] { shownBank_ = b; refreshPatternButtons(); };
        btn->setTooltip("Show bank " + juce::String(b == 0 ? "I" : "II"));
        addAndMakeVisible(btn);
    }
    for (int gr = 0; gr < seq::kGroupsPerBank; ++gr) {
        auto* btn = groupButtons_.add(new juce::TextButton(juce::String(gr + 1)));
        styleButton(*btn, C::orange, false, 10.0f);
        btn->onClick = [this, gr] { shownGroup_ = gr; refreshPatternButtons(); };
        btn->setTooltip("Show pattern group " + juce::String(gr + 1));
        addAndMakeVisible(btn);
    }
    for (int i = 0; i < seq::kPatternsPerGroup; ++i) {
        auto* btn = patternButtons_.add(new PatternButton(juce::String(i + 1)));
        styleButton(*btn, C::orange, false, 10.0f);
        btn->onClick = [this, i] { selectPatternSlot(i); };
        btn->setTooltip("Click: select (queued for the next measure while playing). Shift-click: switch now.");
        addAndMakeVisible(btn);
    }
    for (int pg = 0; pg < kNumPages; ++pg) {
        auto* btn = pageButtons_.add(new PatternButton(juce::String(pg + 1)));
        styleButton(*btn, C::orange, false, 9.5f);
        btn->onClick = [this, pg] { ctx_.state.setPage(pg); };
        btn->setTooltip("Steps " + juce::String(pg * 16 + 1) + "-" + juce::String(pg * 16 + 16));
        addAndMakeVisible(btn);
    }

    // Keys.
    for (int i = 0; i < kStepsPerPage; ++i) {
        auto* key = keys_.add(new StepKey(i));
        key->onPress = [this](int index, const juce::MouseEvent& e) { stepKeyPressed(index, e); };
        key->onDragOver = [this](int index) { keyDragged(index); };
        key->onRelease = [this] { painting_ = false; };
        key->onMenu = [this](int index, juce::Component& target) {
            const int step = ctx_.state.page() * kStepsPerPage + index;
            if (step >= pattern().lengthOf(ctx_.state.selectedTrack())) return;
            stepedit::showStepMenu(ctx_, selectedPattern(), editVariation(), ctx_.state.selectedTrack(), step, target);
        };
        addAndMakeVisible(key);
    }

    // Tracks.
    for (int t = 0; t < seq::kNumTracks; ++t) {
        auto* btn = trackButtons_.add(new juce::TextButton(voiceCode(t)));
        styleButton(*btn, C::orange, false, 9.5f);
        btn->onClick = [this, t] { ctx_.state.setSelectedTrack(t); };
        btn->setTooltip(t < kNumVoices ? "Edit the " + voiceName(t) + " track" : juce::String("Edit the total accent row"));
        addAndMakeVisible(btn);
    }

    // Modes and tools.
    for (int m = 0; m < kNumStepModes; ++m) {
        auto* btn = modeButtons_.add(new juce::TextButton(kStepModeNames[m]));
        styleButton(*btn, C::orange, false, 9.0f);
        btn->onClick = [this, m] { ctx_.state.setStepMode(static_cast<StepMode>(m)); };
        addAndMakeVisible(btn);
    }
    modeButtons_[0]->setTooltip("Click a key: off > normal > accent. Shift-click: weak.");
    modeButtons_[1]->setTooltip("Click an active step to toggle its flam");
    modeButtons_[2]->setTooltip("Click an active step to cycle sub-steps x1..x4 (shift: backwards)");
    modeButtons_[3]->setTooltip("Click an active step to cycle its probability (shift: backwards)");
    for (auto* b : {&copyButton_, &pasteButton_, &clearButton_, &shiftLeftButton_, &shiftRightButton_, &randomButton_, &seedPrevButton_,
                    &seedNextButton_, &euclidButton_}) {
        styleButton(*b, C::orange, false, 9.0f);
        addAndMakeVisible(b);
    }
    styleButton(authenticButton_, C::orange, true, 9.0f);
    addAndMakeVisible(authenticButton_);
    copyButton_.setTooltip("Copy the pattern (shift: the selected track)");
    pasteButton_.setTooltip("Paste the pattern (shift: the selected track)");
    clearButton_.setTooltip("Clear this variation (shift: the selected track)");
    shiftLeftButton_.setTooltip("Rotate one step earlier (shift: the selected track)");
    shiftRightButton_.setTooltip("Rotate one step later (shift: the selected track)");
    randomButton_.setTooltip("Random pattern from a new seed (shift: the selected track)");
    seedPrevButton_.setTooltip("Previous seed: regenerate (shift: the selected track)");
    seedNextButton_.setTooltip("Next seed: regenerate (shift: the selected track)");
    euclidButton_.setTooltip("Euclidean rhythm on the selected track");
    authenticButton_.setTooltip("Authentic editing: new edits keep to what the original machine can hold "
                                "(16 steps, 4 scales, hardware accents and flams, no weak, sub-steps or probability). "
                                "Existing pattern data is only rewritten if you choose to conform it.");
    auto shiftHeld = [] { return juce::ModifierKeys::currentModifiers.isShiftDown(); };
    copyButton_.onClick = [this, shiftHeld] { copy(shiftHeld()); };
    pasteButton_.onClick = [this, shiftHeld] { paste(shiftHeld()); };
    clearButton_.onClick = [this, shiftHeld] { clear(shiftHeld()); };
    shiftLeftButton_.onClick = [this, shiftHeld] { shift(-1, shiftHeld()); };
    shiftRightButton_.onClick = [this, shiftHeld] { shift(1, shiftHeld()); };
    randomButton_.onClick = [this, shiftHeld] {
        const auto t = static_cast<std::uint32_t>(juce::Time::getMillisecondCounter());
        seedValue_ = 1 + (t * 2654435761u >> 8) % 9999u;
        randomise(seedValue_, shiftHeld());
    };
    seedPrevButton_.onClick = [this, shiftHeld] {
        seedValue_ = seedValue_ <= 1 ? 9999 : seedValue_ - 1;
        randomise(seedValue_, shiftHeld());
    };
    seedNextButton_.onClick = [this, shiftHeld] {
        seedValue_ = seedValue_ >= 9999 ? 1 : seedValue_ + 1;
        randomise(seedValue_, shiftHeld());
    };
    euclidButton_.onClick = [this] { showEuclidMenu(); };
    authenticButton_.onClick = [this] { setAuthentic(!ctx_.state.authentic()); };
    authenticButton_.onMenu = [this](juce::Component&) { showAuthenticMenu(!ctx_.state.authentic()); };
    seed_ = std::make_unique<ValueBox>("", 1, 9999, 1);
    seed_->getValue = [this] { return static_cast<int>(seedValue_); };
    seed_->setValue = [this](int v) {
        seedValue_ = static_cast<std::uint32_t>(v);
        randomise(seedValue_, juce::ModifierKeys::currentModifiers.isShiftDown());
    };
    seed_->format = [](int v) { return "#" + juce::String(v); };
    seed_->setTooltip("Random seed: the pattern RANDOM wrote. Drag or wheel to regenerate from another seed.");
    addAndMakeVisible(*seed_);

    // Right block: pattern fields.
    lastStep_ = std::make_unique<ValueBox>("LAST STEP", 1, seq::kMaxSteps, 16);
    lastStep_->getValue = [this] { return static_cast<int>(pattern().length); };
    lastStep_->setValue = [this](int v) { editSelected([v](seq::Pattern& p) { p.length = static_cast<std::uint8_t>(v); }); };
    lastStep_->setTooltip("Pattern length in steps (pages follow it)");
    scale_ = std::make_unique<ValueBox>("SCALE", 0, static_cast<int>(seq::Scale::kNumScales) - 1, 0);
    for (int s = 0; s < static_cast<int>(seq::Scale::kNumScales); ++s) scale_->choices.add(stepedit::scaleName(static_cast<seq::Scale>(s)));
    scale_->menuParent = &ctx_.menuParent;
    scale_->getValue = [this] { return static_cast<int>(pattern().scale); };
    scale_->setValue = [this](int v) { editSelected([v](seq::Pattern& p) { seq::setScale(p, static_cast<seq::Scale>(v)); }); };
    scale_->format = [](int v) { return juce::String(stepedit::scaleName(static_cast<seq::Scale>(v))); };
    scale_->setTooltip("Step length. Switching between straight and triplet scales moves LAST STEP between 16 and 12.");
    trackLength_ = std::make_unique<ValueBox>("TRACK LEN", 0, seq::kMaxSteps, 0);
    trackLength_->getValue = [this] {
        const int t = ctx_.state.selectedTrack();
        return t < kNumVoices ? static_cast<int>(pattern().trackLength[static_cast<std::size_t>(t)]) : 0;
    };
    trackLength_->setValue = [this](int v) {
        const int t = ctx_.state.selectedTrack();
        if (t >= kNumVoices) return;
        editSelected([v, t](seq::Pattern& p) { p.trackLength[static_cast<std::size_t>(t)] = static_cast<std::uint8_t>(v); });
    };
    trackLength_->format = [](int v) { return v == 0 ? juce::String("= PTN") : juce::String(v); };
    trackLength_->setTooltip("Loop length of the selected voice track (polymeter). = PTN follows LAST STEP.");
    flam_ = std::make_unique<ValueBox>("FLAM", seq::kMinFlamMs, seq::kMaxFlamMs, 24);
    flam_->getValue = [this] { return static_cast<int>(pattern().flamMs); };
    flam_->setValue = [this](int v) { editSelected([v](seq::Pattern& p) { p.flamMs = static_cast<std::uint8_t>(v); }); };
    flam_->format = [](int v) { return juce::String(v) + " ms"; };
    flam_->setTooltip("Flam interval of this pattern. The original's flam keys are 20 to 48 ms in 4 ms steps.");
    shuffleFine_ = std::make_unique<ValueBox>("FINE", 0, seq::kMaxShuffleTicks, 0);
    shuffleFine_->getValue = [this] { return static_cast<int>(pattern().shuffle); };
    shuffleFine_->setValue = [this](int v) { editSelected([v](seq::Pattern& p) { p.shuffle = static_cast<std::uint8_t>(v); }); };
    shuffleFine_->format = [](int v) { return juce::String(v) + "/96"; };
    shuffleFine_->setTooltip("Shuffle: delay of the off-beat steps in 1/96 of a beat");
    auto percentBox = [this](const char* legend, std::uint8_t seq::Pattern::*field, const char* tip) {
        auto box = std::make_unique<ValueBox>(legend, 0, 100, 100);
        box->getValue = [this, field] { return static_cast<int>(pattern().*field); };
        box->setValue = [this, field](int v) { editSelected([v, field](seq::Pattern& p) { p.*field = static_cast<std::uint8_t>(v); }); };
        box->format = [](int v) { return juce::String(v) + " %"; };
        box->setTooltip(tip);
        return box;
    };
    grace_ = percentBox("FLAM GRACE", &seq::Pattern::flamGrace, "Level of a flam's first (grace) hit, in % of the main hit");
    subStart_ = percentBox("SUB START", &seq::Pattern::subStart, "Sub-step level ramp: the first sub-hit, in % of the step's level");
    subEnd_ = percentBox("SUB END", &seq::Pattern::subEnd, "Sub-step level ramp: the last sub-hit, in % of the step's level");
    for (auto* vb : {lastStep_.get(), scale_.get(), trackLength_.get(), flam_.get(), shuffleFine_.get(), grace_.get(), subStart_.get(),
                     subEnd_.get()})
        addAndMakeVisible(vb);
    for (int k = 0; k < kNumShuffleKeys; ++k) {
        auto* btn = shuffleKeys_.add(new juce::TextButton(juce::String(k + 1)));
        styleButton(*btn, C::orange, false, 9.5f);
        btn->onClick = [this, k] {
            const auto ticks = seq::shuffleTicksForKey(k + 1);
            editSelected([ticks](seq::Pattern& p) { p.shuffle = ticks; });
        };
        btn->setTooltip("Shuffle key " + juce::String(k + 1) + ": " + juce::String(static_cast<int>(seq::shuffleTicksForKey(k + 1))) +
                        "/96 beat");
        addAndMakeVisible(btn);
    }
    if (auto* p = ctx_.param(paramid::autoFill)) {
        autoFill_ = std::make_unique<ParamValueBox>(ctx_, *p, "AUTO FILL", true);
        autoFill_->setTooltip("Play the fill pattern every N measures");
        addAndMakeVisible(*autoFill_);
    }
    if (auto* p = ctx_.param(paramid::fillSlot)) {
        fillSlot_ = std::make_unique<ParamValueBox>(ctx_, *p, "FILL PTN", true);
        fillSlot_->setTooltip("Which pattern of the current group plays as the fill");
        addAndMakeVisible(*fillSlot_);
    }
    addAndMakeVisible(midiHandle_);

    model.addListener(this);
    ctx_.state.addListener(this);
    shownBank_ = seq::bankOf(model.selectedPattern());
    shownGroup_ = seq::groupOf(model.selectedPattern());
    shownVariation_ = editVariation();
    refreshAll();
}

SequencerStrip::~SequencerStrip() {
    ctx_.state.removeListener(this);
    ctx_.processor.patterns().removeListener(this);
}

void SequencerStrip::resized() {
    legends_.clear();
    auto r = getLocalBounds().reduced(kPad, kPad);
    auto legend = [this](juce::Rectangle<int> area, const juce::String& text) { legends_.add({area, text}); };

    // Left block.
    leftBlock_ = r.removeFromLeft(kLeftWidth);
    {
        auto left = leftBlock_;
        transportBand_ = left.removeFromTop(kBandH);
        left.reduce(6, 0);
        left.removeFromTop(6);
        if (run_) {
            auto row = left.removeFromTop(36);
            playLamp_.setBounds(row.removeFromLeft(12).withSizeKeepingCentre(9, 9));
            row.removeFromLeft(4);
            run_->setBounds(row);
        }
        left.removeFromTop(6);
        {
            auto row = left.removeFromTop(22);
            if (record_) record_->setBounds(row.removeFromLeft(78));
            row.removeFromLeft(4);
            fillButton_.setBounds(row);
        }
        left.removeFromTop(5);
        legend(left.removeFromTop(kLegendH), "VARIATION");
        if (variation_) variation_->setBounds(left.removeFromTop(22));
        left.removeFromTop(5);
        legend(left.removeFromTop(kLegendH), "PLAY MODE");
        if (playMode_) playMode_->setBounds(left.removeFromTop(3 * 18 + 2 * 2));
        left.removeFromTop(5);
        songEditButton_.setBounds(left.removeFromTop(22));
    }
    r.removeFromLeft(12);

    // Right block.
    rightBlock_ = r.removeFromRight(kRightWidth);
    {
        auto right = rightBlock_;
        patternBand_ = right.removeFromTop(kBandH);
        right.reduce(8, 0);
        right.removeFromTop(6);
        layoutRow(right.removeFromTop(34), {lastStep_.get(), scale_.get(), trackLength_.get()}, 8);
        right.removeFromTop(6);
        layoutRow(right.removeFromTop(34), {flam_.get(), autoFill_.get(), fillSlot_.get()}, 8);
        right.removeFromTop(6);
        layoutRow(right.removeFromTop(34), {grace_.get(), subStart_.get(), subEnd_.get()}, 8);
        right.removeFromTop(6);
        {
            auto row = right.removeFromTop(34);
            auto fine = row.removeFromRight(70);
            shuffleFine_->setBounds(fine);
            row.removeFromRight(8);
            legend(row.removeFromTop(kLegendH), "SHUFFLE");
            row.removeFromTop(1);
            const int n = shuffleKeys_.size();
            for (int i = 0; i < n; ++i) {
                const int w = (row.getWidth() - 2 * (n - 1 - i)) / (n - i);
                shuffleKeys_[i]->setBounds(row.removeFromLeft(w));
                row.removeFromLeft(2);
            }
        }
        right.removeFromTop(8);
        midiHandle_.setBounds(right.removeFromTop(28));
    }
    r.removeFromRight(12);

    // Middle: bank / group / pattern row.
    {
        auto legendRow = r.removeFromTop(kLegendH);
        auto row = r.removeFromTop(22);
        auto banks = row.removeFromLeft(2 * 28 + 2);
        legend(legendRow.removeFromLeft(banks.getWidth()), "BANK");
        for (auto* b : bankButtons_) { b->setBounds(banks.removeFromLeft(28)); banks.removeFromLeft(2); }
        row.removeFromLeft(10);
        legendRow.removeFromLeft(10);
        auto groups = row.removeFromLeft(3 * 26 + 4);
        legend(legendRow.removeFromLeft(groups.getWidth()), "GROUP");
        for (auto* b : groupButtons_) { b->setBounds(groups.removeFromLeft(26)); groups.removeFromLeft(2); }
        row.removeFromLeft(10);
        legendRow.removeFromLeft(10);
        legend(legendRow, "PATTERN");
        const int n = patternButtons_.size();
        for (int i = 0; i < n; ++i) {
            const int w = (row.getWidth() - 2 * (n - 1 - i)) / (n - i);
            patternButtons_[i]->setBounds(row.removeFromLeft(w));
            row.removeFromLeft(2);
        }
    }
    r.removeFromTop(6);
    {
        auto keyRow = r.removeFromTop(98);
        const int n = keys_.size();
        const int gap = 4;
        for (int i = 0; i < n; ++i) {
            const int w = (keyRow.getWidth() - gap * (n - 1 - i)) / (n - i);
            keys_[i]->setBounds(keyRow.removeFromLeft(w));
            keyRow.removeFromLeft(gap);
        }
    }
    r.removeFromTop(5);
    {
        auto legendRow = r.removeFromTop(kLegendH);
        auto row = r.removeFromTop(22);
        auto pages = row.removeFromRight(4 * 32 + 3 * 2);
        legend(legendRow.removeFromRight(pages.getWidth()), "PAGE");
        for (auto* b : pageButtons_) { b->setBounds(pages.removeFromLeft(32)); pages.removeFromLeft(2); }
        row.removeFromRight(12);
        legendRow.removeFromRight(12);
        legend(legendRow, "TRACK");
        const int n = trackButtons_.size();
        for (int i = 0; i < n; ++i) {
            const int w = (row.getWidth() - 2 * (n - 1 - i)) / (n - i);
            trackButtons_[i]->setBounds(row.removeFromLeft(w));
            row.removeFromLeft(2);
        }
    }
    r.removeFromTop(4);
    {
        auto legendRow = r.removeFromTop(kLegendH);
        auto row = r.removeFromTop(22);
        auto modes = row.removeFromLeft(4 * 46 + 3 * 2);
        legend(legendRow.removeFromLeft(modes.getWidth()), "STEP EDIT");
        for (auto* b : modeButtons_) { b->setBounds(modes.removeFromLeft(46)); modes.removeFromLeft(2); }
        row.removeFromLeft(12);
        legendRow.removeFromLeft(12);
        struct Item { juce::Component* c; int w; int gapAfter; };
        const Item items[] = {{&copyButton_, 44, 2},       {&pasteButton_, 46, 2},     {&clearButton_, 46, 8},
                              {&shiftLeftButton_, 56, 2},  {&shiftRightButton_, 56, 8}, {&randomButton_, 58, 2},
                              {&seedPrevButton_, 20, 2},   {seed_.get(), 46, 2},       {&seedNextButton_, 20, 8},
                              {&euclidButton_, 52, 8},     {&authenticButton_, 0, 0}};
        legend(legendRow.withWidth(44 + 2 + 46 + 2 + 46 + 8 + 56 + 2 + 56), "TOOLS");
        legend(legendRow.withTrimmedLeft(44 + 2 + 46 + 2 + 46 + 8 + 56 + 2 + 56 + 8).withWidth(58 + 2 + 20 + 2 + 46 + 2 + 20), "RANDOM SEED");
        for (const auto& it : items) {
            const int w = it.w > 0 ? it.w : row.getWidth();
            it.c->setBounds(row.removeFromLeft(w));
            row.removeFromLeft(it.gapAfter);
        }
    }
    r.removeFromTop(3);
    hintArea_ = r;
    invalidateCache();
}

void SequencerStrip::paintStatic(juce::Graphics& g) {
    g.fillAll(C::panelShade);
    g.setColour(C::panelEdge);
    g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
    for (auto block : {leftBlock_, rightBlock_}) mw909LookAndFeel::drawPlate(g, block.toFloat(), C::panelLight, C::panelEdge, 0.0f);
    mw909LookAndFeel::drawBand(g, transportBand_, "TRANSPORT");
    mw909LookAndFeel::drawBand(g, patternBand_, "PATTERN");
    for (const auto& l : legends_) mw909LookAndFeel::drawLegend(g, l.second, l.first, C::inkDim, 9.0f, juce::Justification::centredLeft);
}

void SequencerStrip::paintDynamic(juce::Graphics& g) {
    if (!g.clipRegionIntersects(hintArea_)) return;
    g.setColour(C::inkDim);
    g.setFont(mw909LookAndFeel::legible(g, mw909LookAndFeel::text(10.5f), static_cast<float>(hintArea_.getHeight())));
    g.drawFittedText(shownHint_, hintArea_, juce::Justification::centredLeft, 1, 0.75f);
}

juce::String SequencerStrip::hintText() const {
    const int track = ctx_.state.selectedTrack();
    const bool authentic = ctx_.state.authentic();
    juce::String h;
    if (authentic) {
        h = seq::followsHardwareRules(pattern()) ? "AUTHENTIC: edits keep to the original's rules.  "
                                                 : "AUTHENTIC: this pattern holds more than the original could; new edits keep to its "
                                                   "rules (right-click AUTHENTIC to conform it).  ";
    }
    if (track >= kNumVoices) return h + "TOTAL ACCENT row: click a step to toggle its accent.  Drag to paint.";
    switch (ctx_.state.stepMode()) {
        case StepMode::Flam:
            return h + "FLAM: click an active step to toggle its flam (FLAM sets the interval).  Click an empty step to set it.  "
                       "Right-click a step for its menu.";
        case StepMode::Sub:
            return h + "SUB: click an active step to cycle x1 > x2 > x3 > x4, shift-click to go back.  "
                       "SUB START / SUB END ramp the level.";
        case StepMode::Prob:
            return h + "PROB: click an active step to raise its probability, shift-click to lower it.  Right-click a step for its menu.";
        case StepMode::Level: break;
    }
    return h + (authentic ? "Click a step: off > normal > accent.  Drag to paint.  Right-click a step for its menu."
                          : "Click a step: off > normal > accent.  Shift-click: weak.  Drag to paint.  Right-click a step for its menu.  "
                            "Hold shift on a tool to work on the selected track only.");
}

void SequencerStrip::refreshHint() {
    const juce::String h = hintText();
    if (h == shownHint_) return;
    shownHint_ = h;
    repaint(hintArea_);
}

// --- model / state listeners ----------------------------------------------------------------------------

void SequencerStrip::patternChanged(int index) {
    if (index != selectedPattern()) return;
    refreshKeys();
    refreshPageButtons();  // LAST STEP / TRACK LEN decide which pages are reachable
    refreshValueBoxes();
    refreshShuffleKeys();
    refreshHint();
}

void SequencerStrip::bankChanged() { refreshAll(); }

void SequencerStrip::selectionChanged() {
    shownBank_ = seq::bankOf(selectedPattern());
    shownGroup_ = seq::groupOf(selectedPattern());
    refreshAll();
}

void SequencerStrip::uiStateChanged() { refreshAll(); }

void SequencerStrip::refreshAll() {
    refreshPatternButtons();
    refreshTrackButtons();
    refreshPageButtons();
    refreshModeButtons();
    refreshKeys();
    refreshValueBoxes();
    refreshShuffleKeys();
    refreshHint();
}

void SequencerStrip::refreshValueBoxes() {
    for (auto* vb : {lastStep_.get(), scale_.get(), trackLength_.get(), flam_.get(), shuffleFine_.get(), grace_.get(), subStart_.get(),
                     subEnd_.get(), seed_.get()})
        vb->refresh();
    trackLength_->setEnabled(ctx_.state.selectedTrack() < kNumVoices && !ctx_.state.authentic());
}

void SequencerStrip::refreshShuffleKeys() {
    const int ticks = pattern().shuffle;
    for (int k = 0; k < shuffleKeys_.size(); ++k)
        shuffleKeys_[k]->setToggleState(ticks == seq::shuffleTicksForKey(k + 1), juce::dontSendNotification);
}

void SequencerStrip::refreshKeys() {
    const auto& p = pattern();
    const int track = ctx_.state.selectedTrack();
    const int var = editVariation();
    const int page = ctx_.state.page();
    const int len = p.lengthOf(track);
    const StepMode mode = ctx_.state.stepMode();
    for (int i = 0; i < keys_.size(); ++i) {
        StepKey::State s;
        s.absoluteStep = page * kStepsPerPage + i;
        s.inRange = s.absoluteStep < len;
        s.step = p.at(var, track, s.absoluteStep);
        s.running = runningStep_ == s.absoluteStep;
        s.mode = mode;
        s.track = track;
        keys_[i]->setState(s);
    }
}

void SequencerStrip::refreshPatternButtons() {
    const int sel = selectedPattern();
    for (int b = 0; b < bankButtons_.size(); ++b) bankButtons_[b]->setToggleState(b == shownBank_, juce::dontSendNotification);
    for (int gr = 0; gr < groupButtons_.size(); ++gr) groupButtons_[gr]->setToggleState(gr == shownGroup_, juce::dontSendNotification);
    for (int i = 0; i < patternButtons_.size(); ++i) {
        const int index = seq::patternIndex(shownBank_, shownGroup_, i);
        patternButtons_[i]->setToggleState(index == sel, juce::dontSendNotification);
        patternButtons_[i]->setBlink(index == lastQueued_ && index != lastCurrent_, blinkPhase_);
        patternButtons_[i]->setPlaying(lastPlaying_ && index == lastCurrent_ && index != sel);
    }
}

void SequencerStrip::refreshTrackButtons() {
    const int sel = ctx_.state.selectedTrack();
    for (int t = 0; t < trackButtons_.size(); ++t) trackButtons_[t]->setToggleState(t == sel, juce::dontSendNotification);
}

void SequencerStrip::refreshPageButtons() {
    const int page = ctx_.state.page();
    const int runPage = runningStep_ >= 0 ? runningStep_ / kStepsPerPage : -1;
    const int len = pattern().lengthOf(ctx_.state.selectedTrack());
    for (int pg = 0; pg < pageButtons_.size(); ++pg) {
        auto* b = pageButtons_[pg];
        b->setToggleState(pg == page, juce::dontSendNotification);
        b->setEnabled(pg * kStepsPerPage < len || pg == page);
        b->setPlaying(pg == runPage);  // the lamp follows the playhead's page
    }
}

void SequencerStrip::refreshModeButtons() {
    const int mode = static_cast<int>(ctx_.state.stepMode());
    const bool authentic = ctx_.state.authentic();
    for (int m = 0; m < modeButtons_.size(); ++m) {
        modeButtons_[m]->setToggleState(m == mode, juce::dontSendNotification);
        modeButtons_[m]->setEnabled(!authentic || m <= static_cast<int>(StepMode::Flam) || m == mode);
    }
    authenticButton_.setToggleState(authentic, juce::dontSendNotification);
}

// --- polling -----------------------------------------------------------------------------------------------

void SequencerStrip::poll(const PlayState& ps) {
    const bool playing = ps.playing.load(std::memory_order_relaxed);
    const int queued = ps.queuedPattern.load(std::memory_order_relaxed);
    const int current = ps.currentPattern.load(std::memory_order_relaxed);
    // The keys show the selected pattern: light a running step only when that
    // is the pattern sounding. The position word holds the measure and the
    // step together, so a polymeter track's own playhead is exact.
    int step = -1;
    if (playing && current == selectedPattern()) {
        const auto pos = ps.position.load(std::memory_order_relaxed);
        const int patternStep = static_cast<int>(pos & 0xffU) - 1;
        if (patternStep >= 0) {
            const auto measure = static_cast<std::int64_t>(pos >> 8);
            const seq::Pattern& p = pattern();
            const int trackLen = juce::jmax(1, p.lengthOf(ctx_.state.selectedTrack()));
            step = trackLen == p.length ? patternStep
                                        : static_cast<int>((measure * static_cast<std::int64_t>(p.length) + patternStep) % trackLen);
        }
    }
    const int var = editVariation();

    const int lampState = playing ? 1 : 0;
    if (lampState != runLampState_) {
        runLampState_ = lampState;
        playLamp_.setLit(playing ? 1.0f : 0.0f);
    }

    bool keysDirty = false;
    if (step != runningStep_) {
        const int oldPage = runningStep_ / kStepsPerPage, newPage = step / kStepsPerPage;
        runningStep_ = step;
        keysDirty = true;
        if (oldPage != newPage || step < 0) refreshPageButtons();
    }
    if (var != shownVariation_) {
        shownVariation_ = var;
        keysDirty = true;
    }
    if (keysDirty) refreshKeys();

    bool patternsDirty = false;
    if (queued >= 0 && queued != current) {
        if (++blinkTick_ >= kBlinkTicks) {
            blinkTick_ = 0;
            blinkPhase_ = !blinkPhase_;
            patternsDirty = true;
        }
    } else if (blinkPhase_ || blinkTick_ != 0) {
        blinkPhase_ = false;
        blinkTick_ = 0;
        patternsDirty = true;
    }
    if (queued != lastQueued_ || current != lastCurrent_ || playing != lastPlaying_) {
        lastQueued_ = queued;
        lastCurrent_ = current;
        lastPlaying_ = playing;
        patternsDirty = true;
    }
    if (patternsDirty) refreshPatternButtons();
}

// --- step editing -----------------------------------------------------------------------------------------

void SequencerStrip::editSelected(std::function<void(seq::Pattern&)> fn) { ctx_.editPattern(selectedPattern(), fn); }

void SequencerStrip::selectPatternSlot(int slot) {
    const int index = seq::patternIndex(shownBank_, shownGroup_, slot);
    const bool playing = ctx_.processor.playState().playing.load(std::memory_order_relaxed);
    const bool immediate = juce::ModifierKeys::currentModifiers.isShiftDown() || !playing;
    ctx_.processor.patterns().selectPattern(index, immediate);
}

void SequencerStrip::editStep(int absoluteStep, std::function<seq::Step(seq::Step)> fn) {
    const int track = ctx_.state.selectedTrack();
    const int var = editVariation();
    ctx_.editPattern(selectedPattern(), [&](seq::Pattern& p) {
        if (absoluteStep < 0 || absoluteStep >= seq::kMaxSteps) return;
        seq::Step& s = p.at(var, track, absoluteStep);
        s = fn(s);
    });
}

void SequencerStrip::stepKeyPressed(int index, const juce::MouseEvent& e) {
    painting_ = false;  // a press on a dim key must not leave the previous brush armed
    const int step = ctx_.state.page() * kStepsPerPage + index;
    const int track = ctx_.state.selectedTrack();
    if (step >= pattern().lengthOf(track)) return;
    const StepMode mode = ctx_.state.stepMode();
    const bool shift = e.mods.isShiftDown();
    const bool authentic = ctx_.state.authentic();
    seq::Step result{};
    editStep(step, [&](seq::Step s) {
        result = stepedit::clicked(s, mode, track, shift, authentic);
        return result;
    });
    painting_ = true;
    brush_ = result;
    paintTrack_ = track;
    lastPainted_ = step;
}

void SequencerStrip::keyDragged(int index) {
    if (!painting_) return;
    const int step = ctx_.state.page() * kStepsPerPage + index;
    if (step == lastPainted_) return;
    const int track = ctx_.state.selectedTrack();
    if (track != paintTrack_ || step >= pattern().lengthOf(track)) return;
    lastPainted_ = step;
    const StepMode mode = ctx_.state.stepMode();
    const seq::Step brush = brush_;
    editStep(step, [&](seq::Step s) { return stepedit::painted(s, brush, mode, track); });
}

// --- tools -----------------------------------------------------------------------------------------------------

void SequencerStrip::copy(bool trackOnly) {
    const auto& p = pattern();
    if (trackOnly) {
        const int track = ctx_.state.selectedTrack();
        trackClip_ = p.steps[static_cast<std::size_t>(editVariation())][static_cast<std::size_t>(track)];
        trackClipLength_ = track < kNumVoices ? p.trackLength[static_cast<std::size_t>(track)] : 0;
    } else {
        patternClip_ = p;
    }
}

void SequencerStrip::paste(bool trackOnly) {
    if (trackOnly) {
        if (!trackClip_) return;
        const int track = ctx_.state.selectedTrack();
        const int var = editVariation();
        const auto clip = *trackClip_;
        const auto len = trackClipLength_;
        editSelected([&](seq::Pattern& p) {
            p.steps[static_cast<std::size_t>(var)][static_cast<std::size_t>(track)] = clip;
            if (track < kNumVoices) p.trackLength[static_cast<std::size_t>(track)] = len;
        });
    } else if (patternClip_) {
        const seq::Pattern clip = *patternClip_;
        editSelected([&clip](seq::Pattern& p) { p = clip; });
    }
}

void SequencerStrip::clear(bool trackOnly) {
    const int track = ctx_.state.selectedTrack();
    const int var = editVariation();
    editSelected([&](seq::Pattern& p) {
        if (trackOnly) seq::clearTrack(p, var, track);
        else seq::clearPattern(p, var);
    });
}

void SequencerStrip::shift(int steps, bool trackOnly) {
    const int track = ctx_.state.selectedTrack();
    const int var = editVariation();
    editSelected([&](seq::Pattern& p) {
        if (trackOnly) seq::shiftTrack(p, var, track, steps);
        else seq::shiftPattern(p, var, steps);
    });
}

void SequencerStrip::randomise(std::uint32_t seed, bool trackOnly) {
    const int track = ctx_.state.selectedTrack();
    const int var = editVariation();
    const auto densities = stepedit::defaultDensities();
    editSelected([&](seq::Pattern& p) {
        if (trackOnly) seq::randomizeTrack(p, var, track, densities[static_cast<std::size_t>(track)] + 0.2f, seed);
        else seq::randomizePattern(p, var, densities, seed);
    });
    seed_->refresh();
}

void SequencerStrip::showEuclidMenu() {
    const int track = ctx_.state.selectedTrack();
    const int len = pattern().lengthOf(track);
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx_.lnf);
    menu.addSectionHeader("Euclidean " + voiceCode(track) + " over " + juce::String(len) + " steps");
    for (int hits = 1; hits <= juce::jmin(len, 32); ++hits) menu.addItem(hits, juce::String(hits) + (hits == 1 ? " hit" : " hits"));
    menu.addSeparator();
    menu.addItem(100, "Rotate earlier");
    menu.addItem(101, "Rotate later");
    menu.addItem(102, "Invert track");
    juce::Component::SafePointer<SequencerStrip> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&euclidButton_).withParentComponent(&ctx_.menuParent),
                       [safe, track](int result) {
                           if (safe == nullptr || result <= 0) return;
                           auto& self = *safe;
                           const int var = self.editVariation();
                           self.editSelected([&](seq::Pattern& p) {
                               if (result <= 64) seq::setEuclidean(p, var, track, result, 0);
                               else if (result == 100) seq::shiftTrack(p, var, track, -1);
                               else if (result == 101) seq::shiftTrack(p, var, track, 1);
                               else if (result == 102) seq::invertTrack(p, var, track);
                           });
                       });
}

// AUTHENTIC never rewrites existing data by itself (Context::editPattern
// constrains only what an edit changes). Turning it on over a pattern that
// holds more than the original could asks first, and offers to conform the
// pattern, with an undo kept until the pattern is edited again.
void SequencerStrip::setAuthentic(bool on) {
    if (on && !seq::followsHardwareRules(pattern())) {
        showAuthenticMenu(true);
        return;
    }
    if (!on && conformUndo_ && conformUndo_->pattern == selectedPattern() &&
        ctx_.processor.patterns().pattern(selectedPattern()) == conformUndo_->after) {
        showAuthenticMenu(false);
        return;
    }
    ctx_.state.setAuthentic(on);
    if (on) {
        const auto mode = ctx_.state.stepMode();
        if (mode == StepMode::Sub || mode == StepMode::Prob) ctx_.state.setStepMode(StepMode::Level);
    }
}

void SequencerStrip::conformSelected() {
    const int index = selectedPattern();
    ConformUndo undo;
    undo.pattern = index;
    undo.before = ctx_.processor.patterns().pattern(index);
    ctx_.processor.patterns().edit(index, [](seq::Pattern& p) { seq::applyHardwareRules(p); });
    undo.after = ctx_.processor.patterns().pattern(index);
    if (undo.after != undo.before) conformUndo_ = undo;
    refreshHint();
}

void SequencerStrip::showAuthenticMenu(bool turningOn) {
    enum { kKeep = 1, kConform, kOff, kRestore };
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx_.lnf);
    const bool conforms = seq::followsHardwareRules(pattern());
    menu.addSectionHeader(conforms ? juce::String("This pattern follows the original's rules")
                                   : "This pattern holds more than the original could");
    if (turningOn) {
        menu.addItem(kKeep, conforms ? juce::String("Turn AUTHENTIC on") : "Turn AUTHENTIC on for new edits (keep the pattern)");
        menu.addItem(kConform, "Turn on and conform this pattern (drops weak, sub-steps, probability...)", !conforms);
    } else {
        const bool canRestore = conformUndo_ && conformUndo_->pattern == selectedPattern() &&
                                ctx_.processor.patterns().pattern(selectedPattern()) == conformUndo_->after;
        menu.addItem(kConform, "Conform this pattern now (drops weak, sub-steps, probability...)", !conforms);
        menu.addSeparator();
        menu.addItem(kOff, canRestore ? "Turn AUTHENTIC off (keep the conformed pattern)" : "Turn AUTHENTIC off");
        if (canRestore) menu.addItem(kRestore, "Turn off and restore the pattern as it was before conforming");
    }
    juce::Component::SafePointer<SequencerStrip> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&authenticButton_).withParentComponent(&ctx_.menuParent),
                       [safe](int result) {
                           if (safe == nullptr || result <= 0) return;
                           auto& self = *safe;
                           switch (result) {
                               case kKeep:
                               case kConform:
                                   if (result == kConform) self.conformSelected();
                                   self.ctx_.state.setAuthentic(true);
                                   if (self.ctx_.state.stepMode() == StepMode::Sub || self.ctx_.state.stepMode() == StepMode::Prob)
                                       self.ctx_.state.setStepMode(StepMode::Level);
                                   break;
                               case kRestore:
                                   if (self.conformUndo_ && self.conformUndo_->pattern == self.selectedPattern()) {
                                       const seq::Pattern before = self.conformUndo_->before;
                                       self.ctx_.processor.patterns().edit(self.conformUndo_->pattern,
                                                                           [&before](seq::Pattern& p) { p = before; });
                                   }
                                   self.conformUndo_.reset();
                                   self.ctx_.state.setAuthentic(false);
                                   break;
                               case kOff: self.ctx_.state.setAuthentic(false); break;
                               default: break;
                           }
                       });
}

}  // namespace mw909::ui
