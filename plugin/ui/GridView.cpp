// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "GridView.h"

namespace mw909::ui {

namespace C = mw909Colours;

GridView::GridView(Context& ctx) : ctx_(ctx) {
    setOpaque(true);
    lanePlayhead_.fill(-1);
    ctx_.processor.patterns().addListener(this);
    ctx_.state.addListener(this);
    shownVariation_ = ctx_.editVariation();
}

GridView::~GridView() {
    ctx_.state.removeListener(this);
    ctx_.processor.patterns().removeListener(this);
}

void GridView::resized() {
    auto r = getLocalBounds().reduced(6);
    headerArea_ = r.removeFromLeft(kHeaderWidth);
    r.removeFromLeft(4);
    gridArea_ = r;
}

int GridView::columns() const {
    const auto& p = pattern();
    int cols = p.length;
    for (int t = 0; t < kNumVoices; ++t) cols = juce::jmax(cols, p.lengthOf(t));
    // Whole pages, so the layout is stable while editing within a page.
    return juce::jlimit(16, seq::kMaxSteps, ((cols + 15) / 16) * 16);
}

juce::Rectangle<int> GridView::columnBounds(int step) const {
    const int cols = shownColumns_;
    const int x0 = gridArea_.getX() + gridArea_.getWidth() * step / cols;
    const int x1 = gridArea_.getX() + gridArea_.getWidth() * (step + 1) / cols;
    return {x0, gridArea_.getY(), x1 - x0, gridArea_.getHeight()};
}

juce::Rectangle<int> GridView::laneBounds(int track) const {
    const auto grid = gridArea_.withTrimmedTop(kTopRuler);
    const int y0 = grid.getY() + grid.getHeight() * track / seq::kNumTracks;
    const int y1 = grid.getY() + grid.getHeight() * (track + 1) / seq::kNumTracks;
    return {headerArea_.getX(), y0, headerArea_.getWidth(), y1 - y0};
}

juce::Rectangle<int> GridView::cellBounds(int track, int step) const {
    const auto col = columnBounds(step);
    const auto lane = laneBounds(track);
    return {col.getX(), lane.getY(), col.getWidth(), lane.getHeight()};
}

bool GridView::cellAt(juce::Point<int> pos, int& track, int& step) const {
    const auto grid = gridArea_.withTrimmedTop(kTopRuler);
    if (!grid.contains(pos)) return false;
    const int cols = shownColumns_;
    step = juce::jlimit(0, cols - 1, (pos.x - gridArea_.getX()) * cols / juce::jmax(1, gridArea_.getWidth()));
    track = juce::jlimit(0, seq::kNumTracks - 1, (pos.y - grid.getY()) * seq::kNumTracks / juce::jmax(1, grid.getHeight()));
    return true;
}

int GridView::laneAt(juce::Point<int> pos) const {
    const auto header = headerArea_.withTrimmedTop(kTopRuler);
    if (!header.contains(pos)) return -1;
    return juce::jlimit(0, seq::kNumTracks - 1, (pos.y - header.getY()) * seq::kNumTracks / juce::jmax(1, header.getHeight()));
}

void GridView::paint(juce::Graphics& g) {
    g.fillAll(C::panel);
    const auto& p = pattern();
    const int var = ctx_.editVariation();
    shownColumns_ = columns();
    const int cols = shownColumns_;
    const int selTrack = ctx_.state.selectedTrack();
    const StepMode mode = ctx_.state.stepMode();
    const auto clip = g.getClipBounds();

    // Header corner: pattern, variation, mode.
    if (clip.intersects(headerArea_.withHeight(kTopRuler))) {
        g.setColour(C::inkDim);
        g.setFont(mw909LookAndFeel::legend(9.0f));
        g.drawFittedText(patternLabel(selectedPattern()) + (var == 0 ? " A" : " B"), headerArea_.withHeight(kTopRuler),
                         juce::Justification::centredLeft, 1, 0.8f);
    }
    // Ruler: step numbers (every step when there is room, else every 4th).
    const juce::Font inRuler = mw909LookAndFeel::bold(9.0f), outRuler = mw909LookAndFeel::text(9.0f);
    const bool every = gridArea_.getWidth() / cols >= 16;
    for (int s = 0; s < cols; ++s) {
        const auto ruler = columnBounds(s).withHeight(kTopRuler);
        if (!clip.intersects(ruler)) continue;
        if (!every && s % 4 != 0) continue;
        // Past LAST STEP: plain weight, still AA contrast (inkDim ~5.8:1).
        g.setFont(s < p.length ? inRuler : outRuler);
        g.setColour(s < p.length ? C::ink : C::inkDim);
        g.drawText(juce::String(s + 1), every ? ruler : ruler.withWidth(ruler.getWidth() * 3), every ? juce::Justification::centred
                                                                                                      : juce::Justification::centredLeft,
                   false);
    }

    for (int t = 0; t < seq::kNumTracks; ++t) {
        const int len = p.lengthOf(t);
        const bool selected = t == selTrack;
        // Lane header: a dark key with the voice code in orange (the selected
        // lane's header is orange with dark text).
        const auto lane = laneBounds(t);
        if (clip.intersects(lane)) {
            const auto key = lane.reduced(0, 1).toFloat();
            g.setColour(selected ? C::orange : C::band);
            g.fillRoundedRectangle(key, 2.0f);
            g.setColour(selected ? C::ink : C::legend);
            g.setFont(mw909LookAndFeel::bold(juce::jlimit(9.0f, 12.0f, static_cast<float>(lane.getHeight()) * 0.42f)));
            g.drawText(voiceCode(t), lane.reduced(8, 0), juce::Justification::centredLeft, false);
            if (t < kNumVoices && p.trackLength[static_cast<std::size_t>(t)] != 0) {
                // A lane with its own length shows it.
                g.setColour(selected ? C::ink : C::cream);
                g.setFont(mw909LookAndFeel::text(9.5f));
                g.drawText(juce::String(len), lane.reduced(6, 0), juce::Justification::centredRight, false);
            }
        }
        const int head = lanePlayhead_[static_cast<std::size_t>(t)];
        for (int s = 0; s < cols; ++s) {
            const auto cell = cellBounds(t, s);
            if (!clip.intersects(cell)) continue;
            const bool inRange = s < len;
            const seq::Step step = p.at(var, t, s);
            const auto inner = cell.reduced(1, 1).toFloat();
            juce::Colour face;
            if (!inRange) face = C::cellOutside;
            else if (!step.on()) face = (s / 4) % 2 == 0 ? C::cellOff : C::cellOffAlt;
            else {
                const auto level = step.level();
                face = level == seq::StepLevel::Weak ? C::orangeSoft : level == seq::StepLevel::Normal ? C::orange : C::orangeDeep;
            }
            g.setColour(face);
            g.fillRoundedRectangle(inner, 2.0f);
            if (inRange && step.on() && step.level() == seq::StepLevel::Accent) {
                // Accent: a dark bar along the foot.
                auto bar = inner.reduced(inner.getWidth() * 0.2f, 0.0f);
                bar = bar.removeFromBottom(juce::jmax(2.0f, inner.getHeight() * 0.16f)).translated(0.0f, -1.5f);
                g.setColour(C::ink.withAlpha(0.75f));
                g.fillRoundedRectangle(bar, 1.0f);
            }
            if (selected && inRange) {
                g.setColour(C::orangeDeep.withAlpha(0.55f));
                g.drawRoundedRectangle(inner.reduced(0.5f), 2.0f, 1.0f);
            }
            if (t == hoverTrack_ && s == hoverStep_) {
                g.setColour(C::hover);
                g.fillRoundedRectangle(inner, 2.0f);
            }
            if (s == head) {
                // This lane's playhead.
                g.setColour(C::ink);
                g.drawRoundedRectangle(inner.reduced(0.5f), 2.0f, 2.0f);
                g.setColour(juce::Colours::white.withAlpha(step.on() && inRange ? 0.18f : 0.45f));
                g.fillRoundedRectangle(inner.reduced(2.5f), 1.5f);
            }
            const juce::String badge = inRange ? stepedit::badge(step, mode, t) : juce::String();
            if (badge.isNotEmpty() && cell.getWidth() >= 14) {
                const auto font = mw909LookAndFeel::bold(juce::jlimit(7.0f, 10.0f, static_cast<float>(cell.getHeight()) * 0.4f));
                g.setFont(font);
                if (face == C::orangeDeep) {
                    // Neither ink nor cream reads on the accent face: a cream chip carries the badge.
                    const float w = juce::jmin(inner.getWidth() - 2.0f,
                                               juce::GlyphArrangement::getStringWidth(font, badge) + 4.0f);
                    const auto chip = juce::Rectangle<float>(w, font.getHeight() + 1.0f).withCentre(inner.getCentre());
                    g.setColour(C::cream);
                    g.fillRoundedRectangle(chip, 2.0f);
                    g.setColour(C::ink);
                } else {
                    g.setColour(C::textOn(face));
                }
                g.drawFittedText(badge, cell.reduced(1), juce::Justification::centred, 1, 0.5f);
            }
        }
    }
    // Beat rules every 4 steps.
    g.setColour(C::panelEdge);
    for (int s = 4; s < cols; s += 4) {
        const auto col = columnBounds(s);
        if (clip.intersects(col)) g.fillRect(col.getX(), gridArea_.getY() + kTopRuler, 1, gridArea_.getHeight() - kTopRuler);
    }
}

void GridView::poll(const PlayState& ps) {
    const bool playing = ps.playing.load(std::memory_order_relaxed);
    // Only the pattern on screen gets playheads (not a fill or another pattern).
    const bool showing = playing && ps.currentPattern.load(std::memory_order_relaxed) == selectedPattern();
    const auto pos = ps.position.load(std::memory_order_relaxed);
    const int patternStep = static_cast<int>(pos & 0xffU) - 1;
    const auto measure = static_cast<std::int64_t>(pos >> 8);
    const int var = ctx_.editVariation();
    const auto& p = pattern();

    std::array<int, seq::kNumTracks> heads{};
    for (int t = 0; t < seq::kNumTracks; ++t) {
        int h = -1;
        if (showing && patternStep >= 0) {
            const int len = juce::jmax(1, p.lengthOf(t));
            h = len == p.length ? patternStep : static_cast<int>((measure * static_cast<std::int64_t>(p.length) + patternStep) % len);
        }
        heads[static_cast<std::size_t>(t)] = h;
    }
    if (var != shownVariation_ || columns() != shownColumns_) {
        shownVariation_ = var;
        lanePlayhead_ = heads;
        repaint();
        return;
    }
    for (int t = 0; t < seq::kNumTracks; ++t) {
        const int old = lanePlayhead_[static_cast<std::size_t>(t)];
        const int now = heads[static_cast<std::size_t>(t)];
        if (old == now) continue;
        lanePlayhead_[static_cast<std::size_t>(t)] = now;
        if (old >= 0 && old < shownColumns_) repaint(cellBounds(t, old));
        if (now >= 0 && now < shownColumns_) repaint(cellBounds(t, now));
    }
}

void GridView::patternChanged(int index) {
    if (index == selectedPattern()) repaint();
}

void GridView::mouseDown(const juce::MouseEvent& e) {
    int track, step;
    const int lane = laneAt(e.getPosition());
    if (lane >= 0) {
        ctx_.state.setSelectedTrack(lane);
        if (lane < kNumVoices && !e.mods.isPopupMenu()) ctx_.processor.audition(static_cast<VoiceId>(lane), e.mods.isShiftDown());
        return;
    }
    if (!cellAt(e.getPosition(), track, step)) return;
    if (step >= pattern().lengthOf(track)) return;
    if (e.mods.isPopupMenu()) {
        stepedit::showStepMenu(ctx_, selectedPattern(), ctx_.editVariation(), track, step, *this);
        return;
    }
    const StepMode mode = ctx_.state.stepMode();
    const bool shift = e.mods.isShiftDown();
    const bool authentic = ctx_.state.authentic();
    const int var = ctx_.editVariation();
    seq::Step result{};
    ctx_.editPattern(selectedPattern(), [&](seq::Pattern& p) {
        seq::Step& s = p.at(var, track, step);
        s = result = stepedit::clicked(s, mode, track, shift, authentic);
    });
    painting_ = true;
    brush_ = result;
    paintTrack_ = track;
    lastPainted_ = step;
}

void GridView::mouseDrag(const juce::MouseEvent& e) {
    if (!painting_) return;
    int track, step;
    if (!cellAt(e.getPosition(), track, step)) return;
    track = paintTrack_;  // paint along the lane the drag started in
    if (step == lastPainted_ || step >= pattern().lengthOf(track)) return;
    lastPainted_ = step;
    const StepMode mode = ctx_.state.stepMode();
    const int var = ctx_.editVariation();
    const seq::Step brush = brush_;
    ctx_.editPattern(selectedPattern(), [&](seq::Pattern& p) {
        seq::Step& s = p.at(var, track, step);
        s = stepedit::painted(s, brush, mode, track);
    });
}

void GridView::mouseUp(const juce::MouseEvent&) { painting_ = false; }

void GridView::mouseMove(const juce::MouseEvent& e) {
    int track = -1, step = -1;
    if (!cellAt(e.getPosition(), track, step)) { track = -1; step = -1; }
    if (track == hoverTrack_ && step == hoverStep_) return;
    if (hoverTrack_ >= 0) repaint(cellBounds(hoverTrack_, hoverStep_));
    hoverTrack_ = track;
    hoverStep_ = step;
    if (hoverTrack_ >= 0) repaint(cellBounds(hoverTrack_, hoverStep_));
}

void GridView::mouseExit(const juce::MouseEvent&) {
    if (hoverTrack_ >= 0) repaint(cellBounds(hoverTrack_, hoverStep_));
    hoverTrack_ = hoverStep_ = -1;
}

}  // namespace mw909::ui
