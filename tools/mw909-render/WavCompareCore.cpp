// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The WAV comparison behind mw909-wavcompare and `mw909-render compare`:
// format, peak/RMS of both files, max |diff| and where it is, RMS of the
// difference in dBFS and relative to the reference, the first differing
// sample, and the difference per tenth of the file. Default tolerance: one
// LSB of the blessed file's bit depth (0 for float files).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Wav.h"

namespace mw909::tools {

namespace {

double toDb(double x)
{
    return x > 0.0 ? 20.0 * std::log10(x) : -999.0;
}

} // namespace

int compareWavFiles(const std::string& candidatePath, const std::string& blessedPath,
                    const std::string& tolText, bool quiet)
{
    WavData cand, ref;
    std::string error;
    if (!readWav(candidatePath, cand, error) || !readWav(blessedPath, ref, error))
    {
        std::fprintf(stderr, "wav compare: %s\n", error.c_str());
        return 2;
    }

    const double lsb = lsbOf(ref.bitsPerSample, ref.isFloat);
    double tol = lsb;
    if (!tolText.empty())
    {
        char* end = nullptr;
        if (tolText.rfind("lsb:", 0) == 0)
            tol = std::strtod(tolText.c_str() + 4, &end) * lsb;
        else
            tol = std::strtod(tolText.c_str(), &end);
        if (end == nullptr || *end != '\0' || !(tol >= 0.0))
        {
            std::fprintf(stderr, "wav compare: --tol: use an absolute value or lsb:<n>\n");
            return 2;
        }
    }

    std::printf("golden compare\n  candidate: %s\n  blessed  : %s\n", candidatePath.c_str(),
                blessedPath.c_str());
    const auto fmt = [](const WavData& w) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d ch, %d Hz, %d-bit %s, %d frames", w.channels,
                      w.sampleRate, w.bitsPerSample, w.isFloat ? "float" : "PCM", w.numFrames());
        return std::string(buf);
    };
    std::printf("  format   : candidate %s\n             blessed   %s\n", fmt(cand).c_str(),
                fmt(ref).c_str());

    if (cand.channels != ref.channels || cand.sampleRate != ref.sampleRate
        || cand.numFrames() != ref.numFrames())
    {
        std::printf("RESULT: FAIL (format or length differs)\n");
        return 1;
    }

    const int ch = ref.channels;
    const int frames = ref.numFrames();
    double maxDiff = 0.0, sumSqDiff = 0.0, sumSqRef = 0.0, peakRef = 0.0, peakCand = 0.0;
    std::size_t maxAt = 0, firstAt = 0, differing = 0, nonFinite = 0, firstNonFiniteAt = 0;
    bool anyDiff = false;
    constexpr int kSegments = 10;
    double segMax[kSegments] = {};
    for (std::size_t i = 0; i < ref.samples.size(); ++i)
    {
        const double a = cand.samples[i], b = ref.samples[i];
        // A NaN or infinity on either side is never a match (NaN compares
        // false with everything, so `d > 0` alone would let it through).
        // It is counted, excluded from the statistics, and fails the compare.
        if (!std::isfinite(a) || !std::isfinite(b))
        {
            if (nonFinite++ == 0)
                firstNonFiniteAt = i;
            if (!anyDiff)
            {
                anyDiff = true;
                firstAt = i;
            }
            ++differing;
            continue;
        }
        const double d = std::abs(a - b);
        peakRef = std::max(peakRef, std::abs(b));
        peakCand = std::max(peakCand, std::abs(a));
        sumSqRef += b * b;
        sumSqDiff += d * d;
        if (!(d == 0.0))
        {
            ++differing;
            if (!anyDiff)
            {
                anyDiff = true;
                firstAt = i;
            }
        }
        if (d > maxDiff)
        {
            maxDiff = d;
            maxAt = i;
        }
        const int frame = static_cast<int>(i / static_cast<std::size_t>(ch));
        const int seg = frames > 0 ? std::min(kSegments - 1, frame * kSegments / frames) : 0;
        segMax[seg] = std::max(segMax[seg], d);
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, ref.samples.size()));
    const double rmsRef = std::sqrt(sumSqRef / n), rmsDiff = std::sqrt(sumSqDiff / n);
    const double rate = static_cast<double>(ref.sampleRate);

    std::printf("  blessed  : peak %.2f dBFS, rms %.2f dBFS\n", toDb(peakRef), toDb(rmsRef));
    std::printf("  candidate: peak %.2f dBFS\n", toDb(peakCand));
    if (!anyDiff)
    {
        std::printf("  identical: every sample matches\nRESULT: PASS (bit-exact)\n");
        return 0;
    }
    const std::size_t maxFrame = maxAt / static_cast<std::size_t>(ch),
                      firstFrame = firstAt / static_cast<std::size_t>(ch);
    char lsbBuf[64] = "";
    if (lsb > 0.0)
        std::snprintf(lsbBuf, sizeof lsbBuf, " = %.1f LSB", maxDiff / lsb);
    const std::string lsbText = lsbBuf;
    std::printf("  max |diff|: %.3g (%.2f dBFS%s) at frame %zu ch %zu (t = %.5f s)\n", maxDiff,
                toDb(maxDiff), lsbText.c_str(), maxFrame, maxAt % static_cast<std::size_t>(ch),
                static_cast<double>(maxFrame) / rate);
    std::printf("  rms diff  : %.2f dBFS (%.2f dB relative to the blessed rms)\n", toDb(rmsDiff),
                toDb(rmsDiff) - toDb(rmsRef));
    std::printf("  differing : %zu of %zu samples; first at frame %zu ch %zu (t = %.5f s): "
                "candidate %.8f, blessed "
                "%.8f\n",
                differing, ref.samples.size(), firstFrame, firstAt % static_cast<std::size_t>(ch),
                static_cast<double>(firstFrame) / rate, static_cast<double>(cand.samples[firstAt]),
                static_cast<double>(ref.samples[firstAt]));
    if (nonFinite > 0)
    {
        const std::size_t f = firstNonFiniteAt / static_cast<std::size_t>(ch);
        std::printf("  non-finite: %zu sample(s) are NaN or infinite; first at frame %zu ch %zu: candidate %g, "
                    "blessed %g\n",
                    nonFinite, f, firstNonFiniteAt % static_cast<std::size_t>(ch),
                    static_cast<double>(cand.samples[firstNonFiniteAt]),
                    static_cast<double>(ref.samples[firstNonFiniteAt]));
    }
    if (!quiet)
    {
        std::printf("  max |diff| per tenth of the file (dBFS):\n   ");
        for (int s = 0; s < kSegments; ++s)
            std::printf(" %7.1f", toDb(segMax[s]));
        std::printf("\n");
    }
    if (nonFinite > 0)
    {
        std::printf("RESULT: FAIL (%zu non-finite sample(s))\n", nonFinite);
        return 1;
    }
    const bool pass = maxDiff <= tol; // maxDiff is finite here
    std::printf("RESULT: %s (max |diff| %.3g %s tolerance %.3g)\n", pass ? "PASS" : "FAIL", maxDiff,
                pass ? "<=" : ">", tol);
    return pass ? 0 : 1;
}

} // namespace mw909::tools
