// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// romFromAudio: turns a mono recording into 6-bit ROM content the way the
// original machine's sample data was prepared (research/02 §3.3, §9.3;
// research/05 §5.7): the cymbal's own decay is divided out ("companded") so
// the six bits are used throughout the note, and the analog VCA puts a decay
// back at playback. Used by tools/rombuild for the factory images and by the
// plugin for user imports. Message thread only (allocates).
//
// Steps:
//   1. validate; remove the mean; trim leading silence (onset = first sample
//      within -26 dB of the peak, minus 1 ms);
//   2. low-pass (Kaiser-windowed sinc, ~100 dB stop band; corner 0.40 x the
//      ROM rate for the hats = 12.8 kHz, 0.45 x for the cymbals = 17 kHz,
//      FITTED to the band levels of the original's output, research/05 §10.2)
//      and resample to the region's build rate (pcm::romBuildRateHz:
//      the region's clock with the knobs at default, so the source plays at
//      its natural pitch);
//   3. flatten the envelope: divide by a centred 25 ms RMS tracker, with the
//      divisor floored 40 dB below its maximum so a recording's noise floor
//      is not pulled up to full scale;
//   4. normalise so the 99.9th percentile of |x| reaches full scale (the
//      rare peaks above clip, as an A/D converter would);
//   5. quantise to 6 bits, unsigned, mid-scale 32, no dither; a 2 ms fade to
//      mid-scale where the material ends (or the region does); pad with mid-scale.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "mw909/dsp/PcmClock.h"
#include "mw909/dsp/Rom.h"

namespace mw909 {

namespace {

constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1.0e-17 * sum) break;
    }
    return sum;
}

// Band-limited resampling of x (rate fsIn) to `count` samples at fsOut,
// corner `cutHz`, Kaiser beta 10, 24 zero crossings each side.
std::vector<double> resample(const std::vector<double>& x, double fsIn, double fsOut, double cutHz, int count) {
    constexpr double kBeta = 10.0;
    constexpr double kZeros = 24.0;
    const double fc = cutHz / fsIn;                 // cycles per input sample
    const double half = kZeros / (2.0 * fc);        // kernel half-width, input samples
    // Kaiser window tabulated over |r| in [0, 1] (linear interpolation, error < 1e-9).
    constexpr int kWinPoints = 8192;
    const double i0b = besselI0(kBeta);
    std::vector<double> win(kWinPoints + 2);
    for (int i = 0; i <= kWinPoints + 1; ++i) {
        const double r = std::min(1.0, static_cast<double>(i) / kWinPoints);
        win[static_cast<std::size_t>(i)] = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
    }
    std::vector<double> y(static_cast<std::size_t>(count), 0.0);
    const auto n = static_cast<std::ptrdiff_t>(x.size());
    for (int m = 0; m < count; ++m) {
        const double t = static_cast<double>(m) * fsIn / fsOut;   // position in input samples
        const auto lo = std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(std::ceil(t - half)));
        const auto hi = std::min<std::ptrdiff_t>(n - 1, static_cast<std::ptrdiff_t>(std::floor(t + half)));
        double acc = 0.0;
        for (std::ptrdiff_t i = lo; i <= hi; ++i) {
            const double d = static_cast<double>(i) - t;
            const double pos = std::abs(d) / half * kWinPoints;
            const auto wi = std::min(static_cast<int>(pos), kWinPoints);
            const double wf = pos - static_cast<double>(wi);
            const double w = win[static_cast<std::size_t>(wi)] + wf * (win[static_cast<std::size_t>(wi) + 1U] - win[static_cast<std::size_t>(wi)]);
            const double a = 2.0 * kPi * fc * d;
            const double s = std::abs(a) < 1.0e-12 ? 1.0 : std::sin(a) / a;
            acc += x[static_cast<std::size_t>(i)] * 2.0 * fc * s * w;
        }
        y[static_cast<std::size_t>(m)] = acc;
    }
    return y;
}

}  // namespace

bool romFromAudio(const float* samples, int numSamples, double sampleRate, RomRegion region, RomImage& image) {
    if (samples == nullptr || numSamples <= 0 || !std::isfinite(sampleRate) || sampleRate < 4000.0 ||
        sampleRate > 1.0e6)
        return false;
    const int r = static_cast<int>(region);
    if (r < 0 || r > static_cast<int>(RomRegion::Ride)) return false;

    // 1. validate, mean, onset
    double mean = 0.0, peak = 0.0;
    for (int i = 0; i < numSamples; ++i) {
        const double v = static_cast<double>(samples[i]);
        if (!std::isfinite(v)) return false;
        mean += v;
    }
    mean /= static_cast<double>(numSamples);
    for (int i = 0; i < numSamples; ++i) peak = std::max(peak, std::abs(static_cast<double>(samples[i]) - mean));
    if (!(peak > 1.0e-9)) return false;
    int onset = 0;
    while (onset < numSamples && std::abs(static_cast<double>(samples[onset]) - mean) < 0.05 * peak) ++onset;
    onset = std::max(0, onset - static_cast<int>(0.001 * sampleRate));

    const auto span = dsp::pcm::romSpan(region);
    const double fsOut = dsp::pcm::romBuildRateHz(region);
    const bool hat = region == RomRegion::HatOpen || region == RomRegion::HatClosed;
    const double cut = (hat ? 0.40 : 0.45) * std::min(fsOut, sampleRate);
    // Only what the region can hold (plus the kernel's reach) is needed.
    const double needIn = static_cast<double>(span.length()) * sampleRate / fsOut + 64.0 * sampleRate / cut;
    const int last = static_cast<int>(std::min<double>(numSamples, onset + needIn));
    std::vector<double> x(static_cast<std::size_t>(last - onset));
    for (int i = onset; i < last; ++i) x[static_cast<std::size_t>(i - onset)] = static_cast<double>(samples[i]) - mean;

    // 2. resample
    const int avail = static_cast<int>(std::floor(static_cast<double>(numSamples - onset) * fsOut / sampleRate));
    const int count = std::max(1, std::min(span.length(), avail));
    std::vector<double> y = resample(x, sampleRate, fsOut, cut, count);

    // 3. envelope flattening: centred 25 ms RMS
    const int win = std::max(3, static_cast<int>(0.025 * fsOut));
    std::vector<double> prefix(static_cast<std::size_t>(count) + 1U, 0.0);
    for (int i = 0; i < count; ++i)
        prefix[static_cast<std::size_t>(i) + 1U] = prefix[static_cast<std::size_t>(i)] + y[static_cast<std::size_t>(i)] * y[static_cast<std::size_t>(i)];
    std::vector<double> env(static_cast<std::size_t>(count));
    double envMax = 0.0;
    for (int i = 0; i < count; ++i) {
        const int a = std::max(0, i - win / 2), b = std::min(count, i + win / 2 + 1);
        const double ms = (prefix[static_cast<std::size_t>(b)] - prefix[static_cast<std::size_t>(a)]) / static_cast<double>(b - a);
        env[static_cast<std::size_t>(i)] = std::sqrt(std::max(ms, 0.0));
        envMax = std::max(envMax, env[static_cast<std::size_t>(i)]);
    }
    if (!(envMax > 0.0)) return false;
    const double floorEnv = envMax * 0.01;   // -40 dB
    for (int i = 0; i < count; ++i) y[static_cast<std::size_t>(i)] /= std::max(env[static_cast<std::size_t>(i)], floorEnv);

    // 4. normalise to the 99.9th percentile
    std::vector<double> mag(y.size());
    for (std::size_t i = 0; i < y.size(); ++i) mag[i] = std::abs(y[i]);
    const auto k = static_cast<std::size_t>(std::floor(0.999 * static_cast<double>(mag.size() - 1)));
    std::nth_element(mag.begin(), mag.begin() + static_cast<std::ptrdiff_t>(k), mag.end());
    const double ref = mag[k] > 0.0 ? mag[k] : 1.0;

    // 5. fade where the material ends inside the region, quantise, pad
    // The material always fades out over its last 2 ms and the last word is
    // mid-scale, so a stopped counter leaves the latch at rest (no offset into
    // the coupling capacitor on the next trigger).
    const int fade = std::max(1, std::min(count, static_cast<int>(0.002 * fsOut)));
    for (int i = 0; i < span.length(); ++i) {
        int code = kRomMidScale;
        if (i < count) {
            double v = y[static_cast<std::size_t>(i)] / ref;
            if (i >= count - fade && fade > 0) v *= static_cast<double>(count - i) / static_cast<double>(fade + 1);
            code = 32 + static_cast<int>(std::floor(31.5 * v + 0.5));
            code = std::clamp(code, 0, 63);
            if (i == count - 1) code = kRomMidScale;
        }
        image.data[static_cast<std::size_t>(span.start + i)] = static_cast<std::uint8_t>(code);
    }
    return true;
}

}  // namespace mw909
