// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Helpers shared by every mixer and FX block (fx/*.h): parameter sanitising,
// smoothers, the control-rate grid, the sleep gate, fast log/exp for the
// compressor gain computers, a soft-knee clipper and tempo-sync arithmetic.
// Header-only, allocation-free, deterministic (no fast-math tricks that
// depend on the compiler: the approximations below are plain arithmetic).
//
// Conventions used by all FX blocks:
//  * Digital full scale is 1.0. The sleep floor is -120 dBFS (1e-6).
//  * Parameters are smoothed. Slow quantities (filter frequencies, Q, times)
//    move on a control grid of about 0.33 ms (controlInterval()); gains and
//    mixes move per sample. The grid is counted from the moment a block wakes
//    up, so output does not depend on how the host splits blocks.
//  * A block sleeps once its input has been below the floor for long enough and
//    every state that could still produce output is below the floor; then it
//    clears that state, so it wakes with exactly the state it would have had
//    (zeros) and a sleeping block costs nothing.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "mw909/dsp/Math.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

inline constexpr float kSleepFloor = 1.0e-6f;   // -120 dBFS
inline constexpr double kDefaultSmoothSeconds = 0.020;

// ---------------------------------------------------------------------------
// Parameter sanitising: non-finite -> default, then clamp.

inline float sanitize(float v, float def, float lo, float hi) noexcept {
    if (!std::isfinite(v)) v = def;
    return v < lo ? lo : (v > hi ? hi : v);
}

template <typename E>
inline E sanitizeEnum(E v, E def, int count) noexcept {
    const int i = static_cast<int>(v);
    return (i >= 0 && i < count) ? v : def;
}

inline double sanitizeBpm(double bpm) noexcept {
    if (!std::isfinite(bpm)) return 120.0;
    return bpm < 1.0 ? 1.0 : (bpm > 1000.0 ? 1000.0 : bpm);
}

// Samples between control ticks: about 0.33 ms at every rate.
inline int controlInterval(double sampleRate) noexcept {
    const long c = std::lround(sampleRate / 3000.0);
    return static_cast<int>(c < 8 ? 8 : (c > 128 ? 128 : c));
}

// ---------------------------------------------------------------------------
// The last parameter block a setParams() applied, so a repeat of the same
// block (the engine calls setParams every block) returns at once. Compared
// bytewise: a false mismatch only costs a recompute.

template <typename P>
struct LastParams {
    P p{};
    double extra = 0.0;
    bool valid = false;
};

template <typename P>
inline bool same(LastParams<P>& last, const P& p, double extra = 0.0) noexcept {
    if (last.valid && last.extra == extra && std::memcmp(&last.p, &p, sizeof(P)) == 0) return true;
    std::memcpy(&last.p, &p, sizeof(P));
    last.extra = extra;
    last.valid = true;
    return false;
}

// ---------------------------------------------------------------------------
// One-pole smoother in double precision. Snaps to the target once within
// `eps`, so a settled smoother is exactly at its target.

class Smoothed {
public:
    void setTime(double seconds, double tickRate) noexcept {
        a_ = seconds > 0.0 ? 1.0 - std::exp(-1.0 / (seconds * tickRate)) : 1.0;
    }
    void setEps(double eps) noexcept { eps_ = eps; }
    void setTarget(double t) noexcept { target_ = t; }
    void snap() noexcept { v_ = target_; }
    void snapTo(double t) noexcept { v_ = target_ = t; }
    double tick() noexcept {
        if (v_ != target_) {
            v_ += (target_ - v_) * a_;
            if (std::abs(target_ - v_) <= eps_) v_ = target_;
        }
        return v_;
    }
    double value() const noexcept { return v_; }
    double target() const noexcept { return target_; }
    bool settled() const noexcept { return v_ == target_; }

private:
    double a_ = 1.0;
    double eps_ = 1.0e-6;
    double v_ = 0.0;
    double target_ = 0.0;
};

// ---------------------------------------------------------------------------
// Crossfade weight 0..1: a linear ramp shaped by smoothstep, so the weight
// starts and ends with zero slope (no kink when a section is switched).
// The ramp position is an integer sample count, so advancing it by n samples
// at once (advance()) lands exactly where n single ticks would, and the
// number of samples left (remaining()) is exact: a fade's end is a sample
// position, whatever the host block size.

class Fade {
public:
    void setTime(double seconds, double sampleRate) noexcept {
        const double t = position();
        const double len = seconds > 0.0 ? std::round(seconds * sampleRate) : 1.0;
        len_ = len < 1.0 ? 1 : (len > 1.0e8 ? 100000000 : static_cast<std::int32_t>(len));
        inv_ = 1.0 / static_cast<double>(len_);
        pos_ = static_cast<std::int32_t>(std::lround(t * static_cast<double>(len_)));
        goal_ = target_ > 0.5 ? len_ : 0;
    }
    void setTarget(double t) noexcept {
        target_ = t > 0.5 ? 1.0 : 0.0;
        goal_ = target_ > 0.5 ? len_ : 0;
    }
    void snap() noexcept { pos_ = goal_; }
    void snapTo(double t) noexcept {
        setTarget(t);
        snap();
    }
    double tick() noexcept {
        if (pos_ != goal_) pos_ += pos_ < goal_ ? 1 : -1;
        return value();
    }
    // n ticks at once (n >= 0).
    void advance(int n) noexcept {
        if (pos_ < goal_) pos_ = goal_ - pos_ > n ? pos_ + n : goal_;
        else if (pos_ > goal_) pos_ = pos_ - goal_ > n ? pos_ - n : goal_;
    }
    // Ticks until the fade reaches its target (0 when settled).
    int remaining() const noexcept { return pos_ < goal_ ? goal_ - pos_ : pos_ - goal_; }
    double value() const noexcept {
        const double t = position();
        return t * t * (3.0 - 2.0 * t);
    }
    double target() const noexcept { return target_; }
    bool settled() const noexcept { return pos_ == goal_; }

private:
    double position() const noexcept {
        return pos_ <= 0 ? 0.0 : (pos_ >= len_ ? 1.0 : static_cast<double>(pos_) * inv_);
    }

    std::int32_t len_ = 1, pos_ = 0, goal_ = 0;
    double inv_ = 1.0;
    double target_ = 0.0;
};

// ---------------------------------------------------------------------------
// floor() without the libm call the baseline x86-64 target makes for it
// (no SSE4.1 round instruction). Exact for |x| < 2^62.

inline double fastFloor(double x) noexcept {
    const auto t = static_cast<double>(static_cast<long long>(x));
    return t > x ? t - 1.0 : t;
}
inline float fastFloor(float x) noexcept {
    const auto t = static_cast<float>(static_cast<int>(x));
    return t > x ? t - 1.0f : t;
}

// ---------------------------------------------------------------------------
// Fast log2 / exp2 (max error ~3e-6 in log2, ~1e-7 relative in exp2), for the
// per-sample compressor gain computers. Pure integer/float arithmetic, so the
// result is the same on every run.

inline float fastLog2(float x) noexcept {
    if (!(x > 1.0e-30f)) return -100.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    const int e = static_cast<int>((bits >> 23) & 0xFFu) - 127;
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;
    float m;
    std::memcpy(&m, &bits, sizeof m);   // m in [1, 2)
    // log2(m) on [1,2): least-squares polynomial in t = m - 1 (max error 2.5e-6).
    const float t = m - 1.0f;
    const float p = t * (1.44253477f + t * (-0.71803340f + t * (0.45715712f +
                    t * (-0.27733943f + t * (0.12147073f + t * -0.02579153f)))));
    return static_cast<float>(e) + p;
}

inline float fastExp2(float x) noexcept {
    if (x < -126.0f) return 0.0f;
    if (x > 126.0f) x = 126.0f;
    const float fl = fastFloor(x);
    const float f = x - fl;   // [0,1)
    // 2^f on [0,1): least-squares polynomial (relative error 3e-9 before rounding).
    const float p = 1.0f + f * (0.693146929f + f * (0.240230501f + f * (0.0554804277f +
                    f * (0.00968457896f + f * (0.00123878265f + f * 0.000218775106f)))));
    const int e = static_cast<int>(fl) + 127;
    const std::uint32_t bits = static_cast<std::uint32_t>(e) << 23;
    float s;
    std::memcpy(&s, &bits, sizeof s);
    return p * s;
}

inline constexpr float kDbPerLog2 = 6.02059991f;   // 20*log10(2)
inline float fastGainToDb(float g) noexcept { return kDbPerLog2 * fastLog2(g); }
inline float fastDbToGain(float db) noexcept { return fastExp2(db * (1.0f / kDbPerLog2)); }

// Parabolic-corrected sine of a phase in cycles (any real), max error ~1e-3.
inline float fastSinCycles(double phase) noexcept {
    const double p = phase - fastFloor(phase);      // [0,1)
    float x = static_cast<float>(p * 2.0 - 1.0);    // [-1,1) -> sin(pi*x) negated
    // y = sin(pi x) approximated by 4x(1-|x|), then refined.
    float y = 4.0f * x * (1.0f - std::abs(x));
    y = 0.225f * (y * std::abs(y) - y) + y;
    return -y;
}

// ---------------------------------------------------------------------------
// tanh by its [7/6] Pade approximant, clamped at +-1 where it reaches 1
// (|x| = 4.97). Max error 1e-4, odd, monotonic; no libm call.

inline float fastTanh(float x) noexcept {
    const float a = std::abs(x);
    if (a >= 4.97179f) return x < 0.0f ? -1.0f : 1.0f;
    const float x2 = x * x;
    const float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2));
    const float y = num / den;
    return y > 1.0f ? 1.0f : (y < -1.0f ? -1.0f : y);
}

// ---------------------------------------------------------------------------
// Soft-knee clipper: linear up to `lin`, then a tanh approach to `ceil`.
// With lin == ceil it is a hard clip. Odd, monotonic, slope-continuous.

inline float kneeClip(float x, float lin, float ceil) noexcept {
    const float a = std::abs(x);
    if (a <= lin) return x;
    const float w = ceil - lin;
    const float y = w > 1.0e-9f ? lin + w * fastTanh((a - lin) / w) : ceil;
    return x < 0.0f ? -y : y;
}

// ---------------------------------------------------------------------------
// Tempo sync. One whole note = 4 beats.

inline double syncSeconds(SyncDiv div, SyncKind kind, double bpm) noexcept {
    static constexpr double kWhole[] = {1.0 / 32.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0};
    const int d = static_cast<int>(sanitizeEnum(div, SyncDiv::D1_8, static_cast<int>(SyncDiv::kCount)));
    double s = kWhole[d] * 4.0 * 60.0 / sanitizeBpm(bpm);
    switch (kind) {
        case SyncKind::Dotted: s *= 1.5; break;
        case SyncKind::Triplet: s *= 2.0 / 3.0; break;
        default: break;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Peak of |x| over a buffer, and helpers for the sleep gate.

inline float peakAbs(const float* x, int n) noexcept {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float a = std::abs(x[i]);
        p = a > p ? a : p;
    }
    return p;
}

// First index whose |L| or |R| exceeds the floor, or n.
inline int firstAbove(const float* l, const float* r, int n, float floor) noexcept {
    for (int i = 0; i < n; ++i)
        if (std::abs(l[i]) > floor || (r != nullptr && std::abs(r[i]) > floor)) return i;
    return n;
}

inline void zeroBuf(float* x, int n) noexcept {
    if (x != nullptr && n > 0) std::memset(x, 0, sizeof(float) * static_cast<std::size_t>(n));
}

// Map a resonance knob 0..1 to Q: 0 = Butterworth (0.707), 1 = Q of about `qMax`.
inline double resonanceToQ(double r, double qMax = 12.0) noexcept {
    return 0.70710678118654752 * std::pow(qMax / 0.70710678118654752, dsp::clamp(r, 0.0, 1.0));
}

// Bandwidth in octaves -> Q of the analog prototype (centre over -3 dB width,
// measured on the boost/cut's half-gain points as in the classic definition).
inline double octavesToQ(double oct) noexcept {
    const double p = std::exp2(oct);
    return std::sqrt(p) / (p - 1.0);
}

}  // namespace mw909::fx
