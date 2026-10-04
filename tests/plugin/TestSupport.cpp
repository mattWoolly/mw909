// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "TestSupport.h"

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

namespace {

// Brings JUCE up for the whole run: the message manager lives on the thread
// that runs the tests, which makes it the message thread.
class JuceEnvironment final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        // Never the user's real preset directory, also when the binary is run
        // by hand rather than through CTest (which sets MW909_PRESET_DIR).
        if (juce::SystemStats::getEnvironmentVariable("MW909_PRESET_DIR", {}).isEmpty())
            setenv("MW909_PRESET_DIR", MW909_TEST_SCRATCH_DIR "/test-presets", 1);
        juce_ = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
    }
    void testRunEnded(const Catch::TestRunStats&) override { juce_.reset(); }

private:
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce_;
};

}  // namespace

CATCH_REGISTER_LISTENER(JuceEnvironment)

namespace plugintest {

juce::File scratchDirectory(const juce::String& name) {
    const juce::File root(MW909_TEST_SCRATCH_DIR);
    const juce::File dir = root.getChildFile(juce::File::createLegalFileName(name));
    // Only ever a directory of our own, under the build tree.
    if (dir.isAChildOf(root) && dir.isDirectory()) dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

}  // namespace plugintest

namespace plugintest {

juce::File writeTestWav(const juce::File& file, double rate, double seconds, std::uint32_t seed) {
    file.deleteFile();
    const int n = std::max(1, static_cast<int>(rate * seconds));
    juce::AudioBuffer<float> audio(1, n);
    Rng rng(seed);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    for (int i = 0; i < n; ++i)
        audio.setSample(0, i, std::clamp(noise(rng) * std::exp(-3.0f * static_cast<float>(i) / static_cast<float>(n)), -1.0f, 1.0f));
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(16);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr) return {};
    writer->writeFromAudioSampleBuffer(audio, 0, n);
    writer.reset();
    return file;
}

}  // namespace plugintest
