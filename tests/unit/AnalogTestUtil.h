// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Shared helpers for the analog-voice tests (BD, SD, toms, RS, CP): a render
// rig that drives any voice sample-accurately with the shared noise source,
// signal measurements (pitch tracks and sweep fits, decay, level, spectra,
// harmonics), WAV I/O for the reference recordings, and the test bodies that
// are the same for every voice (fuzz, determinism, sleep, partitioning).
// Adapted from the sibling 808 emulation's bridged-T test utilities.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "mw909/Voices.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/Noise.h"

namespace analog_test {

using mw909::Hit;
using mw909::VoiceParams;
using mw909::VoiceSetup;
namespace dsp = mw909::dsp;

inline constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};
inline constexpr std::uint32_t kNoiseSeed = 0x909U;

// ---------------------------------------------------------------- render rig

template <class V>
void renderBlock(V& voice, float* out, const float* noise, int n) {
    if constexpr (requires { voice.render(out, noise, n); }) {
        voice.render(out, noise, n);
    } else {
        (void)noise;
        voice.render(out, n);
    }
}

struct Event {
    double seconds;
    Hit hit;
};

inline VoiceSetup setupAt(double fs, std::uint32_t unitSeed = 0, float variance = 0.0f,
                          mw909::Revision rev = mw909::Revision::Late) {
    VoiceSetup s;
    s.sampleRate = fs;
    s.oversample = 1;
    s.unitSeed = unitSeed;
    s.variance = variance;
    s.revision = rev;
    return s;
}

// A voice is prepared by a callable (the toms need their VoiceId).
template <class V>
using Prep = std::function<void(V&, const VoiceSetup&)>;

template <class V>
struct Rig {
    V voice;
    dsp::WhiteNoise bus;
    std::vector<float> noise;
    double fs = 48000.0;

    Rig(const Prep<V>& prepFn, const VoiceSetup& setup, std::uint32_t noiseSeed = kNoiseSeed) : fs(setup.sampleRate) {
        prepFn(voice, setup);
        bus.prepare(setup.sampleRate);
        bus.seed(noiseSeed);
    }

    // Append n samples to `out`, in blocks of at most `block`.
    void run(std::vector<float>& out, int n, int block = 64) {
        const std::size_t start = out.size();
        out.resize(start + static_cast<std::size_t>(n));
        noise.resize(static_cast<std::size_t>(std::max(block, 1)));
        int done = 0;
        while (done < n) {
            const int m = std::min(block, n - done);
            bus.fill(noise.data(), m);
            renderBlock(voice, out.data() + start + static_cast<std::size_t>(done), noise.data(), m);
            done += m;
        }
    }
};

// One-shot render: prepare, set params, fire the events at their sample
// positions, render `seconds`.
template <class V>
std::vector<float> render(const Prep<V>& prep, const VoiceSetup& setup, const VoiceParams& params,
                          const std::vector<Event>& events, double seconds, std::uint32_t noiseSeed = kNoiseSeed,
                          int block = 64) {
    Rig<V> rig(prep, setup, noiseSeed);
    rig.voice.setParams(params);
    std::vector<float> out;
    const int total = static_cast<int>(std::lround(seconds * setup.sampleRate));
    out.reserve(static_cast<std::size_t>(total));
    int pos = 0;
    for (const Event& e : events) {
        const int at = std::min(total, static_cast<int>(std::lround(e.seconds * setup.sampleRate)));
        if (at > pos) {
            rig.run(out, at - pos, block);
            pos = at;
        }
        rig.voice.trigger(e.hit);
    }
    if (total > pos) rig.run(out, total - pos, block);
    return out;
}

template <class V>
std::vector<float> renderHit(const Prep<V>& prep, double fs, const VoiceParams& params, const Hit& hit,
                             double seconds, std::uint32_t noiseSeed = kNoiseSeed) {
    return render<V>(prep, setupAt(fs), params, {{0.0, hit}}, seconds, noiseSeed);
}

// ------------------------------------------------------------- measurements

inline bool allFinite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

inline double peakAbs(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, static_cast<double>(std::abs(v)));
    return p;
}

inline double vpp(const std::vector<float>& x, std::size_t a = 0, std::size_t b = SIZE_MAX) {
    b = std::min(b, x.size());
    if (b <= a) return 0.0;
    const auto [lo, hi] = std::minmax_element(x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b));
    return static_cast<double>(*hi) - static_cast<double>(*lo);
}

inline double vppWindow(const std::vector<float>& x, double fs, double t0, double t1) {
    return vpp(x, static_cast<std::size_t>(t0 * fs), static_cast<std::size_t>(t1 * fs));
}

inline double rms(const std::vector<float>& x, double fs, double t0, double t1) {
    const std::size_t a = static_cast<std::size_t>(std::max(0.0, t0) * fs);
    const std::size_t b = std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}

inline double toDb(double ratio) { return 20.0 * std::log10(std::max(ratio, 1.0e-30)); }

// First sample whose magnitude reaches `fraction` of the peak (the reference
// recordings are trimmed at such a point).
inline std::size_t onsetIndex(const std::vector<float>& x, double fraction = 0.01) {
    const double p = peakAbs(x);
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) >= fraction * p) return i;
    return 0;
}

inline std::vector<float> fromOnset(const std::vector<float>& x, double fraction = 0.01) {
    const std::size_t o = onsetIndex(x, fraction);
    return std::vector<float>(x.begin() + static_cast<std::ptrdiff_t>(o), x.end());
}

// Second-order Butterworth sections run forward and backward (zero phase).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    static Biquad lowpass(double f, double fs, double q = 0.70710678) {
        const double w = dsp::kTwoPi * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2.0 * q);
        Biquad b;
        const double a0 = 1.0 + al;
        b.b0 = (1.0 - c) / 2.0 / a0;
        b.b1 = (1.0 - c) / a0;
        b.b2 = b.b0;
        b.a1 = -2.0 * c / a0;
        b.a2 = (1.0 - al) / a0;
        return b;
    }
    static Biquad highpass(double f, double fs, double q = 0.70710678) {
        const double w = dsp::kTwoPi * f / fs, c = std::cos(w), s = std::sin(w), al = s / (2.0 * q);
        Biquad b;
        const double a0 = 1.0 + al;
        b.b0 = (1.0 + c) / 2.0 / a0;
        b.b1 = -(1.0 + c) / a0;
        b.b2 = b.b0;
        b.a1 = -2.0 * c / a0;
        b.a2 = (1.0 - al) / a0;
        return b;
    }
    void run(std::vector<double>& x) const {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (double& v : x) {
            const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = y;
            v = y;
        }
    }
};

inline std::vector<double> toDouble(const std::vector<float>& x) { return std::vector<double>(x.begin(), x.end()); }

inline std::vector<double> zeroPhase(std::vector<double> x, const Biquad& bq, int order2 = 1) {
    for (int k = 0; k < order2; ++k) {
        bq.run(x);
        std::reverse(x.begin(), x.end());
        bq.run(x);
        std::reverse(x.begin(), x.end());
    }
    return x;
}

// Vpp below `hz` (zero-phase, two Butterworth biquads each way). The noise
// voices (SD snappy, CP) carry content up to the host Nyquist, so their raw
// Vpp grows with the host rate (+3..11 % from 48 to 192 kHz) while their
// in-band level does not; their level is calibrated and checked in this band.
inline double vppInBand(const std::vector<float>& x, double fs, double hz = 16000.0) {
    if (x.empty()) return 0.0;
    const auto v = zeroPhase(toDouble(x), Biquad::lowpass(hz, fs), 2);
    return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
}

// In-place radix-2 FFT.
inline void fft(std::vector<std::complex<double>>& a, bool inverse = false) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> wl = std::polar(1.0, (inverse ? 1.0 : -1.0) * dsp::kTwoPi / static_cast<double>(len));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& v : a) v /= static_cast<double>(n);
}

// Magnitude of the analytic signal (Hilbert envelope).
inline std::vector<double> hilbertEnvelope(const std::vector<double>& x) {
    std::size_t n = 1;
    while (n < x.size()) n <<= 1;
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    fft(a);
    for (std::size_t i = 1; i < n / 2; ++i) a[i] *= 2.0;
    for (std::size_t i = n / 2 + 1; i < n; ++i) a[i] = 0.0;
    fft(a, true);
    std::vector<double> e(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) e[i] = std::abs(a[i]);
    return e;
}

inline std::vector<double> movingAverage(const std::vector<double>& x, std::size_t w) {
    std::vector<double> y(x.size());
    if (w < 1) w = 1;
    const std::size_t half = w / 2;
    std::vector<double> c(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) c[i + 1] = c[i] + x[i];
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t a = i >= half ? i - half : 0;
        const std::size_t b = std::min(x.size(), i + half + 1);
        y[i] = (c[b] - c[a]) / static_cast<double>(b - a);
    }
    return y;
}

// The research's decay metric (research/01 F1): low-pass, Hilbert envelope
// smoothed over `smoothSeconds`, 0 dB = its mean over [ref0, ref1), and the
// first time after ref1 at which it falls below `db`. Time from x[0].
inline double decayTime(const std::vector<float>& x, double fs, double db, double lowpassHz = 400.0,
                        double smoothSeconds = 0.020, double ref0 = 0.020, double ref1 = 0.050) {
    std::vector<double> v = toDouble(x);
    if (lowpassHz > 0.0) v = zeroPhase(v, Biquad::lowpass(lowpassHz, fs));
    std::vector<double> e = movingAverage(hilbertEnvelope(v), static_cast<std::size_t>(smoothSeconds * fs));
    const std::size_t a = static_cast<std::size_t>(ref0 * fs), b = std::min(e.size(), static_cast<std::size_t>(ref1 * fs));
    double ref = 0.0;
    for (std::size_t i = a; i < b; ++i) ref += e[i];
    ref /= static_cast<double>(std::max<std::size_t>(1, b - a));
    const double thr = ref * std::pow(10.0, db / 20.0);
    for (std::size_t i = b; i < e.size(); ++i)
        if (e[i] < thr) return static_cast<double>(i) / fs;
    return static_cast<double>(e.size()) / fs;
}

// Generic first crossing of a smoothed RMS envelope below `db` re its maximum
// (for voices without a plateau), time from x[0].
inline double rmsDecayTime(const std::vector<float>& x, double fs, double db, double windowSeconds = 0.005) {
    std::vector<double> sq(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) sq[i] = static_cast<double>(x[i]) * x[i];
    const std::vector<double> m = movingAverage(sq, static_cast<std::size_t>(windowSeconds * fs));
    const auto im = static_cast<std::size_t>(std::max_element(m.begin(), m.end()) - m.begin());
    const double thr = m[im] * std::pow(10.0, db / 10.0);
    for (std::size_t i = im; i < m.size(); ++i)
        if (m[i] < thr) return static_cast<double>(i) / fs;
    return static_cast<double>(m.size()) / fs;
}

// Upward zero crossings (sub-sample) of a low-passed copy.
inline std::vector<double> zeroCrossings(const std::vector<float>& x, double fs, double lowpassHz) {
    std::vector<double> v = toDouble(x);
    if (lowpassHz > 0.0) v = zeroPhase(v, Biquad::lowpass(lowpassHz, fs), 2);
    std::vector<double> t;
    for (std::size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] < 0.0 && v[i] >= 0.0) t.push_back((static_cast<double>(i - 1) + v[i - 1] / (v[i - 1] - v[i])) / fs);
    return t;
}

// Per-cycle frequency track: (time of the cycle's centre, 1 / period).
struct Track {
    std::vector<double> t, f;
};
inline Track cycleTrack(const std::vector<float>& x, double fs, double lowpassHz, double t0, double t1) {
    const std::vector<double> z = zeroCrossings(x, fs, lowpassHz);
    Track tr;
    for (std::size_t i = 1; i < z.size(); ++i) {
        const double c = 0.5 * (z[i] + z[i - 1]);
        if (c < t0 || c > t1) continue;
        tr.t.push_back(c);
        tr.f.push_back(1.0 / (z[i] - z[i - 1]));
    }
    return tr;
}

// Least-squares fit of f(t) = fb + df e^(-t / tau): tau by golden search, fb
// and df linear.
struct SweepFit {
    double fb = 0.0, df = 0.0, tau = 0.0, rmsErr = 0.0;
};
inline SweepFit fitSweep(const Track& tr) {
    auto solve = [&](double tau, SweepFit& out) {
        double s1 = 0, se = 0, see = 0, sf = 0, sfe = 0;
        const double n = static_cast<double>(tr.t.size());
        for (std::size_t i = 0; i < tr.t.size(); ++i) {
            const double e = std::exp(-tr.t[i] / tau);
            s1 += 1;
            se += e;
            see += e * e;
            sf += tr.f[i];
            sfe += tr.f[i] * e;
        }
        const double det = s1 * see - se * se;
        out.tau = tau;
        out.fb = det != 0.0 ? (sf * see - se * sfe) / det : 0.0;
        out.df = det != 0.0 ? (s1 * sfe - se * sf) / det : 0.0;
        double err = 0.0;
        for (std::size_t i = 0; i < tr.t.size(); ++i) {
            const double r = tr.f[i] - (out.fb + out.df * std::exp(-tr.t[i] / tau));
            err += r * r;
        }
        out.rmsErr = n > 0 ? std::sqrt(err / n) : 0.0;
        return out.rmsErr;
    };
    double lo = std::log(0.5e-3), hi = std::log(0.3);
    SweepFit best;
    // coarse scan then golden refinement in log tau
    double bestErr = 1e300, bestL = lo;
    for (int i = 0; i <= 60; ++i) {
        const double l = lo + (hi - lo) * i / 60.0;
        SweepFit f;
        const double e = solve(std::exp(l), f);
        if (e < bestErr) {
            bestErr = e;
            bestL = l;
        }
    }
    double a = bestL - (hi - lo) / 60.0, b = bestL + (hi - lo) / 60.0;
    const double g = 0.6180339887;
    for (int i = 0; i < 60; ++i) {
        const double c = b - g * (b - a), d = a + g * (b - a);
        SweepFit f1, f2;
        if (solve(std::exp(c), f1) < solve(std::exp(d), f2)) b = d;
        else a = c;
    }
    solve(std::exp(0.5 * (a + b)), best);
    return best;
}

// Hann-windowed DTFT magnitude at `freq` over x[a, b).
inline double dtftMagnitude(const std::vector<float>& x, double fs, std::size_t a, std::size_t b, double freq) {
    const double w = dsp::kTwoPi * freq / fs;
    const double n = static_cast<double>(b - a);
    std::complex<double> acc = 0.0;
    const std::complex<double> rot = std::polar(1.0, -w);
    std::complex<double> ph = 1.0;
    for (std::size_t i = a; i < b; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(dsp::kTwoPi * static_cast<double>(i - a) / n);
        acc += ph * (hann * static_cast<double>(x[i]));
        ph *= rot;
    }
    return std::abs(acc) / n;
}

// Frequency of the strongest spectral peak in [fLo, fHi] over [t0, t1).
inline double peakFreq(const std::vector<float>& x, double fs, double fLo, double fHi, double t0, double t1) {
    const std::size_t a = static_cast<std::size_t>(t0 * fs);
    const std::size_t b = std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    if (b <= a + 8) return 0.0;
    double best = fLo, bestMag = -1.0;
    const int coarse = 300;
    for (int i = 0; i <= coarse; ++i) {
        const double f = fLo + (fHi - fLo) * i / coarse;
        const double m = dtftMagnitude(x, fs, a, b, f);
        if (m > bestMag) {
            bestMag = m;
            best = f;
        }
    }
    double span = (fHi - fLo) / coarse;
    for (int pass = 0; pass < 5; ++pass) {
        const double lo = best - span, hi = best + span;
        for (int i = 0; i <= 20; ++i) {
            const double f = lo + (hi - lo) * i / 20;
            const double m = dtftMagnitude(x, fs, a, b, f);
            if (m > bestMag) {
                bestMag = m;
                best = f;
            }
        }
        span /= 10.0;
    }
    return best;
}

// Harmonic levels (dB re the fundamental) of a steady segment: the fundamental
// is located near f0Guess, harmonics read at exact multiples.
inline std::vector<double> harmonicsDb(const std::vector<float>& x, double fs, double t0, double t1, double f0Guess, int count) {
    const double f0 = peakFreq(x, fs, 0.8 * f0Guess, 1.2 * f0Guess, t0, t1);
    const std::size_t a = static_cast<std::size_t>(t0 * fs), b = std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    const double h1 = dtftMagnitude(x, fs, a, b, f0);
    std::vector<double> h;
    for (int k = 2; k <= count; ++k) h.push_back(toDb(dtftMagnitude(x, fs, a, b, f0 * k) / h1));
    return h;
}

// Power spectrum over [t0, t1) (Hann, zero-padded to a power of two).
struct Spectrum {
    std::vector<double> power;
    double binHz = 0.0;
    double band(double lo, double hi) const {
        double s = 0.0;
        for (std::size_t i = 0; i < power.size(); ++i) {
            const double f = static_cast<double>(i) * binHz;
            if (f >= lo && f < hi) s += power[i];
        }
        return s;
    }
    double centroid(double lo, double hi) const {
        double num = 0.0, den = 0.0;
        for (std::size_t i = 0; i < power.size(); ++i) {
            const double f = static_cast<double>(i) * binHz;
            if (f >= lo && f < hi) {
                num += f * power[i];
                den += power[i];
            }
        }
        return den > 0.0 ? num / den : 0.0;
    }
    // Frequency of the largest value of the power smoothed over `smoothHz`.
    double peakHz(double lo, double hi, double smoothHz) const {
        const auto w = static_cast<std::size_t>(std::max(1.0, smoothHz / binHz));
        const std::vector<double> s = movingAverage(power, w);
        double best = lo, bestP = -1.0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const double f = static_cast<double>(i) * binHz;
            if (f >= lo && f < hi && s[i] > bestP) {
                bestP = s[i];
                best = f;
            }
        }
        return best;
    }
};

inline Spectrum spectrum(const std::vector<float>& x, double fs, double t0, double t1, std::size_t minSize = 0) {
    const std::size_t a = static_cast<std::size_t>(t0 * fs);
    const std::size_t b = std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    Spectrum s;
    if (b <= a) return s;
    std::size_t n = 1;
    while (n < std::max(b - a, minSize)) n <<= 1;
    std::vector<std::complex<double>> buf(n);
    const double len = static_cast<double>(b - a);
    for (std::size_t i = a; i < b; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(dsp::kTwoPi * static_cast<double>(i - a) / len);
        buf[i - a] = hann * static_cast<double>(x[i]);
    }
    fft(buf);
    s.power.resize(n / 2);
    for (std::size_t i = 0; i < n / 2; ++i) s.power[i] = std::norm(buf[i]) / (len * fs);  // density-like, rate-comparable
    s.binHz = fs / static_cast<double>(n);
    return s;
}

// Octave-band levels (dB re the strongest band) between `lo` and `hi`.
inline std::vector<double> octaveBandsDb(const Spectrum& s, double lo, int bands) {
    std::vector<double> v;
    double f = lo;
    for (int i = 0; i < bands; ++i, f *= 2.0) v.push_back(s.band(f, 2.0 * f));
    const double m = *std::max_element(v.begin(), v.end());
    for (double& d : v) d = 10.0 * std::log10(std::max(d / m, 1e-30));
    return v;
}

// Largest sample-to-sample step in [t0, t1).
inline double maxStep(const std::vector<float>& x, double fs, double t0, double t1) {
    const std::size_t a = std::max<std::size_t>(1, static_cast<std::size_t>(t0 * fs));
    const std::size_t b = std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    double m = 0.0;
    for (std::size_t i = a; i < b; ++i) m = std::max(m, std::abs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
    return m;
}

// ------------------------------------------------------------------ WAV I/O

struct Wav {
    double fs = 0.0;
    std::vector<float> x;
};

// 16-, 24- or 32-bit integer PCM, or 32-bit float; first channel; the DC of
// the last 50 ms removed (as research/05 measured them).
inline bool readWav(const std::string& path, Wav& out, bool removeDc = true) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::string(d.begin(), d.begin() + 4) != "RIFF") return false;
    auto u16 = [&d](std::size_t p) { return static_cast<unsigned>(d[p] | (d[p + 1] << 8)); };
    auto u32 = [&d](std::size_t p) {
        return static_cast<std::uint32_t>(d[p]) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
               (static_cast<std::uint32_t>(d[p + 2]) << 16) | (static_cast<std::uint32_t>(d[p + 3]) << 24);
    };
    unsigned channels = 1, bits = 16, format = 1;
    std::size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const std::string id(d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos) + 4);
        const std::size_t len = u32(pos + 4);
        const std::size_t body = pos + 8;
        if (id == "fmt " && body + 16 <= d.size()) {
            format = u16(body);
            channels = u16(body + 2);
            out.fs = static_cast<double>(u32(body + 4));
            bits = u16(body + 14);
        } else if (id == "data") {
            if (channels == 0) return false;
            const std::size_t bytes = bits / 8;
            const std::size_t end = std::min(d.size(), body + len);
            const std::size_t frame = bytes * channels;
            for (std::size_t p = body; p + frame <= end; p += frame) {
                float v = 0.0f;
                if (format == 3 && bits == 32) {
                    const std::uint32_t u = u32(p);
                    std::memcpy(&v, &u, 4);
                } else if (bits == 16) {
                    v = static_cast<float>(static_cast<std::int16_t>(u16(p))) / 32768.0f;
                } else if (bits == 24) {
                    std::int32_t s = static_cast<std::int32_t>(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16));
                    if (s & 0x800000) s -= 0x1000000;
                    v = static_cast<float>(s) / 8388608.0f;
                } else if (bits == 32) {
                    v = static_cast<float>(static_cast<std::int32_t>(u32(p))) / 2147483648.0f;
                } else {
                    return false;
                }
                out.x.push_back(v);
            }
            if (removeDc && !out.x.empty()) {
                const std::size_t tail = std::min(out.x.size(), static_cast<std::size_t>(0.05 * out.fs));
                double m = 0.0;
                for (std::size_t i = out.x.size() - tail; i < out.x.size(); ++i) m += out.x[i];
                m /= static_cast<double>(std::max<std::size_t>(1, tail));
                for (float& v : out.x) v = static_cast<float>(v - m);
            }
            return !out.x.empty();
        }
        pos = body + len + (len & 1U);
    }
    return false;
}

// 32-bit float mono (levels above 1.0 survive).
inline bool writeWav(const std::string& path, const std::vector<float>& x, double fs, float gain = 1.0f) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto put32 = [&f](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&f](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(x.size() * 4);
    f.write("RIFF", 4);
    put32(36 + bytes);
    f.write("WAVEfmt ", 8);
    put32(16);
    put16(3);
    put16(1);
    put32(static_cast<std::uint32_t>(fs));
    put32(static_cast<std::uint32_t>(fs) * 4);
    put16(4);
    put16(32);
    f.write("data", 4);
    put32(bytes);
    for (float v : x) {
        const float g = v * gain;
        f.write(reinterpret_cast<const char*>(&g), 4);
    }
    return static_cast<bool>(f);
}

// Reference recordings of the original machine (test data only, never
// shipped): the set under the local reference directory that holds the
// per-voice folders (the set is not named here). Empty when absent.
inline std::string refDir() {
    namespace fs = std::filesystem;
    const fs::path root = MW909_REF_SAMPLES_DIR;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {};
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (e.is_directory(ec) && fs::is_directory(e.path() / "BassDrum", ec) && fs::is_directory(e.path() / "HandClap", ec))
            return e.path().string();
    }
    return {};
}
inline bool haveRefs() { return !refDir().empty(); }
inline bool loadRef(const std::string& rel, Wav& w) { return haveRefs() && readWav(refDir() + "/" + rel, w); }

inline std::string renderDir() {
    const char* env = std::getenv("MW909_RENDER_DIR");
    const std::filesystem::path p = env != nullptr ? std::filesystem::path(env)
                                                   : std::filesystem::path(MW909_SOURCE_DIR) / "build" / "renders-analog";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

inline std::string pct(int v) {
    char b[16];
    std::snprintf(b, sizeof b, "%03d", v);
    return b;
}

// -------------------------------------------------- test bodies shared by voices

struct Rng {
    std::uint32_t s;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float uni() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * uni(); }
};

inline VoiceParams randomParams(Rng& rng, int voice) {
    const auto& info = mw909::kVoiceInfo[static_cast<std::size_t>(voice)];
    VoiceParams p{};
    for (int i = 0; i < info.numKnobs; ++i) {
        const auto& k = info.knobs[static_cast<std::size_t>(i)];
        const float u = rng.uni();
        p.k[static_cast<std::size_t>(i)] = u < 0.15f ? k.min : (u > 0.85f ? k.max : rng.range(k.min, k.max));
    }
    return p;
}

inline Hit randomHit(Rng& rng) {
    Hit h;
    const float u = rng.uni();
    h.acc = u < 0.2f ? 5.0f : (u < 0.3f ? 0.0f : rng.range(0.0f, 5.0f));
    h.pitchSemis = rng.uni() < 0.5f ? 0.0f : rng.range(-36.0f, 36.0f);
    return h;
}

// Every op-amp in these circuits runs on +-15 V: no output can exceed the rails.
inline constexpr double kRailBound = 15.0;

// Random parameters and triggers, then a trigger on every sample with the
// knobs at extremes, then parameters AND triggers changing on every sample,
// then silence. Every sample must be finite and within the rails; the voice
// must go to sleep within `maxTailSeconds` and then render exact zeros.
template <class V>
void fuzzRobustness(const Prep<V>& prep, int voice, double fs, std::uint32_t seed, double maxTailSeconds,
                    float variance = 0.0f) {
    Rng rng{seed | 1U};
    Rig<V> rig(prep, setupAt(fs, seed, variance));
    std::vector<float> out;
    double worst = 0.0;
    bool finite = true;
    auto check = [&]() {
        finite = finite && allFinite(out);
        worst = std::max(worst, peakAbs(out));
        out.clear();
    };
    int remaining = static_cast<int>(2.0 * fs);
    while (remaining > 0) {
        rig.voice.setParams(randomParams(rng, voice));
        const int hits = static_cast<int>(rng.next() % 4U);
        for (int h = 0; h < hits; ++h) {
            rig.voice.trigger(randomHit(rng));
            rig.run(out, 1 + static_cast<int>(rng.next() % 64U), 1 + static_cast<int>(rng.next() % 32U));
        }
        const int n = 1 + static_cast<int>(rng.next() % 2048U);
        rig.run(out, n, 1 + static_cast<int>(rng.next() % 256U));
        remaining -= n;
        check();
    }
    const auto& info = mw909::kVoiceInfo[static_cast<std::size_t>(voice)];
    for (int corner = 0; corner < 4; ++corner) {
        VoiceParams p{};
        for (int i = 0; i < info.numKnobs; ++i) {
            const auto& k = info.knobs[static_cast<std::size_t>(i)];
            p.k[static_cast<std::size_t>(i)] = ((corner >> (i % 2)) & 1) != 0 ? k.max : k.min;
        }
        rig.voice.setParams(p);
        const int n = static_cast<int>(0.25 * fs);
        for (int i = 0; i < n; ++i) {
            Hit h;
            h.acc = (i % 3 == 0) ? 5.0f : rng.range(0.0f, 5.0f);
            h.pitchSemis = (i % 1000 < 500) ? 36.0f : -36.0f;
            rig.voice.trigger(h);
            rig.run(out, 1, 1);
        }
        check();
    }
    for (int i = 0; i < static_cast<int>(0.1 * fs); ++i) {
        rig.voice.setParams(randomParams(rng, voice));
        if (rng.uni() < 0.5f) rig.voice.trigger(randomHit(rng));
        rig.run(out, 1, 1);
    }
    check();
    rig.voice.setParams(mw909::defaultParams(voice));
    const int limit = static_cast<int>(maxTailSeconds * fs);
    int waited = 0;
    while (rig.voice.isActive() && waited < limit) {
        rig.run(out, 4096, 4096);
        waited += 4096;
        check();
    }
    INFO("fs " << fs << " seed " << seed << " worst |x| " << worst << " tail " << waited / fs << " s");
    CHECK(finite);
    CHECK(worst < kRailBound);
    REQUIRE_FALSE(rig.voice.isActive());
    rig.run(out, 4096, 512);
    bool zeros = true;
    for (float v : out) zeros = zeros && v == 0.0f;
    CHECK(zeros);
}

// Non-finite control values must not poison the voice.
template <class V>
void checkNonFiniteParams(const Prep<V>& prep, int voice, double fs) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Rig<V> rig(prep, setupAt(fs));
    std::vector<float> out;
    const float bads[] = {nan, inf, -inf, 1e30f, -1e30f};
    for (float bad : bads) {
        for (int k = -1; k < mw909::kMaxKnobs; ++k) {
            VoiceParams p = mw909::defaultParams(voice);
            for (int i = 0; i < mw909::kMaxKnobs; ++i)
                if (k < 0 || i == k) p.k[static_cast<std::size_t>(i)] = bad;
            rig.voice.setParams(mw909::defaultParams(voice));
            rig.voice.trigger(Hit::accented(true, true, 1.0f));
            rig.run(out, 64);
            rig.voice.setParams(p);  // while ringing
            rig.run(out, 256);
            rig.voice.trigger(Hit{4.0f, inf});
            rig.voice.trigger(Hit{inf, nan});
            rig.voice.trigger(Hit{nan, 0.0f});
            rig.voice.setAccLine(nan);
            rig.run(out, 256);
        }
    }
    INFO("fs " << fs);
    CHECK(allFinite(out));
    CHECK(peakAbs(out) < kRailBound);
    CHECK(peakAbs(out) > 0.01);
    rig.voice.setParams(mw909::defaultParams(voice));
    out.clear();
    int waited = 0;
    while (rig.voice.isActive() && waited < static_cast<int>(30.0 * fs)) {
        rig.run(out, 4096, 4096);
        waited += 4096;
    }
    CHECK(allFinite(out));
    CHECK_FALSE(rig.voice.isActive());
}

// A corrupt unit variance must behave as the nearest legal value.
template <class V>
void checkBadVariance(const Prep<V>& prep, double fs) {
    const float bad[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, 3.0f};
    for (float variance : bad) {
        Rig<V> rig(prep, setupAt(fs, 7U, variance));
        rig.voice.trigger(Hit::normal());
        std::vector<float> out;
        while (rig.voice.isActive() && out.size() < static_cast<std::size_t>(30.0 * fs)) rig.run(out, 4096, 4096);
        INFO("variance " << variance);
        CHECK(allFinite(out));
        CHECK(peakAbs(out) > 0.01);
        CHECK_FALSE(rig.voice.isActive());
    }
}

// The voice may only go to sleep once inaudible: nothing in the last
// `windowSeconds` before isActive() falls may exceed 1e-5 V (-120 dBFS), and
// the samples after are exact zeros.
template <class V>
double checkQuietBeforeSleep(const Prep<V>& prep, double fs, const VoiceParams& params, const Hit& hit,
                             double windowSeconds, double maxSeconds = 30.0) {
    Rig<V> rig(prep, setupAt(fs));
    rig.voice.setParams(params);
    rig.voice.trigger(hit);
    std::vector<float> out;
    const std::size_t w = static_cast<std::size_t>(windowSeconds * fs);
    while (rig.voice.isActive() && out.size() < static_cast<std::size_t>(maxSeconds * fs)) rig.run(out, 1, 1);
    REQUIRE_FALSE(rig.voice.isActive());
    const std::size_t from = out.size() > w ? out.size() - w : 0;
    const double tail = peakAbs(std::vector<float>(out.begin() + static_cast<std::ptrdiff_t>(from), out.end()));
    INFO("fs " << fs << " slept at " << static_cast<double>(out.size()) / fs << " s, last " << windowSeconds * 1e3
               << " ms peak " << tail);
    CHECK(tail < 1.0e-5);
    std::vector<float> after;
    rig.run(after, 256, 64);
    for (float v : after) REQUIRE(v == 0.0f);
    return static_cast<double>(out.size()) / fs;
}

// A hit on a voice that went to sleep equals a hit on a freshly prepared one
// (same noise from the trigger on): the wake is exact and click-free.
template <class V>
void checkWakeIsFresh(const Prep<V>& prep, double fs, const VoiceParams& params, double seconds, double tol = 0.0) {
    Rig<V> fresh(prep, setupAt(fs), 11U);
    fresh.voice.setParams(params);
    Rig<V> slept(prep, setupAt(fs), 22U);
    slept.voice.setParams(params);
    slept.voice.trigger(Hit::accented(true, true, 1.0f));
    std::vector<float> junk;
    while (slept.voice.isActive() && junk.size() < static_cast<std::size_t>(30.0 * fs)) slept.run(junk, 512, 512);
    REQUIRE_FALSE(slept.voice.isActive());
    slept.run(junk, 4800, 64);  // asleep a while
    slept.bus.seed(11U);
    fresh.bus.seed(11U);
    fresh.voice.trigger(Hit::normal());
    slept.voice.trigger(Hit::normal());
    std::vector<float> a, b;
    fresh.run(a, static_cast<int>(seconds * fs), 64);
    slept.run(b, static_cast<int>(seconds * fs), 64);
    double diff = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) diff = std::max(diff, std::abs(static_cast<double>(a[i]) - b[i]));
    INFO("fs " << fs << " max |fresh - woken| " << diff << " peak " << peakAbs(a));
    CHECK(diff <= tol * peakAbs(a));
    CHECK(maxStep(b, fs, 0.0, 0.003) < 0.5 * peakAbs(b) + 1e-9);
}

// Two instances given the same seed and calls must agree bit for bit.
template <class V>
void checkDeterministic(const Prep<V>& prep, int voice, double fs) {
    auto play = [&](std::vector<float>& out) {
        Rng rng{0xC0FFEEU};
        Rig<V> rig(prep, setupAt(fs, 77U, 0.5f));
        for (int i = 0; i < 40; ++i) {
            rig.voice.setParams(randomParams(rng, voice));
            rig.voice.trigger(Hit{rng.range(0.5f, 5.0f), 0.0f});
            rig.run(out, 200 + static_cast<int>(rng.next() % 3000U), 1 + static_cast<int>(rng.next() % 128U));
        }
    };
    std::vector<float> a, b;
    play(a);
    play(b);
    REQUIRE(a.size() == b.size());
    bool same = true;
    for (std::size_t i = 0; i < a.size(); ++i) same = same && a[i] == b[i];
    CHECK(same);
    CHECK(peakAbs(a) > 0.01);
}

// The output does not depend on how the host splits the blocks.
template <class V>
void checkPartitionIndependent(const Prep<V>& prep, const VoiceParams& params, double fs) {
    const std::vector<Event> ev = {{0.0, Hit::normal()}, {0.0731, Hit::accented(true, false, 0.0f)},
                                   {0.1502, Hit::weak()}, {0.3, Hit::accented(true, true, 0.7f)}};
    std::vector<float> ref;
    for (int block : {1, 7, 64, 333, 4096}) {
        Rig<V> rig(prep, setupAt(fs));
        rig.voice.setParams(params);
        std::vector<float> out;
        int pos = 0;
        for (const Event& e : ev) {
            const int at = static_cast<int>(std::lround(e.seconds * fs));
            if (at > pos) rig.run(out, at - pos, block);
            pos = at;
            rig.voice.trigger(e.hit);
        }
        rig.run(out, static_cast<int>(0.8 * fs) - pos, block);
        if (ref.empty()) ref = out;
        bool same = ref.size() == out.size();
        for (std::size_t i = 0; same && i < out.size(); ++i) same = ref[i] == out[i];
        INFO("block " << block);
        CHECK(same);
    }
}

// variance = 0 must ignore the unit seed entirely.
template <class V>
void checkSeedIgnoredAtZeroVariance(const Prep<V>& prep, double fs, const VoiceParams& params) {
    const auto a = render<V>(prep, setupAt(fs, 1U, 0.0f), params, {{0.0, Hit::normal()}}, 0.3);
    const auto b = render<V>(prep, setupAt(fs, 987654U, 0.0f), params, {{0.0, Hit::normal()}}, 0.3);
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) same = a[i] == b[i];
    CHECK(same);
}

// Wall-clock bounds mean something only in an optimised, uninstrumented build:
// sanitizer and Debug builds report the figure but do not check it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool kTimingChecks = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
inline constexpr bool kTimingChecks = false;
#elif defined(NDEBUG)
inline constexpr bool kTimingChecks = true;
#else
inline constexpr bool kTimingChecks = false;
#endif
#elif defined(NDEBUG)
inline constexpr bool kTimingChecks = true;
#else
inline constexpr bool kTimingChecks = false;
#endif

inline void checkCpu(double ns, double bound) {
    INFO("ns/sample " << ns << ", bound " << bound << (kTimingChecks ? "" : " (not checked: sanitizer or Debug build)"));
    if (kTimingChecks) CHECK(ns < bound);
    else SUCCEED();
}

// A sounding voice, re-hit every `periodSeconds`: nanoseconds per output sample.
template <class V>
double benchNsPerSample(const Prep<V>& prep, double fs, const VoiceParams& params, double periodSeconds,
                        double seconds = 10.0) {
    Rig<V> rig(prep, setupAt(fs));
    rig.voice.setParams(params);
    const int block = 64;
    std::vector<float> out(static_cast<std::size_t>(block)), noise(static_cast<std::size_t>(block));
    rig.bus.fill(noise.data(), block);
    const int blocks = static_cast<int>(seconds * fs) / block;
    const int every = std::max(1, static_cast<int>(periodSeconds * fs) / block);
    double sink = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) {
        if (b % every == 0) rig.voice.trigger((b / every) % 4 == 0 ? Hit::accented(true, true, 1.0f) : Hit::normal());
        renderBlock(rig.voice, out.data(), noise.data(), block);
        sink += static_cast<double>(out[0]);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    CHECK(std::isfinite(sink));
    return ns / (static_cast<double>(blocks) * block);
}

// Mean of a measurement over `hits` renders with different noise seeds.
template <class V, class F>
double meanOverHits(const Prep<V>& prep, double fs, const VoiceParams& p, const Hit& hit, double seconds, int hits, F&& measure) {
    double s = 0.0;
    for (int h = 0; h < hits; ++h) {
        const auto x = renderHit<V>(prep, fs, p, hit, seconds, 1000U + 7919U * static_cast<std::uint32_t>(h));
        s += measure(x);
    }
    return s / hits;
}

}  // namespace analog_test
