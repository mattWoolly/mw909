// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The editor's LookAndFeel: fonts, the vector drawing vocabulary every mw909
// component uses (bands, plates, legends, lamps, knobs, faders, keys) and the
// LookAndFeel_V4 overrides for the stock JUCE widgets the editor still uses
// (text buttons, labels, popup menus, text editors, alert windows).
//
// Everything is drawn with juce::Graphics; there are no images, no embedded
// fonts (JUCE's default sans), no OpenGL.
//
// The knob family is mw909's own: a ring of printed scale dots that light
// orange up to the value, and either a knurled cap on a wide skirt (the
// original panel's knobs) or a flat cap with an orange pointer dot (the
// extended controls).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "mw909Colours.h"

class mw909LookAndFeel final : public juce::LookAndFeel_V4 {
public:
    mw909LookAndFeel();
    ~mw909LookAndFeel() override = default;

    // --- fonts (JUCE pixel heights at the 1280 x 760 base) --------------------
    static juce::Font legend(float height = 10.0f);      // uppercase legends, tracked
    static juce::Font text(float height = 12.0f);        // plain text
    static juce::Font bold(float height = 12.0f);
    static juce::Font readout(float height = 12.0f);     // numeric readouts
    // Text never smaller than this on screen (when its box allows it).
    static constexpr float kMinPhysicalTextPx = 6.5f;
    // `height` grown so it draws at least kMinPhysicalTextPx physical pixels
    // under g's transform, capped at `maxHeight` (the box it has to fit).
    static float legibleHeight(juce::Graphics& g, float height, float maxHeight);
    static juce::Font legible(juce::Graphics& g, const juce::Font& f, float maxHeight);

    // --- drawing vocabulary -----------------------------------------------------
    // A raised plate on the panel.
    static void drawPlate(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill = mw909Colours::panelLight,
                          juce::Colour edge = mw909Colours::panelEdge, float corner = 3.0f);
    // A dark inset well (readouts, value boxes, meters).
    static void drawInset(juce::Graphics& g, juce::Rectangle<float> r, float corner = 2.5f);
    // A dark section band with an orange legend. `legendJust` places the text;
    // the band has square corners and a fine orange rule along its foot.
    static void drawBand(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& legend,
                         juce::Justification legendJust = juce::Justification::centred, float height = 10.0f);
    // Uppercase legend text.
    static void drawLegend(juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                           juce::Colour colour = mw909Colours::inkDim, float height = 10.0f,
                           juce::Justification just = juce::Justification::centred);
    // A small caption with fine rules either side (on the light panel).
    static void drawRuleTitle(juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                              juce::Colour colour = mw909Colours::inkDim);
    // A round lamp. `lit` 0..1 blends off -> on.
    static void drawLamp(juce::Graphics& g, juce::Rectangle<float> r, float lit, juce::Colour on = mw909Colours::ledOn,
                         juce::Colour off = mw909Colours::ledOff);
    // A rectangular LED (step LEDs).
    static void drawLedBar(juce::Graphics& g, juce::Rectangle<float> r, float lit, juce::Colour on = mw909Colours::ledOn,
                           juce::Colour off = mw909Colours::ledOff);

    struct KnobStyle {
        bool hardware = true;          // skirted, knurled cap (else flat cap)
        bool bipolar = false;          // dots light from the centre (pan, tune)
        bool hover = false;
        bool enabled = true;
        bool learning = false;         // MIDI learn armed on this control
        bool bound = false;            // a controller is bound to it
        bool onDark = false;           // drawn on a dark band (unlit dots lighter)
    };
    static void drawKnob(juce::Graphics& g, juce::Rectangle<float> r, float value01, const KnobStyle& style);

    // A vertical fader. `value01` 0 = bottom.
    static void drawFader(juce::Graphics& g, juce::Rectangle<float> r, float value01, const KnobStyle& style);

    // One sequencer step key (the LED above it is drawn by the caller).
    struct KeyState {
        int indexInPage = 0;           // picks the cream / grey face
        int level = 0;                 // 0 off, 1 weak, 2 normal, 3 accent
        bool inRange = true;
        bool running = false;
        bool hover = false;
        bool down = false;
        juce::String caption;          // "1".."64"
        juce::String badge;            // "F", "x3", "75%"
    };
    static void drawStepKey(juce::Graphics& g, juce::Rectangle<float> r, const KeyState& state);

    // A panel button face. With `lamp` the face stays light and a small LED at
    // the left shows the state; otherwise an "on" face takes the accent colour.
    static void drawButtonFace(juce::Graphics& g, juce::Rectangle<float> r, bool on, bool hover, bool down,
                               juce::Colour accent, bool lamp = false, bool enabled = true);

    static constexpr float kKnobStartAngle = juce::MathConstants<float>::pi * 1.25f;
    static constexpr float kKnobEndAngle = juce::MathConstants<float>::pi * 2.75f;

    // --- LookAndFeel_V4 overrides ----------------------------------------------
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;
    void drawLabel(juce::Graphics& g, juce::Label& label) override;
    juce::Font getLabelFont(juce::Label& label) override;
    void fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override;
    void drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor) override;
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcutKeyText, const juce::Drawable* icon,
                           const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area,
                                    const juce::String& sectionName) override;
    juce::Font getPopupMenuFont() override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                   int& idealWidth, int& idealHeight) override;
    void drawCallOutBoxBackground(juce::CallOutBox& box, juce::Graphics& g, const juce::Path& path,
                                  juce::Image& cachedImage) override;
    int getCallOutBoxBorderSize(const juce::CallOutBox&) override { return 12; }
    void drawAlertBox(juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea,
                      juce::TextLayout& layout) override;
    int getAlertWindowButtonHeight() override { return 26; }
    juce::Font getAlertWindowTitleFont() override { return bold(16.0f); }
    juce::Font getAlertWindowMessageFont() override { return text(13.0f); }
    juce::Font getAlertWindowFont() override { return text(13.0f); }
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                       bool isScrollbarVertical, int thumbStartPosition, int thumbSize, bool isMouseOver,
                       bool isMouseDown) override;
    void drawCornerResizer(juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging) override;
    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(mw909LookAndFeel)
};
