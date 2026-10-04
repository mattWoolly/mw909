// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Shared helpers for the sample-voice tests (hats, cymbals, the PCM chain):
// adapters that give each sample voice one face, level/envelope/spectrum
// measurements, WAV reading for the local reference recordings (statistics
// only; skipped when absent), and the behaviour checks every voice must pass.
// Adapted from the sibling project's metal-voice test helpers.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "mw909/Voices.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/Rom.h"
#include "mw909/voices/Cymbal.h"
#include "mw909/voices/HiHat.h"

namespace pcmtest {

using mw909::Hit;
using mw909::VoiceParams;
using mw909::VoiceSetup;

inline constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

inline VoiceSetup setupFor(double fs, int os = 2, std::uint32_t seed = 0, float variance = 0.0f,
                           mw909::Revision rev = mw909::Revision::Late) {
    VoiceSetup s;
    s.sampleRate = fs;
    s.oversample = os;
    s.unitSeed = seed;
    s.variance = variance;
    s.revision = rev;
    return s;
}

inline Hit normalHit() { return Hit::normal(); }
inline Hit fullAccent() { return Hit::fromCode(mw909::AccentDac::kMaxCode); }

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------

// One hat of the HiHat pair as a single voice (choke on, the other hat at defaults).
template <bool Open>
struct Hat {
    static constexpr int kVoice = Open ? mw909::OH : mw909::CH;
    mw909::voices::HiHat voice;
    VoiceParams other = mw909::defaultParams(Open ? mw909::CH : mw909::OH);
    std::vector<float> scratch;
    void prepare(const VoiceSetup& s) {
        voice.prepare(s);
        voice.setRoms(&mw909::factoryRom(mw909::RomSlot::Hat), nullptr);
    }
    void setParams(const VoiceParams& p) {
        if (Open) voice.setParams(other, p);
        else voice.setParams(p, other);
    }
    void trigger(const Hit& h) {
        if (Open) voice.triggerOpen(h);
        else voice.triggerClosed(h);
    }
    void render(float* out, int n) {
        if (scratch.size() < static_cast<std::size_t>(n)) scratch.resize(static_cast<std::size_t>(n));
        if (Open) voice.render(scratch.data(), out, n);
        else voice.render(out, scratch.data(), n);
    }
    bool isActive() const { return voice.isActive(); }
    void reset() { voice.reset(); }
    int latency() const { return voice.latencySamples(); }
};
using ClosedHat = Hat<false>;
using OpenHat = Hat<true>;

template <mw909::VoiceId Which>
struct Cym {
    static constexpr int kVoice = Which;
    mw909::voices::Cymbal voice;
    void prepare(const VoiceSetup& s) {
        voice.prepare(s, Which);
        voice.setRoms(&mw909::factoryRom(Which == mw909::RD ? mw909::RomSlot::Ride : mw909::RomSlot::Crash), nullptr);
    }
    void setParams(const VoiceParams& p) { voice.setParams(p); }
    void trigger(const Hit& h) { voice.trigger(h); }
    void render(float* out, int n) { voice.render(out, n); }
    bool isActive() const { return voice.isActive(); }
    void reset() { voice.reset(); }
    int latency() const { return voice.latencySamples(); }
};
using Crash = Cym<mw909::CR>;
using Ride = Cym<mw909::RD>;

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Render in uneven blocks, as a host would.
template <class A>
void renderBlocks(A& a, float* out, std::size_t n, int first = 37) {
    std::size_t pos = 0;
    int block = first;
    while (pos < n) {
        const int m = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), n - pos));
        a.render(out + pos, m);
        pos += static_cast<std::size_t>(m);
        block = block == 37 ? 256 : (block == 256 ? 61 : 37);
    }
}

template <class A>
std::vector<float> renderHit(A& a, const VoiceSetup& s, const VoiceParams& p, Hit hit, double seconds) {
    a.prepare(s);
    a.setParams(p);
    a.trigger(hit);
    std::vector<float> out(static_cast<std::size_t>(seconds * s.sampleRate));
    renderBlocks(a, out.data(), out.size());
    return out;
}

template <class A>
std::vector<float> oneHit(double fs, const VoiceParams& p, Hit hit, double seconds, int os = 2, std::uint32_t seed = 0,
                          float variance = 0.0f) {
    A a;
    return renderHit(a, setupFor(fs, os, seed, variance), p, hit, seconds);
}

// ---------------------------------------------------------------------------
// Level and envelope
// ---------------------------------------------------------------------------

inline double vpp(const std::vector<float>& x) {
    double lo = 0.0, hi = 0.0;
    for (float v : x) {
        lo = std::min(lo, static_cast<double>(v));
        hi = std::max(hi, static_cast<double>(v));
    }
    return hi - lo;
}

inline bool allFinite(const std::vector<float>& x) {
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

inline double toDb(double r) { return 20.0 * std::log10(std::max(r, 1.0e-30)); }

// Peak envelope: max |x| in 5 ms windows, 1 ms hop.
inline std::vector<double> peakEnvelope(const std::vector<float>& x, double fs) {
    const auto win = static_cast<std::size_t>(fs * 0.005), hop = static_cast<std::size_t>(fs * 0.001);
    std::vector<double> env;
    for (std::size_t i = 0; i + win < x.size(); i += hop) {
        double p = 0.0;
        for (std::size_t k = i; k < i + win; ++k) p = std::max(p, static_cast<double>(std::abs(x[k])));
        env.push_back(p);
    }
    return env;
}

// The service-manual decay measure: the last time (ms from the buffer start)
// at which the 5 ms peak envelope is still within `db` of its maximum.
inline double timeToDb(const std::vector<float>& x, double fs, double db) {
    const auto env = peakEnvelope(x, fs);
    if (env.empty()) return 0.0;
    const double peak = *std::max_element(env.begin(), env.end());
    const double thr = peak * std::pow(10.0, db / 20.0);
    std::size_t last = 0;
    for (std::size_t i = 0; i < env.size(); ++i)
        if (env[i] >= thr) last = i;
    return static_cast<double>(last) * (std::floor(fs * 0.001) / fs) * 1000.0 + 5.0;
}

// The RMS-envelope decay measure (5 ms windows unless given): first time after
// the peak at which the envelope has fallen `db` (research/05 §8.2). Longer
// windows give a steadier figure for comparisons between renders.
inline double rmsTimeToDb(const std::vector<float>& x, double fs, double db, double window = 0.005) {
    const auto w = static_cast<std::size_t>(window * fs);
    std::vector<double> env(x.size() / w, 0.0);
    for (std::size_t j = 0; j < env.size(); ++j) {
        for (std::size_t k = 0; k < w; ++k) env[j] += static_cast<double>(x[j * w + k]) * x[j * w + k];
        env[j] /= static_cast<double>(w);
    }
    if (env.empty()) return 0.0;
    const auto peak = std::max_element(env.begin(), env.end());
    const double thr = *peak * std::pow(10.0, db / 10.0);
    for (auto it = peak; it != env.end(); ++it)
        if (*it < thr) return static_cast<double>(it - env.begin()) * window * 1000.0;
    return static_cast<double>(env.size()) * window * 1000.0;
}

inline double rms(const std::vector<float>& x, double fs, double t0, double t1) {
    const auto a = static_cast<std::size_t>(t0 * fs);
    const auto b = std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
    return b > a ? std::sqrt(s / static_cast<double>(b - a)) : 0.0;
}

// ---------------------------------------------------------------------------
// Spectra
// ---------------------------------------------------------------------------

inline void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<std::complex<double>> tw(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) {
        const double ang = -2.0 * mw909::dsp::kPi * static_cast<double>(k) / static_cast<double>(n);
        tw[k] = {std::cos(ang), std::sin(ang)};
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t stride = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * tw[k * stride];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

struct Spectrum {
    std::vector<double> power;  // |X|^2, bins 0..N/2
    double binHz = 1.0;
    double energy(double f0, double f1) const {
        double e = 0.0;
        const auto a = static_cast<std::size_t>(std::max(0.0, f0 / binHz));
        const auto b = std::min(power.size(), static_cast<std::size_t>(f1 / binHz) + 1);
        for (std::size_t i = a; i < b; ++i) e += power[i];
        return e;
    }
    double centroid(double f0, double f1) const {
        double num = 0.0, den = 0.0;
        const auto a = static_cast<std::size_t>(std::max(0.0, f0 / binHz));
        const auto b = std::min(power.size(), static_cast<std::size_t>(f1 / binHz) + 1);
        for (std::size_t i = a; i < b; ++i) {
            num += static_cast<double>(i) * binHz * power[i];
            den += power[i];
        }
        return den > 0.0 ? num / den : 0.0;
    }
};

// Windowed power spectrum of x[t0..t1) (seconds), zero-padded to a power of
// two. window: 0 = Hann, otherwise a Kaiser window with that beta.
inline Spectrum spectrumOf(const std::vector<float>& x, double fs, double t0 = 0.0, double t1 = -1.0, double kaiser = 0.0) {
    const auto a = static_cast<std::size_t>(t0 * fs);
    const auto b = t1 < 0.0 ? x.size() : std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    const std::size_t len = b > a ? b - a : 0;
    std::size_t n = 1;
    while (n < len) n <<= 1;
    std::vector<std::complex<double>> buf(n);
    auto i0 = [](double v) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 300; ++k) {
            term *= (0.25 * v * v) / (static_cast<double>(k) * k);
            sum += term;
            if (term < 1e-18 * sum) break;
        }
        return sum;
    };
    const double i0b = kaiser > 0.0 ? i0(kaiser) : 1.0;
    for (std::size_t i = 0; i < len; ++i) {
        double w;
        if (kaiser > 0.0) {
            const double r = 2.0 * static_cast<double>(i) / static_cast<double>(len - 1) - 1.0;
            w = i0(kaiser * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        } else {
            w = 0.5 - 0.5 * std::cos(2.0 * mw909::dsp::kPi * static_cast<double>(i) / static_cast<double>(len));
        }
        buf[i] = static_cast<double>(x[a + i]) * w;
    }
    fft(buf);
    Spectrum s;
    s.binHz = fs / static_cast<double>(n);
    s.power.resize(n / 2 + 1);
    for (std::size_t i = 0; i <= n / 2; ++i) s.power[i] = std::norm(buf[i]);
    return s;
}

// Inverse of fft() (via conjugation).
inline void ifft(std::vector<std::complex<double>>& a) {
    for (auto& v : a) v = std::conj(v);
    fft(a);
    const double inv = 1.0 / static_cast<double>(a.size());
    for (auto& v : a) v = std::conj(v) * inv;
}

// x low-passed by a brick wall at `hz` (FFT, zero-padded): levels compared
// across host rates are compared in the band every rate carries.
inline std::vector<float> bandLimit(const std::vector<float>& x, double fs, double hz) {
    std::size_t n = 1;
    while (n < 2 * x.size()) n <<= 1;
    std::vector<std::complex<double>> b(n);
    for (std::size_t i = 0; i < x.size(); ++i) b[i] = x[i];
    fft(b);
    const auto cut = static_cast<std::size_t>(hz / fs * static_cast<double>(n));
    for (std::size_t i = cut + 1; i < n - cut; ++i) b[i] = 0.0;
    ifft(b);
    std::vector<float> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) y[i] = static_cast<float>(b[i].real());
    return y;
}

// Vpp of x low-passed at `hz`, evaluated on a grid at least 8x finer than
// that band (FFT interpolation), so inter-sample peaks count the same at
// every host rate.
inline double bandLimitedVpp(const std::vector<float>& x, double fs, double hz) {
    std::size_t n = 1;
    while (n < 2 * x.size()) n <<= 1;
    std::size_t up = 1;
    while (fs * static_cast<double>(up) < 16.0 * hz) up <<= 1;
    std::vector<std::complex<double>> b(n);
    for (std::size_t i = 0; i < x.size(); ++i) b[i] = x[i];
    fft(b);
    const auto cut = static_cast<std::size_t>(hz / fs * static_cast<double>(n));
    std::vector<std::complex<double>> big(n * up);
    for (std::size_t i = 0; i <= cut; ++i) big[i] = b[i];
    for (std::size_t i = 1; i <= cut; ++i) big[n * up - i] = b[n - i];
    ifft(big);
    double lo = 0.0, hi = 0.0;
    for (std::size_t i = 0; i < x.size() * up; ++i) {
        const double v = big[i].real() * static_cast<double>(up);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return hi - lo;
}

// The ZOH's sinc null: the clock frequency, independent of the data
// (research/05 §5.1). Welch average (Hann, 8192), 200 Hz smoothing, minimum in [lo, hi].
inline double zohNullHz(const std::vector<float>& x, double fs, double lo, double hi, double t0 = 0.0, double t1 = -1.0) {
    const std::size_t seg = 8192;
    const auto a = static_cast<std::size_t>(t0 * fs);
    const auto b = t1 < 0.0 ? x.size() : std::min(x.size(), static_cast<std::size_t>(t1 * fs));
    std::vector<double> p(seg / 2 + 1, 0.0);
    for (std::size_t s = a; s + seg <= b; s += seg / 2) {
        std::vector<std::complex<double>> buf(seg);
        for (std::size_t i = 0; i < seg; ++i)
            buf[i] = static_cast<double>(x[s + i]) * (0.5 - 0.5 * std::cos(2.0 * mw909::dsp::kPi * static_cast<double>(i) / seg));
        fft(buf);
        for (std::size_t i = 0; i <= seg / 2; ++i) p[i] += std::norm(buf[i]);
    }
    const double bin = fs / seg;
    const auto half = static_cast<std::ptrdiff_t>(100.0 / bin);
    double best = 1e300, at = 0.0;
    for (auto i = static_cast<std::ptrdiff_t>(lo / bin); i <= static_cast<std::ptrdiff_t>(hi / bin); ++i) {
        double sum = 0.0;
        for (auto k = i - half; k <= i + half; ++k)
            if (k >= 0 && k < static_cast<std::ptrdiff_t>(p.size())) sum += p[static_cast<std::size_t>(k)];
        if (sum < best) {
            best = sum;
            at = static_cast<double>(i) * bin;
        }
    }
    return at;
}

// Time (s) of the last sample above `rel` x the peak: the counter stop.
inline double endTime(const std::vector<float>& x, double fs, double rel = 1.0e-3) {
    double pk = 0.0;
    for (float v : x) pk = std::max(pk, static_cast<double>(std::abs(v)));
    std::size_t last = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) > rel * pk) last = i;
    return static_cast<double>(last) / fs;
}

// ---------------------------------------------------------------------------
// WAV reading (reference recordings: 16/24-bit PCM, mono or first channel)
// ---------------------------------------------------------------------------

struct Wav {
    std::vector<float> x;
    double fs = 0.0;
};

inline bool readWav(const std::string& path, Wav& wav) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::string(d.begin(), d.begin() + 4) != "RIFF") return false;
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
               (static_cast<std::uint32_t>(d[o + 2]) << 16) | (static_cast<std::uint32_t>(d[o + 3]) << 24);
    };
    auto u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8); };
    std::size_t pos = 12;
    std::uint32_t channels = 1, bits = 16;
    while (pos + 8 <= d.size()) {
        const std::string id(d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos + 4));
        const std::size_t size = u32(pos + 4);
        if (id == "fmt ") {
            channels = u16(pos + 10);
            wav.fs = static_cast<double>(u32(pos + 12));
            bits = u16(pos + 22);
        } else if (id == "data") {
            if ((bits != 16 && bits != 24) || channels == 0) return false;
            const std::size_t bytes = bits / 8, frame = bytes * channels;
            const std::size_t end = std::min(d.size(), pos + 8 + size);
            for (std::size_t i = pos + 8; i + frame <= end; i += frame) {
                if (bits == 16) {
                    wav.x.push_back(static_cast<float>(static_cast<std::int16_t>(u16(i))) / 32768.0f);
                } else {
                    const auto s = static_cast<std::int32_t>((static_cast<std::uint32_t>(d[i]) | (static_cast<std::uint32_t>(d[i + 1]) << 8) |
                                                              (static_cast<std::uint32_t>(d[i + 2]) << 16))
                                                             << 8) >>
                                   8;
                    wav.x.push_back(static_cast<float>(s) / 8388608.0f);
                }
            }
            return !wav.x.empty();
        }
        pos += 8 + size + (size & 1U);
    }
    return false;
}

// Find a reference recording "<Group>/<file>" in any set under the local
// reference directory (the set directories are not named here). Empty when
// the directory or the file is missing.
inline std::string findRef(const std::string& group, const std::string& file) {
    namespace fs = std::filesystem;
    const fs::path root = MW909_REF_SAMPLES_DIR;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {};
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        const fs::path p = e.path() / group / file;
        if (fs::exists(p, ec)) return p.string();
    }
    return {};
}

// Remove the DC (mean of the last 50 ms) of a recording.
inline void removeTailDc(std::vector<float>& x, double fs) {
    const auto n = std::min(x.size(), static_cast<std::size_t>(0.05 * fs));
    if (n == 0) return;
    double m = 0.0;
    for (std::size_t i = x.size() - n; i < x.size(); ++i) m += x[i];
    m /= static_cast<double>(n);
    for (float& v : x) v = static_cast<float>(v - m);
}

// ---------------------------------------------------------------------------
// Behaviour checks
// ---------------------------------------------------------------------------

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    std::uint32_t next() { return s = s * 1664525U + 1013904223U; }
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

template <class A>
VoiceParams randomParams(Lcg& r, bool extremes) {
    const auto& info = mw909::kVoiceInfo[static_cast<std::size_t>(A::kVoice)];
    VoiceParams p{};
    for (int i = 0; i < info.numKnobs; ++i) {
        const auto& k = info.knobs[static_cast<std::size_t>(i)];
        p.k[static_cast<std::size_t>(i)] = extremes ? (r.unit() < 0.5f ? k.min : k.max) : r.range(k.min, k.max);
    }
    return p;
}

// Random parameters and triggers, a trigger on every sample, then silence:
// finite, bounded, then inactive with exact zeros.
template <class A>
void checkRobust(double fs, double maxSleepSeconds) {
    A a;
    a.prepare(setupFor(fs, 2, 99U, 1.0f));
    Lcg rng(0xC0FFEEU + static_cast<std::uint32_t>(fs));
    std::vector<float> buf(512);
    double worst = 0.0;
    bool finite = true;
    auto run = [&](int n) {
        a.render(buf.data(), n);
        for (int i = 0; i < n; ++i) {
            finite = finite && std::isfinite(buf[static_cast<std::size_t>(i)]);
            worst = std::max(worst, static_cast<double>(std::abs(buf[static_cast<std::size_t>(i)])));
        }
    };
    for (int block = 0; block < 300; ++block) {
        if (rng.unit() < 0.3f) a.setParams(randomParams<A>(rng, rng.unit() < 0.3f));
        if (rng.unit() < 0.4f) a.trigger(Hit{rng.range(0.0f, 5.0f), rng.range(-12.0f, 12.0f)});
        run(1 + static_cast<int>(rng.next() % 500U));
    }
    for (int corner = 0; corner < 2; ++corner) {
        a.setParams(randomParams<A>(rng, true));
        for (int i = 0; i < static_cast<int>(fs / 4); ++i) {
            a.trigger(fullAccent());
            run(1);
        }
    }
    INFO("fs " << fs << " worst |x| " << worst);
    REQUIRE(finite);
    REQUIRE(worst < 10.0);
    a.setParams(mw909::defaultParams(A::kVoice));
    double waited = 0.0;
    while (a.isActive() && waited < maxSleepSeconds) {
        run(512);
        waited += 512.0 / fs;
    }
    INFO("went inactive after " << waited << " s");
    REQUIRE(finite);
    REQUIRE_FALSE(a.isActive());
    for (int k = 0; k < 4; ++k) {
        a.render(buf.data(), 512);
        for (float v : buf) REQUIRE(v == 0.0f);
    }
}

// NaN/Inf parameters take the defaults; NaN hit values a normal hit.
template <class A>
void checkNanSafe(double fs) {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    auto render = [&](const VoiceParams& p, Hit h) {
        A a;
        a.prepare(setupFor(fs));
        a.setParams(p);
        a.trigger(h);
        std::vector<float> out(static_cast<std::size_t>(0.1 * fs));
        a.render(out.data(), static_cast<int>(out.size()));
        return out;
    };
    const VoiceParams def = mw909::defaultParams(A::kVoice);
    const auto stock = render(def, Hit::normal());
    VoiceParams pn{};
    pn.k.fill(nan);
    REQUIRE(render(pn, Hit::normal()) == stock);
    REQUIRE(render(def, Hit{nan, nan}) == stock);
    // non-finite -> the default (architecture §1), for knobs and hit values alike
    VoiceParams pi{};
    pi.k.fill(-inf);
    REQUIRE(render(pi, Hit{inf, -inf}) == stock);
    // out of range -> clamped
    VoiceParams big{}, pmax{};
    big.k.fill(1.0e6f);
    const auto& info = mw909::kVoiceInfo[static_cast<std::size_t>(A::kVoice)];
    for (int i = 0; i < info.numKnobs; ++i) pmax.k[static_cast<std::size_t>(i)] = info.knobs[static_cast<std::size_t>(i)].max;
    REQUIRE(render(big, Hit{1.0e6f, 1.0e6f}) == render(pmax, Hit{5.0f, 24.0f}));
    A a;
    a.prepare(setupFor(fs, 2, 7U, 1.0f));
    const float junk[] = {nan, inf, -inf, 1.0e30f, -1.0e30f, 1.0e-40f, -0.0f, 36.0f, -36.0f};
    std::vector<float> one(1);
    bool finite = true;
    for (int i = 0; i < static_cast<int>(fs / 8); ++i) {
        VoiceParams p{};
        for (std::size_t k = 0; k < p.k.size(); ++k) p.k[k] = junk[(static_cast<std::size_t>(i) + k * 3U) % 9U];
        a.setParams(p);
        if (i % 3 == 0) a.trigger(Hit{junk[static_cast<std::size_t>(i) % 9U], junk[static_cast<std::size_t>(i / 9) % 9U]});
        a.render(one.data(), 1);
        finite = finite && std::isfinite(one[0]);
    }
    REQUIRE(finite);
}

// Same seed and calls -> identical samples; variance 0 -> independent of the
// seed; variance 1 -> different seeds differ.
template <class A>
void checkDeterminism(double seconds) {
    auto render = [&](std::uint32_t seed, float variance) {
        A a;
        a.prepare(setupFor(48000.0, 2, seed, variance));
        a.setParams(mw909::defaultParams(A::kVoice));
        std::vector<float> out(static_cast<std::size_t>(seconds * 48000.0));
        std::vector<float> pre(1000);
        a.render(pre.data(), 1000);
        a.trigger(Hit::normal());
        a.render(out.data(), static_cast<int>(out.size() / 2));
        a.trigger(fullAccent());
        a.render(out.data() + out.size() / 2, static_cast<int>(out.size() - out.size() / 2));
        return out;
    };
    const auto a1 = render(7U, 1.0f), a2 = render(7U, 1.0f);
    REQUIRE(a1 == a2);
    REQUIRE(render(1U, 0.0f) == render(4242U, 0.0f));
    REQUIRE(a1 != render(8U, 1.0f));
    REQUIRE(a1 != render(1U, 0.0f));
    REQUIRE(allFinite(a1));
}

// A retrigger during a ring differs from a fresh hit (state carried: VCA
// control, coupling capacitors, filter ringing), and makes no step larger
// than the natural signal does.
template <class A>
void checkRetrigger(double gapSeconds, double seconds) {
    const double fs = 48000.0;
    const auto gap = static_cast<std::size_t>(gapSeconds * fs);
    const auto n = static_cast<std::size_t>(seconds * fs);
    const VoiceSetup s = setupFor(fs);
    const VoiceParams p = mw909::defaultParams(A::kVoice);
    A one;
    const std::vector<float> single = renderHit(one, s, p, Hit::normal(), seconds);
    A two;
    two.prepare(s);
    two.setParams(p);
    std::vector<float> twice(n);
    two.trigger(Hit::normal());
    two.render(twice.data(), static_cast<int>(gap));
    two.trigger(Hit::normal());
    two.render(twice.data() + gap, static_cast<int>(n - gap));
    // the retriggered part vs a fresh hit at the same time
    double diff = 0.0, ref = 0.0;
    for (std::size_t i = gap; i < n; ++i) {
        const double fresh = single[i - gap];
        diff += (twice[i] - fresh) * (twice[i] - fresh);
        ref += fresh * fresh;
    }
    double stepAny = 0.0, stepRetrig = 0.0;
    for (std::size_t i = 1; i < n; ++i) stepAny = std::max(stepAny, static_cast<double>(std::abs(single[i] - single[i - 1])));
    for (std::size_t i = gap; i < gap + static_cast<std::size_t>(0.004 * fs); ++i)
        stepRetrig = std::max(stepRetrig, static_cast<double>(std::abs(twice[i] - twice[i - 1])));
    INFO("difference / fresh energy " << diff / ref << ", retrigger step " << stepRetrig << " vs max step " << stepAny);
    REQUIRE(diff > 1.0e-7 * ref);
    REQUIRE(diff < 0.5 * ref);
    REQUIRE(stepRetrig <= 2.0 * stepAny);
    REQUIRE(allFinite(twice));
}

// Any block partition gives the same bytes.
template <class A>
void checkPartition(double fs, double seconds) {
    auto run = [&](int fixedBlock) {
        A a;
        a.prepare(setupFor(fs, 2, 5U, 0.5f));
        a.setParams(mw909::defaultParams(A::kVoice));
        std::vector<float> out(static_cast<std::size_t>(seconds * fs));
        std::size_t pos = 0;
        int hits = 0;
        Lcg r(42U);
        while (pos < out.size()) {
            if (pos >= static_cast<std::size_t>(hits) * static_cast<std::size_t>(0.15 * fs)) {
                // triggers land on fixed sample positions whatever the partition: split there
                a.trigger(hits % 2 ? fullAccent() : Hit::normal());
                ++hits;
            }
            const std::size_t nextHit = static_cast<std::size_t>(hits) * static_cast<std::size_t>(0.15 * fs);
            std::size_t m = fixedBlock > 0 ? static_cast<std::size_t>(fixedBlock) : 1 + (r.next() % 700U);
            m = std::min({m, out.size() - pos, nextHit > pos ? nextHit - pos : m});
            a.render(out.data() + pos, static_cast<int>(m));
            pos += m;
        }
        return out;
    };
    const auto ref = run(8192);
    REQUIRE(run(1) == ref);
    REQUIRE(run(64) == ref);
    REQUIRE(run(0) == ref);
}

// Sleep and wake: after a hit the voice goes inactive; a hit after sleeping
// renders exactly what a hit on a fresh voice renders (its state is the
// converged one), so waking cannot click.
template <class A>
void checkSleepWake(double fs, double maxSleepSeconds) {
    A a;
    a.prepare(setupFor(fs));
    a.setParams(mw909::defaultParams(A::kVoice));
    a.trigger(Hit::normal());
    std::vector<float> buf(256);
    double waited = 0.0, lastAbs = 1.0;
    while (a.isActive() && waited < maxSleepSeconds) {
        a.render(buf.data(), 256);
        waited += 256.0 / fs;
        lastAbs = 0.0;
        for (float v : buf) lastAbs = std::max(lastAbs, static_cast<double>(std::abs(v)));
    }
    INFO("asleep after " << waited << " s, last block peak " << lastAbs);
    REQUIRE_FALSE(a.isActive());
    // 1e-5 V is -140 dBFS (kDigitalPerVolt = 0.1: 10 V peak = 0 dBFS), well below
    // the contract's -120 dBFS sleep threshold.
    REQUIRE(lastAbs < 1.0e-5);
    for (int i = 0; i < 8; ++i) {
        a.render(buf.data(), 256);
        for (float v : buf) REQUIRE(v == 0.0f);
    }
    a.trigger(Hit::normal());
    std::vector<float> woke(static_cast<std::size_t>(0.2 * fs));
    a.render(woke.data(), static_cast<int>(woke.size()));
    A fresh;
    fresh.prepare(setupFor(fs));
    fresh.setParams(mw909::defaultParams(A::kVoice));
    fresh.trigger(Hit::normal());
    std::vector<float> f1(static_cast<std::size_t>(0.2 * fs));
    fresh.render(f1.data(), static_cast<int>(f1.size()));
    // The woken hit differs from a fresh one only by what the hardware carries
    // over (the DAC latch holds the last word, so the first edge is a step from
    // there): no larger sample-to-sample step than a fresh hit makes anywhere.
    double stepWoke = 0.0, stepFresh = 0.0;
    for (std::size_t i = 1; i < f1.size(); ++i) {
        stepWoke = std::max(stepWoke, static_cast<double>(std::abs(woke[i] - woke[i - 1])));
        stepFresh = std::max(stepFresh, static_cast<double>(std::abs(f1[i] - f1[i - 1])));
    }
    INFO("largest step: woken " << stepWoke << ", fresh " << stepFresh);
    REQUIRE(stepWoke <= 1.5 * stepFresh);
    REQUIRE(std::abs(rms(woke, fs, 0.0, 0.2) / rms(f1, fs, 0.0, 0.2) - 1.0) < 0.05);
    REQUIRE(woke[0] == 0.0f);
}

// ns per output sample while sounding (retriggered every 100 ms).
template <class A>
double benchNsPerSample(double fs, const VoiceParams& p, int os = 2) {
    A a;
    a.prepare(setupFor(fs, os));
    a.setParams(p);
    std::vector<float> buf(256);
    const int blocks = static_cast<int>(fs * 2.0 / 256.0);
    const int perHit = std::max(1, static_cast<int>(fs * 0.1 / 256.0));
    double best = 1.0e30;
    for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) {
            if (b % perHit == 0) a.trigger(fullAccent());
            a.render(buf.data(), 256);
        }
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / (blocks * 256.0));
    }
    return best;
}

}  // namespace pcmtest
