// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// A stand-in editor so the plugin opens in hosts and validators while the real
// editor (plugin/ui/mw909Editor.h) is being built: the wordmark and a note, and
// nothing else. mw909Processor.cpp selects the editor class in one place
// (CMake option MW909_BUILD_EDITOR).
//
// Deliberately NOT juce::GenericAudioProcessorEditor, and it never registers
// itself anywhere: that editor's rows register as juce::AudioProcessorListener
// on open and unregister on close, and JUCE 8's
// AudioProcessor::ParameterChangeForwarder walks that listener array WITHOUT
// its lock whenever a parameter changes. Hosts change parameters on the audio
// thread (VST3 automation), so opening or closing such an editor during
// playback is a use-after-free (mw808's stress test found it under ASan).
// This one has no controls, no listeners, no timers and no popups.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "mw909Processor.h"

namespace mw909::plugin {

class PlaceholderEditor final : public juce::AudioProcessorEditor {
public:
    explicit PlaceholderEditor(::mw909Processor& owner) : juce::AudioProcessorEditor(owner) {
        setResizable(true, true);
        setResizeLimits(320, 200, 2560, 1520);
        setSize(640, 380);
    }

    void paint(juce::Graphics& g) override {
        const auto bounds = getLocalBounds().toFloat();
        g.fillAll(juce::Colour(0xffd9d6cf));  // warm light grey panel

        // Wordmark: lowercase "mw" and the model number, orange accent bar.
        const float size = juce::jlimit(28.0f, 96.0f, bounds.getHeight() * 0.22f);
        const juce::Font mark(juce::FontOptions(size, juce::Font::bold));
        const juce::String mw("mw"), number("909");
        const float mwWidth = juce::GlyphArrangement::getStringWidth(mark, mw);
        const float numberWidth = juce::GlyphArrangement::getStringWidth(mark, number);
        const float x = bounds.getCentreX() - 0.5f * (mwWidth + numberWidth);
        const float y = bounds.getCentreY() - size;
        g.setFont(mark);
        g.setColour(juce::Colour(0xff3a3835));
        g.drawText(mw, juce::Rectangle<float>(x, y, mwWidth + 2.0f, size * 1.2f), juce::Justification::left, false);
        g.setColour(juce::Colour(0xffe8701a));
        g.drawText(number, juce::Rectangle<float>(x + mwWidth, y, numberWidth + 2.0f, size * 1.2f),
                   juce::Justification::left, false);
        g.fillRect(juce::Rectangle<float>(x, y + size * 1.25f, mwWidth + numberWidth, juce::jmax(2.0f, size * 0.05f)));

        g.setColour(juce::Colour(0xff5a5753));
        g.setFont(juce::FontOptions(juce::jlimit(11.0f, 18.0f, size * 0.22f)));
        g.drawFittedText("The editor is under construction. Every control is available as a host parameter.",
                         getLocalBounds().reduced(24).withTrimmedTop(static_cast<int>(bounds.getCentreY() + size * 0.6f) - 24),
                         juce::Justification::centredTop, 3);
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaceholderEditor)
};

}  // namespace mw909::plugin
