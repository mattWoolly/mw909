// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "VoicePanel.h"

namespace mw909::ui {

namespace C = mw909Colours;
namespace paramid = plugin::paramid;
using paramid::VoiceParam;

namespace {
constexpr int kGlobalWidth = 150;
constexpr int kPad = 6;
constexpr int kBandH = 20;

RomSlot romSlotFor(int voice) noexcept {
    return voice == CR ? RomSlot::Crash : voice == RD ? RomSlot::Ride : RomSlot::Hat;
}
RomRegion romRegionFor(int voice) noexcept {
    return voice == CH ? RomRegion::HatClosed : voice == OH ? RomRegion::HatOpen : voice == CR ? RomRegion::Crash : RomRegion::Ride;
}
juce::String knobLegend(int voice, int k) {
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voice)].knobs[static_cast<std::size_t>(k)];
    juce::String s(std::string(info.name));
    if (s == "Tune Depth") return "DEPTH";
    if (s == "Tune Decay") return "SWEEP";
    return s.toUpperCase();
}
}  // namespace

// --- NamePlate ---------------------------------------------------------------------------

NamePlate::NamePlate(int voice) : voice_(voice) {
    setRepaintsOnMouseActivity(false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip(voiceName(voice) + ". Click: audition and select its track. Shift-click: accented hit.");
}

void NamePlate::setSelected(bool selected) {
    if (selected == selected_) return;
    selected_ = selected;
    repaint();
}

void NamePlate::setImpliedMute(bool muted) {
    if (muted == impliedMute_) return;
    impliedMute_ = muted;
    repaint();
}

void NamePlate::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat().reduced(0.5f).withTrimmedBottom(1.0f);
    mw909LookAndFeel::drawButtonFace(g, r, selected_, hover_, down_, C::orange);
    if (impliedMute_) {
        // Silenced by another voice's solo: a recessed, hatched plate.
        g.setColour(C::panelShade.withAlpha(0.7f));
        g.fillRoundedRectangle(r, 2.5f);
        g.setColour(C::inkFaint.withAlpha(0.5f));
        g.saveState();
        g.reduceClipRegion(r.toNearestInt());
        for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 6.0f)
            g.drawLine(x, r.getBottom(), x + r.getHeight(), r.getY(), 1.0f);
        g.restoreState();
    }
    g.setColour(impliedMute_ ? C::inkDim : C::ink);
    g.setFont(mw909LookAndFeel::bold(juce::jlimit(10.0f, 15.0f, r.getHeight() * 0.5f)));
    g.drawText(voiceCode(voice_), r.toNearestInt(), juce::Justification::centred, false);
}

void NamePlate::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) return;
    down_ = true;
    repaint();
    if (onAudition) onAudition(e.mods.isShiftDown());
    if (onSelect) onSelect();
}

void NamePlate::mouseUp(const juce::MouseEvent&) {
    down_ = false;
    repaint();
}

// --- VoiceColumn ---------------------------------------------------------------------------

VoiceColumn::VoiceColumn(Context& ctx, int voice) : ctx_(ctx), voice_(voice), plate_(voice) {
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voice)];
    auto make = [&](const std::string& id, const juce::String& legend, bool hw, bool bipolar) -> ParamControl* {
        auto* p = ctx_.param(id);
        if (p == nullptr) return nullptr;
        auto* c = new ParamControl(ctx_, *p, ParamControl::Style::Knob, legend, hw);
        c->setBipolar(bipolar);
        addAndMakeVisible(c);
        return c;
    };
    if (auto* level = make(paramid::voice(voice, VoiceParam::Level), "LEVEL", true, false)) hardware_.add(level);
    for (int k = 0; k < info.numKnobs; ++k) {
        const auto& knob = info.knobs[static_cast<std::size_t>(k)];
        if (knob.unit == KnobUnit::Choice) continue;
        const bool bipolar = knob.unit == KnobUnit::Semitones;
        if (auto* c = make(paramid::knob(voice, k), knobLegend(voice, k), knob.panel, bipolar)) {
            if (knob.panel) hardware_.add(c);
            else extended_.add(c);
        }
    }
    if (isSampleVoice(voice)) {
        if (auto* p = ctx_.param(paramid::knob(voice, knob::PcmSource))) {
            source_ = std::make_unique<ChoiceButtons>(ctx_, *p, juce::StringArray{"ROM", "SYN", "USR"}, false);
            source_->setFontHeight(8.5f);
            source_->setGap(1);
            for (int i = 0; i < source_->numChoices(); ++i)
                source_->button(i)->setTooltip(i == 0 ? "Source: the 6-bit ROM" : i == 1 ? "Source: the synthetic metal source (Color, Density)"
                                                                                   : "Source: your own ROM image (LOAD)");
            addAndMakeVisible(*source_);
        }
        styleButton(loadButton_, C::orange, true, 8.5f);
        loadButton_.onClick = [this] { chooseUserRom(); };
        loadButton_.onMenu = [this](juce::Component&) { showUserRomMenu(); };
        addAndMakeVisible(loadButton_);
        refreshUserRom();
    }
    addAndMakeVisible(lamp_);
    if (auto* m = ctx_.param(paramid::voice(voice, VoiceParam::Mute))) {
        mute_ = std::make_unique<ParamButton>(ctx_, *m, "M", C::mute);
        mute_->setTooltip("Mute " + voiceCode(voice) + " (the tail rings out)");
        addAndMakeVisible(*mute_);
    }
    if (auto* s = ctx_.param(paramid::voice(voice, VoiceParam::Solo))) {
        solo_ = std::make_unique<ParamButton>(ctx_, *s, "S", C::solo);
        solo_->setTooltip("Solo " + voiceCode(voice));
        addAndMakeVisible(*solo_);
    }
    plate_.onAudition = [this](bool accent) { ctx_.processor.audition(static_cast<VoiceId>(voice_), accent); };
    plate_.onSelect = [this] { ctx_.state.setSelectedTrack(voice_); };
    addAndMakeVisible(plate_);
    setInterceptsMouseClicks(false, true);
}

VoiceColumn::~VoiceColumn() = default;

// --- SoloWatch ---------------------------------------------------------------------------------

SoloWatch::SoloWatch(Context& ctx) {
    for (int v = 0; v < kNumVoices; ++v) solo_[static_cast<std::size_t>(v)] = ctx.rawPointer(paramid::voice(v, VoiceParam::Solo));
}

std::array<bool, kNumVoices> SoloWatch::impliedMutes() const {
    std::array<bool, kNumVoices> soloed{}, muted{};
    bool any = false;
    for (std::size_t v = 0; v < soloed.size(); ++v) {
        soloed[v] = solo_[v] != nullptr && solo_[v]->load(std::memory_order_relaxed) >= 0.5f;
        any = any || soloed[v];
    }
    for (std::size_t v = 0; v < muted.size(); ++v) muted[v] = any && !soloed[v];
    return muted;
}

void VoiceColumn::refreshUserRom() {
    if (!isSampleVoice(voice_)) return;
    const auto rom = ctx_.processor.userRom(romSlotFor(voice_));
    loadButton_.setToggleState(!rom.empty(), juce::dontSendNotification);
    const juce::String name = rom.empty() ? juce::String() : juce::File::createFileWithoutCheckingPath(rom.name).getFileNameWithoutExtension();
    if (name != userRomName_) {
        userRomName_ = name;
        repaint(0, rows_.sourceTop + 39, getWidth(), 14);
    }
    const juce::String slot = voice_ == CR ? "crash" : voice_ == RD ? "ride" : "hi-hat";
    loadButton_.setTooltip(rom.empty() ? "Load an audio file into the user " + slot + " ROM. Right-click for more."
                                       : "User " + slot + " ROM: " + rom.name + ". Click to replace, right-click to clear.");
}

void VoiceColumn::chooseUserRom() {
    juce::Component::SafePointer<VoiceColumn> safe(this);
    ctx_.launchFileChooser(std::make_unique<juce::FileChooser>("Load audio into the user ROM", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg"),
                           juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                           [safe](const juce::FileChooser& fc) {
                               if (safe == nullptr) return;
                               const auto file = fc.getResult();
                               if (!file.existsAsFile()) return;
                               if (safe->ctx_.processor.importUserRom(file, romRegionFor(safe->voice_)))
                                   safe->ctx_.setParameter(paramid::knob(safe->voice_, knob::PcmSource), static_cast<float>(PcmSource::User));
                           });
}

void VoiceColumn::paint(juce::Graphics& g) {
    if (userRomName_.isEmpty()) return;
    // The loaded user ROM's file name, under LOAD (the tooltip has it in full).
    const juce::Rectangle<int> area(4, rows_.sourceTop + 40, getWidth() - 8, 12);
    g.setColour(C::inkDim);
    g.setFont(mw909LookAndFeel::legible(g, mw909LookAndFeel::text(9.5f), 12.0f));
    g.drawFittedText(userRomName_, area, juce::Justification::centred, 1, 0.85f);
}

void VoiceColumn::showUserRomMenu() {
    const auto rom = ctx_.processor.userRom(romSlotFor(voice_));
    juce::PopupMenu menu;
    menu.setLookAndFeel(&ctx_.lnf);
    menu.addSectionHeader(rom.empty() ? juce::String("User ROM empty") : "User ROM: " + rom.name);
    menu.addItem(1, "Load audio file...");
    menu.addItem(2, "Clear user ROM", !rom.empty());
    juce::Component::SafePointer<VoiceColumn> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&loadButton_).withParentComponent(&ctx_.menuParent),
                       [safe](int result) {
                           if (safe == nullptr) return;
                           if (result == 1) safe->chooseUserRom();
                           else if (result == 2) safe->ctx_.processor.clearUserRom(romSlotFor(safe->voice_));
                       });
}

void VoiceColumn::resized() {
    const int w = getWidth();
    if (w <= 0 || rows_.plateH <= 0) return;
    const int half = w / 2;
    auto cell = [&](int top, int rowH, int index) {
        const int row = index / 2, col = index % 2;
        return juce::Rectangle<int>(col == 0 ? 0 : half, top + row * rowH, col == 0 ? half : w - half, rowH);
    };
    for (int i = 0; i < hardware_.size(); ++i) hardware_[i]->setBounds(cell(rows_.hwTop, rows_.hwRowH, i).reduced(1, 2));
    for (int i = 0; i < extended_.size(); ++i) extended_[i]->setBounds(cell(rows_.extTop, rows_.extRowH, i).reduced(4, 2));
    if (source_) source_->setBounds(4, rows_.sourceTop, w - 8, 18);
    if (isSampleVoice(voice_)) loadButton_.setBounds(4, rows_.sourceTop + 21, w - 8, 18);
    {
        auto row = juce::Rectangle<int>(4, rows_.switchTop, w - 8, 20);
        lamp_.setBounds(row.removeFromLeft(14).withSizeKeepingCentre(9, 9));
        row.removeFromLeft(3);
        const int bw = (row.getWidth() - 3) / 2;
        if (mute_) mute_->setBounds(row.removeFromLeft(bw));
        row.removeFromLeft(3);
        if (solo_) solo_->setBounds(row);
    }
    plate_.setBounds(4, rows_.plateTop, w - 8, rows_.plateH);
}

// --- GlobalBlock ----------------------------------------------------------------------------

GlobalBlock::GlobalBlock(Context& ctx) : ctx_(ctx) {
    auto make = [&](const char* id, const juce::String& legend, bool hw) {
        auto* p = ctx_.param(id);
        if (p == nullptr) return std::unique_ptr<ParamControl>();
        auto c = std::make_unique<ParamControl>(ctx_, *p, ParamControl::Style::Knob, legend, hw);
        addAndMakeVisible(*c);
        return c;
    };
    tempo_ = make(paramid::tempo, "TEMPO", true);
    if (tempo_) tempo_->setLegendHeight(11.0f);
    accent_ = make(paramid::totalAccent, "ACCENT", true);
    volume_ = make(paramid::masterVolume, "VOLUME", true);
    // The pattern's SHUFFLE keys set the swing; this scales all of it at once.
    shuffle_ = make(paramid::swingScale, "SHUF DEPTH", false);
    if (shuffle_) {
        shuffle_->formatValue = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
        shuffle_->parseValue = [](const juce::String& t) -> std::optional<float> {
            const auto n = t.trim().trimCharactersAtEnd("% ").trim();
            if (n.isEmpty() || !(juce::CharacterFunctions::isDigit(n[0]) || n[0] == '.')) return std::nullopt;
            return juce::jlimit(0.0f, 1.0f, n.getFloatValue() / 100.0f);
        };
        shuffle_->refreshValueText();
        shuffle_->setTooltip("Shuffle depth: scales the swing of every pattern's SHUFFLE setting (100 % = as written, 0 % = straight)");
    }
    if (auto* p = ctx_.param(paramid::seqMode)) {
        mode_ = std::make_unique<ChoiceButtons>(ctx_, *p, juce::StringArray{"OFF", "HOST SYNC", "INTERNAL", "HOST NOTE"}, true);
        mode_->setFontHeight(9.0f);
        mode_->setLamp(true);
        mode_->setGap(3);
        addAndMakeVisible(*mode_);
    }
}

void GlobalBlock::resized() {
    auto r = getLocalBounds();
    globalBand_ = r.removeFromTop(kBandH);
    r.reduce(6, 0);
    r.removeFromTop(6);
    if (tempo_) tempo_->setBounds(r.removeFromTop(112).reduced(14, 0));
    r.removeFromTop(4);
    {
        auto row = r.removeFromTop(80);
        const int half = row.getWidth() / 2;
        if (accent_) accent_->setBounds(row.removeFromLeft(half));
        if (volume_) volume_->setBounds(row);
    }
    r.removeFromTop(2);
    if (shuffle_) shuffle_->setBounds(r.removeFromTop(66).withSizeKeepingCentre(70, 66));
    r.removeFromTop(8);
    seqBand_ = r.removeFromTop(kBandH).expanded(6, 0);
    r.removeFromTop(6);
    if (mode_) mode_->setBounds(r.removeFromTop(4 * 21 + 3 * 3));
    invalidateCache();
}

void GlobalBlock::paintStatic(juce::Graphics& g) {
    g.setColour(C::panelLight);
    g.fillRect(getLocalBounds());
    g.setColour(C::panelEdge);
    g.drawRect(getLocalBounds());
    mw909LookAndFeel::drawBand(g, globalBand_, "GLOBAL");
    mw909LookAndFeel::drawBand(g, seqBand_, "SEQUENCER");
}

// --- VoicePanel ------------------------------------------------------------------------------

VoicePanel::VoicePanel(Context& ctx) : ctx_(ctx), global_(ctx), solos_(ctx) {
    setOpaque(true);
    addAndMakeVisible(global_);
    for (int v = 0; v < kNumVoices; ++v) addAndMakeVisible(columns_.add(new VoiceColumn(ctx_, v)));
    if (auto* p = ctx_.param(paramid::hatChoke)) {
        choke_ = std::make_unique<ParamButton>(ctx_, *p, "CHOKE", C::orange, true);
        choke_->getProperties().set("fontHeight", 8.5);
        choke_->setTooltip("Open hat is cut by the closed hat, as on the original");
        addAndMakeVisible(*choke_);
    }
    ctx_.state.addListener(this);
    ctx_.processor.patterns().addListener(this);
    uiStateChanged();
}

VoicePanel::~VoicePanel() {
    ctx_.processor.patterns().removeListener(this);
    ctx_.state.removeListener(this);
}

void VoicePanel::resized() {
    auto r = getLocalBounds().reduced(kPad, kPad);
    global_.setBounds(r.removeFromLeft(kGlobalWidth));
    r.removeFromLeft(kPad);
    voiceArea_ = r;

    // Rows shared by every column (column-local y).
    const int h = r.getHeight();
    rows_.plateH = 30;
    rows_.plateTop = h - rows_.plateH;
    rows_.switchTop = rows_.plateTop - 4 - 20;
    rows_.hwTop = kBandH + 4;
    rows_.hwRowH = 82;
    rows_.extTop = rows_.hwTop + 2 * rows_.hwRowH + 12;
    rows_.extRowH = 66;
    rows_.sourceTop = rows_.extTop + 2 * rows_.extRowH + 6;

    const int n = columns_.size();
    for (int i = 0; i < n; ++i) {
        const int x0 = r.getX() + r.getWidth() * i / n;
        const int x1 = r.getX() + r.getWidth() * (i + 1) / n;
        columns_[i]->setBounds(x0, r.getY(), x1 - x0, h);
        columns_[i]->setRows(rows_);
    }
    if (choke_) {
        const auto hats = columns_[CH]->getBounds().getUnion(columns_[OH]->getBounds());
        choke_->setBounds(hats.getRight() - 62, hats.getY() + 2, 56, kBandH - 5);
    }
    invalidateCache();
}

void VoicePanel::paintStatic(juce::Graphics& g) {
    g.fillAll(C::panel);
    if (columns_.isEmpty()) return;
    // The voice section's ground: one raised plate behind all columns.
    mw909LookAndFeel::drawPlate(g, voiceArea_.toFloat(), C::panelLight, C::panelEdge, 0.0f);
    const int n = columns_.size();
    // Bands: one per voice, one HI HAT band across CH and OH.
    for (int v = 0; v < n; ++v) {
        if (v == OH) continue;
        auto b = columns_[v]->getBounds().withHeight(kBandH);
        if (v == CH) {
            b = b.getUnion(columns_[OH]->getBounds().withHeight(kBandH));
            mw909LookAndFeel::drawBand(g, b.reduced(1, 0), "HI HAT", juce::Justification::centredLeft);
        } else {
            mw909LookAndFeel::drawBand(g, b.reduced(1, 0), voiceName(v));
        }
    }
    // Column rules, and the dashed rule between original and extended knobs.
    for (int v = 0; v < n; ++v) {
        const auto c = columns_[v]->getBounds();
        if (v > 0) {
            g.setColour(v == OH ? C::panelEdge.withAlpha(0.45f) : C::panelEdge);
            g.drawVerticalLine(c.getX(), static_cast<float>(c.getY() + kBandH), static_cast<float>(c.getBottom()));
        }
        const float y = static_cast<float>(c.getY() + rows_.extTop - 6);
        for (float x = static_cast<float>(c.getX() + 6); x < static_cast<float>(c.getRight() - 6); x += 6.0f) {
            g.setColour(C::inkFaint.withAlpha(0.7f));
            g.fillRect(x, y, 3.0f, 1.0f);
        }
    }
    // Light separators above the switch row.
    g.setColour(C::panelEdge.withAlpha(0.6f));
    g.drawHorizontalLine(voiceArea_.getY() + rows_.switchTop - 6, static_cast<float>(voiceArea_.getX() + 4),
                         static_cast<float>(voiceArea_.getRight() - 4));
}

void VoicePanel::uiStateChanged() {
    const int sel = ctx_.state.selectedTrack();
    for (int v = 0; v < columns_.size(); ++v) columns_[v]->setSelected(v == sel);
}

void VoicePanel::userRomChanged(int) {
    for (auto* c : columns_) c->refreshUserRom();
}

void VoicePanel::bankChanged() {
    for (auto* c : columns_) c->refreshUserRom();
}

void VoicePanel::poll(const PlayState& ps) {
    for (int v = 0; v < kNumVoices; ++v) {
        const auto h = ps.hits[static_cast<std::size_t>(v)].load(std::memory_order_relaxed);
        if (hitsPrimed_ && h != lastHits_[static_cast<std::size_t>(v)]) columns_[v]->flash();
        lastHits_[static_cast<std::size_t>(v)] = h;
        columns_[v]->tick();
    }
    hitsPrimed_ = true;
    const auto muted = solos_.impliedMutes();
    for (int v = 0; v < kNumVoices; ++v) columns_[v]->setImpliedMute(muted[static_cast<std::size_t>(v)]);
}

}  // namespace mw909::ui
