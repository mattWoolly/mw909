// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "FxView.h"

#include <algorithm>

namespace mw909::ui {

namespace C = mw909Colours;
namespace paramid = plugin::paramid;

namespace {
constexpr int kPad = 6;
constexpr int kBandH = 20;
constexpr int kLegendH = 11;
constexpr const char* kShortNames[kNumFxInstances] = {"DIST", "EQ A", "EQ B", "DELAY", "REVERB", "CHORUS", "CRUSH", "DYN A", "DYN B", "FILTER"};

juce::String fxId(int instance, const char* field) { return juce::String(paramid::fx(instance, field)); }

// The full display name of an instance, from its "on" parameter ("Distortion On").
juce::String longName(Context& ctx, int instance) {
    if (auto* p = ctx.param(fxId(instance, "on"))) return p->getName(64).upToLastOccurrenceOf(" On", false, false);
    return FxRack::shortName(instance);
}

// A short legend for an effect parameter: its name without the effect's,
// with the long words abbreviated so they fit a knob cell.
juce::String legendFor(const juce::String& paramName, const juce::String& effectName) {
    juce::String s = paramName.startsWith(effectName + " ") ? paramName.substring(effectName.length() + 1) : paramName;
    s = s.toUpperCase();
    const std::pair<const char*, const char*> abbreviations[] = {
        {"RESONANCE", "RESO"},  {"DIFFUSION", "DIFF"},   {"THRESHOLD", "THRESH"}, {"DIVISION", "DIV"},
        {"TRIGGER", "TRIG"},    {"APPROX FILTER", "APPROX FLT"}, {"IMAGES FILTER", "IMAGE FLT"}, {"MODULATION", "MOD"},
        {"SIDECHAIN", "SIDECHAIN"}};
    for (const auto& [from, to] : abbreviations) s = s.replace(from, to);
    return s;
}
}  // namespace

// --- FxRack -------------------------------------------------------------------------------------

juce::String FxRack::shortName(int instance) {
    return instance >= 0 && instance < kNumFxInstances ? kShortNames[instance] : "?";
}

FxRack::FxRack(Context& ctx) : ctx_(ctx) {
    setRepaintsOnMouseActivity(false);
    setTooltip("Click a slot to edit it, click its lamp to switch it on or off, drag it to reorder or to move it to the other bus.");
    for (int i = 0; i < kNumFxInstances; ++i)
        for (const char* field : {"on", "bus", "order"})
            if (auto* p = ctx_.param(fxId(i, field)))
                attachments_.push_back(std::make_unique<juce::ParameterAttachment>(*p, [this](float) { repaint(); }));
}

FxRack::~FxRack() { attachments_.clear(); }

void FxRack::setSelected(int instance) {
    if (instance == selected_) return;
    selected_ = instance;
    repaint();
}

juce::Array<int> FxRack::busOrder(int bus) const {
    juce::Array<int> ids;
    for (int i = 0; i < kNumFxInstances; ++i)
        if (ctx_.choice(fxId(i, "bus")) == bus) ids.add(i);
    std::stable_sort(ids.begin(), ids.end(), [this](int a, int b) {
        const int oa = ctx_.choice(fxId(a, "order")), ob = ctx_.choice(fxId(b, "order"));
        return oa != ob ? oa < ob : a < b;
    });
    return ids;
}

void FxRack::resized() {
    auto r = getLocalBounds();
    const int h = (r.getHeight() - 6) / 2;
    rows_[0] = r.removeFromTop(h);
    r.removeFromTop(6);
    rows_[1] = r.removeFromTop(h);
}

juce::Rectangle<int> FxRack::slotBounds(int bus, int position) const {
    const auto row = rows_[static_cast<std::size_t>(juce::jlimit(0, 1, bus))].withTrimmedLeft(kLabelWidth + 6);
    // One width for both rows, with room for the longer one (and a spare slot to drop into).
    const int slots = juce::jmax(6, juce::jmax(busOrder(0).size(), busOrder(1).size()) + 1);
    const int w = (row.getWidth() - (slots - 1) * 4) / slots;
    return {row.getX() + position * (w + 4), row.getY() + 3, w, row.getHeight() - 6};
}

juce::Rectangle<int> FxRack::lampBounds(int bus, int position) const {
    const auto s = slotBounds(bus, position);
    return s.withHeight(14).withWidth(14).translated(3, 3);
}

bool FxRack::hitSlot(juce::Point<int> pos, int& bus, int& position) const {
    for (int b = 0; b < 2; ++b) {
        const int n = busOrder(b).size();
        for (int i = 0; i < n; ++i)
            if (slotBounds(b, i).contains(pos)) {
                bus = b;
                position = i;
                return true;
            }
    }
    return false;
}

void FxRack::dropTarget(juce::Point<int> pos, int& bus, int& index) const {
    bus = rows_[1].expanded(0, 3).contains(pos) || pos.y > rows_[1].getY() ? 1 : 0;
    const auto order = busOrder(bus);
    index = 0;
    for (int i = 0; i < order.size(); ++i) {
        if (order[i] == dragInstance_) continue;
        if (pos.x > slotBounds(bus, i).getCentreX()) index = i + 1;
    }
    // Index in the list without the dragged instance.
    const int self = order.indexOf(dragInstance_);
    if (self >= 0 && self < index) --index;
}

void FxRack::move(int instance, int bus, int index) {
    juce::Array<int> target = busOrder(bus);
    target.removeFirstMatchingValue(instance);
    index = juce::jlimit(0, target.size(), index);
    target.insert(index, instance);
    const int from = ctx_.choice(fxId(instance, "bus"));
    if (from != bus) {
        ctx_.setParameter(fxId(instance, "bus"), static_cast<float>(bus));
        // Renumber the bus it left, so both keep contiguous order keys.
        juce::Array<int> source = busOrder(from);
        source.removeFirstMatchingValue(instance);
        for (int i = 0; i < source.size(); ++i)
            if (ctx_.choice(fxId(source[i], "order")) != i) ctx_.setParameter(fxId(source[i], "order"), static_cast<float>(i));
    }
    for (int i = 0; i < target.size(); ++i)
        if (ctx_.choice(fxId(target[i], "order")) != i) ctx_.setParameter(fxId(target[i], "order"), static_cast<float>(i));
}

void FxRack::paint(juce::Graphics& g) {
    for (int b = 0; b < 2; ++b) {
        const auto row = rows_[static_cast<std::size_t>(b)];
        mw909LookAndFeel::drawInset(g, row.withTrimmedLeft(kLabelWidth).toFloat(), 3.0f);
        // The bus label: a dark key with an orange legend.
        const auto label = row.withWidth(kLabelWidth - 4).toFloat();
        g.setColour(C::band);
        g.fillRoundedRectangle(label, 2.5f);
        g.setColour(C::legend);
        g.setFont(mw909LookAndFeel::legend(10.0f));
        g.drawText(b == 0 ? "FX 1" : "FX 2", label.toNearestInt(), juce::Justification::centred, false);
        const auto order = busOrder(b);
        for (int i = 0; i < order.size(); ++i) {
            const int id = order[i];
            if (id == dragInstance_ && dragMoved_) continue;
            const auto s = slotBounds(b, i).toFloat();
            const bool on = ctx_.raw(fxId(id, "on")) >= 0.5f;
            mw909LookAndFeel::drawButtonFace(g, s, id == selected_, false, false, C::orange);
            mw909LookAndFeel::drawLamp(g, lampBounds(b, i).toFloat().reduced(3.0f), on ? 1.0f : 0.0f);
            g.setColour(C::ink);
            g.setFont(mw909LookAndFeel::legend(9.5f));
            g.drawFittedText(shortName(id), s.toNearestInt().reduced(2, 0).withTrimmedTop(static_cast<int>(s.getHeight() * 0.35f)),
                             juce::Justification::centred, 1, 0.7f);
            g.setColour(C::inkDim);
            g.setFont(mw909LookAndFeel::text(8.5f));
            g.drawText(juce::String(i + 1), s.toNearestInt().reduced(4, 3), juce::Justification::topRight, false);
        }
        if (order.isEmpty()) {
            g.setColour(C::creamDim);
            g.setFont(mw909LookAndFeel::text(11.0f));
            g.drawText("empty: drag an effect here", row.withTrimmedLeft(kLabelWidth + 10), juce::Justification::centredLeft, false);
        }
    }
    if (dragInstance_ >= 0 && dragMoved_) {
        int bus, index;
        dropTarget(dragPos_, bus, index);
        // Insertion marker, then the slot under the mouse.
        const auto order = busOrder(bus);
        int shownIndex = 0;
        for (int i = 0, k = 0; i < order.size(); ++i) {
            if (order[i] == dragInstance_) continue;
            if (k == index) { shownIndex = i; break; }
            ++k;
            shownIndex = i + 1;
        }
        const auto slot = slotBounds(bus, shownIndex);
        g.setColour(C::orange);
        g.fillRect(slot.getX() - 3, slot.getY() - 2, 3, slot.getHeight() + 4);
        const auto ghost = slotBounds(0, 0).withCentre(dragPos_).toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillRoundedRectangle(ghost.translated(2.0f, 3.0f), 3.0f);
        mw909LookAndFeel::drawButtonFace(g, ghost, true, false, false, C::orange);
        g.setColour(C::ink);
        g.setFont(mw909LookAndFeel::legend(9.5f));
        g.drawFittedText(shortName(dragInstance_), ghost.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
}

void FxRack::mouseDown(const juce::MouseEvent& e) {
    dragInstance_ = -1;
    dragMoved_ = false;
    int bus, position;
    if (!hitSlot(e.getPosition(), bus, position)) return;
    const int id = busOrder(bus)[position];
    if (e.mods.isPopupMenu()) {
        if (auto* p = ctx_.param(fxId(id, "on")))
            showParameterMenu(ctx_, *p, *this, [this, id] { ctx_.setParameter(fxId(id, "on"), 0.0f); });
        return;
    }
    if (lampBounds(bus, position).contains(e.getPosition())) {
        ctx_.setParameter(fxId(id, "on"), ctx_.raw(fxId(id, "on")) >= 0.5f ? 0.0f : 1.0f);
        return;
    }
    if (onSelect) onSelect(id);
    dragInstance_ = id;
    dragPos_ = e.getPosition();
}

void FxRack::mouseDrag(const juce::MouseEvent& e) {
    if (dragInstance_ < 0) return;
    if (!dragMoved_ && e.getDistanceFromDragStart() < 5) return;
    dragMoved_ = true;
    dragPos_ = e.getPosition();
    repaint();
}

void FxRack::mouseUp(const juce::MouseEvent& e) {
    if (dragInstance_ >= 0 && dragMoved_) {
        int bus, index;
        dragPos_ = e.getPosition();
        dropTarget(dragPos_, bus, index);
        const int id = dragInstance_;
        dragInstance_ = -1;
        dragMoved_ = false;
        move(id, bus, index);
    }
    dragInstance_ = -1;
    dragMoved_ = false;
    repaint();
}

// --- FxEditor --------------------------------------------------------------------------------------

FxEditor::FxEditor(Context& ctx) : ctx_(ctx) {}

void FxEditor::setInstance(int instance) {
    if (instance == instance_) return;
    instance_ = instance;
    controls_.clear();
    on_.reset();
    busAttachment_.reset();
    orderAttachment_.reset();
    if (instance < 0 || instance >= kNumFxInstances) return;
    const juce::String name = longName(ctx_, instance);
    if (auto* p = ctx_.param(fxId(instance, "on"))) {
        // Names both states and fills when on, so it never reads "ON" for an effect that is off.
        on_ = std::make_unique<ParamButton>(ctx_, *p, "ON", C::orange, false);
        on_->getProperties().set("offText", "OFF");
        on_->getProperties().set("fontHeight", 9.0);
        on_->setTooltip("Switch " + name + " on or off");
        addAndMakeVisible(*on_);
    }
    if (auto* p = ctx_.param(fxId(instance, "bus"))) busAttachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float) { repaint(band_); });
    if (auto* p = ctx_.param(fxId(instance, "order")))
        orderAttachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float) { repaint(band_); });
    const std::string prefix = "fx_" + std::string(paramid::kFxIds[static_cast<std::size_t>(instance)]) + "_";
    for (const auto& def : plugin::parameterDefs()) {
        if (def.id.rfind(prefix, 0) != 0) continue;
        const std::string field = def.id.substr(prefix.size());
        if (field == "on" || field == "bus" || field == "order") continue;
        auto* p = ctx_.param(def.id);
        if (p == nullptr) continue;
        const juce::String legend = legendFor(juce::String(def.name), name);
        juce::Component* c = nullptr;
        if (def.kind == plugin::ParamKind::Toggle) {
            auto* b = new ParamButton(ctx_, *p, legend, C::orange, true);
            b->getProperties().set("fontHeight", 8.5);
            c = b;
        } else if (def.kind == plugin::ParamKind::Choice || def.kind == plugin::ParamKind::Int) {
            auto* box = new ParamValueBox(ctx_, *p, legend, def.kind == plugin::ParamKind::Choice);
            box->setLegendHeight(10);
            c = box;
        } else {
            auto* k = new ParamControl(ctx_, *p, ParamControl::Style::Knob, legend, false);
            k->setLegendHeight(8.5f);
            k->setBipolar(def.low < 0.0f && def.high > 0.0f);
            c = k;
        }
        controls_.add(c);
        addAndMakeVisible(c);
    }
    resized();
    repaint();
}

void FxEditor::resized() {
    auto r = getLocalBounds();
    band_ = r.removeFromTop(kBandH);
    if (on_) on_->setBounds(band_.withTrimmedLeft(band_.getWidth() - 56).reduced(3, 2).translated(-2, -1));
    r.removeFromTop(6);
    r.reduce(4, 0);
    const int n = controls_.size();
    if (n == 0) return;
    // One row up to 8 controls, else two balanced rows (the filter has 17).
    const int cols = n <= 8 ? n : (n + 1) / 2;
    const int rows = (n + cols - 1) / cols;
    constexpr int kRowGap = 14;
    const int rowH = juce::jmin(rows == 1 ? 92 : 84, (r.getHeight() - (rows - 1) * kRowGap) / rows);
    const int cellW = juce::jmin(rows == 1 ? 86 : 80, r.getWidth() / cols);
    // Centre the block both ways instead of leaving the right side or the foot empty.
    const int blockH = rows * rowH + (rows - 1) * kRowGap;
    r.removeFromTop(juce::jmax(0, (r.getHeight() - blockH) / 2 - 4));
    for (int i = 0; i < n; ++i) {
        const int row = i / cols, col = i % cols;
        const int inRow = juce::jmin(cols, n - row * cols);
        const int x0 = r.getX() + (r.getWidth() - inRow * cellW) / 2;
        const juce::Rectangle<int> cell(x0 + col * cellW, r.getY() + row * (rowH + kRowGap), cellW, rowH);
        auto* c = controls_[i];
        if (dynamic_cast<ParamControl*>(c) != nullptr) c->setBounds(cell.reduced(4, 2));
        else if (dynamic_cast<ValueBox*>(c) != nullptr) c->setBounds(cell.reduced(5, 0).withSizeKeepingCentre(cell.getWidth() - 10, 36));
        else c->setBounds(cell.reduced(5, 0).withSizeKeepingCentre(cell.getWidth() - 10, 22));
    }
}

void FxEditor::paint(juce::Graphics& g) {
    if (instance_ < 0) return;
    const juce::String name = longName(ctx_, instance_);
    const int bus = ctx_.choice(fxId(instance_, "bus"));
    mw909LookAndFeel::drawBand(g, band_, name, juce::Justification::centredLeft);
    g.setColour(C::creamDim);
    g.setFont(mw909LookAndFeel::legend(9.0f));
    g.drawText("ON FX " + juce::String(bus + 1), band_.withTrimmedRight(66), juce::Justification::centredRight, false);
}

// --- FxView ------------------------------------------------------------------------------------------

FxView::FxView(Context& ctx) : ctx_(ctx), rack_(ctx), editor_(ctx) {
    setOpaque(true);
    addAndMakeVisible(rack_);
    addAndMakeVisible(editor_);
    rack_.onSelect = [this](int id) {
        rack_.setSelected(id);
        editor_.setInstance(id);
    };
    editor_.setInstance(rack_.selected());

    auto knob = [&](const juce::String& id, const juce::String& legend, bool hw = false, bool bipolar = false) {
        auto* p = ctx_.param(id);
        if (p == nullptr) return std::unique_ptr<ParamControl>();
        auto c = std::make_unique<ParamControl>(ctx_, *p, ParamControl::Style::Knob, legend, hw);
        c->setLegendHeight(8.5f);
        c->setBipolar(bipolar);
        addAndMakeVisible(*c);
        return c;
    };
    auto toggle = [&](const juce::String& id, const juce::String& label) {
        auto* p = ctx_.param(id);
        if (p == nullptr) return std::unique_ptr<ParamButton>();
        auto b = std::make_unique<ParamButton>(ctx_, *p, label, C::orange, true);
        b->getProperties().set("fontHeight", 8.5);
        addAndMakeVisible(*b);
        return b;
    };
    auto box = [&](const juce::String& id, const juce::String& legend) {
        auto* p = ctx_.param(id);
        if (p == nullptr) return std::unique_ptr<ParamValueBox>();
        auto b = std::make_unique<ParamValueBox>(ctx_, *p, legend, true);
        b->setLegendHeight(legend.isEmpty() ? 0 : 10);
        addAndMakeVisible(*b);
        return b;
    };

    // Returns.
    fx1Level_ = knob(paramid::fxBus(0, "level"), "FX 1 LEVEL");
    fx1Out_ = box(paramid::fxBus(0, "out"), "FX 1 OUT");
    fx1To2_ = knob(paramid::fx1To2, "TO FX 2");
    fx1To2Pre_ = toggle(paramid::fx1To2Pre, "PRE");
    fx2Level_ = knob(paramid::fxBus(1, "level"), "FX 2 LEVEL");
    fx2Out_ = box(paramid::fxBus(1, "out"), "FX 2 OUT");
    if (fx1To2Pre_) fx1To2Pre_->setTooltip("FX 1 to FX 2 send taken before the FX 1 return level");

    // Master chain.
    if (auto* p = ctx_.param(paramid::outputStage)) {
        stage_ = std::make_unique<ChoiceButtons>(ctx_, *p, juce::StringArray{"OFF", "MODERN", "ORIGINAL"}, false);
        stage_->setFontHeight(8.5f);
        addAndMakeVisible(*stage_);
    }
    drive_ = knob(paramid::masterDrive, "DRIVE");
    volume_ = knob(paramid::masterVolume, "VOLUME", true);
    softClip_ = toggle(paramid::softClip, "SOFT CLIP");
    limiter_ = toggle(paramid::limiter, "LIMITER");
    limThresh_ = knob(paramid::limThresh, "THRESH");
    limAttack_ = knob(paramid::limAttack, "ATTACK");
    limRelease_ = knob(paramid::limRelease, "RELEASE");
    mcompOn_ = toggle(paramid::mcompOn, "3-BAND");
    if (mcompOn_) mcompOn_->setTooltip("Three-band compressor on the main bus");
    xLow_ = knob(paramid::mcompXLow, "LOW X");
    xHigh_ = knob(paramid::mcompXHigh, "HIGH X");
    const char* bandKnobs[] = {"thresh", "ratio", "attack", "release", "makeup"};
    for (int b = 0; b < 3; ++b) {
        auto& row = bands_[static_cast<std::size_t>(b)];
        row.on = toggle(paramid::band(b, "on"), "ON");
        if (row.on) row.on->getProperties().set("offText", "OFF");
        row.solo = toggle(paramid::band(b, "solo"), "SOLO");
        for (int k = 0; k < 5; ++k) {
            row.knobs[static_cast<std::size_t>(k)] = knob(paramid::band(b, bandKnobs[k]), "");
            if (auto& c = row.knobs[static_cast<std::size_t>(k)]) c->setValueBeside(true);
        }
    }
    // Trigger outs.
    for (int t = 0; t < 3; ++t) {
        auto& row = trigs_[static_cast<std::size_t>(t)];
        row.on = toggle(paramid::trig(t, "on"), "TRIG " + juce::String(t + 1));
        row.source = box(paramid::trig(t, "source"), "");
        row.route = box(paramid::trig(t, "route"), "");
        row.out = box(paramid::trig(t, "out"), "");
        row.pan = knob(paramid::trig(t, "pan"), "", false, true);
        row.level = knob(paramid::trig(t, "level"), "");
        for (auto* k : {row.pan.get(), row.level.get()})
            if (k != nullptr) k->setValueBeside(true);
        if (row.on) row.on->setTooltip("Audible trigger out " + juce::String(t + 1) + ": a click on every hit of its source voice");
    }
}

FxView::~FxView() = default;

void FxView::resized() {
    legends_.clear();
    auto legend = [this](juce::Rectangle<int> a, const juce::String& s) { legends_.add({a, s}); };
    auto r = getLocalBounds().reduced(kPad, kPad);
    leftArea_ = r.removeFromLeft(632);
    r.removeFromLeft(kPad);
    rightArea_ = r;

    // Left: buses, returns, the selected effect.
    {
        auto a = leftArea_;
        busBand_ = a.removeFromTop(kBandH);
        a.reduce(6, 0);
        a.removeFromTop(6);
        rack_.setBounds(a.removeFromTop(2 * 44 + 6));
        a.removeFromTop(6);
        returnsArea_ = a.removeFromTop(58);
        {
            auto row = returnsArea_;
            struct Item { juce::Component* c; int w; };
            const Item items[] = {{fx1Level_.get(), 64}, {fx1Out_.get(), 76}, {fx1To2_.get(), 64}, {fx1To2Pre_.get(), 52},
                                  {nullptr, 40},          {fx2Level_.get(), 64}, {fx2Out_.get(), 76}};
            for (const auto& it : items) {
                auto cell = row.removeFromLeft(it.w);
                row.removeFromLeft(10);
                if (it.c == nullptr) continue;
                if (dynamic_cast<ValueBox*>(it.c) != nullptr) it.c->setBounds(cell.withSizeKeepingCentre(it.w, 34));
                else if (dynamic_cast<juce::Button*>(it.c) != nullptr) it.c->setBounds(cell.withSizeKeepingCentre(it.w, 22));
                else it.c->setBounds(cell);
            }
        }
        a.removeFromTop(8);
        editor_.setBounds(a.withTrimmedLeft(-6).withTrimmedRight(-6));
    }

    // Right: master chain and trigger outs.
    {
        auto a = rightArea_;
        masterBand_ = a.removeFromTop(kBandH);
        a.reduce(6, 0);
        a.removeFromTop(4);
        {
            auto row = a.removeFromTop(60);
            auto stage = row.removeFromLeft(196);
            legend(stage.removeFromTop(kLegendH), "OUTPUT STAGE");
            if (stage_) stage_->setBounds(stage.withSizeKeepingCentre(stage.getWidth(), 22));
            row.removeFromLeft(10);
            if (drive_) drive_->setBounds(row.removeFromLeft(62));
            row.removeFromLeft(10);
            if (softClip_) softClip_->setBounds(row.removeFromLeft(84).withSizeKeepingCentre(84, 22));
            row.removeFromLeft(10);
            if (volume_) volume_->setBounds(row.removeFromLeft(70));
        }
        a.removeFromTop(4);
        {
            auto row = a.removeFromTop(60);
            if (limiter_) limiter_->setBounds(row.removeFromLeft(76).withSizeKeepingCentre(76, 22));
            row.removeFromLeft(4);
            for (auto* k : {limThresh_.get(), limAttack_.get(), limRelease_.get()})
                if (k != nullptr) k->setBounds(row.removeFromLeft(58));
            row.removeFromLeft(18);
            if (mcompOn_) mcompOn_->setBounds(row.removeFromLeft(72).withSizeKeepingCentre(72, 22));
            row.removeFromLeft(4);
            for (auto* k : {xLow_.get(), xHigh_.get()})
                if (k != nullptr) k->setBounds(row.removeFromLeft(58));
        }
        a.removeFromTop(4);
        // Band table: column legends, then LOW / MID / HIGH rows.
        {
            auto head = a.removeFromTop(kLegendH);
            auto labelCol = head.removeFromLeft(40);
            legend(labelCol, "BAND");
            head.removeFromLeft(4);
            legend(head.removeFromLeft(48), "");
            head.removeFromLeft(4);
            legend(head.removeFromLeft(52), "");
            head.removeFromLeft(8);
            const char* names[] = {"THRESH", "RATIO", "ATTACK", "RELEASE", "MAKEUP"};
            const int kw = head.getWidth() / 5;
            for (const char* n : names) legend(head.removeFromLeft(kw), n);
            const char* bandNames[] = {"LOW", "MID", "HIGH"};
            for (int b = 0; b < 3; ++b) {
                auto row = a.removeFromTop(38);
                legend(row.removeFromLeft(40), bandNames[b]);
                row.removeFromLeft(4);
                auto& br = bands_[static_cast<std::size_t>(b)];
                if (br.on) br.on->setBounds(row.removeFromLeft(48).withSizeKeepingCentre(48, 20));
                row.removeFromLeft(4);
                if (br.solo) br.solo->setBounds(row.removeFromLeft(52).withSizeKeepingCentre(52, 20));
                row.removeFromLeft(8);
                const int w = row.getWidth() / 5;
                for (auto& k : br.knobs)
                    if (k) k->setBounds(row.removeFromLeft(w));
            }
        }
        a.removeFromTop(10);
        trigBand_ = a.removeFromTop(kBandH).expanded(6, 0);
        a.removeFromTop(3);
        {
            auto head = a.removeFromTop(kLegendH);
            head.removeFromLeft(80);
            const int widths[] = {84, 84, 84};
            const char* names[] = {"SOURCE", "ROUTE", "OUTPUT"};
            for (int i = 0; i < 3; ++i) {
                legend(head.removeFromLeft(widths[i]), names[i]);
                head.removeFromLeft(6);
            }
            const int kw = head.getWidth() / 2;
            legend(head.removeFromLeft(kw), "PAN");
            legend(head, "LEVEL");
            for (int t = 0; t < 3; ++t) {
                auto row = a.removeFromTop(38);
                auto& tr = trigs_[static_cast<std::size_t>(t)];
                if (tr.on) tr.on->setBounds(row.removeFromLeft(74).withSizeKeepingCentre(74, 21));
                row.removeFromLeft(6);
                for (auto* b : {tr.source.get(), tr.route.get(), tr.out.get()}) {
                    if (b != nullptr) b->setBounds(row.removeFromLeft(84).withSizeKeepingCentre(84, 22));
                    row.removeFromLeft(6);
                }
                const int w = row.getWidth() / 2;
                if (tr.pan) tr.pan->setBounds(row.removeFromLeft(w));
                if (tr.level) tr.level->setBounds(row);
            }
        }
    }
    invalidateCache();
}

void FxView::paintStatic(juce::Graphics& g) {
    g.fillAll(C::panel);
    mw909LookAndFeel::drawPlate(g, leftArea_.toFloat(), C::panelLight, C::panelEdge, 0.0f);
    mw909LookAndFeel::drawPlate(g, rightArea_.toFloat(), C::panelLight, C::panelEdge, 0.0f);
    mw909LookAndFeel::drawBand(g, busBand_, "FX BUSES", juce::Justification::centredLeft);
    mw909LookAndFeel::drawBand(g, masterBand_, "MASTER CHAIN", juce::Justification::centredLeft);
    mw909LookAndFeel::drawBand(g, trigBand_, "TRIGGER OUTS", juce::Justification::centredLeft);
    mw909LookAndFeel::drawRuleTitle(g, "RETURNS", returnsArea_.withHeight(1).translated(0, -4).withHeight(8));
    for (const auto& l : legends_)
        if (l.second.isNotEmpty()) mw909LookAndFeel::drawLegend(g, l.second, l.first, C::inkDim, 8.5f);
}

}  // namespace mw909::ui
