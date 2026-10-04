// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "UiCommon.h"

#include <cmath>
#include <optional>

namespace mw909::ui {

namespace C = mw909Colours;

namespace {
constexpr float kDragPixelsForFullRange = 200.0f;
constexpr float kFineFactor = 0.1f;
constexpr float kWheelStep = 1.0f / 40.0f;
}  // namespace

juce::String patternLabel(int patternIndex) {
    const int i = juce::jlimit(0, seq::kNumPatterns - 1, patternIndex);
    return juce::String(seq::bankOf(i) == 0 ? "I-" : "II-") + juce::String(seq::groupOf(i) + 1) + "-" +
           juce::String(seq::slotOf(i) + 1).paddedLeft('0', 2);
}

void layoutRow(juce::Rectangle<int> row, std::initializer_list<juce::Component*> items, int gap) {
    const int n = static_cast<int>(items.size());
    int i = 0;
    for (auto* c : items) {
        const int w = (row.getWidth() - gap * (n - 1 - i)) / (n - i);
        auto cell = row.removeFromLeft(w);
        row.removeFromLeft(gap);
        if (c != nullptr) c->setBounds(cell);
        ++i;
    }
}

void constrainEditToHardware(const seq::Pattern& before, seq::Pattern& p) noexcept {
    if (p == before) return;
    seq::Pattern ruled = p;
    seq::applyHardwareRules(ruled);
    auto field = [](auto& now, const auto& was, const auto& rule) {
        if (now != was) now = rule;
    };
    field(p.length, before.length, ruled.length);
    field(p.scale, before.scale, ruled.scale);
    field(p.shuffle, before.shuffle, ruled.shuffle);
    field(p.flamMs, before.flamMs, ruled.flamMs);
    field(p.flamGrace, before.flamGrace, ruled.flamGrace);
    field(p.subStart, before.subStart, ruled.subStart);
    field(p.subEnd, before.subEnd, ruled.subEnd);
    for (std::size_t v = 0; v < p.trackLength.size(); ++v) {
        field(p.trackLength[v], before.trackLength[v], ruled.trackLength[v]);
        field(p.trackShuffle[v], before.trackShuffle[v], ruled.trackShuffle[v]);
    }
    for (int var = 0; var < seq::kNumVariations; ++var)
        for (int t = 0; t < seq::kNumTracks; ++t)
            for (int s = 0; s < seq::kMaxSteps; ++s) {
                if (p.at(var, t, s) == before.at(var, t, s)) continue;
                p.at(var, t, s) = ruled.at(var, t, s);
                // A closed and an open hat never share a step on the original.
                if (t == CH || t == OH) {
                    const int other = t == CH ? OH : CH;
                    p.at(var, other, s) = ruled.at(var, other, s);
                }
            }
}

bool learnedBinding(plugin::MidiLearn& learn, const juce::String& parameterId) {
    const int cc = learn.controllerFor(parameterId);
    if (cc < 0) return false;
    for (const auto& [factoryCc, id] : plugin::MidiLearn::factoryMap())
        if (factoryCc == cc) return id != parameterId;
    return true;
}

// --- parameter menu ---------------------------------------------------------------------

void showParameterMenu(Context& ctx, juce::RangedAudioParameter& param, juce::Component& target,
                       std::function<void()> onReset) {
    auto& learn = ctx.processor.midiLearn();
    const juce::String id = param.getParameterID();
    const int cc = learn.controllerFor(id);
    const bool learning = learn.isLearning() && learn.learningParameter() == id;

    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx.lnf);
    menu.addSectionHeader(param.getName(64));
    menu.addItem(1, "Reset to default");
    menu.addSeparator();
    menu.addItem(2, learning ? "Cancel MIDI learn" : "MIDI learn", true, learning);
    menu.addItem(3, cc >= 0 ? "Forget CC " + juce::String(cc) : "No controller bound", cc >= 0);

    juce::Component::SafePointer<juce::Component> safeTarget(&target);
    mw909Processor* processor = &ctx.processor;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target).withParentComponent(&ctx.menuParent),
                       [safeTarget, processor, id, onReset](int result) {
                           if (safeTarget == nullptr) return;
                           auto& ml = processor->midiLearn();
                           switch (result) {
                               case 1:
                                   if (onReset) onReset();
                                   break;
                               case 2:
                                   if (ml.isLearning() && ml.learningParameter() == id) ml.cancelLearn();
                                   else ml.beginLearn(id);
                                   break;
                               case 3: ml.forget(id); break;
                               default: break;
                           }
                       });
}

// --- CachedBackground ---------------------------------------------------------------------

void CachedBackground::paint(juce::Graphics& g) {
    const float physical = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0) return;
    if (cache_.isNull() || cacheW_ != w || cacheH_ != h || std::abs(cacheScale_ - physical) > 1.0e-3f) {
        const int pw = juce::jmax(1, juce::roundToInt(static_cast<float>(w) * physical));
        const int ph = juce::jmax(1, juce::roundToInt(static_cast<float>(h) * physical));
        cache_ = juce::Image(juce::Image::ARGB, pw, ph, true);
        juce::Graphics ig(cache_);
        ig.addTransform(juce::AffineTransform::scale(static_cast<float>(pw) / static_cast<float>(w),
                                                     static_cast<float>(ph) / static_cast<float>(h)));
        paintStatic(ig);
        cacheW_ = w;
        cacheH_ = h;
        cacheScale_ = physical;
    }
    g.drawImage(cache_, getLocalBounds().toFloat());
    paintDynamic(g);
}

// --- ParamControl ----------------------------------------------------------------------------

ParamControl::ParamControl(Context& ctx, juce::RangedAudioParameter& param, Style style, const juce::String& legend,
                           bool hardware)
    : ctx_(ctx), param_(param), style_(style), legend_(legend), hardware_(hardware) {
    valueLabel_.setJustificationType(juce::Justification::centred);
    valueLabel_.setFont(mw909LookAndFeel::readout(9.5f));
    valueLabel_.setColour(juce::Label::textColourId, C::inkDim);
    valueLabel_.setEditable(false, true, false);
    valueLabel_.setBorderSize({0, 0, 0, 0});
    valueLabel_.setInterceptsMouseClicks(true, false);
    valueLabel_.addListener(this);
    valueLabel_.setMinimumHorizontalScale(0.6f);
    addAndMakeVisible(valueLabel_);
    attachment_ = std::make_unique<juce::ParameterAttachment>(param_, [this](float v) { parameterChanged(v); });
    attachment_->sendInitialUpdate();
    setRepaintsOnMouseActivity(false);
    ctx_.controls.add(this);
    // Controls made after the editor opened (an expanded strip, another
    // effect) start with the right MIDI-learn mark.
    auto& learn = ctx_.processor.midiLearn();
    bound_ = learnedBinding(learn, param_.getParameterID());
    learning_ = learn.isLearning() && learn.learningParameter() == param_.getParameterID();
}

ParamControl::~ParamControl() {
    ctx_.controls.removeFirstMatchingValue(this);
    // Closed mid-drag (host closes the window): do not leave the host's gesture open.
    if (dragging_ && attachment_ != nullptr) attachment_->endGesture();
    attachment_.reset();
    valueLabel_.removeListener(this);
}

void ParamControl::setShowValue(bool show) {
    showValue_ = show;
    valueLabel_.setVisible(show);
    resized();
}

void ParamControl::setLearnState(bool learning, bool bound) {
    if (learning == learning_ && bound == bound_) return;
    learning_ = learning;
    bound_ = bound;
    repaint(controlArea_);
}

void ParamControl::parameterChanged(float denormalised) {
    const float norm = param_.convertTo0to1(denormalised);
    if (std::abs(norm - value_) < 1.0e-6f && valueLabel_.getText().isNotEmpty()) return;
    value_ = norm;
    refreshValueText();
    repaint(controlArea_);
}

juce::String ParamControl::valueText(float norm) const {
    if (formatValue) return formatValue(param_.convertFrom0to1(norm));
    juce::String t = param_.getText(norm, 10);
    const juce::String label = param_.getLabel();
    // ":1" (ratios) hugs the number; other units are spaced.
    if (label.isNotEmpty() && !t.endsWith(label)) t += (label.startsWithChar(':') ? "" : " ") + label;
    return t;
}

void ParamControl::refreshValueText() {
    if (!showValue_) return;
    valueLabel_.setText(valueText(value_), juce::dontSendNotification);
}

void ParamControl::setValueBeside(bool beside) {
    valueBeside_ = beside;
    valueLabel_.setJustificationType(beside ? juce::Justification::centredLeft : juce::Justification::centred);
    resized();
}

void ParamControl::resized() {
    auto r = getLocalBounds();
    const int legendH = legend_.isNotEmpty() ? juce::roundToInt(legendHeight_ + 2.0f) : 0;
    legendArea_ = r.removeFromTop(legendH);
    if (valueBeside_ && showValue_) {
        // Compact rows (tables): the knob on the left, its value on the right,
        // both on the row's centre line.
        const int side = juce::jmin(r.getHeight(), r.getWidth() / 2, 32);
        r = r.withSizeKeepingCentre(juce::jmin(r.getWidth(), side + 4 + 58), r.getHeight());  // the pair sits centred in its cell
        controlArea_ = r.removeFromLeft(side).withSizeKeepingCentre(side, side);
        r.removeFromLeft(4);
        valueArea_ = r;
        valueLabel_.setBounds(valueArea_.withSizeKeepingCentre(valueArea_.getWidth(), 12));
        valueLabel_.setFont(mw909LookAndFeel::readout(9.5f));
        return;
    }
    const int valueH = showValue_ ? 11 : 0;
    valueArea_ = r.removeFromBottom(valueH);
    controlArea_ = r;
    valueLabel_.setBounds(valueArea_.expanded(4, 0));
    valueLabel_.setFont(mw909LookAndFeel::readout(juce::jlimit(8.0f, 10.0f, static_cast<float>(valueH) - 1.0f)));
}

void ParamControl::paint(juce::Graphics& g) {
    if (legend_.isNotEmpty())
        mw909LookAndFeel::drawLegend(g, legend_, legendArea_, hardware_ ? C::ink : C::inkDim, legendHeight_);
    mw909LookAndFeel::KnobStyle st;
    st.hardware = hardware_;
    st.bipolar = bipolar_;
    st.hover = hover_ || dragging_;
    st.enabled = isEnabled();
    st.learning = learning_;
    st.bound = bound_;
    if (style_ == Style::Knob) mw909LookAndFeel::drawKnob(g, controlArea_.toFloat(), value_, st);
    else mw909LookAndFeel::drawFader(g, controlArea_.toFloat(), value_, st);
}

void ParamControl::setNormalisedFromGesture(float norm) {
    norm = juce::jlimit(0.0f, 1.0f, norm);
    attachment_->setValueAsPartOfGesture(param_.convertFrom0to1(norm));
}

void ParamControl::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        auto* attachment = attachment_.get();
        auto& param = param_;
        showParameterMenu(ctx_, param_, *this, [attachment, &param] {
            attachment->setValueAsCompleteGesture(param.convertFrom0to1(param.getDefaultValue()));
        });
        return;
    }
    if (!controlArea_.contains(e.getPosition())) return;
    dragging_ = true;
    dragStart_ = e.getPosition();
    dragStartValue_ = value_;
    attachment_->beginGesture();
    repaint(controlArea_);
}

void ParamControl::mouseDrag(const juce::MouseEvent& e) {
    if (!dragging_) return;
    const float dy = static_cast<float>(dragStart_.y - e.getPosition().y);
    const float dx = static_cast<float>(e.getPosition().x - dragStart_.x);
    // Vertical drag; a horizontal component helps too, as users expect of knobs.
    const float pixels = style_ == Style::Fader ? dy : dy + dx * 0.5f;
    const float fine = e.mods.isShiftDown() ? kFineFactor : 1.0f;
    float range = kDragPixelsForFullRange;
    if (style_ == Style::Fader) range = juce::jmax(60.0f, static_cast<float>(controlArea_.getHeight()) * 0.9f);
    setNormalisedFromGesture(dragStartValue_ + pixels / range * fine);
}

void ParamControl::mouseUp(const juce::MouseEvent&) {
    if (!dragging_) return;
    dragging_ = false;
    attachment_->endGesture();
    repaint(controlArea_);
}

void ParamControl::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    if (!controlArea_.contains(e.getPosition())) return;
    attachment_->setValueAsCompleteGesture(param_.convertFrom0to1(param_.getDefaultValue()));
}

void ParamControl::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    const float delta = (!juce::approximatelyEqual(wheel.deltaY, 0.0f) ? wheel.deltaY : -wheel.deltaX);
    if (juce::approximatelyEqual(delta, 0.0f)) return;
    const float fine = e.mods.isShiftDown() ? kFineFactor : 1.0f;
    const float step = (wheel.isReversed ? -1.0f : 1.0f) * (delta > 0.0f ? 1.0f : -1.0f) * kWheelStep * fine;
    // Choice parameters step one choice per notch.
    const int steps = param_.getNumSteps();
    float norm;
    if (steps > 1 && steps <= 64) {
        const float stepSize = 1.0f / static_cast<float>(steps - 1);
        norm = value_ + (step > 0.0f ? stepSize : -stepSize);
    } else {
        norm = value_ + step;
    }
    attachment_->setValueAsCompleteGesture(param_.convertFrom0to1(juce::jlimit(0.0f, 1.0f, norm)));
}

void ParamControl::mouseEnter(const juce::MouseEvent&) {
    hover_ = true;
    repaint(controlArea_);
}

void ParamControl::mouseExit(const juce::MouseEvent&) {
    hover_ = false;
    repaint(controlArea_);
}

std::optional<float> ParamControl::parseText(const juce::String& typed) const {
    const juce::String text = typed.trim();
    if (text.isEmpty()) return std::nullopt;
    if (parseValue) return parseValue(text);
    // A choice: one of its names (any case).
    if (auto* choice = dynamic_cast<const juce::AudioParameterChoice*>(&param_)) {
        for (int i = 0; i < choice->choices.size(); ++i)
            if (choice->choices[i].equalsIgnoreCase(text)) return param_.getNormalisableRange().start + static_cast<float>(i);
        return std::nullopt;
    }
    juce::String number = text;
    const juce::String unit = param_.getLabel();
    if (unit.isNotEmpty() && number.endsWithIgnoreCase(unit)) number = number.dropLastCharacters(unit.length()).trim();
    // A number must lead ("-3", "+.5", "250 ms"); anything else is either a
    // word the parameter itself prints (e.g. "Off") or a typo, which must not
    // silently set the control to its minimum.
    const auto leadsWithNumber = [](const juce::String& s) {
        auto t = s.getCharPointer();
        if (*t == '+' || *t == '-') ++t;
        if (*t == '.') ++t;
        return juce::CharacterFunctions::isDigit(*t);
    };
    if (!leadsWithNumber(number)) {
        const float norm = param_.getValueForText(number);
        if (!param_.getText(norm, 64).trim().equalsIgnoreCase(number)) return std::nullopt;
        return param_.convertFrom0to1(norm);
    }
    return param_.convertFrom0to1(juce::jlimit(0.0f, 1.0f, param_.getValueForText(number)));
}

void ParamControl::labelTextChanged(juce::Label* label) {
    if (const auto value = parseText(label->getText())) {
        const auto& range = param_.getNormalisableRange();
        attachment_->setValueAsCompleteGesture(juce::jlimit(range.start, range.end, *value));
    }
    refreshValueText();  // unparsable text: the readout goes back to the value, nothing changes
}

void ParamControl::editorShown(juce::Label*, juce::TextEditor& editor) {
    editor.setJustification(juce::Justification::centred);
    editor.setFont(valueLabel_.getFont());
    editor.setInputRestrictions(12, "0123456789.-+ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz/% ");
    editor.selectAll();
}

void ParamControl::editorHidden(juce::Label*, juce::TextEditor&) { refreshValueText(); }

// --- ParamButton -------------------------------------------------------------------------------

ParamButton::ParamButton(Context& ctx, juce::RangedAudioParameter& param, const juce::String& label, juce::Colour accent,
                         bool lamp)
    : juce::TextButton(label), ctx_(ctx), param_(param) {
    setClickingTogglesState(true);
    styleButton(*this, accent, lamp);
    attachment_ = std::make_unique<juce::ButtonParameterAttachment>(param_, *this);
    attachment_->sendInitialUpdate();
}

ParamButton::~ParamButton() { attachment_.reset(); }

void ParamButton::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        auto& param = param_;
        showParameterMenu(ctx_, param_, *this, [&param] {
            param.beginChangeGesture();
            param.setValueNotifyingHost(param.getDefaultValue());
            param.endChangeGesture();
        });
        return;
    }
    juce::TextButton::mouseDown(e);
}

// --- ChoiceButtons --------------------------------------------------------------------------------

namespace {
// A radio button inside ChoiceButtons: right-click opens the parameter's menu.
class ChoiceButton final : public juce::TextButton {
public:
    ChoiceButton(const juce::String& label, std::function<void(juce::Component&)> menu) : juce::TextButton(label), menu_(std::move(menu)) {}
    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) {
            if (menu_) menu_(*this);
            return;
        }
        juce::TextButton::mouseDown(e);
    }

private:
    std::function<void(juce::Component&)> menu_;
};
}  // namespace

ChoiceButtons::ChoiceButtons(Context& ctx, juce::RangedAudioParameter& param, const juce::StringArray& labels,
                             bool vertical, juce::Colour accent)
    : ctx_(ctx), param_(param), vertical_(vertical) {
    for (int i = 0; i < labels.size(); ++i) {
        auto* b = buttons_.add(new ChoiceButton(labels[i], [this](juce::Component& target) {
            auto* attachment = attachment_.get();
            auto& p = param_;
            showParameterMenu(ctx_, param_, target, [attachment, &p] {
                attachment->setValueAsCompleteGesture(p.convertFrom0to1(p.getDefaultValue()));
            });
        }));
        styleButton(*b, accent);
        b->setClickingTogglesState(false);
        b->setConnectedEdges(0);
        b->onClick = [this, i] { select(i); };
        addAndMakeVisible(b);
    }
    attachment_ = std::make_unique<juce::ParameterAttachment>(param_, [this](float v) { parameterChanged(v); });
    attachment_->sendInitialUpdate();
}

ChoiceButtons::~ChoiceButtons() { attachment_.reset(); }

void ChoiceButtons::setFontHeight(float h) {
    for (auto* b : buttons_) b->getProperties().set("fontHeight", static_cast<double>(h));
}

void ChoiceButtons::setLamp(bool lamp) {
    for (auto* b : buttons_) b->getProperties().set("lamp", lamp);
}

void ChoiceButtons::resized() {
    const int n = buttons_.size();
    if (n == 0) return;
    auto r = getLocalBounds();
    for (int i = 0; i < n; ++i) {
        juce::Rectangle<int> cell;
        if (vertical_) {
            const int h = (r.getHeight() - gap_ * (n - 1 - i)) / (n - i);
            cell = r.removeFromTop(h);
            r.removeFromTop(gap_);
        } else {
            const int w = (r.getWidth() - gap_ * (n - 1 - i)) / (n - i);
            cell = r.removeFromLeft(w);
            r.removeFromLeft(gap_);
        }
        buttons_[i]->setBounds(cell);
    }
}

void ChoiceButtons::select(int index) {
    const auto& range = param_.getNormalisableRange();
    const float value = range.start + static_cast<float>(index);
    attachment_->setValueAsCompleteGesture(juce::jlimit(range.start, range.end, value));
}

void ChoiceButtons::parameterChanged(float denormalised) {
    const int index = juce::roundToInt(denormalised - param_.getNormalisableRange().start);
    selected_ = index;
    for (int i = 0; i < buttons_.size(); ++i) buttons_[i]->setToggleState(i == index, juce::dontSendNotification);
}

// --- ValueBox ------------------------------------------------------------------------------------------

ValueBox::ValueBox(const juce::String& legend, int minValue, int maxValue, int defaultValue)
    : legend_(legend), min_(minValue), max_(maxValue), default_(defaultValue) {
    setRepaintsOnMouseActivity(false);
}

void ValueBox::refresh() {
    if (!getValue) return;
    const int v = getValue();
    if (v != shown_) {
        shown_ = v;
        repaint();
    }
}

void ValueBox::paint(juce::Graphics& g) {
    auto r = getLocalBounds();
    if (getValue) shown_ = getValue();
    const float alpha = isEnabled() ? 1.0f : 0.45f;
    if (legend_.isNotEmpty()) {
        mw909LookAndFeel::drawLegend(g, legend_, r.removeFromTop(legendHeight_), C::inkDim.withMultipliedAlpha(alpha),
                                     static_cast<float>(legendHeight_) - 1.5f);
        r.removeFromTop(1);
    }
    mw909LookAndFeel::drawInset(g, r.toFloat().withTrimmedBottom(1.0f));
    if (hover_ && isEnabled()) {
        g.setColour(C::hoverOnDark);
        g.fillRoundedRectangle(r.toFloat().withTrimmedBottom(1.0f), 2.5f);
    }
    g.setColour((isEnabled() ? C::cream : C::creamDim));
    g.setFont(mw909LookAndFeel::legible(g, mw909LookAndFeel::readout(juce::jlimit(8.5f, 12.5f, static_cast<float>(r.getHeight()) * 0.58f)),
                                        static_cast<float>(r.getHeight()) - 2.0f));
    const juce::String text = format ? format(shown_) : juce::String(shown_);
    // Narrow boxes (a collapsed mixer channel beside an expanded strip) give
    // the caret's room to the text.
    const bool compact = r.getWidth() < 46;
    auto textArea = r.reduced(compact ? 2 : 4, 0).withTrimmedBottom(1);
    if (choices.size() > 0 && !compact) {
        // A small caret marks the boxes that open a list; the text keeps clear of it.
        auto caret = textArea.removeFromRight(7).toFloat();
        textArea.removeFromRight(3);
        juce::Path p;
        p.addTriangle(caret.getCentreX() - 3.0f, caret.getCentreY() - 1.5f, caret.getCentreX() + 3.0f, caret.getCentreY() - 1.5f,
                      caret.getCentreX(), caret.getCentreY() + 2.0f);
        g.setColour(C::legend.withMultipliedAlpha(alpha));
        g.fillPath(p);
        g.setColour((isEnabled() ? C::cream : C::creamDim));
    }
    g.drawFittedText(text, textArea, juce::Justification::centred, 1, compact ? 0.6f : 0.7f);
}

void ValueBox::apply(int v) {
    v = juce::jlimit(min_, max_, v);
    if (setValue && v != shown_) setValue(v);
    refresh();
}

void ValueBox::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        if (onRightClick) onRightClick(*this);
        return;
    }
    dragged_ = false;
    dragStart_ = e.getPosition();
    dragStartValue_ = getValue ? getValue() : shown_;
}

void ValueBox::mouseDrag(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    const int dy = dragStart_.y - e.getPosition().y;
    if (std::abs(dy) > 2) dragged_ = true;
    if (!dragged_) return;
    const int span = max_ - min_;
    const float perPixel = e.mods.isShiftDown() ? 0.05f : juce::jmax(0.1f, static_cast<float>(span) / 150.0f);
    apply(dragStartValue_ + juce::roundToInt(static_cast<float>(dy) * perPixel));
}

void ValueBox::mouseUp(const juce::MouseEvent& e) {
    if (dragged_ || e.mods.isPopupMenu() || e.mouseWasDraggedSinceMouseDown()) return;
    if (choices.isEmpty() || !setValue) return;
    juce::PopupMenu menu;
    if (auto* lnf = dynamic_cast<mw909LookAndFeel*>(&getLookAndFeel())) menu.setLookAndFeel(lnf);
    const int current = getValue ? getValue() : shown_;
    for (int i = 0; i < choices.size(); ++i) menu.addItem(i + 1, choices[i], true, i + min_ == current);
    juce::Component::SafePointer<ValueBox> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withParentComponent(menuParent),
                       [safe](int result) {
                           if (safe == nullptr || result <= 0) return;
                           safe->apply(safe->min_ + result - 1);
                       });
}

void ValueBox::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    apply(default_);
}

void ValueBox::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) {
    const float delta = !juce::approximatelyEqual(wheel.deltaY, 0.0f) ? wheel.deltaY : -wheel.deltaX;
    if (juce::approximatelyEqual(delta, 0.0f)) return;
    wheelAccum_ += (wheel.isReversed ? -delta : delta);
    const int steps = static_cast<int>(wheelAccum_ / 0.08f);
    if (steps == 0) return;
    wheelAccum_ -= static_cast<float>(steps) * 0.08f;
    apply((getValue ? getValue() : shown_) + steps);
}

// --- ParamValueBox -----------------------------------------------------------------------------------------

ParamValueBox::ParamValueBox(Context& ctx, juce::RangedAudioParameter& param, const juce::String& legend, bool asChoice)
    : ValueBox(legend, juce::roundToInt(param.getNormalisableRange().start), juce::roundToInt(param.getNormalisableRange().end),
               juce::roundToInt(param.convertFrom0to1(param.getDefaultValue()))),
      ctx_(ctx), param_(param) {
    menuParent = &ctx.menuParent;
    if (asChoice) {
        const auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(&param_);
        if (choiceParam != nullptr) choices = choiceParam->choices;
    }
    getValue = [this] { return juce::roundToInt(param_.convertFrom0to1(param_.getValue())); };
    format = [this](int v) {
        if (choices.isEmpty()) return param_.getText(param_.convertTo0to1(static_cast<float>(v)), 8);
        const int i = v - static_cast<int>(param_.getNormalisableRange().start);
        return i >= 0 && i < choices.size() ? choices[i] : juce::String(v);
    };
    setValue = [this](int v) { attachment_->setValueAsCompleteGesture(static_cast<float>(v)); };
    onRightClick = [this](juce::Component& c) {
        auto* attachment = attachment_.get();
        auto& p = param_;
        showParameterMenu(ctx_, param_, c, [attachment, &p] { attachment->setValueAsCompleteGesture(p.convertFrom0to1(p.getDefaultValue())); });
    };
    attachment_ = std::make_unique<juce::ParameterAttachment>(param_, [this](float) { refresh(); });
    attachment_->sendInitialUpdate();
}

ParamValueBox::~ParamValueBox() { attachment_.reset(); }

}  // namespace mw909::ui
