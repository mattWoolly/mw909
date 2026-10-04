// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Shared helpers for the fx_* unit tests: impulse-response DFT, analog
// prototype responses, level measurement, block-partition drivers, a seeded
// signal generator and the per-effect ns/sample timer.

#pragma once

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

namespace fxtest {

using cplx = std::complex<double>;
inline constexpr double kPi = 3.14159265358979323846;
inline const std::vector<double> kRates{44100.0, 48000.0, 96000.0, 192000.0};

// DFT of h at frequency f (Hz).
inline cplx dftAt(const std::vector<float>& h, double f, double fs) {
    cplx acc{0.0, 0.0};
    const double w = 2.0 * kPi * f / fs;
    // Recurrence-free direct sum (accurate enough for a few thousand taps).
    for (std::size_t n = 0; n < h.size(); ++n) acc += static_cast<double>(h[n]) * std::polar(1.0, -w * static_cast<double>(n));
    return acc;
}

inline double magDb(cplx h) { return 20.0 * std::log10(std::max(std::abs(h), 1.0e-300)); }

// Analog 2nd-order prototypes, s normalised to the centre frequency.
inline cplx protoHp(cplx s, double q) { return s * s / (s * s + s / q + 1.0); }
inline cplx protoLp(cplx s, double q) { return 1.0 / (s * s + s / q + 1.0); }
inline cplx protoBell(cplx s, double q, double db) {
    const double A = std::pow(10.0, db / 40.0);
    return (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0);
}
inline cplx protoLowShelf(cplx s, double q, double db) {
    const double A = std::pow(10.0, db / 40.0);
    const double sa = std::sqrt(A);
    return A * (s * s + s * (sa / q) + A) / (A * s * s + s * (sa / q) + 1.0);
}
inline cplx protoHighShelf(cplx s, double q, double db) {
    const double A = std::pow(10.0, db / 40.0);
    const double sa = std::sqrt(A);
    return A * (A * s * s + s * (sa / q) + 1.0) / (s * s + s * (sa / q) + A);
}

// The bilinear transform with the centre pre-warped maps digital frequency f
// to the analog prototype's normalised frequency tan(pi f/fs)/tan(pi f0/fs).
inline cplx warpedS(double f, double f0, double fs) {
    return {0.0, std::tan(kPi * f / fs) / std::tan(kPi * f0 / fs)};
}

inline double rms(const float* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}
inline double peak(const float* x, std::size_t n) {
    double p = 0.0;
    for (std::size_t i = 0; i < n; ++i) p = std::max(p, std::abs(static_cast<double>(x[i])));
    return p;
}
inline bool allFinite(const std::vector<float>& x) {
    for (float v : x)
        if (!std::isfinite(v)) return false;
    return true;
}

inline std::vector<float> sine(double f, double fs, int n, double amp = 1.0, double phase = 0.0) {
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * f * i / fs + phase));
    return x;
}

// Deterministic test noise in [-amp, amp).
struct TestRng {
    std::uint32_t s = 0x9E3779B9u;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
    }
};
inline std::vector<float> noise(int n, float amp = 1.0f, std::uint32_t seed = 1234u) {
    TestRng r{seed};
    std::vector<float> x(static_cast<std::size_t>(n));
    for (auto& v : x) v = amp * r.next();
    return x;
}

// Drum-like test material: decaying noise bursts every `period` samples.
inline std::vector<float> bursts(int n, int period, double fs, float amp = 0.5f, std::uint32_t seed = 77u) {
    TestRng r{seed};
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        const int t = i % period;
        const double env = std::exp(-static_cast<double>(t) / (0.03 * fs));
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * env * (0.6 * r.next() + 0.4 * std::sin(2.0 * kPi * 80.0 * t / fs)));
    }
    return x;
}

// Block sizes for partition tests: an irregular, repeatable sequence.
inline std::vector<int> partition(int total, std::uint32_t seed, int maxBlock = 700) {
    std::vector<int> out;
    TestRng r{seed};
    int left = total;
    while (left > 0) {
        int b = 1 + static_cast<int>((r.next() * 0.5f + 0.5f) * static_cast<float>(maxBlock));
        if (b > left) b = left;
        out.push_back(b);
        left -= b;
    }
    return out;
}

// Schroeder backward integration of an impulse response; returns T60 from the
// -5..-35 dB slope (T30 extrapolated), seconds.
inline double rt60(const std::vector<float>& h, double fs, double fromDb = -5.0, double toDb = -35.0) {
    std::vector<double> e(h.size() + 1, 0.0);
    for (std::size_t i = h.size(); i-- > 0;) e[i] = e[i + 1] + static_cast<double>(h[i]) * h[i];
    const double e0 = e[0];
    if (e0 <= 0.0) return 0.0;
    // Least-squares line through the EDC between the two levels.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    for (std::size_t i = 0; i < h.size(); ++i) {
        const double db = 10.0 * std::log10(std::max(e[i] / e0, 1e-300));
        if (db > fromDb) continue;
        if (db < toDb) break;
        const double t = static_cast<double>(i) / fs;
        sx += t; sy += db; sxx += t * t; sxy += t * db; ++cnt;
    }
    if (cnt < 10) return 0.0;
    const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);   // dB/s
    return -60.0 / slope;
}

// Time a callable processing `samples` samples; returns ns per sample.
inline double nsPerSample(const std::function<void()>& run, long samples, int repeats = 3) {
    double best = 1e30;
    for (int r = 0; r < repeats; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        run();
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(samples);
        best = std::min(best, ns);
    }
    return best;
}

}  // namespace fxtest
