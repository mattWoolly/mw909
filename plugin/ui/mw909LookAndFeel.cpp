// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "mw909LookAndFeel.h"

#include <cmath>

namespace C = mw909Colours;

namespace {

constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

juce::Point<float> polar(juce::Point<float> centre, float radius, float angle) noexcept {
    return {centre.x + std::sin(angle) * radius, centre.y - std::cos(angle) * radius};
}

}  // namespace

mw909LookAndFeel::mw909LookAndFeel() {
    setColour(juce::ResizableWindow::backgroundColourId, C::panel);
    setColour(juce::TextButton::buttonColourId, C::buttonFace);
    setColour(juce::TextButton::buttonOnColourId, C::orange);
    setColour(juce::TextButton::textColourOffId, C::ink);
    setColour(juce::TextButton::textColourOnId, C::ink);
    setColour(juce::Label::textColourId, C::ink);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::outlineWhenEditingColourId, C::orange);
    setColour(juce::Label::backgroundWhenEditingColourId, C::inset);
    setColour(juce::Label::textWhenEditingColourId, C::cream);
    setColour(juce::TextEditor::backgroundColourId, C::inset);
    setColour(juce::TextEditor::textColourId, C::cream);
    setColour(juce::TextEditor::highlightColourId, C::orange);
    setColour(juce::TextEditor::highlightedTextColourId, C::ink);
    setColour(juce::TextEditor::outlineColourId, C::bandEdge);
    setColour(juce::TextEditor::focusedOutlineColourId, C::orange);
    setColour(juce::CaretComponent::caretColourId, C::orange);
    setColour(juce::PopupMenu::backgroundColourId, C::band);
    setColour(juce::PopupMenu::textColourId, C::cream);
    setColour(juce::PopupMenu::headerTextColourId, C::legend);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, C::orange);
    setColour(juce::PopupMenu::highlightedTextColourId, C::ink);
    setColour(juce::AlertWindow::backgroundColourId, C::band);
    setColour(juce::AlertWindow::textColourId, C::cream);
    setColour(juce::AlertWindow::outlineColourId, C::bandEdge);
    setColour(juce::ScrollBar::thumbColourId, C::bandEdge);
    setColour(juce::ScrollBar::trackColourId, C::inset);
    setColour(juce::TooltipWindow::backgroundColourId, C::band);
    setColour(juce::TooltipWindow::textColourId, C::cream);
    setColour(juce::TooltipWindow::outlineColourId, C::bandEdge);
    setColour(juce::ComboBox::backgroundColourId, C::inset);
    setColour(juce::ComboBox::textColourId, C::cream);
    setColour(juce::ComboBox::outlineColourId, C::bandEdge);
    setColour(juce::ComboBox::arrowColourId, C::creamDim);
    setColour(juce::ListBox::backgroundColourId, C::inset);
    setColour(juce::ListBox::textColourId, C::cream);
    setColour(juce::FileBrowserComponent::currentPathBoxBackgroundColourId, C::inset);
    setColour(juce::FileBrowserComponent::currentPathBoxTextColourId, C::cream);
    setColour(juce::FileBrowserComponent::filenameBoxBackgroundColourId, C::inset);
    setColour(juce::FileBrowserComponent::filenameBoxTextColourId, C::cream);
    setColour(juce::DirectoryContentsDisplayComponent::textColourId, C::cream);
    setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, C::orange.withAlpha(0.6f));
    setColour(juce::DirectoryContentsDisplayComponent::highlightedTextColourId, C::ink);
}

// --- fonts ---------------------------------------------------------------------

juce::Font mw909LookAndFeel::legend(float height) {
    return juce::Font(juce::FontOptions(height, juce::Font::bold)).withExtraKerningFactor(0.06f);
}
juce::Font mw909LookAndFeel::text(float height) { return juce::Font(juce::FontOptions(height)); }
juce::Font mw909LookAndFeel::bold(float height) { return juce::Font(juce::FontOptions(height, juce::Font::bold)); }
juce::Font mw909LookAndFeel::readout(float height) {
    return juce::Font(juce::FontOptions(height, juce::Font::bold)).withExtraKerningFactor(0.02f);
}

float mw909LookAndFeel::legibleHeight(juce::Graphics& g, float height, float maxHeight) {
    // At small UI scales the 8-10 px base legends land at ~5 px on screen.
    // Grow them to a readable physical size, but never past their box.
    const float physical = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (physical <= 0.0f || height * physical >= kMinPhysicalTextPx) return height;
    return juce::jmax(height, juce::jmin(kMinPhysicalTextPx / physical, maxHeight));
}

juce::Font mw909LookAndFeel::legible(juce::Graphics& g, const juce::Font& f, float maxHeight) {
    const float h = legibleHeight(g, f.getHeight(), maxHeight);
    return juce::approximatelyEqual(h, f.getHeight()) ? f : f.withHeight(h);
}

// --- vocabulary ------------------------------------------------------------------

void mw909LookAndFeel::drawPlate(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour edge,
                                 float corner) {
    g.setColour(fill);
    g.fillRoundedRectangle(r, corner);
    g.setColour(edge);
    g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
}

void mw909LookAndFeel::drawInset(juce::Graphics& g, juce::Rectangle<float> r, float corner) {
    g.setColour(C::inset);
    g.fillRoundedRectangle(r, corner);
    g.setColour(C::insetEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.drawHorizontalLine(static_cast<int>(r.getBottom()), r.getX() + corner, r.getRight() - corner);
}

void mw909LookAndFeel::drawBand(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& s,
                                juce::Justification legendJust, float height) {
    g.setColour(C::band);
    g.fillRect(r);
    // The foot rule: a fine orange line under the band, inset at both ends.
    g.setColour(C::orangeDeep);
    g.fillRect(r.getX() + 3, r.getBottom() - 2, juce::jmax(0, r.getWidth() - 6), 1);
    if (s.isNotEmpty()) {
        g.setColour(C::legend);
        const auto area = r.reduced(6, 0).withTrimmedBottom(1);
        g.setFont(legible(g, legend(height), static_cast<float>(area.getHeight())));
        g.drawFittedText(s.toUpperCase(), area, legendJust, 1, 0.75f);
    }
}

void mw909LookAndFeel::drawLegend(juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                                  juce::Colour colour, float height, juce::Justification just) {
    g.setColour(colour);
    g.setFont(legible(g, legend(height), static_cast<float>(r.getHeight()) + 2.0f));
    g.drawFittedText(s.toUpperCase(), r, just, 1, 0.6f);
}

void mw909LookAndFeel::drawRuleTitle(juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                                     juce::Colour colour) {
    g.setFont(legible(g, legend(9.0f), 12.0f));
    const juce::String up = s.toUpperCase();
    const int textW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), up) + 10;
    const int cx = r.getCentreX();
    const int cy = r.getCentreY();
    g.setColour(C::inkFaint);
    if (r.getWidth() > textW + 12) {
        g.drawHorizontalLine(cy, static_cast<float>(r.getX()), static_cast<float>(cx - textW / 2));
        g.drawHorizontalLine(cy, static_cast<float>(cx + textW / 2), static_cast<float>(r.getRight()));
    }
    g.setColour(colour);
    g.drawFittedText(up, r, juce::Justification::centred, 1, 0.75f);
}

void mw909LookAndFeel::drawLamp(juce::Graphics& g, juce::Rectangle<float> r, float lit, juce::Colour on,
                                juce::Colour off) {
    const float d = std::min(r.getWidth(), r.getHeight());
    const auto dome = r.withSizeKeepingCentre(d, d);
    lit = juce::jlimit(0.0f, 1.0f, lit);
    if (lit > 0.3f) {
        g.setColour(on.withAlpha(0.22f * lit));
        g.fillEllipse(dome.expanded(d * 0.4f));
    }
    g.setColour(off.interpolatedWith(on, lit));
    g.fillEllipse(dome);
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.drawEllipse(dome.reduced(0.25f), 0.8f);
    if (lit > 0.5f) {
        g.setColour(juce::Colours::white.withAlpha(0.6f * (lit - 0.5f) * 2.0f));
        g.fillEllipse(dome.reduced(d * 0.3f).translated(-d * 0.08f, -d * 0.08f));
    }
}

void mw909LookAndFeel::drawLedBar(juce::Graphics& g, juce::Rectangle<float> r, float lit, juce::Colour on,
                                  juce::Colour off) {
    lit = juce::jlimit(0.0f, 1.0f, lit);
    if (lit > 0.3f) {
        g.setColour(on.withAlpha(0.25f * lit));
        g.fillRoundedRectangle(r.expanded(2.0f, 2.0f), 3.0f);
    }
    g.setColour(off.interpolatedWith(on, lit));
    g.fillRoundedRectangle(r, 1.5f);
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.drawRoundedRectangle(r.reduced(0.5f), 1.5f, 0.8f);
    if (lit > 0.6f) {
        g.setColour(juce::Colours::white.withAlpha(0.45f * (lit - 0.6f) / 0.4f));
        g.fillRoundedRectangle(r.reduced(r.getWidth() * 0.2f, r.getHeight() * 0.3f).translated(0.0f, -r.getHeight() * 0.1f), 1.0f);
    }
}

void mw909LookAndFeel::drawKnob(juce::Graphics& g, juce::Rectangle<float> r, float value01, const KnobStyle& style) {
    const float d = std::min(r.getWidth(), r.getHeight());
    if (d < 6.0f) return;
    const auto area = r.withSizeKeepingCentre(d, d);
    const auto centre = area.getCentre();
    value01 = juce::jlimit(0.0f, 1.0f, value01);
    const float span = kKnobEndAngle - kKnobStartAngle;
    const float angle = kKnobStartAngle + value01 * span;
    const float alpha = style.enabled ? 1.0f : 0.45f;

    // The printed scale: 11 dots that light up to the value (from the centre
    // for bipolar controls). The ends and the centre are larger.
    constexpr int kDots = 11;
    const float ringR = d * 0.455f;
    const float scaled = value01 * static_cast<float>(kDots - 1);
    const juce::Colour unlit = style.onDark ? C::bandEdge.brighter(0.3f) : C::dotOff;
    for (int i = 0; i < kDots; ++i) {
        const float a = kKnobStartAngle + span * static_cast<float>(i) / static_cast<float>(kDots - 1);
        float lit;
        if (style.bipolar) {
            constexpr float mid = static_cast<float>(kDots - 1) * 0.5f;
            const float fi = static_cast<float>(i);
            if (std::abs(fi - mid) < 0.01f) lit = 1.0f;
            else if (scaled >= mid) lit = fi > mid ? juce::jlimit(0.0f, 1.0f, scaled - fi + 1.0f) : 0.0f;
            else lit = fi < mid ? juce::jlimit(0.0f, 1.0f, fi - scaled + 1.0f) : 0.0f;
        } else {
            lit = i == 0 ? 1.0f : juce::jlimit(0.0f, 1.0f, scaled - static_cast<float>(i) + 1.0f);
        }
        const bool major = i == 0 || i == kDots - 1 || i == kDots / 2;
        const float dotR = juce::jmax(1.0f, d * (major ? 0.034f : 0.025f));
        const auto p = polar(centre, ringR, a);
        g.setColour(unlit.interpolatedWith(C::orange, lit).withMultipliedAlpha(alpha));
        g.fillEllipse(p.x - dotR, p.y - dotR, dotR * 2.0f, dotR * 2.0f);
    }

    if (style.hardware) {
        // Skirted knob: a wide skirt, a knurled cap, an orange line from the
        // cap's centre across the skirt.
        const float skirtR = d * 0.37f;
        const float capR = d * 0.255f;
        g.setColour(juce::Colours::black.withAlpha(0.25f * alpha));
        g.fillEllipse(juce::Rectangle<float>(skirtR * 2.0f, skirtR * 2.0f).withCentre(centre.translated(0.0f, d * 0.025f)));
        const auto skirt = juce::Rectangle<float>(skirtR * 2.0f, skirtR * 2.0f).withCentre(centre);
        juce::ColourGradient skirtGrad(C::knobSkirt.brighter(0.18f), centre.x, skirt.getY(), C::knobSkirt.darker(0.3f), centre.x,
                                       skirt.getBottom(), false);
        g.setGradientFill(skirtGrad);
        g.setOpacity(alpha);
        g.fillEllipse(skirt);
        g.setColour(juce::Colours::black.withAlpha(0.4f * alpha));
        g.drawEllipse(skirt.reduced(0.4f), 0.8f);
        // Pointer on the skirt (drawn before the cap so the cap overlaps it).
        g.setColour(C::orange.withAlpha(alpha));
        const float lineW = juce::jmax(1.4f, d * 0.045f);
        g.drawLine(juce::Line<float>(polar(centre, capR * 0.9f, angle), polar(centre, skirtR * 0.94f, angle)), lineW);
        // Cap.
        const auto cap = juce::Rectangle<float>(capR * 2.0f, capR * 2.0f).withCentre(centre);
        juce::ColourGradient capGrad(C::knobCap.brighter(0.35f), cap.getX(), cap.getY(), C::knobCap.darker(0.25f),
                                     cap.getRight(), cap.getBottom(), false);
        g.setGradientFill(capGrad);
        g.setOpacity(alpha);
        g.fillEllipse(cap);
        // Knurling around the cap's edge.
        if (d >= 18.0f) {
            g.setColour(juce::Colours::black.withAlpha(0.35f * alpha));
            constexpr int kKnurls = 24;
            for (int i = 0; i < kKnurls; ++i) {
                const float a = angle + kTwoPi * static_cast<float>(i) / static_cast<float>(kKnurls);
                g.drawLine(juce::Line<float>(polar(centre, capR * 0.8f, a), polar(centre, capR * 0.98f, a)), 0.7f);
            }
        }
        g.setColour(juce::Colours::black.withAlpha(0.5f * alpha));
        g.drawEllipse(cap.reduced(0.3f), 0.7f);
        // The pointer continues across the cap.
        g.setColour(C::orange.withAlpha(alpha));
        g.drawLine(juce::Line<float>(polar(centre, capR * 0.15f, angle), polar(centre, capR * 0.9f, angle)), lineW);
        if (style.hover && style.enabled) {
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillEllipse(skirt);
        }
    } else {
        // Flat cap with a rim and an orange pointer dot.
        const float capR = d * 0.31f;
        const auto cap = juce::Rectangle<float>(capR * 2.0f, capR * 2.0f).withCentre(centre);
        g.setColour(juce::Colours::black.withAlpha(0.22f * alpha));
        g.fillEllipse(cap.translated(0.0f, d * 0.02f));
        g.setColour(C::knobFlat.withAlpha(alpha));
        g.fillEllipse(cap);
        g.setColour(C::knobRim.withAlpha(alpha));
        g.drawEllipse(cap.reduced(0.8f), juce::jmax(1.0f, d * 0.03f));
        const float dotR = juce::jmax(1.5f, d * 0.05f);
        const auto p = polar(centre, capR * 0.62f, angle);
        g.setColour(C::orange.withAlpha(alpha));
        g.fillEllipse(p.x - dotR, p.y - dotR, dotR * 2.0f, dotR * 2.0f);
        if (style.hover && style.enabled) {
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillEllipse(cap);
        }
    }

    if (style.learning || style.bound) {
        const float dotD = juce::jmax(4.0f, d * 0.13f);
        const auto dot = juce::Rectangle<float>(dotD, dotD).withCentre({area.getRight() - dotD * 0.6f, area.getY() + dotD * 0.6f});
        g.setColour(style.learning ? C::record : C::ledGreenOn.darker(0.2f));
        g.fillEllipse(dot);
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawEllipse(dot, 0.7f);
    }
}

void mw909LookAndFeel::drawFader(juce::Graphics& g, juce::Rectangle<float> r, float value01, const KnobStyle& style) {
    value01 = juce::jlimit(0.0f, 1.0f, value01);
    const float alpha = style.enabled ? 1.0f : 0.45f;
    const float capH = juce::jlimit(12.0f, 22.0f, r.getHeight() * 0.1f);
    const float travelTop = r.getY() + capH * 0.5f;
    const float travelBottom = r.getBottom() - capH * 0.5f;
    const float cx = r.getCentreX();
    // Scale ticks.
    g.setColour(C::inkFaint.withAlpha(alpha));
    for (int i = 0; i <= 10; ++i) {
        const float y = std::round(travelTop + (travelBottom - travelTop) * static_cast<float>(i) / 10.0f);
        const float len = (i % 5 == 0) ? 6.0f : 3.5f;
        g.fillRect(cx - 7.0f - len, y, len, 1.0f);
        g.fillRect(cx + 7.0f, y, len, 1.0f);
    }
    // Slot, filled orange below the cap.
    const auto slot = juce::Rectangle<float>(5.0f, travelBottom - travelTop + 4.0f).withCentre({cx, (travelTop + travelBottom) * 0.5f});
    drawInset(g, slot, 2.0f);
    const float capY = travelBottom - value01 * (travelBottom - travelTop);
    g.setColour(C::orange.withAlpha(0.85f * alpha));
    g.fillRoundedRectangle(juce::Rectangle<float>(cx - 1.5f, capY, 3.0f, travelBottom - capY + 1.0f), 1.5f);
    // Cap.
    const float capW = juce::jmin(r.getWidth() - 2.0f, 26.0f);
    const auto cap = juce::Rectangle<float>(capW, capH).withCentre({cx, capY});
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(cap.translated(0.0f, 1.5f), 2.0f);
    juce::ColourGradient grad(C::knobCap.brighter(0.4f), cap.getX(), cap.getY(), C::knobCap.darker(0.3f), cap.getX(),
                              cap.getBottom(), false);
    g.setGradientFill(grad);
    g.setOpacity(alpha);
    g.fillRoundedRectangle(cap, 2.0f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawRoundedRectangle(cap.reduced(0.4f), 2.0f, 0.8f);
    g.setColour(juce::Colours::white.withAlpha(0.18f));
    for (float dy : {-capH * 0.28f, capH * 0.28f}) g.fillRect(cap.getX() + 3.0f, capY + dy, cap.getWidth() - 6.0f, 1.0f);
    g.setColour(C::orange.withAlpha(alpha));
    g.fillRect(cap.getX() + 2.0f, capY - 1.0f, cap.getWidth() - 4.0f, 2.0f);
    if (style.hover && style.enabled) {
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRoundedRectangle(cap, 2.0f);
    }
    if (style.learning || style.bound) {
        const auto dot = juce::Rectangle<float>(6.0f, 6.0f).withCentre({r.getRight() - 4.0f, r.getY() + 4.0f});
        g.setColour(style.learning ? C::record : C::ledGreenOn.darker(0.2f));
        g.fillEllipse(dot);
    }
}

void mw909LookAndFeel::drawStepKey(juce::Graphics& g, juce::Rectangle<float> r, const KeyState& s) {
    const float corner = juce::jmin(3.0f, r.getWidth() * 0.08f);
    juce::Colour face = s.inRange ? C::stepKeyFace(s.indexInPage) : C::panelShade;
    if (s.down) face = face.darker(0.12f);
    g.setColour(juce::Colours::black.withAlpha(0.28f));
    g.fillRoundedRectangle(r.translated(0.0f, 1.5f), corner);
    juce::ColourGradient grad(face.brighter(0.05f), r.getX(), r.getY(), face.darker(0.08f), r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(r, corner);
    g.setColour(s.inRange ? C::buttonEdge : C::panelEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
    if (s.hover && s.inRange) {
        g.setColour(C::hover);
        g.fillRoundedRectangle(r, corner);
    }

    // Level pips along the foot: one for weak, two for normal, three for accent.
    if (s.inRange) {
        const float pipH = juce::jlimit(2.5f, 5.0f, r.getHeight() * 0.05f);
        const float pipGap = juce::jmax(2.0f, r.getWidth() * 0.05f);
        const float pipW = (r.getWidth() * 0.62f - 2.0f * pipGap) / 3.0f;
        const float y = r.getBottom() - r.getHeight() * 0.12f - pipH;
        float x = r.getCentreX() - (3.0f * pipW + 2.0f * pipGap) * 0.5f;
        for (int i = 0; i < 3; ++i) {
            const auto pip = juce::Rectangle<float>(x, y, pipW, pipH);
            if (i < s.level) {
                g.setColour(s.level == 3 ? C::orangeDeep : C::orange);
                g.fillRoundedRectangle(pip, 1.0f);
            } else {
                g.setColour(C::inkFaint.withAlpha(0.45f));
                g.drawRoundedRectangle(pip.reduced(0.5f), 1.0f, 0.8f);
            }
            x += pipW + pipGap;
        }
    }
    if (s.running) {
        g.setColour(C::orange);
        g.drawRoundedRectangle(r.reduced(1.0f), corner, 2.0f);
    }
    const juce::Colour textCol = s.inRange ? C::ink : C::inkDim;
    const float textH = legibleHeight(g, juce::jlimit(8.0f, 12.0f, r.getHeight() * 0.15f), r.getHeight() * 0.26f);
    if (s.caption.isNotEmpty()) {
        g.setColour(textCol);
        g.setFont(bold(textH));
        g.drawText(s.caption, r.withTrimmedTop(r.getHeight() * 0.42f).withHeight(r.getHeight() * 0.26f).toNearestInt(),
                   juce::Justification::centred, false);
    }
    if (s.badge.isNotEmpty()) {
        g.setColour(C::badgeInk);
        g.setFont(bold(textH));
        g.drawFittedText(s.badge, r.reduced(2.0f, 0.0f).withTrimmedTop(r.getHeight() * 0.08f).withHeight(r.getHeight() * 0.3f).toNearestInt(),
                         juce::Justification::centred, 1, 0.7f);
    }
}

void mw909LookAndFeel::drawButtonFace(juce::Graphics& g, juce::Rectangle<float> r, bool on, bool hover, bool down,
                                      juce::Colour accent, bool lamp, bool enabled) {
    const float corner = 2.5f;
    const bool filled = on && !lamp;
    juce::Colour face = filled ? accent : C::buttonFace;
    if (down) face = face.darker(0.12f);
    g.setColour(juce::Colours::black.withAlpha(0.22f));
    g.fillRoundedRectangle(r.translated(0.0f, 1.0f), corner);
    juce::ColourGradient grad(face.brighter(0.05f), r.getX(), r.getY(), face.darker(0.08f), r.getX(), r.getBottom(), false);
    g.setGradientFill(grad);
    g.fillRoundedRectangle(r, corner);
    g.setColour(filled ? accent.darker(0.4f) : C::buttonEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), corner, 1.0f);
    if (lamp) {
        const float d = juce::jlimit(4.0f, 7.0f, r.getHeight() * 0.32f);
        drawLamp(g, juce::Rectangle<float>(d, d).withCentre({r.getX() + d * 0.5f + 4.0f, r.getCentreY()}), on ? 1.0f : 0.0f,
                 accent, C::ledOff);
    }
    if (hover && enabled) {
        g.setColour(C::hover);
        g.fillRoundedRectangle(r, corner);
    }
    if (!enabled) {
        g.setColour(C::panel.withAlpha(0.55f));
        g.fillRoundedRectangle(r, corner);
    }
}

// --- LookAndFeel_V4 overrides ----------------------------------------------------------

namespace {
juce::Colour accentOf(const juce::Button& b) {
    const auto& props = b.getProperties();
    if (props.contains("accent")) return juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(props["accent"])));
    return C::orange;
}
}  // namespace

void mw909LookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) {
    const bool lamp = static_cast<bool>(button.getProperties()["lamp"]);
    drawButtonFace(g, button.getLocalBounds().toFloat().reduced(0.5f, 0.5f).withTrimmedBottom(1.0f), button.getToggleState(),
                   shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, accentOf(button), lamp, button.isEnabled());
}

void mw909LookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) {
    const auto& props = button.getProperties();
    const bool lamp = static_cast<bool>(props["lamp"]);
    const bool filled = button.getToggleState() && !lamp;
    auto area = button.getLocalBounds().reduced(3, 1).withTrimmedBottom(1);
    g.setFont(legible(g, getTextButtonFont(button, button.getHeight()), static_cast<float>(area.getHeight())));
    juce::Colour col = filled ? C::textOn(accentOf(button)) : C::ink;
    if (!button.isEnabled()) col = col.withAlpha(0.45f);
    g.setColour(col);
    if (lamp) area.removeFromLeft(static_cast<int>(juce::jlimit(4.0f, 7.0f, static_cast<float>(button.getHeight()) * 0.32f)) + 5);
    // A toggle may name both of its states ("ON" / "OFF").
    const juce::String text = !button.getToggleState() && props.contains("offText") ? props["offText"].toString() : button.getButtonText();
    g.drawFittedText(text, area, juce::Justification::centred, 1, 0.7f);
}

juce::Font mw909LookAndFeel::getTextButtonFont(juce::TextButton& button, int buttonHeight) {
    const auto& props = button.getProperties();
    if (props.contains("fontHeight")) return legend(static_cast<float>(static_cast<double>(props["fontHeight"])));
    return legend(juce::jlimit(8.0f, 11.0f, static_cast<float>(buttonHeight) * 0.45f));
}

void mw909LookAndFeel::drawLabel(juce::Graphics& g, juce::Label& label) {
    g.fillAll(label.findColour(juce::Label::backgroundColourId));
    if (!label.isBeingEdited()) {
        const float alpha = label.isEnabled() ? 1.0f : 0.5f;
        g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
        const auto area = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
        g.setFont(legible(g, getLabelFont(label), static_cast<float>(area.getHeight()) + 2.0f));
        g.drawFittedText(label.getText(), area, label.getJustificationType(), 1, 0.7f);
    }
    g.setColour(label.findColour(juce::Label::outlineColourId));
    g.drawRect(label.getLocalBounds());
}

juce::Font mw909LookAndFeel::getLabelFont(juce::Label& label) { return label.getFont(); }

void mw909LookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor) {
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)), 2.5f);
}

void mw909LookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) {
    g.setColour(editor.findColour(editor.hasKeyboardFocus(true) ? juce::TextEditor::focusedOutlineColourId
                                                                : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle(juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f),
                           2.5f, 1.0f);
}

void mw909LookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height) {
    g.setColour(C::band);
    g.fillRect(0, 0, width, height);
    g.setColour(C::orangeDeep);
    g.fillRect(0, 0, width, 2);
    g.setColour(C::bandEdge);
    g.drawRect(0, 0, width, height, 1);
}

void mw909LookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                         bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                         const juce::String& text, const juce::String& shortcutKeyText,
                                         const juce::Drawable*, const juce::Colour* textColour) {
    if (isSeparator) {
        g.setColour(C::bandEdge);
        g.drawHorizontalLine(area.getCentreY(), static_cast<float>(area.getX() + 8), static_cast<float>(area.getRight() - 8));
        return;
    }
    auto r = area.reduced(2, 0);
    if (isHighlighted && isActive) {
        g.setColour(C::orange);
        g.fillRoundedRectangle(r.toFloat(), 2.0f);
    }
    juce::Colour col = textColour != nullptr ? *textColour : C::cream;
    if (isHighlighted && isActive) col = C::ink;
    if (!isActive) col = C::creamDim;
    g.setColour(col);
    g.setFont(getPopupMenuFont());
    auto textArea = r.reduced(8, 0);
    const auto tick = textArea.removeFromLeft(14);
    if (isTicked) g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(tick.getCentre().toFloat()));
    if (hasSubMenu) {
        const auto arrow = textArea.removeFromRight(12).toFloat();
        juce::Path p;
        p.addTriangle(arrow.getCentreX() - 3.0f, arrow.getCentreY() - 4.0f, arrow.getCentreX() - 3.0f,
                      arrow.getCentreY() + 4.0f, arrow.getCentreX() + 3.0f, arrow.getCentreY());
        g.fillPath(p);
    }
    g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1);
    if (shortcutKeyText.isNotEmpty()) {
        g.setFont(getPopupMenuFont().withHeight(11.0f));
        g.drawText(shortcutKeyText, textArea, juce::Justification::centredRight, true);
    }
}

void mw909LookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                  const juce::String& sectionName) {
    g.setColour(C::legend);
    g.setFont(legend(10.0f));
    g.drawFittedText(sectionName.toUpperCase(), area.reduced(8, 0), juce::Justification::centredLeft, 1);
    g.setColour(C::bandEdge);
    g.drawHorizontalLine(area.getBottom() - 1, static_cast<float>(area.getX() + 8), static_cast<float>(area.getRight() - 8));
}

juce::Font mw909LookAndFeel::getPopupMenuFont() { return text(13.0f); }

void mw909LookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                                 int& idealWidth, int& idealHeight) {
    if (isSeparator) {
        idealWidth = 50;
        idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight / 2 : 8;
        return;
    }
    const auto font = getPopupMenuFont();
    idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : 22;
    idealWidth = juce::GlyphArrangement::getStringWidthInt(font, text) + idealHeight * 2 + 20;
}

void mw909LookAndFeel::drawCallOutBoxBackground(juce::CallOutBox&, juce::Graphics& g, const juce::Path& path,
                                                juce::Image&) {
    g.setColour(C::band.withAlpha(0.97f));
    g.fillPath(path);
    g.setColour(C::bandEdge);
    g.strokePath(path, juce::PathStrokeType(1.0f));
}

void mw909LookAndFeel::drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea,
                                    juce::TextLayout& layout) {
    auto bounds = alert.getLocalBounds().toFloat();
    g.setColour(C::band);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(C::bandEdge);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);
    g.setColour(C::orange);
    g.fillRect(bounds.removeFromTop(3.0f).reduced(4.0f, 0.0f));
    g.setColour(C::cream);
    layout.draw(g, textArea.toFloat());
}

void mw909LookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                                     bool isScrollbarVertical, int thumbStartPosition, int thumbSize, bool isMouseOver,
                                     bool) {
    g.setColour(C::inset);
    g.fillRect(x, y, width, height);
    const juce::Rectangle<int> thumb = isScrollbarVertical ? juce::Rectangle<int>(x + 2, thumbStartPosition, width - 4, thumbSize)
                                                           : juce::Rectangle<int>(thumbStartPosition, y + 2, thumbSize, height - 4);
    g.setColour(isMouseOver ? C::creamDim : C::bandEdge);
    g.fillRoundedRectangle(thumb.toFloat(), 2.0f);
}

void mw909LookAndFeel::drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver, bool) {
    // Kept inside the 8 px gutter round the face (it must not cross a plate).
    g.setColour(isMouseOver ? C::orange : C::inkFaint);
    for (int i = 1; i <= 3; ++i) {
        const float o = static_cast<float>(i) * 2.5f;
        g.drawLine(static_cast<float>(w) - o, static_cast<float>(h), static_cast<float>(w), static_cast<float>(h) - o, 1.0f);
    }
}

void mw909LookAndFeel::drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) {
    const juce::Rectangle<float> r(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(C::band);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(C::bandEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
    g.setColour(C::cream);
    g.setFont(mw909LookAndFeel::text(12.0f));
    g.drawFittedText(text, r.toNearestInt().reduced(6, 2), juce::Justification::centredLeft, 3);
}
