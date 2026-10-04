// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "MixerView.h"

#include <cmath>

namespace mw909::ui {

namespace C = mw909Colours;
namespace paramid = plugin::paramid;
using paramid::VoiceParam;

namespace {
constexpr int kPad = 6;
constexpr int kGap = 3;

std::unique_ptr<ParamControl> makeKnob(Context& ctx, juce::Component& parent, const juce::String& id, const juce::String& legend,
                                       bool bipolar = false) {
    auto* p = ctx.param(id);
    if (p == nullptr) return {};
    auto c = std::make_unique<ParamControl>(ctx, *p, ParamControl::Style::Knob, legend, false);
    c->setBipolar(bipolar);
    c->setLegendHeight(8.5f);
    parent.addAndMakeVisible(*c);
    return c;
}

std::unique_ptr<ParamButton> makeToggle(Context& ctx, juce::Component& parent, const juce::String& id, const juce::String& label) {
    auto* p = ctx.param(id);
    if (p == nullptr) return {};
    auto b = std::make_unique<ParamButton>(ctx, *p, label, C::orange, true);
    b->getProperties().set("fontHeight", 8.5);
    parent.addAndMakeVisible(*b);
    return b;
}

juce::String vid(int voice, VoiceParam p) { return juce::String(paramid::voice(voice, p)); }
}  // namespace

// --- LevelMeter ------------------------------------------------------------------------------

LevelMeter::LevelMeter(int channels) : channels_(juce::jlimit(1, 2, channels)) { setInterceptsMouseClicks(false, false); }

float LevelMeter::toDisplay(float linear) noexcept {
    if (linear <= 1.0e-5f) return 0.0f;
    return juce::jlimit(0.0f, 1.0f, (20.0f * std::log10(linear) + 60.0f) / 66.0f);
}

void LevelMeter::setPeak(int channel, float linear) {
    if (channel < 0 || channel >= channels_) return;
    const auto c = static_cast<std::size_t>(channel);
    const float v = toDisplay(linear);
    const float fall = 0.025f;
    const float next = v > shown_[c] ? v : juce::jmax(v, shown_[c] - fall);
    bool changed = std::abs(next - shown_[c]) > 0.004f;
    shown_[c] = next;
    if (next >= held_[c]) {
        if (next > held_[c]) changed = true;
        held_[c] = next;
        holdAge_[c] = 0;
    } else if (++holdAge_[c] > 30) {
        held_[c] = juce::jmax(next, held_[c] - fall * 2.0f);
        changed = true;
    }
    if (changed) repaint();
}

void LevelMeter::paint(juce::Graphics& g) {
    const auto r = getLocalBounds();
    mw909LookAndFeel::drawInset(g, r.toFloat(), 1.5f);
    auto inner = r.reduced(2, 2);
    const int gap = 1;
    const int barW = (inner.getWidth() - gap * (channels_ - 1)) / channels_;
    for (int c = 0; c < channels_; ++c) {
        auto bar = inner.removeFromLeft(barW);
        inner.removeFromLeft(gap);
        const float h = static_cast<float>(bar.getHeight());
        const float level = shown_[static_cast<std::size_t>(c)];
        const float y12 = h * (48.0f / 66.0f), y0 = h * (60.0f / 66.0f);
        auto seg = [&](float from, float to, juce::Colour col) {
            const float a = juce::jlimit(0.0f, h * level, from), b = juce::jlimit(0.0f, h * level, to);
            if (b <= a) return;
            g.setColour(col);
            g.fillRect(static_cast<float>(bar.getX()), static_cast<float>(bar.getBottom()) - b, static_cast<float>(bar.getWidth()), b - a);
        };
        seg(0.0f, y12, C::meterLow);
        seg(y12, y0, C::meterMid);
        seg(y0, h, C::meterHot);
        const float held = held_[static_cast<std::size_t>(c)];
        if (held > 0.01f) {
            g.setColour(held > 60.0f / 66.0f ? C::meterHot : C::cream);
            g.fillRect(static_cast<float>(bar.getX()), static_cast<float>(bar.getBottom()) - h * held, static_cast<float>(bar.getWidth()), 1.5f);
        }
    }
}

// --- StripSummary -----------------------------------------------------------------------------

StripSummary::StripSummary(Context& ctx, int voice) : ctx_(ctx) {
    setRepaintsOnMouseActivity(false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip("Channel strip: EQ, cuts, resonance, two bands, compressor. Click to expand.");
    if (auto* p = ctx_.param(vid(voice, VoiceParam::EqOn)))
        eqAttachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float v) { eqOn_ = v >= 0.5f; repaint(); });
    if (auto* p = ctx_.param(vid(voice, VoiceParam::CompOn)))
        compAttachment_ = std::make_unique<juce::ParameterAttachment>(*p, [this](float v) { compOn_ = v >= 0.5f; repaint(); });
    if (eqAttachment_) eqAttachment_->sendInitialUpdate();
    if (compAttachment_) compAttachment_->sendInitialUpdate();
}

StripSummary::~StripSummary() {
    compAttachment_.reset();
    eqAttachment_.reset();
}

void StripSummary::paint(juce::Graphics& g) {
    const auto r = getLocalBounds().toFloat().reduced(0.5f).withTrimmedBottom(1.0f);
    mw909LookAndFeel::drawButtonFace(g, r, expanded_, hover_, false, C::orange);
    auto inner = getLocalBounds().reduced(5, 3);
    auto title = inner.removeFromTop(inner.getHeight() / 2);
    g.setColour(C::ink);
    g.setFont(mw909LookAndFeel::legend(8.5f));
    g.drawFittedText(expanded_ ? "STRIP  <" : "STRIP  >", title, juce::Justification::centred, 1, 0.8f);
    const int half = inner.getWidth() / 2;
    auto item = [&](juce::Rectangle<int> a, const char* text, bool on) {
        mw909LookAndFeel::drawLamp(g, a.removeFromLeft(9).withSizeKeepingCentre(6, 6).toFloat(), on ? 1.0f : 0.0f);
        a.removeFromLeft(2);
        g.setColour(C::ink);
        g.setFont(mw909LookAndFeel::legend(8.0f));
        g.drawFittedText(text, a, juce::Justification::centredLeft, 1, 0.75f);
    };
    item(inner.removeFromLeft(half), "EQ", eqOn_);
    item(inner, "CMP", compOn_);
}

void StripSummary::mouseUp(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu() || !getLocalBounds().contains(e.getPosition())) return;
    if (onToggle) onToggle();
}

// --- StripPanel ---------------------------------------------------------------------------------

StripPanel::StripPanel(Context& ctx, int voice) : ctx_(ctx), voice_(voice) {
    setOpaque(true);
    eqOn_ = makeToggle(ctx_, *this, vid(voice, VoiceParam::EqOn), "ON");
    lowCut_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::LowCut), "LOW CUT");
    highCut_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::HighCut), "HIGH CUT");
    reso_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::Reso), "RESO");
    band1On_ = makeToggle(ctx_, *this, vid(voice, VoiceParam::Eq1On), "ON");
    band2On_ = makeToggle(ctx_, *this, vid(voice, VoiceParam::Eq2On), "ON");
    const VoiceParam b1[] = {VoiceParam::Eq1Freq, VoiceParam::Eq1Width, VoiceParam::Eq1Gain};
    const VoiceParam b2[] = {VoiceParam::Eq2Freq, VoiceParam::Eq2Width, VoiceParam::Eq2Gain};
    const char* names[] = {"FREQ", "WIDTH", "GAIN"};
    for (int i = 0; i < 3; ++i) {
        band1_[static_cast<std::size_t>(i)] = makeKnob(ctx_, *this, vid(voice, b1[i]), names[i], i == 2);
        band2_[static_cast<std::size_t>(i)] = makeKnob(ctx_, *this, vid(voice, b2[i]), names[i], i == 2);
    }
    compOn_ = makeToggle(ctx_, *this, vid(voice, VoiceParam::CompOn), "ON");
    compPre_ = makeToggle(ctx_, *this, vid(voice, VoiceParam::CompPre), "PRE EQ");
    thresh_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::CompThresh), "THRESH");
    ratio_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::CompRatio), "RATIO");
    attack_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::CompAttack), "ATTACK");
    release_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::CompRelease), "RELEASE");
    makeup_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::CompMakeup), "MAKEUP");
    if (eqOn_) eqOn_->setTooltip("EQ section on (cuts, resonance, bands)");
    if (compPre_) compPre_->setTooltip("Compressor before the EQ");
}

void StripPanel::resized() {
    auto r = getLocalBounds().reduced(6, 4);
    const int rowH = 58;
    eqBand_ = r.removeFromTop(18);
    if (eqOn_) eqOn_->setBounds(eqBand_.withTrimmedLeft(eqBand_.getWidth() - 46).reduced(2, 2).translated(-2, -1));
    r.removeFromTop(4);
    layoutRow(r.removeFromTop(rowH), {lowCut_.get(), highCut_.get(), reso_.get()}, 4);
    r.removeFromTop(2);
    auto bandRow = [&](std::unique_ptr<ParamButton>& on, std::array<std::unique_ptr<ParamControl>, 3>& knobs, juce::Rectangle<int>& label) {
        auto row = r.removeFromTop(rowH);
        auto left = row.removeFromLeft(44);
        label = left.removeFromTop(16);
        if (on) on->setBounds(left.removeFromTop(20).reduced(0, 1));
        row.removeFromLeft(4);
        layoutRow(row, {knobs[0].get(), knobs[1].get(), knobs[2].get()}, 2);
    };
    bandRow(band1On_, band1_, band1Label_);
    r.removeFromTop(2);
    bandRow(band2On_, band2_, band2Label_);
    r.removeFromTop(8);
    compBand_ = r.removeFromTop(18);
    {
        auto b = compBand_.withTrimmedLeft(compBand_.getWidth() - 112).reduced(2, 2).translated(-2, -1);
        if (compPre_) compPre_->setBounds(b.removeFromRight(60));
        b.removeFromRight(4);
        if (compOn_) compOn_->setBounds(b);
    }
    r.removeFromTop(4);
    layoutRow(r.removeFromTop(rowH), {thresh_.get(), ratio_.get(), attack_.get()}, 4);
    r.removeFromTop(2);
    auto last = r.removeFromTop(rowH);
    layoutRow(last.withWidth(last.getWidth() * 2 / 3), {release_.get(), makeup_.get()}, 4);
    invalidateCache();
}

void StripPanel::paintStatic(juce::Graphics& g) {
    g.fillAll(C::panelLight);
    g.setColour(C::panelEdge);
    g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));
    mw909LookAndFeel::drawBand(g, eqBand_, voiceCode(voice_) + "  EQ", juce::Justification::centredLeft, 9.5f);
    mw909LookAndFeel::drawBand(g, compBand_, "COMP", juce::Justification::centredLeft, 9.5f);
    mw909LookAndFeel::drawLegend(g, "BAND 1", band1Label_, C::inkDim, 8.5f, juce::Justification::centredLeft);
    mw909LookAndFeel::drawLegend(g, "BAND 2", band2Label_, C::inkDim, 8.5f, juce::Justification::centredLeft);
}

// --- MixerChannel -------------------------------------------------------------------------

MixerChannel::MixerChannel(Context& ctx, int voice) : ctx_(ctx), voice_(voice), summary_(ctx, voice), plate_(voice) {
    auto box = [&](VoiceParam p, const juce::String& legend, const juce::String& tip) {
        auto* param = ctx_.param(vid(voice, p));
        if (param == nullptr) return std::unique_ptr<ParamValueBox>();
        auto b = std::make_unique<ParamValueBox>(ctx_, *param, legend, true);
        b->setLegendHeight(10);
        b->setTooltip(tip);
        addAndMakeVisible(*b);
        return b;
    };
    out_ = box(VoiceParam::Out, "OUT", "Output bus. A voice on an individual out leaves the main mix.");
    route_ = box(VoiceParam::Route, "ROUTE", "Where the channel goes: its output, or wholly into FX 1 / FX 2");
    sendBus_ = box(VoiceParam::SendBus, "BUS", "Aux send to an FX bus");
    if (route_) {
        auto* routeBox = route_.get();
        routeBox->format = [routeBox](int v) {
            return juce::String(v == 0 ? (routeBox->getWidth() >= 52 ? "Direct" : "Dir") : v == 1 ? "FX 1" : "FX 2");
        };
    }
    sendPre_ = makeToggle(ctx_, *this, vid(voice, VoiceParam::SendPre), "PRE");
    if (sendPre_) {
        sendPre_->setTooltip("Send before the fader");
        sendPre_->getProperties().set("lamp", false);  // a lit face: the narrow channels have no room for a lamp
    }
    send_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::Send), "SEND");
    pan_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::Pan), "PAN", true);
    spread_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::Spread), "SPREAD");
    drive_ = makeKnob(ctx_, *this, vid(voice, VoiceParam::Drive), "DRIVE");
    if (auto* p = ctx_.param(vid(voice, VoiceParam::Level))) {
        level_ = std::make_unique<ParamControl>(ctx_, *p, ParamControl::Style::Fader, "", true);
        addAndMakeVisible(*level_);
    }
    summary_.onToggle = [this] { if (onStripToggle) onStripToggle(); };
    addAndMakeVisible(summary_);
    addAndMakeVisible(lamp_);
    addAndMakeVisible(meter_);
    if (auto* m = ctx_.param(vid(voice, VoiceParam::Mute))) {
        mute_ = std::make_unique<ParamButton>(ctx_, *m, "M", C::mute);
        addAndMakeVisible(*mute_);
    }
    if (auto* s = ctx_.param(vid(voice, VoiceParam::Solo))) {
        solo_ = std::make_unique<ParamButton>(ctx_, *s, "S", C::solo);
        addAndMakeVisible(*solo_);
    }
    plate_.onAudition = [this](bool accent) { ctx_.processor.audition(static_cast<VoiceId>(voice_), accent); };
    plate_.onSelect = [this] { ctx_.state.setSelectedTrack(voice_); };
    addAndMakeVisible(plate_);
    setInterceptsMouseClicks(false, true);
}

void MixerChannel::setExpanded(bool expanded) {
    if (expanded == expanded_) return;
    expanded_ = expanded;
    summary_.setExpanded(expanded);
    if (expanded) {
        strip_ = std::make_unique<StripPanel>(ctx_, voice_);
        addAndMakeVisible(*strip_);
    } else {
        strip_.reset();
    }
    resized();
}

void MixerChannel::resized() {
    auto r = getLocalBounds();
    if (strip_) strip_->setBounds(r.removeFromRight(kStripWidth).withTrimmedTop(kBandH));
    r = r.reduced(4, 0);
    r.removeFromTop(kBandH + 4);
    const int w = r.getWidth();
    {
        auto row = r.removeFromTop(32);
        const int half = (w - 4) / 2;
        if (out_) out_->setBounds(row.removeFromLeft(half));
        row.removeFromLeft(4);
        if (sendBus_) sendBus_->setBounds(row);
    }
    r.removeFromTop(4);
    {
        auto row = r.removeFromTop(32);
        const int routeW = (w - 4) * 62 / 100;
        if (route_) route_->setBounds(row.removeFromLeft(routeW));
        row.removeFromLeft(4);
        if (sendPre_) sendPre_->setBounds(row.removeFromBottom(21));
    }
    r.removeFromTop(4);
    {
        const int rowH = 54;
        auto row1 = r.removeFromTop(rowH);
        layoutRow(row1, {send_.get(), pan_.get()}, 2);
        auto row2 = r.removeFromTop(rowH);
        layoutRow(row2, {spread_.get(), drive_.get()}, 2);
    }
    r.removeFromTop(4);
    summary_.setBounds(r.removeFromTop(34));
    auto plate = r.removeFromBottom(26);
    plate_.setBounds(plate);
    r.removeFromBottom(5);
    r.removeFromTop(6);
    {
        auto row = r.removeFromTop(20);
        lamp_.setBounds(row.removeFromLeft(12).withSizeKeepingCentre(9, 9));
        row.removeFromLeft(3);
        const int bw = (row.getWidth() - 3) / 2;
        if (mute_) mute_->setBounds(row.removeFromLeft(bw));
        row.removeFromLeft(3);
        if (solo_) solo_->setBounds(row);
    }
    r.removeFromTop(6);
    auto meter = r.removeFromRight(8);
    meter_.setBounds(meter.reduced(0, 4));
    if (level_) level_->setBounds(r);
}

// --- MasterColumn -------------------------------------------------------------------------------

MasterColumn::MasterColumn(Context& ctx) : ctx_(ctx) {
    fx1_ = makeKnob(ctx_, *this, paramid::fxBus(0, "level"), "FX 1 RET");
    fx2_ = makeKnob(ctx_, *this, paramid::fxBus(1, "level"), "FX 2 RET");
    if (auto* p = ctx_.param(paramid::masterVolume)) {
        volume_ = std::make_unique<ParamControl>(ctx_, *p, ParamControl::Style::Fader, "VOLUME", true);
        volume_->setLegendHeight(9.0f);
        addAndMakeVisible(*volume_);
    }
    addAndMakeVisible(meter_);
    setInterceptsMouseClicks(false, true);
}

void MasterColumn::resized() {
    auto r = getLocalBounds().reduced(6, 0);
    r.removeFromTop(MixerChannel::kBandH + 6);
    if (fx1_) fx1_->setBounds(r.removeFromTop(60).reduced(8, 0));
    r.removeFromTop(4);
    if (fx2_) fx2_->setBounds(r.removeFromTop(60).reduced(8, 0));
    r.removeFromTop(10);
    r.removeFromBottom(6);
    auto meter = r.removeFromRight(14);
    meter_.setBounds(meter.withTrimmedTop(16).reduced(0, 4));
    if (volume_) volume_->setBounds(r);
}

// --- MixerView -----------------------------------------------------------------------------

MixerView::MixerView(Context& ctx) : ctx_(ctx), master_(ctx), solos_(ctx) {
    setOpaque(true);
    for (int v = 0; v < kNumVoices; ++v) {
        auto* ch = channels_.add(new MixerChannel(ctx_, v));
        ch->onStripToggle = [this, v] { setExpanded(expanded_ == v ? -1 : v); };
        addAndMakeVisible(ch);
    }
    addAndMakeVisible(master_);
    ctx_.state.addListener(this);
    uiStateChanged();
}

MixerView::~MixerView() { ctx_.state.removeListener(this); }

void MixerView::setExpanded(int voice) {
    voice = voice >= 0 && voice < kNumVoices ? voice : -1;
    if (voice == expanded_) return;
    expanded_ = voice;
    for (int v = 0; v < channels_.size(); ++v) channels_[v]->setExpanded(v == expanded_);
    resized();
    repaint();
}

void MixerView::resized() {
    auto r = getLocalBounds().reduced(kPad, kPad);
    master_.setBounds(r.removeFromRight(kMasterWidth));
    r.removeFromRight(kPad);
    const int n = channels_.size();
    const int extra = expanded_ >= 0 ? MixerChannel::kStripWidth : 0;
    // Equal widths; the remainder pixels are spread one per channel.
    const int avail = r.getWidth() - extra - kGap * (n - 1);
    int x = r.getX();
    for (int i = 0; i < n; ++i) {
        const int w = avail * (i + 1) / n - avail * i / n + (i == expanded_ ? extra : 0);
        channels_[i]->setBounds(x, r.getY(), w, r.getHeight());
        x += w + kGap;
    }
    invalidateCache();
}

void MixerView::paintStatic(juce::Graphics& g) {
    g.fillAll(C::panel);
    const int bandH = MixerChannel::kBandH;
    for (int v = 0; v < channels_.size(); ++v) {
        const auto b = channels_[v]->getBounds();
        mw909LookAndFeel::drawPlate(g, b.toFloat(), C::panelLight, C::panelEdge, 0.0f);
        const bool wide = b.getWidth() - (channels_[v]->expanded() ? MixerChannel::kStripWidth : 0) >= 96;
        mw909LookAndFeel::drawBand(g, b.withHeight(bandH), wide ? voiceName(v) : voiceCode(v), juce::Justification::centred, 9.0f);
    }
    const auto m = master_.getBounds();
    mw909LookAndFeel::drawPlate(g, m.toFloat(), C::panelLight, C::panelEdge, 0.0f);
    mw909LookAndFeel::drawBand(g, m.withHeight(bandH), "MASTER", juce::Justification::centred, 9.0f);
}

void MixerView::uiStateChanged() {
    const int sel = ctx_.state.selectedTrack();
    for (int v = 0; v < channels_.size(); ++v) channels_[v]->setSelected(v == sel);
}

void MixerView::poll(const PlayState& ps) {
    for (int v = 0; v < kNumVoices; ++v) {
        const auto h = ps.hits[static_cast<std::size_t>(v)].load(std::memory_order_relaxed);
        if (hitsPrimed_ && h != lastHits_[static_cast<std::size_t>(v)]) channels_[v]->flash();
        lastHits_[static_cast<std::size_t>(v)] = h;
        channels_[v]->tick();
        channels_[v]->setPeak(ps.channelPeak[static_cast<std::size_t>(v)].load(std::memory_order_relaxed));
    }
    master_.setPeaks(ps.peakLeft.load(std::memory_order_relaxed), ps.peakRight.load(std::memory_order_relaxed));
    hitsPrimed_ = true;
    const auto muted = solos_.impliedMutes();
    for (int v = 0; v < kNumVoices; ++v) channels_[v]->setImpliedMute(muted[static_cast<std::size_t>(v)]);
}

}  // namespace mw909::ui
