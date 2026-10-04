// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The mw909 palette (docs/design/plugin.md §7): a light warm-grey panel with
// orange accents. Dark section bands carry orange legends, the knob caps are
// a darker grey with orange pointers, the step keys are cream and warm grey
// under orange LEDs. Every colour the editor draws comes from this table so
// the look can be tuned in one place.
//
// Contrast (WCAG 2.x, normal text needs 4.5:1):
//   ink on panel          ~13:1     inkDim on panel   ~5.6:1
//   legend on band        ~5.3:1    cream on band     ~13:1
//   ink on orange         ~5.4:1    cream on inset    ~12:1
//   badgeInk on keyGrey   ~5.3:1    ink on orangeSoft ~8:1
// Neither ink nor cream reaches 4.5:1 on orangeDeep: text on it sits on a
// cream chip. inkFaint is for rules and tick marks only, never for text.

#pragma once

#include <juce_graphics/juce_graphics.h>

#include "mw909/Voices.h"

namespace mw909Colours {

// --- panel -------------------------------------------------------------------
inline const juce::Colour panel          {0xffd3cfc7};  // the chassis: light warm grey
inline const juce::Colour panelLight     {0xffdedad3};  // raised plates (voice columns, channel strips)
inline const juce::Colour panelShade     {0xffc4bfb6};  // recessed fields, the sequencer ground
inline const juce::Colour panelEdge      {0xffa39d93};  // 1 px plate outlines, column rules
inline const juce::Colour band           {0xff2b2926};  // dark section bands (header bar too)
inline const juce::Colour bandLight      {0xff3a3733};  // controls sitting on a band
inline const juce::Colour bandEdge       {0xff514d47};
inline const juce::Colour inset          {0xff2a2825};  // readouts, value boxes, meters
inline const juce::Colour insetEdge      {0xff15140f};

// --- text ----------------------------------------------------------------------
inline const juce::Colour ink            {0xff23201c};  // primary legend on the panel
inline const juce::Colour inkDim         {0xff4d4842};  // secondary legend on the panel
inline const juce::Colour inkFaint       {0xff8a847a};  // rules, ticks (not text)
inline const juce::Colour cream          {0xffefe9db};  // text on dark bands and readouts
inline const juce::Colour creamDim       {0xffb5ad9e};  // secondary text on dark
inline const juce::Colour legend         {0xfff08a2e};  // orange legend on a dark band
inline const juce::Colour badgeInk       {0xff6e2d07};  // step-key badges (F, x3, 75%): dark burnt orange

// --- accent --------------------------------------------------------------------
inline const juce::Colour orange         {0xffee6f1c};  // pointers, selected keys, value dots
inline const juce::Colour orangeDeep     {0xffc4560f};
inline const juce::Colour orangeSoft     {0xfff4a46a};  // weak steps, hover outlines

// --- controls ------------------------------------------------------------------
inline const juce::Colour knobSkirt      {0xff5b5752};  // hardware knobs: the skirt
inline const juce::Colour knobCap        {0xff3c3a36};  // hardware knobs: the cap
inline const juce::Colour knobFlat       {0xff4a4742};  // extended knobs: flat cap
inline const juce::Colour knobRim        {0xff77726a};  // extended knobs: cap rim
inline const juce::Colour dotOff         {0xffa49e94};  // unlit scale dot printed on the panel
inline const juce::Colour keyCream       {0xffefe9da};  // step keys 1-4, 9-12
inline const juce::Colour keyGrey        {0xffc0bab0};  // step keys 5-8, 13-16
inline const juce::Colour buttonFace     {0xffe7e3db};  // panel buttons
inline const juce::Colour buttonFaceLow  {0xffd6d1c8};
inline const juce::Colour buttonEdge     {0xff8e887e};
inline const juce::Colour hover          {0x18000000};  // darkens on the light panel
inline const juce::Colour hoverOnDark    {0x22ffffff};

// --- lamps ---------------------------------------------------------------------
inline const juce::Colour ledOff         {0xff7a5c47};
inline const juce::Colour ledOn          {0xffff7a1a};
inline const juce::Colour ledRun         {0xfffff0d6};  // the running step
inline const juce::Colour ledGreenOff    {0xff4f5a48};
inline const juce::Colour ledGreenOn     {0xff7fd36a};
inline const juce::Colour mute           {0xffe6b13d};  // mute key face
inline const juce::Colour solo           {0xff8cc56f};  // solo key face
inline const juce::Colour record         {0xffd94b2b};
inline const juce::Colour meterLow       {0xff7cc06a};
inline const juce::Colour meterMid       {0xffe9c043};
inline const juce::Colour meterHot       {0xffe0482b};

// --- grid ------------------------------------------------------------------------
inline const juce::Colour cellOff        {0xffe6e2da};
inline const juce::Colour cellOffAlt     {0xffd9d4cb};
inline const juce::Colour cellOutside    {0xffbab4aa};

// Step keys alternate cream and grey in groups of four, like a printed panel.
inline juce::Colour stepKeyFace(int stepInPage) noexcept {
    return (((stepInPage % 16) + 16) % 16 / 4) % 2 == 0 ? keyCream : keyGrey;
}

// Text that reads on a given face.
inline juce::Colour textOn(juce::Colour background) noexcept {
    return background.getPerceivedBrightness() > 0.55f ? ink : cream;
}

}  // namespace mw909Colours
