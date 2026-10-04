// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Minimal RIFF/WAVE reader and writer for the command-line tools. No
// dependencies. Writes 16-bit or 24-bit integer PCM or 32-bit IEEE float;
// reads those plus 32-bit integer PCM and WAVE_FORMAT_EXTENSIBLE wrappers.
// Integer quantisation is round-half-away-from-zero with hard clipping, so the
// same float input always gives the same bytes.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mw909::tools {

enum class SampleFormat
{
    Pcm16,
    Pcm24,
    Float32
};

// Parses "16", "24", "32f"/"float"/"f32", "pcm16", "pcm24". Returns false on junk.
bool parseSampleFormat(const std::string& text, SampleFormat& out);
const char* sampleFormatName(SampleFormat format);

struct WavData
{
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    std::vector<float> samples; // interleaved, numFrames() * channels

    int numFrames() const
    {
        return channels > 0 ? static_cast<int>(samples.size() / static_cast<std::size_t>(channels))
                            : 0;
    }
};

struct WriteStats
{
    std::int64_t clipped = 0; // samples that hit the integer range limit
    float peak = 0.0f;        // largest |x| before quantisation
};

// Writes planar channel data (all channels must hold numFrames samples).
// Returns false and fills `error` on I/O failure.
bool writeWav(const std::string& path, const std::vector<const float*>& channels, int numFrames,
              int sampleRate, SampleFormat format, WriteStats* stats, std::string& error);

bool readWav(const std::string& path, WavData& out, std::string& error);

// One least-significant bit of the given integer depth as a float sample value
// (0 for float files).
double lsbOf(int bitsPerSample, bool isFloat);

// Compares a candidate render with a blessed one and prints diagnostics to
// stdout. tolText: "" (one LSB of the blessed file), an absolute value, or
// "lsb:<n>". Returns 0 match within tolerance, 1 mismatch, 2 unreadable file
// or bad tolerance.
int compareWavFiles(const std::string& candidatePath, const std::string& blessedPath,
                    const std::string& tolText, bool quiet);

} // namespace mw909::tools
