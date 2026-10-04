// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The synthetic metal source (PcmSource::Synth): a stand-in for the ROM that
// is computed instead of stored. It runs at the voice's ROM clock, one value
// per clock edge, and returns 6-bit codes, so everything after it (DAC, VCA,
// reconstruction filters, images) is the same chain the ROM feeds.
//
// Like the original ROM data (research/02 §3.3) its output is "companded":
// near-constant amplitude, using the full 6-bit range; the analog VCA puts
// the decay back. Its spectrum evolves over the region (it darkens), which a
// flattened cymbal recording also does.
//
// Sound: a bank of inharmonic sine partials (log-spaced over a band that
// depends on the instrument, jittered from the seed), white noise through a
// one-pole high-pass and low-pass, and their ring-modulated product (the
// "wash": sidebands of every partial with the noise band). The ride adds four
// strong low "bell" partials. A short noise emphasis marks the stick attack.
//   Color   0..1  spectral tilt of the partials (dark .. bright) and the noise
//                 low-pass corner (2 .. 15 kHz)
//   Density 0..1  number of partials (8 .. 32), how irregular their spacing
//                 is, and how much of the mix is wash rather than partials
//
// Frequencies are in ROM-clock units (cycles per step, defined at the region's
// nominal build rate), so TUNE transposes the synth exactly as it transposes a
// ROM. The source restarts from the same state on every hit, as the ROM
// counter does, so it is not free running (kFreeRunning = false), and it ends
// at mid-scale as a ROM image does, so a hit is a pure function of (seed,
// kind, color, density) and does not depend on the previous hit. Phases are
// 32-bit integer accumulators: bit-exact, no drift. Cost: about 12 flops per
// partial per clock edge; MEASURED on the target Xeon (48 kHz, Standard) a
// sample voice on Synth costs about 130..170 ns per host sample more than on
// its ROM at the default clock (≈ 0.7..0.8 % of a core), ≈ 330 ns (1.6 %) for a
// cymbal at its 56.6 kHz maximum clock; all four sample voices on Synth add
// about 2.5..3.5 % of a core.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

class MetalSynth {
public:
    enum class Kind : std::uint8_t { HatClosed = 0, HatOpen, Crash, Ride };
    static constexpr bool kFreeRunning = false;
    static constexpr int kMaxPartials = 36;   // 32 + 4 bell partials

    void prepare(std::uint32_t seed) noexcept {
        seed_ = seed;
        configured_ = false;
    }

    // `nominalRate`: the clock the material is defined at (pcm::romBuildRateHz).
    // `lengthSteps`: the region length (the darkening runs over it).
    // Recomputes only when something changed; allocation-free, cheap.
    void configure(Kind kind, float color, float density, double nominalRate, int lengthSteps) noexcept {
        color = clamp(std::isfinite(color) ? color : 0.5f, 0.0f, 1.0f);
        density = clamp(std::isfinite(density) ? density : 0.5f, 0.0f, 1.0f);
        if (configured_ && kind == kind_ && color == color_ && density == density_ && nominalRate == rate_ &&
            lengthSteps == length_)
            return;
        configured_ = true;
        kind_ = kind;
        color_ = color;
        density_ = density;
        rate_ = nominalRate > 1000.0 ? nominalRate : 32000.0;
        length_ = lengthSteps > 1 ? lengthSteps : 1;

        struct Band { double lo, hi, tilt, noiseHp, wPart, wWash, wNoise; };
        static constexpr Band kBands[4] = {
            {4200.0, 14800.0, 0.6, 6000.0, 0.55, 0.45, 0.70},   // closed hat: tight, high
            {3000.0, 14500.0, 0.3, 4000.0, 0.60, 0.55, 0.60},   // open hat: sizzle
            {450.0, 14000.0, 0.0, 1500.0, 0.55, 0.75, 0.45},    // crash: broad wash
            {1800.0, 14000.0, 0.2, 3000.0, 0.70, 0.40, 0.35},   // ride: ping partials + wash
        };
        const Band& b = kBands[static_cast<int>(kind)];
        const std::uint32_t kseed = hash32(seed_, 0x6d657461U + static_cast<std::uint32_t>(kind));

        numPartials_ = 8 + static_cast<int>(std::lround(24.0f * density));
        const double tilt = b.tilt + 2.0 * (static_cast<double>(color) - 0.5) * 1.5;   // octave exponent
        const double spread = 0.25 + 0.7 * static_cast<double>(density);
        const double ratio = b.hi / b.lo;
        double sumSq = 0.0;
        int p = 0;
        for (int i = 0; i < numPartials_; ++i, ++p) {
            const double j = static_cast<double>(hashToBipolar(hash32(kseed, static_cast<std::uint32_t>(3 * i))));
            const double pos = (static_cast<double>(i) + 0.5 + 0.5 * spread * j) / static_cast<double>(numPartials_);
            const double f = b.lo * std::pow(ratio, clamp(pos, 0.0, 1.0));
            const double r = 0.5 * (1.0 + static_cast<double>(hashToBipolar(hash32(kseed, static_cast<std::uint32_t>(3 * i + 1)))));
            const double a = std::pow(f / 4000.0, tilt * 0.5) * (0.55 + 0.45 * r);
            setPartial(p, f, a, kseed, static_cast<std::uint32_t>(3 * i + 2));
            sumSq += 0.5 * a * a;
        }
        if (kind == Kind::Ride) {   // bell: a few strong low partials
            static constexpr double kBell[4] = {610.0, 1020.0, 1570.0, 2460.0};
            for (int i = 0; i < 4; ++i, ++p) {
                const double j = static_cast<double>(hashToBipolar(hash32(kseed, static_cast<std::uint32_t>(200 + i))));
                const double f = kBell[i] * (1.0 + 0.06 * j * (0.5 + static_cast<double>(density)));
                const double a = 1.6 / (1.0 + 0.4 * i);
                setPartial(p, f, a, kseed, static_cast<std::uint32_t>(300 + i));
                sumSq += 0.5 * a * a;
            }
        }
        numActive_ = p;
        const double partRms = std::sqrt(sumSq > 0.0 ? sumSq : 1.0);
        for (int i = 0; i < numActive_; ++i) amp_[static_cast<std::size_t>(i)] = static_cast<float>(amp_[static_cast<std::size_t>(i)] / partRms);

        noiseHp_ = static_cast<float>(1.0 - std::exp(-kTwoPi * b.noiseHp / rate_));
        lpHz_ = 2000.0 * std::pow(7.5, static_cast<double>(color));
        noiseLp0_ = lpCoeff(lpHz_);

        // Noise normalisation: measure the filtered noise over a fixed run (deterministic).
        {
            std::uint32_t s = hash32(kseed, 0x4e4fU) | 1U;
            float hp = 0.0f, lp = 0.0f;
            double acc = 0.0;
            for (int i = 0; i < 1024; ++i) {
                const float w = white(s);
                hp += noiseHp_ * (w - hp);
                lp += noiseLp0_ * ((w - hp) - lp);
                if (i >= 256) acc += static_cast<double>(lp) * lp;
            }
            noiseNorm_ = static_cast<float>(1.0 / std::sqrt(std::max(acc / 768.0, 1.0e-9)));
        }

        // Mix: density moves weight from the partials to the wash.
        const double d = static_cast<double>(density);
        double wp = b.wPart * (1.25 - 0.6 * d), ww = b.wWash * (0.35 + 1.3 * d), wn = b.wNoise;
        const double norm = 1.0 / std::sqrt(wp * wp + ww * ww + wn * wn);
        wPart_ = static_cast<float>(wp * norm);
        wWash_ = static_cast<float>(ww * norm);
        wNoise_ = static_cast<float>(wn * norm);
        attackSteps_ = static_cast<int>(0.004 * rate_);
        fadeSteps_ = std::max(1, static_cast<int>(0.002 * rate_));
        noiseSeed_ = hash32(kseed, 0x5eedU) | 1U;
    }

    // Restart from the hit's initial state (the counter reset of a trigger).
    void start() noexcept {
        for (int i = 0; i < numActive_; ++i) phase_[static_cast<std::size_t>(i)] = phase0_[static_cast<std::size_t>(i)];
        noise_ = noiseSeed_;
        hp_ = 0.0f;
        lp_ = 0.0f;
        noiseLp_ = noiseLp0_;
        lastDarken_ = 0;
        nextDarken_ = 0;   // re-evaluated on the first edge
    }

    // One clock edge at address `pos` of the region (0 .. length-1): the next
    // 6-bit code (0..63, mid-scale 32). The position (the voice's counter) sets
    // the darkening, the stick-attack emphasis and the end of the material: the
    // last 2 ms fade to mid-scale and the last word is exactly mid-scale, as a
    // ROM image built by romFromAudio ends, so the DAC latch rests at
    // mid-scale after every hit and the next hit does not start on a held offset.
    int next(int pos) noexcept {
        // darken over the region: the noise low-pass falls one octave by the end
        if (pos >= nextDarken_ || pos < lastDarken_) {
            lastDarken_ = pos & ~255;
            nextDarken_ = lastDarken_ + 256;
            const double prog = static_cast<double>(lastDarken_) / static_cast<double>(length_);
            noiseLp_ = lpCoeff(lpHz_ * std::exp2(-std::min(prog, 1.0)));
        }
        float part = 0.0f;
        for (int i = 0; i < numActive_; ++i) {
            const auto k = static_cast<std::size_t>(i);
            part += amp_[k] * sinOfPhase(phase_[k]);
            phase_[k] += inc_[k];
        }
        const float w = white(noise_);
        hp_ += noiseHp_ * (w - hp_);
        lp_ += noiseLp_ * ((w - hp_) - lp_);
        const float nz = lp_ * noiseNorm_;
        float attack = 1.0f;
        if (pos >= 0 && pos < attackSteps_) attack += 1.5f * (1.0f - static_cast<float>(pos) / static_cast<float>(attackSteps_));
        float y = wPart_ * part + wWash_ * part * nz + wNoise_ * nz * attack;
        const int left = length_ - 1 - pos;   // words after this one
        if (left <= 0) return kMid;
        if (left < fadeSteps_) y *= static_cast<float>(left) / static_cast<float>(fadeSteps_);
        // RMS 1 -> 0.3 of full scale: peaks of about 3x RMS just reach the rails.
        const float c = 32.0f + 31.5f * 0.30f * y;
        const int code = static_cast<int>(std::floor(c + 0.5f));
        return code < 0 ? 0 : (code > 63 ? 63 : code);
    }

    int numPartials() const noexcept { return numActive_; }

private:
    void setPartial(int p, double hz, double amp, std::uint32_t kseed, std::uint32_t id) noexcept {
        const auto k = static_cast<std::size_t>(p);
        const double cyc = clamp(hz / rate_, 0.0, 0.49);
        inc_[k] = static_cast<std::uint32_t>(cyc * 4294967296.0);
        phase0_[k] = hash32(kseed, id);
        amp_[k] = static_cast<float>(amp);
    }

    float lpCoeff(double hz) const noexcept {
        return static_cast<float>(1.0 - std::exp(-kTwoPi * std::min(hz, 0.45 * rate_) / rate_));
    }

    static float white(std::uint32_t& s) noexcept {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
    }

    // sin(2 pi phase), phase a 32-bit fraction of a cycle; |error| < 1e-4.
    static float sinOfPhase(std::uint32_t ph) noexcept {
        float x = static_cast<float>(static_cast<std::int32_t>(ph)) * (1.0f / 4294967296.0f);   // [-0.5, 0.5)
        if (x > 0.25f) x = 0.5f - x;
        else if (x < -0.25f) x = -0.5f - x;
        const float y = 6.2831853f * x;
        const float y2 = y * y;
        return y * (1.0f - y2 * (1.0f / 6.0f - y2 * (1.0f / 120.0f - y2 * (1.0f / 5040.0f))));
    }

    std::uint32_t seed_ = 0U;
    bool configured_ = false;
    Kind kind_ = Kind::HatClosed;
    float color_ = 0.5f, density_ = 0.5f;
    double rate_ = 32000.0;
    int length_ = 1;
    int numPartials_ = 0, numActive_ = 0;
    std::array<std::uint32_t, kMaxPartials> inc_{}, phase_{}, phase0_{};
    std::array<float, kMaxPartials> amp_{};
    float noiseHp_ = 0.0f, noiseLp0_ = 0.0f, noiseLp_ = 0.0f, noiseNorm_ = 1.0f;
    double lpHz_ = 8000.0;
    float wPart_ = 0.6f, wWash_ = 0.5f, wNoise_ = 0.6f;
    int attackSteps_ = 128;
    int fadeSteps_ = 64;
    int lastDarken_ = 0, nextDarken_ = 0;
    static constexpr int kMid = 32;
    std::uint32_t noiseSeed_ = 1U, noise_ = 1U;
    float hp_ = 0.0f, lp_ = 0.0f;
};

}  // namespace mw909::dsp
