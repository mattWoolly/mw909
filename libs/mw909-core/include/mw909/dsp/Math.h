// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Small numeric helpers shared by every DSP block. Header-only, allocation-free.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mw909::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

template <typename T>
constexpr T clamp(T x, T lo, T hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

template <typename T>
constexpr T lerp(T a, T b, T t) noexcept { return a + (b - a) * t; }

inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g) noexcept { return 20.0f * std::log10(std::max(g, 1.0e-12f)); }

inline double semitonesToRatio(double semis) noexcept { return std::exp2(semis / 12.0); }

// Per-sample coefficient of a one-pole exponential with time constant tau:
// y[n] = y[n-1] * expDecayCoeff(tau, fs) decays by 1/e every tau seconds.
inline double expDecayCoeff(double tauSeconds, double sampleRate) noexcept {
    return tauSeconds > 0.0 ? std::exp(-1.0 / (tauSeconds * sampleRate)) : 0.0;
}

// Time for an exponential with time constant tau to fall by `db` decibels.
inline double tauToDecayTime(double tauSeconds, double db) noexcept {
    return tauSeconds * db * 0.1151292546497023;  // ln(10)/20
}

// Bilinear pre-warp: g = tan(pi * f / fs), clamped safely below Nyquist.
inline double prewarp(double freqHz, double sampleRate) noexcept {
    const double f = clamp(freqHz, 1.0e-3, 0.49 * sampleRate);
    return std::tan(kPi * f / sampleRate);
}

// The audio thread runs with FTZ/DAZ set, so this is a belt-and-braces guard
// for recursive state in code that may also run without them (tests, tools).
inline float killDenormal(float x) noexcept { return std::abs(x) < 1.0e-20f ? 0.0f : x; }
inline double killDenormal(double x) noexcept { return std::abs(x) < 1.0e-30 ? 0.0 : x; }

// Smooth, cheap saturator: exact tanh is fine at drum-machine voice counts,
// this rational form is for per-sample use inside oversampled chains.
// Max error vs tanh ~2.6e-2 over all x, monotonic, odd, bounded to +-1.
inline float softClip(float x) noexcept {
    const float c = clamp(x, -3.0f, 3.0f);
    return c * (27.0f + c * c) / (27.0f + 9.0f * c * c);
}

// Deterministic 32-bit mix (splitmix-style finaliser). Used to derive
// independent, reproducible values from (seed, id) pairs.
constexpr std::uint32_t hash32(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

constexpr std::uint32_t hash32(std::uint32_t seed, std::uint32_t id) noexcept {
    return hash32(seed ^ (hash32(id + 0x9e3779b9U) + (seed << 6) + (seed >> 2)));
}

// Uniform in [-1, 1) from a hash.
constexpr float hashToBipolar(std::uint32_t h) noexcept {
    return static_cast<float>(static_cast<std::int32_t>(h)) * (1.0f / 2147483648.0f);
}

// Component tolerance: the value of part `componentId` on unit `unitSeed`,
// as a multiplier around 1. `tolerance` is the part's spread (0.20 for the
// +-20% capacitors, 0.05 for the +-5% resistors); `variance` is the user's
// 0..1 amount. Distribution is triangular-ish (mean of two uniforms), which
// keeps most units near nominal like real batches.
constexpr float componentScale(std::uint32_t unitSeed, std::uint32_t componentId,
                               float tolerance, float variance) noexcept {
    const float a = hashToBipolar(hash32(unitSeed, componentId * 2U));
    const float b = hashToBipolar(hash32(unitSeed, componentId * 2U + 1U));
    return 1.0f + 0.5f * (a + b) * tolerance * variance;
}

}  // namespace mw909::dsp
