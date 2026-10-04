// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The sequencer's deterministic per-step random decisions (architecture.md
// §5.3), as pure functions so a render repeats exactly and tests can predict
// them:
//  * step probability: a hash of (pattern, measure, step, track);
//  * timing jitter: one delay in [0, kJitterMaxMs) per (jitterSeed, measure,
//    step), shared by every hit of that step (all voices, sub-steps, flams).
// The hash is a local copy of the splitmix-style finaliser so the decisions
// never change when shared DSP helpers do.

#pragma once

#include <cstdint>

namespace mw909::seq {

inline constexpr double kJitterMaxMs = 4.0;   // the original's interrupt-clock spread (research/03 §4)

namespace seqhash {

constexpr std::uint32_t mix(std::uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

constexpr std::uint32_t combine(std::uint32_t seed, std::uint32_t id) noexcept {
    return mix(seed ^ (mix(id + 0x9e3779b9U) + (seed << 6) + (seed >> 2)));
}

constexpr std::uint32_t measureStep(std::uint32_t seed, std::int64_t measure, int step) noexcept {
    const auto m = static_cast<std::uint64_t>(measure);
    std::uint32_t h = combine(seed, static_cast<std::uint32_t>(m & 0xffffffffU));
    h = combine(h, static_cast<std::uint32_t>(m >> 32));
    return combine(h, static_cast<std::uint32_t>(step));
}

// Uniform in [0, 1) with 24 bits.
constexpr double unit(std::uint32_t h) noexcept { return static_cast<double>(h >> 8) * (1.0 / 16777216.0); }

}  // namespace seqhash

// True when a step with probability code `code` (0..7, Step::probabilityCode)
// plays on this pass.
constexpr bool probabilityPasses(int code, int pattern, std::int64_t measure, int step, int track) noexcept {
    if (code <= 0) return true;
    const std::uint32_t seed = seqhash::combine(0x909A5EEDU, static_cast<std::uint32_t>(pattern));
    const std::uint32_t h = seqhash::combine(seqhash::measureStep(seed, measure, step), static_cast<std::uint32_t>(track));
    return seqhash::unit(h) < static_cast<double>(8 - code) / 8.0;
}

// Timing-jitter delay of every hit of (measure, step), in ms, in [0, kJitterMaxMs).
constexpr double jitterMs(std::uint32_t jitterSeed, std::int64_t measure, int step) noexcept {
    const std::uint32_t seed = seqhash::combine(0x7177E25DU, jitterSeed);
    return kJitterMaxMs * seqhash::unit(seqhash::measureStep(seed, measure, step));
}

}  // namespace mw909::seq
