// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "HeaderBar.h"

#include <cmath>
#include <vector>

#ifndef MW909_VERSION_STRING
 #define MW909_VERSION_STRING "dev"
#endif

namespace mw909::ui {

namespace C = mw909Colours;
namespace paramid = plugin::paramid;
using plugin::PresetKind;

namespace {

const char* const kKindNames[plugin::kNumPresetKinds] = {"KIT", "PATTERNS", "FULL"};

// Menu ids of the settings menu.
enum SettingsId {
    kQualityBase = 100,        // + 0..2
    kVarianceBase = 200,       // + 0..4 (0, 25, 50, 75, 100 %)
    kSerialRandom = 300,
    kSerialEnter = 301,
    kRevisionBase = 350,       // + 0..1
    kScaleBase = 400,          // + index into kScales
    kVelocitySens = 500,
    kMidiOut = 501,
    kJitter = 502,
    kMidiChannelBase = 600,    // + 0..16 (Omni, 1..16)
    kBassChannelBase = 650,    // + 0..16 (Off, 1..16)
    kMidiOutChannelBase = 700, // + 0..15
    kClearMidiLearn = 800,
    kFactoryCcMap = 801,
    kPanic = 802,
    kAbout = 803,
};

constexpr float kScales[] = {0.6f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
constexpr int kNumScales = static_cast<int>(sizeof(kScales) / sizeof(kScales[0]));

}  // namespace

// --- OutputMeter -------------------------------------------------------------------------------

OutputMeter::OutputMeter() { setInterceptsMouseClicks(false, false); }

float OutputMeter::toDisplay(float linear) noexcept {
    if (linear <= 1.0e-5f) return 0.0f;
    const float db = 20.0f * std::log10(linear);
    return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
}

void OutputMeter::setPeaks(float left, float right) {
    const float l = toDisplay(left), r = toDisplay(right);
    // Fast attack, ~1.5 s fall from full scale at 30 Hz.
    const float fall = 0.022f;
    const float nl = l > shownL_ ? l : juce::jmax(l, shownL_ - fall);
    const float nr = r > shownR_ ? r : juce::jmax(r, shownR_ - fall);
    bool changed = std::abs(nl - shownL_) > 0.002f || std::abs(nr - shownR_) > 0.002f;
    shownL_ = nl;
    shownR_ = nr;
    // Peak hold for ~1 s. Only a moving hold marker counts as a change: at
    // silence (0 >= 0) the meter must not repaint.
    auto hold = [&](float v, float& held, int& age) {
        if (v >= held) {
            if (v > held) changed = true;
            held = v;
            age = 0;
        } else if (++age > 30) {
            held = juce::jmax(v, held - fall * 2.0f);
            changed = true;
        }
    };
    hold(nl, holdL_, holdAgeL_);
    hold(nr, holdR_, holdAgeR_);
    const bool cl = left >= 0.999f, cr = right >= 0.999f;
    if (cl != clipL_ || cr != clipR_) { clipL_ = cl; clipR_ = cr; changed = true; }
    if (changed) repaint();
}

void OutputMeter::paint(juce::Graphics& g) {
    auto r = getLocalBounds();
    const auto legendArea = r.removeFromLeft(26);
    mw909LookAndFeel::drawLegend(g, "OUT", legendArea, C::creamDim, 9.0f);
    mw909LookAndFeel::drawInset(g, r.toFloat());
    auto inner = r.reduced(3, 4);
    const int barH = (inner.getHeight() - 2) / 2;
    auto drawBar = [&](juce::Rectangle<int> bar, float v, float held, bool clip) {
        const float w = static_cast<float>(bar.getWidth());
        const float zero = (60.0f / 66.0f);  // 0 dBFS position
        // Segments: green to -12 dB, amber to 0 dB, red above.
        const float x12 = w * (48.0f / 66.0f);
        const float x0 = w * zero;
        const float fill = w * v;
        auto seg = [&](float from, float to, juce::Colour c) {
            const float a = juce::jlimit(0.0f, fill, from), b = juce::jlimit(0.0f, fill, to);
            if (b > a) {
                g.setColour(c);
                g.fillRect(static_cast<float>(bar.getX()) + a, static_cast<float>(bar.getY()), b - a, static_cast<float>(bar.getHeight()));
            }
        };
        seg(0.0f, x12, C::meterLow);
        seg(x12, x0, C::meterMid);
        seg(x0, w, C::meterHot);
        if (held > 0.01f) {
            g.setColour(held > zero ? C::meterHot : C::cream);
            g.fillRect(static_cast<float>(bar.getX()) + w * held - 1.0f, static_cast<float>(bar.getY()), 2.0f,
                       static_cast<float>(bar.getHeight()));
        }
        if (clip) {
            g.setColour(C::meterHot);
            g.fillRect(bar.removeFromRight(3));
        }
        g.setColour(C::creamDim.withAlpha(0.6f));
        g.fillRect(static_cast<float>(bar.getX()) + x0, static_cast<float>(bar.getY()), 1.0f, static_cast<float>(bar.getHeight()));
    };
    drawBar(inner.removeFromTop(barH), shownL_, holdL_, clipL_);
    inner.removeFromTop(2);
    drawBar(inner.removeFromTop(barH), shownR_, holdR_, clipR_);
}

// --- TempoReadout --------------------------------------------------------------------------------

void TempoReadout::set(float bpm, bool playing, const juce::String& source, const juce::String& position) {
    const bool bpmChanged = std::abs(bpm - bpm_) >= 0.05f;
    if (!bpmChanged && playing == playing_ && source == source_ && position == position_) return;
    bpm_ = bpm;
    playing_ = playing;
    source_ = source;
    position_ = position;
    repaint();
}

void TempoReadout::paint(juce::Graphics& g) {
    auto r = getLocalBounds();
    mw909LookAndFeel::drawInset(g, r.toFloat());
    auto inner = r.reduced(7, 3);
    auto right = inner.removeFromRight(juce::jmin(70, inner.getWidth() / 2));
    g.setColour(playing_ ? C::legend : C::cream);
    g.setFont(mw909LookAndFeel::readout(19.0f));
    g.drawFittedText(juce::String(bpm_, 1), inner, juce::Justification::centredLeft, 1, 0.8f);
    g.setFont(mw909LookAndFeel::legend(9.0f));
    g.setColour(C::creamDim);
    g.drawFittedText("BPM  " + source_, right.removeFromTop(right.getHeight() / 2), juce::Justification::centredRight, 1, 0.8f);
    g.setColour(playing_ ? C::cream : C::creamDim);
    g.drawFittedText(position_, right, juce::Justification::centredRight, 1, 0.8f);
}

void TempoReadout::mouseDown(const juce::MouseEvent& e) {
    if (onClick) onClick(*this);
    juce::ignoreUnused(e);
}

// --- HeaderBar ----------------------------------------------------------------------------------------

HeaderBar::HeaderBar(Context& ctx) : ctx_(ctx) {
    setOpaque(true);
    for (int i = 0; i < plugin::kNumPresetKinds; ++i) {
        auto* b = kindButtons_.add(new juce::TextButton(kKindNames[i]));
        styleButton(*b, C::orange, false, 9.0f);
        b->onClick = [this, i] { setKind(static_cast<PresetKind>(i)); };
        addAndMakeVisible(b);
    }
    styleButton(prevButton_, C::orange, false, 11.0f);
    styleButton(nextButton_, C::orange, false, 11.0f);
    styleButton(nameButton_, C::orange, false, 11.0f);
    styleButton(saveButton_, C::orange, false, 9.0f);
    styleButton(presetMenuButton_, C::orange, false, 11.0f);
    prevButton_.onClick = [this] { ctx_.processor.presets().loadPrevious(kind_); };
    nextButton_.onClick = [this] { ctx_.processor.presets().loadNext(kind_); };
    nameButton_.onClick = [this] { showPresetList(); };
    saveButton_.onClick = [this] { askSavePreset(); };
    presetMenuButton_.onClick = [this] { showPresetMenu(); };
    for (auto* b : {&prevButton_, &nextButton_, &nameButton_, &saveButton_, &presetMenuButton_}) addAndMakeVisible(b);
    prevButton_.setTooltip("Previous preset");
    nextButton_.setTooltip("Next preset");
    nameButton_.setTooltip("Choose a preset");
    saveButton_.setTooltip("Save as user preset");
    presetMenuButton_.setTooltip("Preset and MIDI file actions");

    for (int i = 0; i < kNumViews; ++i) {
        auto* b = viewButtons_.add(new juce::TextButton(kViewNames[i]));
        styleButton(*b, C::orange, false, 10.0f);
        b->onClick = [this, i] { ctx_.state.setView(static_cast<View>(i)); };
        addAndMakeVisible(b);
    }

    tempo_.onClick = [this](juce::Component& target) {
        if (auto* p = ctx_.param(paramid::tempo))
            showParameterMenu(ctx_, *p, target, [p] {
                p->beginChangeGesture();
                p->setValueNotifyingHost(p->getDefaultValue());
                p->endChangeGesture();
            });
    };
    tempo_.setTooltip("Effective tempo and position. The TEMPO knob sets the internal clock.");
    addAndMakeVisible(tempo_);
    addAndMakeVisible(meter_);

    settingsButton_.setButtonText("SETUP");
    styleButton(settingsButton_, C::orange, false, 9.0f);
    settingsButton_.setTooltip("Settings: quality, variance, serial number, revision, UI scale, MIDI");
    settingsButton_.onClick = [this] { showSettingsMenu(); };
    addAndMakeVisible(settingsButton_);

    ctx_.processor.presets().addListener(this);
    ctx_.processor.patterns().addListener(this);
    ctx_.state.addListener(this);
    setKind(PresetKind::Full);
    uiStateChanged();
}

HeaderBar::~HeaderBar() {
    ctx_.state.removeListener(this);
    ctx_.processor.patterns().removeListener(this);
    ctx_.processor.presets().removeListener(this);
}

void HeaderBar::resized() {
    auto r = getLocalBounds().reduced(12, 9);
    r.removeFromBottom(2);  // the band's foot rule
    wordmarkArea_ = r.removeFromLeft(150);
    r.removeFromLeft(12);

    // Preset browser.
    presetArea_ = r.removeFromLeft(440);
    auto p = presetArea_;
    const int kindW[] = {36, 66, 42};
    for (int i = 0; i < kindButtons_.size(); ++i) {
        kindButtons_[i]->setBounds(p.removeFromLeft(kindW[i]));
        p.removeFromLeft(2);
    }
    p.removeFromLeft(6);
    prevButton_.setBounds(p.removeFromLeft(22));
    p.removeFromLeft(2);
    presetMenuButton_.setBounds(p.removeFromRight(26));
    p.removeFromRight(2);
    saveButton_.setBounds(p.removeFromRight(42));
    p.removeFromRight(4);
    nextButton_.setBounds(p.removeFromRight(22));
    p.removeFromRight(2);
    nameButton_.setBounds(p);
    r.removeFromLeft(14);

    // View switch.
    viewArea_ = r.removeFromLeft(4 * 58 + 3 * 3);
    auto v = viewArea_;
    for (auto* b : viewButtons_) {
        b->setBounds(v.removeFromLeft(58));
        v.removeFromLeft(3);
    }
    r.removeFromLeft(14);

    settingsButton_.setBounds(r.removeFromRight(50));
    r.removeFromRight(10);
    meter_.setBounds(r.removeFromRight(160));
    r.removeFromRight(10);
    tempo_.setBounds(r);
}

void HeaderBar::paintStatic(juce::Graphics& g) {
    g.fillAll(C::band);
    g.setColour(C::orangeDeep);
    g.fillRect(0, getHeight() - 2, getWidth(), 2);
    // Wordmark: lowercase "mw" in cream and "909" in orange, the mwAudio mark beneath.
    auto w = wordmarkArea_;
    g.setFont(mw909LookAndFeel::bold(24.0f).withExtraKerningFactor(-0.02f));
    const juce::String mw = "mw", num = "909";
    const int mwW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), mw);
    const int numW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), num);
    const auto line = w.removeFromTop(21).translated(0, -4);
    g.setColour(C::cream);
    g.drawText(mw, line, juce::Justification::centredLeft, false);
    g.setColour(C::orange);
    g.drawText(num, line.withTrimmedLeft(mwW), juce::Justification::centredLeft, false);
    // A short orange bar under "909" ties the mark to the panel's foot rules;
    // the maker's mark sits clear of it, a few pixels lower.
    const int barY = line.getBottom() + 3;
    g.setColour(C::orange);
    g.fillRect(w.getX() + mwW + 1, barY, numW - 2, 2);
    g.setColour(C::creamDim);
    g.setFont(mw909LookAndFeel::legible(g, mw909LookAndFeel::legend(8.0f), 9.0f).withExtraKerningFactor(0.12f));
    g.drawText("MWAUDIO", juce::Rectangle<int>(w.getX(), barY + 5, w.getWidth(), 9), juce::Justification::centredLeft, false);
    g.setColour(C::bandEdge);
    for (int x : {presetArea_.getX() - 7, viewArea_.getX() - 8, viewArea_.getRight() + 7})
        g.drawVerticalLine(x, 10.0f, static_cast<float>(getHeight() - 10));
}

void HeaderBar::poll(const PlayState& ps) {
    meter_.setPeaks(ps.peakLeft.load(std::memory_order_relaxed), ps.peakRight.load(std::memory_order_relaxed));
    const bool playing = ps.playing.load(std::memory_order_relaxed);
    const int mode = ctx_.choice(paramid::seqMode);
    const juce::String source = mode == 0 ? "OFF" : mode == 1 ? "HOST" : mode == 2 ? "INT" : "NOTE";
    juce::String position;
    if (playing) {
        const auto pos = ps.position.load(std::memory_order_relaxed);
        const int step = static_cast<int>(pos & 0xffU) - 1;
        const auto measure = static_cast<std::int64_t>(pos >> 8);
        position = patternLabel(ps.currentPattern.load(std::memory_order_relaxed)) +
                   (ps.variation.load(std::memory_order_relaxed) == 0 ? "A " : "B ") + juce::String(measure % 1000 + 1) + "." +
                   juce::String(juce::jmax(0, step) + 1).paddedLeft('0', 2);
    } else {
        position = patternLabel(ctx_.processor.patterns().selectedPattern());
    }
    tempo_.set(ps.tempo.load(std::memory_order_relaxed), playing, source, position);
}

void HeaderBar::uiStateChanged() {
    const int view = static_cast<int>(ctx_.state.view());
    for (int i = 0; i < viewButtons_.size(); ++i) viewButtons_[i]->setToggleState(i == view, juce::dontSendNotification);
}

void HeaderBar::setKind(PresetKind kind) {
    kind_ = kind;
    for (int i = 0; i < kindButtons_.size(); ++i)
        kindButtons_[i]->setToggleState(i == static_cast<int>(kind), juce::dontSendNotification);
    refreshPresetName();
}

void HeaderBar::refreshPresetName() {
    const juce::String name = ctx_.processor.presets().currentName(kind_);
    nameButton_.setButtonText(name.isEmpty() ? juce::String("- no preset -") : name);
}

void HeaderBar::presetListChanged(PresetKind) { refreshPresetName(); }
void HeaderBar::presetLoaded(PresetKind) { refreshPresetName(); }

void HeaderBar::showPresetList() {
    auto& presets = ctx_.processor.presets();
    const int n = presets.numPresets(kind_);
    const int current = presets.currentIndex(kind_);
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx_.lnf);
    // User presets first (flat), then one submenu per factory category in
    // the order the categories first appear; the current preset is ticked,
    // and so is the submenu that holds it.
    juce::StringArray categories;
    std::vector<juce::PopupMenu> subs;
    std::vector<bool> holdsCurrent;
    bool anyUser = false;
    for (int i = 0; i < n; ++i) {
        const auto entry = presets.preset(kind_, i);
        if (!entry.factory) {
            if (!anyUser) menu.addSectionHeader("USER");
            anyUser = true;
            menu.addItem(i + 1, entry.name, true, i == current);
            continue;
        }
        const juce::String cat = entry.category.isEmpty() ? juce::String("Factory") : entry.category;
        int c = categories.indexOf(cat);
        if (c < 0) {
            categories.add(cat);
            subs.emplace_back();
            subs.back().setLookAndFeel(&ctx_.lnf);
            holdsCurrent.push_back(false);
            c = categories.size() - 1;
        }
        subs[static_cast<std::size_t>(c)].addItem(i + 1, entry.name, true, i == current);
        if (i == current) holdsCurrent[static_cast<std::size_t>(c)] = true;
    }
    if (!categories.isEmpty()) {
        menu.addSectionHeader("FACTORY");
        for (int c = 0; c < categories.size(); ++c)
            menu.addSubMenu(categories[c], subs[static_cast<std::size_t>(c)], true, nullptr, holdsCurrent[static_cast<std::size_t>(c)]);
    }
    if (n == 0) menu.addItem(-1, "No presets", false);
    juce::Component::SafePointer<HeaderBar> safe(this);
    const PresetKind kind = kind_;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&nameButton_).withParentComponent(&ctx_.menuParent)
                           .withMinimumWidth(nameButton_.getWidth()),
                       [safe, kind](int result) {
                           if (safe == nullptr || result <= 0) return;
                           safe->ctx_.processor.presets().load(kind, result - 1);
                       });
}

void HeaderBar::showPresetMenu() {
    auto& presets = ctx_.processor.presets();
    const int current = presets.currentIndex(kind_);
    const bool isUser = current >= 0 && !presets.preset(kind_, current).factory;
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx_.lnf);
    menu.addItem(1, "Save as user preset...");
    menu.addItem(2, "Delete user preset", isUser);
    menu.addSeparator();
    menu.addItem(3, "Rescan user presets");
    menu.addItem(4, "Show preset folder");
    menu.addSeparator();
    menu.addItem(5, "Import MIDI file into selected pattern...");
    menu.addItem(6, "Export selected pattern as MIDI file...");
    juce::Component::SafePointer<HeaderBar> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetMenuButton_).withParentComponent(&ctx_.menuParent),
                       [safe](int result) {
                           if (safe == nullptr) return;
                           auto& ctx = safe->ctx_;
                           auto& pm = ctx.processor.presets();
                           switch (result) {
                               case 1: safe->askSavePreset(); break;
                               case 2: {
                                   const int idx = pm.currentIndex(safe->kind_);
                                   if (idx >= 0) pm.deleteUser(safe->kind_, idx);
                                   break;
                               }
                               case 3: pm.refresh(); break;
                               case 4: pm.userDirectory(safe->kind_).revealToUser(); break;
                               case 5:
                                   ctx.launchFileChooser(std::make_unique<juce::FileChooser>("Import MIDI file", juce::File(), "*.mid;*.midi"),
                                                         juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                         [safe](const juce::FileChooser& fc) {
                                                             if (safe == nullptr) return;
                                                             const auto file = fc.getResult();
                                                             if (!file.existsAsFile()) return;
                                                             auto& p = safe->ctx_.processor;
                                                             p.importMidiFile(file, p.patterns().selectedPattern(), safe->ctx_.editVariation());
                                                         });
                                   break;
                               case 6: {
                                   auto& p = ctx.processor;
                                   const juce::File temp = p.exportPatternToMidiFile(p.patterns().selectedPattern(), ctx.editVariation());
                                   if (!temp.existsAsFile()) return;
                                   ctx.launchFileChooser(
                                       std::make_unique<juce::FileChooser>(
                                           "Export pattern as MIDI file",
                                           juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(temp.getFileName()), "*.mid"),
                                       juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                           juce::FileBrowserComponent::warnAboutOverwriting,
                                       [safe, temp](const juce::FileChooser& fc) {
                                           if (safe == nullptr) return;  // the editor closed: the choice is moot
                                           auto target = fc.getResult();
                                           if (target == juce::File()) return;
                                           if (!target.hasFileExtension("mid")) target = target.withFileExtension("mid");
                                           temp.copyFileTo(target);
                                       });
                                   break;
                               }
                               default: break;
                           }
                       });
}

void HeaderBar::askSavePreset() {
    auto* window = new juce::AlertWindow("Save " + juce::String(kKindNames[static_cast<int>(kind_)]).toLowerCase() + " preset",
                                         "Name:", juce::MessageBoxIconType::NoIcon, &ctx_.menuParent);
    window->setLookAndFeel(&ctx_.lnf);
    window->addTextEditor("name", ctx_.processor.presets().currentName(kind_), {});
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<HeaderBar> safe(this);
    const PresetKind kind = kind_;
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe, kind, window](int result) {
                                window->setLookAndFeel(nullptr);
                                if (safe == nullptr || result != 1) return;
                                const juce::String name = window->getTextEditorContents("name").trim();
                                if (name.isNotEmpty()) safe->ctx_.processor.presets().saveUser(kind, name);
                            }),
                            true);
}

void HeaderBar::showSettingsMenu() {
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx_.lnf);
    {
        juce::PopupMenu sub;
        const int q = ctx_.choice(paramid::quality);
        const char* names[] = {"Eco (no oversampling)", "Standard", "High"};
        for (int i = 0; i < 3; ++i) sub.addItem(kQualityBase + i, names[i], true, q == i);
        menu.addSubMenu("Quality", sub);
    }
    {
        juce::PopupMenu sub;
        const float v = ctx_.raw(paramid::variance);
        for (int i = 0; i <= 4; ++i)
            sub.addItem(kVarianceBase + i, i == 0 ? juce::String("Off (nominal parts)") : juce::String(i * 25) + " %", true,
                        std::abs(v - static_cast<float>(i) * 0.25f) < 0.01f);
        menu.addSubMenu("Analog variance", sub);
    }
    {
        juce::PopupMenu sub;
        sub.addItem(kSerialRandom, "Randomise");
        sub.addItem(kSerialEnter, "Enter...");
        sub.addSeparator();
        sub.addItem(-1, "Current: #" + juce::String(static_cast<juce::int64>(ctx_.processor.unitSeed())), false);
        menu.addSubMenu("Serial number (component spread)", sub);
    }
    {
        juce::PopupMenu sub;
        const int rev = ctx_.choice(paramid::revision);
        sub.addItem(kRevisionBase + 0, "Late (all service changes)", true, rev == 0);
        sub.addItem(kRevisionBase + 1, "Early (first production)", true, rev == 1);
        menu.addSubMenu("Unit revision", sub);
    }
    {
        juce::PopupMenu sub;
        const float cur = ctx_.state.scale();
        for (int i = 0; i < kNumScales; ++i)
            sub.addItem(kScaleBase + i, juce::String(juce::roundToInt(kScales[i] * 100.0f)) + " %", true, std::abs(cur - kScales[i]) < 0.03f);
        menu.addSubMenu("UI scale", sub);
    }
    menu.addSeparator();
    {
        juce::PopupMenu sub;
        sub.addItem(kVelocitySens, "Velocity sensitive", true, ctx_.raw(paramid::velocitySens) >= 0.5f);
        juce::PopupMenu inChannel;
        const int mc = ctx_.choice(paramid::midiChannel);
        inChannel.addItem(kMidiChannelBase, "Omni", true, mc == 0);
        for (int ch = 1; ch <= 16; ++ch) inChannel.addItem(kMidiChannelBase + ch, "Channel " + juce::String(ch), true, mc == ch);
        sub.addSubMenu("Input channel", inChannel);
        juce::PopupMenu bass;
        const int bc = ctx_.choice(paramid::bassChannel);
        bass.addItem(kBassChannelBase, "Off", true, bc == 0);
        for (int ch = 1; ch <= 16; ++ch) bass.addItem(kBassChannelBase + ch, "Channel " + juce::String(ch), true, bc == ch);
        sub.addSubMenu("Chromatic bass drum channel", bass);
        sub.addSeparator();
        sub.addItem(kMidiOut, "MIDI out (sequencer notes)", true, ctx_.raw(paramid::midiOut) >= 0.5f);
        juce::PopupMenu outChannel;
        const int oc = ctx_.choice(paramid::midiOutChannel);
        for (int ch = 0; ch < 16; ++ch) outChannel.addItem(kMidiOutChannelBase + ch, "Channel " + juce::String(ch + 1), true, oc == ch);
        sub.addSubMenu("MIDI out channel", outChannel);
        sub.addSeparator();
        sub.addItem(kFactoryCcMap, "Reset CC map to factory");
        const int bound = static_cast<int>(ctx_.processor.midiLearn().getMap().size());
        sub.addItem(kClearMidiLearn, "Clear MIDI learn map (" + juce::String(bound) + " bound)", bound > 0);
        menu.addSubMenu("MIDI", sub);
    }
    menu.addItem(kJitter, "Timing jitter", true, ctx_.raw(paramid::jitter) >= 0.5f);
    menu.addItem(kPanic, "Panic (silence all voices)");
    menu.addSeparator();
    menu.addItem(kAbout, "About mw909...");

    juce::Component::SafePointer<HeaderBar> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&settingsButton_).withParentComponent(&ctx_.menuParent),
                       [safe](int result) {
                           if (safe == nullptr || result <= 0) return;
                           auto& self = *safe;
                           auto& ctx = self.ctx_;
                           auto toggle = [&ctx](const char* id) { ctx.setParameter(id, ctx.raw(id) >= 0.5f ? 0.0f : 1.0f); };
                           if (result >= kQualityBase && result < kQualityBase + 3) ctx.setParameter(paramid::quality, static_cast<float>(result - kQualityBase));
                           else if (result >= kVarianceBase && result <= kVarianceBase + 4)
                               ctx.setParameter(paramid::variance, static_cast<float>(result - kVarianceBase) * 0.25f);
                           else if (result == kSerialRandom)
                               ctx.processor.setUnitSeed(static_cast<std::uint32_t>(juce::Random::getSystemRandom().nextInt(1000000)));
                           else if (result == kSerialEnter) self.askSerialNumber();
                           else if (result >= kRevisionBase && result <= kRevisionBase + 1)
                               ctx.setParameter(paramid::revision, static_cast<float>(result - kRevisionBase));
                           else if (result >= kScaleBase && result < kScaleBase + kNumScales) {
                               if (self.onScaleRequest) self.onScaleRequest(kScales[result - kScaleBase]);
                           } else if (result == kVelocitySens) toggle(paramid::velocitySens);
                           else if (result == kMidiOut) toggle(paramid::midiOut);
                           else if (result == kJitter) toggle(paramid::jitter);
                           else if (result >= kMidiChannelBase && result <= kMidiChannelBase + 16)
                               ctx.setParameter(paramid::midiChannel, static_cast<float>(result - kMidiChannelBase));
                           else if (result >= kBassChannelBase && result <= kBassChannelBase + 16)
                               ctx.setParameter(paramid::bassChannel, static_cast<float>(result - kBassChannelBase));
                           else if (result >= kMidiOutChannelBase && result < kMidiOutChannelBase + 16)
                               ctx.setParameter(paramid::midiOutChannel, static_cast<float>(result - kMidiOutChannelBase));
                           else if (result == kFactoryCcMap) ctx.processor.midiLearn().resetToFactory();
                           else if (result == kClearMidiLearn) ctx.processor.midiLearn().clear();
                           else if (result == kPanic) {
                               if (self.onPanic) self.onPanic();
                               else ctx.processor.panic();
                           } else if (result == kAbout) self.showAbout();
                       });
}

void HeaderBar::askSerialNumber() {
    auto* window = new juce::AlertWindow("Serial number",
                                         "The serial number seeds the component tolerances behind Analog Variance.\n"
                                         "Two instances with the same number sound identical.",
                                         juce::MessageBoxIconType::NoIcon, &ctx_.menuParent);
    window->setLookAndFeel(&ctx_.lnf);
    window->addTextEditor("serial", juce::String(static_cast<juce::int64>(ctx_.processor.unitSeed())), {});
    window->getTextEditor("serial")->setInputRestrictions(10, "0123456789");
    window->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<HeaderBar> safe(this);
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window](int result) {
                                window->setLookAndFeel(nullptr);
                                if (safe == nullptr || result != 1) return;
                                const juce::String text = window->getTextEditorContents("serial").trim();
                                if (text.isNotEmpty())
                                    safe->ctx_.processor.setUnitSeed(static_cast<std::uint32_t>(text.getLargeIntValue() & 0xffffffff));
                            }),
                            true);
}

void HeaderBar::showAbout() {
    auto* window = new juce::AlertWindow("mw909 " MW909_VERSION_STRING,
                                         "Classic hybrid drum machine emulation with a built-in step sequencer.\n\n"
                                         "mwAudio. Licensed under the GNU AGPL v3 or later.\nBuilt with JUCE.",
                                         juce::MessageBoxIconType::NoIcon, &ctx_.menuParent);
    window->setLookAndFeel(&ctx_.lnf);
    window->addButton("Close", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([window](int) { window->setLookAndFeel(nullptr); }), true);
}

}  // namespace mw909::ui
