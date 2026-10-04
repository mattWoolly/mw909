// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Cymbal: the crash (CR) and ride (RD) circuits of the original machine, one
// class, prepare(setup, which). Contract: docs/design/architecture.md §3.2,
// §3.5. Research: research/02 §1, §2, §5, §6, §7; research/05 §5.
//
// Circuit (crash; ride in brackets), research/02 §5.1:
//   gated RC clock R443 [R441] 6.8k + VR25 [VR27] 10k(B) TUNE / C131 [C130] 470p
//   ÷ 2 -> 4040 + 4520 counter (32768 words, stops at 0x8000) -> ROM IC62 [IC54]
//   -> 4174 latch -> RA10 [RA12] RM0621 6-bit DAC -> C127 [C128] 10µ -> VCA Q71
//   [Q69] -> IC51b [IC51a] -> two Sallen-Key low-passes -> C132 [C133] 10µ ->
//   R445 [R446] 22k -> level stage (VR24 [VR26] 50k(B)) -> output.
//   Envelope: address bits A9..A14 -> RA11 [RA13] RM0621 -> IC52b [IC52a] with
//   R432 270k [R439 470k] / C126 [C125] 1000p and the Q70 [Q68] anti-log pair ->
//   VCA base; accent through R426 [R435] 4.7k. The decay is a 64-step staircase
//   locked to the address, so it always spans the sample and stretches with TUNE.
//
// Values used, and where they come from:
//   SCHEMATIC  address-driven 64-step envelope (one step per 512 words);
//              control smoothing R432·C126 = 0.27 ms [R439·C125 = 0.47 ms];
//              reconstruction filters; C132/C133 10 µF output coupling (0.72 Hz).
//   MEASURED   clock law f = 56.6 kHz / (2 − k), k = TUNE pot 0..1: 28.3 kHz at 0,
//              37.7 kHz at centre (research/05 §5.1). The schematic's 2.47:1 range
//              is CONFLICTING with the measured 2.0:1; the measurement is used.
//   FITTED     envelope law g(x) = (1 − a·x)/(1 + b·x), x = step/64 (the
//              converter is linear in the address-DAC current, the shunt VCA
//              compresses it): crash a = 0.992, b = 2.25; ride a = 0.97, b = 3.4,
//              fitted (rendered with the factory ROM, first-crossing r.m.s.) to an
//              original unit's decays (research/05 §10.2: crash -10/-20/-30 dB at
//              354/607/776 ms at TUNE 50, 439/792/1016 at 0, 239/457/545 at 100;
//              ride 194/564/761, 252/662/979, 149/381/525); the model is within
//              ~7 % at all three TUNE settings (ride -10 dB at TUNE 100: +20 %).
//              CONFLICTING: the service manual's drawings (research/02 §5.3) show
//              a crash that holds, then falls to -14 dB only at 0.89 of the sample
//              (the recording: 0.52), and a ride ~4 dB lower at 0.13..0.5 of the
//              sample. The brief fixes only the cymbal lengths to the manual, so
//              the measured unit is followed; the decay ends near -40 dB, as
//              recorded. Accent: gain ∝ exp(k·V_acc), k fitted to the TOTAL
//              ACCENT spans (crash +7.9 dB, ride +7.7 dB, research/05 §0.11).
//              Output level fitted to the service-manual figures, normal hit,
//              knobs centred: crash 2.3 Vpp, ride 2.2 Vpp, with the factory ROM,
//              on the full-band output (192 kHz render; the manual's scope saw
//              the image band, so at 44.1/48 kHz raw Vpp is ~14..18 % lower
//              while the level below 16 kHz is the same at every rate).
//   ASSUMED    DAC coupling 3.4 Hz; RUN/STOP closes the VCA at the counter stop
//              (as on the hat); clock ±8 % and DAC weights under unit variance.
//
// Extensions: Decay (not on the panel): raises the envelope law to the power
// 4^(0.5 − decay), so 0.5 is the stock circuit, 1 halves the decay depth in
// dB and 0 doubles it. Source/Color/Density as on the hats.
//
// The accent line is shared with the partner voice on the hardware (one accent
// DAC for crash and ride): the engine calls setAccLine() when the partner
// fires, and a ringing cymbal follows the new voltage, as the original does.

#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>

#include "mw909/Voices.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/MetalSynth.h"
#include "mw909/dsp/PcmBlep.h"
#include "mw909/dsp/PcmChain.h"
#include "mw909/dsp/PcmClock.h"
#include "mw909/dsp/PcmCommon.h"
#include "mw909/dsp/Rom.h"

namespace mw909::voices {

class Cymbal {
public:
    static constexpr bool kFreeRunning = false;

    struct Constants {
        double lawA, lawB;        // envelope law
        double accentSpanDb;      // TOTAL ACCENT 0 -> 100 %
        double level;             // output volts per (gain x DAC unit)
        double controlTau;        // seconds
        std::uint32_t id;
    };
    static constexpr Constants kCrash{0.992, 2.25, 7.9, 1.0406, 270.0e3 * 1000.0e-12, 0x43520000U};
    static constexpr Constants kRide{0.97, 3.4, 7.7, 0.9895, 470.0e3 * 1000.0e-12, 0x52440000U};

    // same: another sample voice's kernel (Engine passes the hat's and the
    // crash's), used instead of building this voice's own when it is the
    // table this voice would build (identical filters, as at variance 0):
    // one table to build on a reconfigure and to keep in cache.
    void prepare(const VoiceSetup& s, VoiceId which, const dsp::PcmStepKernel* same = nullptr) noexcept {
        which_ = which == RD ? RD : CR;
        prepareWith(s, same);
    }

    void prepare(const VoiceSetup& s) noexcept { prepareWith(s, nullptr); }

    // The step kernel in use (this voice's own or the one it shares).
    const dsp::PcmStepKernel& kernel() const noexcept { return *activeKernel_; }

    // True when the kernel in use is still the table this voice's prepare()
    // asked for (or prepare() has not run). A shared kernel belongs to another
    // voice: re-preparing that voice alone, with other filters or another
    // rate, would change it under this one; render() asserts this in debug
    // builds.
    bool kernelCurrent() const noexcept {
        return kernelTaps_ == 0 || activeKernel_->builtFor(setup_.sampleRate, kernelTaps_, kernelS1_, kernelS2_);
    }

private:
    void prepareWith(const VoiceSetup& s, const dsp::PcmStepKernel* same) noexcept {
        const Constants& k = constants();
        setup_ = s;
        setup_.sampleRate = dsp::clamp(std::isfinite(s.sampleRate) ? s.sampleRate : 48000.0, 8000.0, 768000.0);
        const float var = dsp::clamp(std::isfinite(s.variance) ? s.variance : 0.0f, 0.0f, 1.0f);
        const std::uint32_t seed = s.unitSeed;
        dsp::AnalogLowpass2 s1, s2;
        dsp::pcm::reconstructionFilter(seed, k.id + 0x10U, var, s1, s2);
        const int taps = dsp::pcm::kernelTaps(s.oversample, setup_.sampleRate);
        kernelTaps_ = taps;
        kernelS1_ = s1;
        kernelS2_ = s2;
        if (same != nullptr && same->builtFor(setup_.sampleRate, taps, s1, s2)) {
            activeKernel_ = same;
        } else {
            kernel_.build(setup_.sampleRate, taps, s1, s2);
            activeKernel_ = &kernel_;
        }
        dac_.build(seed, k.id + 0x20U, var);
        clockScale_ = static_cast<double>(dsp::componentScale(seed, k.id + 0x30U, 0.08f, var));
        const double c = 10.0e-6 * static_cast<double>(dsp::componentScale(seed, k.id + 0x32U, 0.10f, var));
        cCtrl_ = 1.0 - std::exp(-1.0 / (k.controlTau * setup_.sampleRate));
        chain_.prepare(setup_.sampleRate, activeKernel_, &dac_, dsp::pcm::kDacCouplingHz, dsp::pcm::outputCouplingHz(c),
                       k.level);
        synth_.prepare(dsp::hash32(seed, k.id + 0x40U));
        reset();
    }

public:
    void reset() noexcept {
        chain_.reset();
        gS_ = 0.0;
        pending_ = false;
        synthOn_ = false;
        restartSynth_ = false;
        accLine_ = AccentDac::volts(AccentDac::kNormalCode);
        pitch_ = 0.0f;
        lawStep_ = -1;
        update();
    }

    void setParams(const VoiceParams& p) noexcept { params_ = dsp::pcm::sanitiseParams(p, which_); }

    void trigger(const Hit& h) noexcept {
        pending_ = true;
        hit_ = h;
    }
    void setAccLine(float volts) noexcept { accLine_ = dsp::pcm::sanitiseAcc(volts); }
    void setRoms(const RomImage* factory, const RomImage* user) noexcept {
        factory_ = factory;
        user_ = user;
    }

    void render(float* out, int n) noexcept {
        assert(kernelCurrent());
        if (pending_) {
            pending_ = false;
            accLine_ = dsp::pcm::sanitiseAcc(hit_.acc);
            pitch_ = dsp::pcm::sanitisePitch(hit_.pitchSemis);
            chain_.start(0, kRomWords, 0);
            restartSynth_ = true;
        }
        update();
        chain_.render(out, nullptr, n, [this](int counter, bool running) noexcept {
            const int st = counter >> 9;   // A9..A14: 64 steps of 512 words
            if (st != lawStep_) {
                lawStep_ = st;
                law_ = lawAt(st);
            }
            const double t = running ? target_ * law_ : 0.0;
            gS_ = dsp::killDenormal(gS_ + (t - gS_) * cCtrl_);
            return gS_;
        });
    }

    bool isActive() const noexcept { return chain_.isActive() || pending_; }
    int latencySamples() const noexcept { return activeKernel_->latency(); }
    VoiceId which() const noexcept { return which_; }

    // Introspection for tests and the UI.
    double clockHz() const noexcept { return clockFor(0.0f); }
    // Envelope gain (no accent) at staircase step 0..63.
    double envelopeAt(int step) const noexcept { return lawAt(step); }

private:
    const Constants& constants() const noexcept { return which_ == RD ? kRide : kCrash; }

    double clockFor(float pitch) const noexcept {
        const double k = static_cast<double>(params_.k[static_cast<std::size_t>(knob::PcmTune)]);
        return dsp::pcm::cymbalClockHz(k, dsp::pcm::kCymbalClockMaxHz * clockScale_) *
               std::exp2(static_cast<double>(pitch) / 12.0);
    }

    double lawAt(int step) const noexcept {
        const Constants& k = constants();
        const double x = static_cast<double>(dsp::clamp(step, 0, 63)) / 64.0;
        const double g = (1.0 - k.lawA * x) / (1.0 + k.lawB * x);
        return std::pow(std::max(g, 1.0e-6), decayPow_);
    }

    void update() noexcept {
        const Constants& k = constants();
        chain_.setClockHz(clockFor(pitch_));
        const double d = static_cast<double>(params_.k[static_cast<std::size_t>(knob::PcmDecay)]);
        const double p = std::pow(4.0, 0.5 - d);
        if (p != decayPow_) {
            decayPow_ = p;
            lawStep_ = -1;
        }
        target_ = dsp::pcm::accentGain(accLine_, k.accentSpanDb);
        const PcmSource src = dsp::pcm::sourceOf(params_);
        chain_.setRom((src == PcmSource::User && user_ != nullptr) ? user_ : factory_);
        if (src == PcmSource::Synth) {
            const auto region = which_ == RD ? RomRegion::Ride : RomRegion::Crash;
            synth_.configure(which_ == RD ? dsp::MetalSynth::Kind::Ride : dsp::MetalSynth::Kind::Crash,
                             params_.k[static_cast<std::size_t>(knob::PcmColor)],
                             params_.k[static_cast<std::size_t>(knob::PcmDensity)], dsp::pcm::romBuildRateHz(region),
                             kRomWords);
            if (!synthOn_ || restartSynth_) synth_.start();
            synthOn_ = true;
            chain_.setSynth(&synth_);
        } else {
            synthOn_ = false;
            chain_.setSynth(nullptr);
        }
        restartSynth_ = false;
    }

    VoiceId which_ = CR;
    VoiceSetup setup_{};
    dsp::PcmStepKernel kernel_;
    const dsp::PcmStepKernel* activeKernel_ = &kernel_;
    int kernelTaps_ = 0;   // what prepare() asked the kernel for (kernelCurrent())
    dsp::AnalogLowpass2 kernelS1_{}, kernelS2_{};
    dsp::PcmDac dac_;
    dsp::PcmCircuit chain_;
    dsp::MetalSynth synth_;
    VoiceParams params_ = defaultParams(CR);
    const RomImage* factory_ = nullptr;
    const RomImage* user_ = nullptr;
    Hit hit_{};
    bool pending_ = false, synthOn_ = false, restartSynth_ = false;
    float accLine_ = AccentDac::volts(AccentDac::kNormalCode);
    float pitch_ = 0.0f;
    double clockScale_ = 1.0, cCtrl_ = 1.0, gS_ = 0.0, target_ = 1.0;
    double decayPow_ = 1.0, law_ = 1.0;
    int lawStep_ = -1;
};

}  // namespace mw909::voices
