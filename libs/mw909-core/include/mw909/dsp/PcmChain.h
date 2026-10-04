// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The common part of the original machine's three sample-voice circuits
// (research/02 §1, §3, §6; research/05 §5): address counter and stop logic,
// ROM (or synthetic source) read, 6-bit latch + resistor DAC, the DAC's
// coupling capacitor, the VCA (its gain is supplied per host sample by the
// voice, which owns the envelope), the reconstruction filters (folded into
// the band-limited step kernel, dsp/PcmBlep.h) and the output coupling
// capacitor before the level stage.
//
//   ROM ─► 4174 latch ─► RM0621 DAC ─► C157/C127/C128 10µ ─► VCA ─► LPF1 ─► LPF2 ─► C134/C132/C133 ─► level
//
// Model, per clock edge k at host time t_k (sub-sample exact):
//   code_k = ROM[start + counter]                         (or MetalSynth::next(counter))
//   d_k    = DAC(code_k)                                  per-unit bit weights (PcmDac)
//   x_k    = d_k − s;  s += c_clk·(d_k − s)               DAC coupling high-pass, ZOH-exact at the clock
//   v_k    = g(t_k) · x_k                                 VCA, g linearly interpolated inside the host sample
//   step   Δ_k = v_k − v_{k−1}  →  band-limited step into the output channel's accumulator
// The VCA multiplies a held value, so g·DAC is itself a staircase on the same
// clock and the step model stays exact (g changes on a much slower time scale
// than the clock period, except at a trigger, where it is smoothed by the
// control loop anyway).
//
// Trigger (start): the counter and the clock phase reset (the oscillator is
// gated by RUN, research/02 §1.3): the first edge latches the region start at
// the trigger instant. Nothing else resets: the coupling capacitor, the VCA
// state, the filter ringing and the output coupling carry on, so a retrigger
// lands on whatever is still sounding (a DAC jump becomes the click the
// hardware makes).
// Stop: one clock edge after the last word, the counter reaches the stop
// address, RUN falls, the latch holds and the VCA is closed by RUN/STOP
// (R488 on the hat; assumed the same on the cymbals): a band-limited step to 0.
//
// Two output channels: the hat circuit's single output is split by which
// region is playing (CH channel / OH channel, architecture §3.4). A step is
// written to the channel of the region that made it; a region change writes
// the old channel's return to zero and the new channel's start, so the two
// channels always sum to the one physical output.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "mw909/dsp/Math.h"
#include "mw909/dsp/MetalSynth.h"
#include "mw909/dsp/PcmBlep.h"
#include "mw909/dsp/Rom.h"

namespace mw909::dsp {

// The 6-bit resistor DAC (RM0621: six binary-weighted resistors to one node,
// research/02 §3.1) with per-unit weight errors (research/05 §5.3): bit i
// weighs 2^i·(1 + e_i), e_i within ±2 % (MSB) .. ±0.5 % (LSB) at variance 1.
// Output normalised so that nominal code 32 is 0 and the full range is about ±1.
class PcmDac {
public:
    void build(std::uint32_t unitSeed, std::uint32_t idBase, float variance) noexcept {
        static constexpr float kTol[6] = {0.005f, 0.005f, 0.0075f, 0.01f, 0.015f, 0.02f};
        std::array<double, 6> w{};
        for (int i = 0; i < 6; ++i)
            w[static_cast<std::size_t>(i)] =
                static_cast<double>(1 << i) *
                static_cast<double>(componentScale(unitSeed, idBase + static_cast<std::uint32_t>(i), kTol[i], variance));
        for (int c = 0; c < 64; ++c) {
            double d = 0.0;
            for (int i = 0; i < 6; ++i)
                if ((c >> i) & 1) d += w[static_cast<std::size_t>(i)];
            lut_[static_cast<std::size_t>(c)] = (d - 32.0) / 32.0;
        }
    }
    double operator()(int code) const noexcept { return lut_[static_cast<std::size_t>(code & 63)]; }

private:
    std::array<double, 64> lut_{};
};

class PcmCircuit {
public:
    static constexpr int kChannels = 2;

    // couplingHz: the DAC coupling capacitor's corner. outputHpHz: the output
    // coupling capacitor into the level stage. outputGain: volts at the
    // individual output per unit of (VCA gain x DAC value).
    void prepare(double sampleRate, const PcmStepKernel* kernel, const PcmDac* dac, double couplingHz,
                 double outputHpHz, double outputGain) noexcept {
        fs_ = sampleRate;
        kernel_ = kernel;
        dac_ = dac;
        couplingHz_ = couplingHz;
        cHost_ = 1.0 - std::exp(-kTwoPi * couplingHz / fs_);
        const double g = prewarp(outputHpHz, fs_);
        hpG_ = g / (1.0 + g);
        outGain_ = outputGain;
        setClockHz(clockHz_);
        reset();
    }

    void reset() noexcept {
        for (auto& a : acc_) a.reset();
        hpS_.fill(0.0);
        running_ = false;
        pending_ = false;
        stopPending_ = false;
        counter_ = 0;
        nextEdge_ = 0.0;
        vEmit_.fill(0.0);
        dLatched_ = dac_ != nullptr ? (*dac_)(kRomMidScale) : 0.0;
        cs_ = dLatched_;
        g0_ = 0.0;
        time_ = 0U;
        active_ = false;
    }

    void setClockHz(double hz) noexcept {
        clockHz_ = clamp(std::isfinite(hz) ? hz : 32000.0, 1000.0, 400000.0);
        period_ = fs_ / clockHz_;
        cClk_ = 1.0 - std::exp(-kTwoPi * couplingHz_ / clockHz_);
    }
    double clockHz() const noexcept { return clockHz_; }

    // The source read at each edge: a ROM image (never null while running)
    // and, when non-null, a synthetic source that replaces it.
    void setRom(const RomImage* rom) noexcept { rom_ = rom; }
    void setSynth(MetalSynth* synth) noexcept { synth_ = synth; }
    void setOutputGain(double g) noexcept { outGain_ = g; }

    // Takes effect at sample 0 of the next render().
    void start(int startAddr, int endAddr, int channel) noexcept {
        startAddr_ = clamp(startAddr, 0, kRomWords - 1);
        length_ = clamp(endAddr - startAddr_, 1, kRomWords - startAddr_);
        newChannel_ = clamp(channel, 0, kChannels - 1);
        pending_ = true;
        stopPending_ = false;
        active_ = true;
    }

    // Stop the counter at sample 0 of the next render(), as the counter stop
    // does: the latch holds, the VCA closes (a band-limited step to 0). Cancels
    // a start that has not been rendered yet.
    void stop() noexcept {
        if (!running_ && !pending_) return;
        pending_ = false;
        stopPending_ = true;
    }

    bool running() const noexcept { return running_ || pending_; }
    int counter() const noexcept { return counter_; }
    int channel() const noexcept { return channel_; }
    bool isActive() const noexcept { return active_; }
    int latencySamples() const noexcept { return kernel_ != nullptr ? kernel_->latency() : 0; }

    // gainFn(counter, running) -> VCA gain at the END of the current host
    // sample (called once per sample, in order). out0/out1 may be null: that
    // channel is then rendered but not written. add: sum into the outputs
    // instead of overwriting them.
    template <class GainFn>
    void render(float* out0, float* out1, int n, GainFn&& gainFn, bool add = false) noexcept {
        if (!active_) {
            if (!add) zero(out0, out1, 0, n);
            return;
        }
        float* outs[kChannels] = {out0, out1};
        const PcmStepKernel& k = *kernel_;
        for (int i = 0; i < n; ++i) {
            const std::uint32_t now = time_;
            if (i == 0 && stopPending_) {
                stopPending_ = false;
                if (running_) {
                    stepTo(k, now, 0.0, 0.0);
                    running_ = false;
                }
            }
            if (i == 0 && pending_) applyStart(now);
            const double g1 = gainFn(counter_, running_);
            if (running_) {
                // nextEdge_ is in host samples from the start of this sample.
                while (nextEdge_ < 1.0) {
                    const double frac = nextEdge_;
                    if (counter_ >= length_) {
                        // RUN/STOP: the latch holds, the VCA closes.
                        stepTo(k, now, frac, 0.0);
                        running_ = false;
                        break;
                    }
                    const int code = synth_ != nullptr ? synth_->next(counter_)
                                                       : (rom_ != nullptr ? rom_->data[static_cast<std::size_t>(startAddr_ + counter_)] : kRomMidScale);
                    const double d = (*dac_)(code);
                    dLatched_ = d;
                    const double x = d - cs_;
                    cs_ += cClk_ * x;
                    const double g = g0_ + frac * (g1 - g0_);
                    stepTo(k, now, frac, g * x);
                    ++counter_;
                    nextEdge_ += period_;
                }
                if (running_) nextEdge_ -= 1.0;
            } else {
                cs_ += cHost_ * (dLatched_ - cs_);
            }
            g0_ = g1;
            for (int c = 0; c < kChannels; ++c) {
                const auto cc = static_cast<std::size_t>(c);
                const double x = acc_[cc].tick(now);
                // one-pole TPT high-pass (output coupling)
                const double v = (x - hpS_[cc]) * hpG_;
                const double lp = v + hpS_[cc];
                hpS_[cc] = lp + v;
                if (outs[c] != nullptr) {
                    const auto y = static_cast<float>(outGain_ * (x - lp));
                    outs[c][i] = add ? outs[c][i] + y : y;
                }
            }
            ++time_;
            if (!running_ && !pending_ && settled()) {
                // Everything has decayed below about -140 dB: snap the states to the
                // values they converge to (so a wake is exactly what it would have been).
                cs_ = dLatched_;
                hpS_.fill(0.0);
                for (auto& a : acc_) a.clearHold();
                vEmit_.fill(0.0);
                g0_ = 0.0;
                active_ = false;
                if (!add) zero(out0, out1, i + 1, n);
                return;
            }
        }
    }

private:
    void applyStart(std::uint32_t now) noexcept {
        pending_ = false;
        if (newChannel_ != channel_) stepTo(*kernel_, now, 0.0, 0.0);   // the old region's output returns to rest
        channel_ = newChannel_;
        counter_ = 0;
        nextEdge_ = 0.0;
        running_ = true;
    }

    // Move the current channel's output to v. The steps are float; vEmit_
    // keeps their exact running sum, so a return to 0 is exact and the hold
    // value of a stopped circuit is 0 up to one rounding (no drift).
    void stepTo(const PcmStepKernel& k, std::uint32_t now, double frac, double v) noexcept {
        double& e = vEmit_[static_cast<std::size_t>(channel_)];
        const auto delta = static_cast<float>(v - e);
        if (delta == 0.0f) return;
        e += static_cast<double>(delta);
        acc_[static_cast<std::size_t>(channel_)].addStep(k, now + 1U, frac, delta);
    }

    bool settled() const noexcept {
        constexpr double kEps = 1.0e-7;
        for (int c = 0; c < kChannels; ++c) {
            const auto cc = static_cast<std::size_t>(c);
            if (!acc_[cc].drained() || std::abs(acc_[cc].hold()) > kEps || std::abs(hpS_[cc]) > kEps) return false;
        }
        return std::abs(dLatched_ - cs_) < kEps;
    }

    static void zero(float* a, float* b, int from, int n) noexcept {
        for (int i = from; i < n; ++i) {
            if (a != nullptr) a[i] = 0.0f;
            if (b != nullptr) b[i] = 0.0f;
        }
    }

    double fs_ = 48000.0;
    const PcmStepKernel* kernel_ = nullptr;
    const PcmDac* dac_ = nullptr;
    const RomImage* rom_ = nullptr;
    MetalSynth* synth_ = nullptr;
    std::array<PcmStepAccumulator, kChannels> acc_{};
    std::array<double, kChannels> hpS_{};
    double hpG_ = 0.0;
    double outGain_ = 1.0;
    double couplingHz_ = 3.4;
    double clockHz_ = 32000.0, period_ = 1.5, cClk_ = 0.0, cHost_ = 0.0;
    int startAddr_ = 0, length_ = kRomWords, counter_ = 0;
    int channel_ = 0, newChannel_ = 0;
    bool running_ = false, pending_ = false, stopPending_ = false, active_ = false;
    double nextEdge_ = 0.0;
    std::array<double, kChannels> vEmit_{};
    double dLatched_ = 0.0, cs_ = 0.0, g0_ = 0.0;
    std::uint32_t time_ = 0U;
};

}  // namespace mw909::dsp
