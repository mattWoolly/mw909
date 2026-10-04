// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// HiHat: the original machine's one hi-hat circuit, rendered as two channels
// (CH, OH). Contract: docs/design/architecture.md §3.4. Research: research/02
// §1, §2, §4, §6, §7; research/05 §5.
//
// Circuit (voicing board, research/02 §4.1):
//   gated RC clock R492 12k / C160 470p ÷ 2 -> 4040 + 4520 counter -> ROM IC69
//   (open hat 0x0000-0x5FFF, closed hat 0x6000-0x7FFF; CLOSED/OPEN forces A13/A14
//   and the stop address) -> 4174 latch -> RA9 RM0621 6-bit DAC -> C157 10µ ->
//   VCA Q85 (shunt "voltage-controlled potentiometer" with R482 22 Ω, control
//   from the C135 envelope through the R453 220k / IC67a / Q84 log-exp
//   converter, accent through R481 4.7k) -> IC67b -> two Sallen-Key low-passes
//   -> C134 -> R444 22k -> level stage IC65b (VR22 50k(B)) -> output.
//
// Values used, and where they come from:
//   SCHEMATIC  C135 1 µF (±10 % under variance); CH decay path R451 10k +
//              VR21 100k(B, linear); OH decay path R452 100k + VR23 1M(A, audio,
//              10 % at mid-rotation); in CLOSED mode both paths charge C135 in
//              parallel (why CH decay moves a little with the OH knob), in OPEN
//              mode only the OH path. Trigger: Q74 holds C135 discharged for the
//              2 ms trigger pulse. Reconstruction filters, C134 0.01 µF (late
//              units, 723 Hz high-pass on the output) / 10 µF (Revision::Early).
//              Region map and stop logic.
//   MEASURED   clock 31.9 kHz (research/05 §5.1; the notes say ~30 kHz).
//   FITTED     VCA law: gain = A(V_acc) · exp(-t/τ) · R(address), τ the C135 RC
//              (the converter is linear in the C135 charging current; a fit with
//              the exponent free returns 1.0 · t/τ, so the schematic τ is kept).
//              R is the residual decay of the original's companded data, which
//              was compressed, not flattened: -40 dB · x^1.25 over the closed
//              region and -36 dB · x^6 over the open region, x = address / region
//              length (address-locked, so it follows TUNE as the data would).
//              mw909's ROM images are fully flattened (romFromAudio), so the
//              voice applies R for every source. Fitted to:
//                CH: the measured knob law (research/05 §5.6, §10.2: -20 dB at
//                    20/54/65/79/88 ms for DECAY 0/25/50/75/100 %; model
//                    25/55/70/80/85 ms); the service manual's 65..100 ms band at
//                    the centre (model 70 ms, last-above peak measure).
//                OH: the service manual at the centre (-14/-20 dB at 290/490 ms;
//                    model 300/420 ms) and, at DECAY 100 (τ 1.1 s), a decay
//                    well below -20 dB before the counter stops at 770 ms (model
//                    -20 dB at 660 ms, -36 dB at the stop). CONFLICTING: the
//                    recording of an original at DECAY 100 reaches -20 dB at
//                    0.25 s, shorter than the manual's centre setting; the brief
//                    calibrates to the manual, so the recording is not matched
//                    there (research/05 §10.2).
//              Accent: no change of the decay time (research/05 §10.2: the
//              recorded OH decay does not move with accent).
//              A(V) = exp(k·(V - V_normal)), k fitted to the measured TOTAL
//              ACCENT spans (CH +6.8 dB, OH +7.2 dB, research/05 §0.11), which
//              were recorded with the total-accent step, i.e. AccentDac codes
//              kNormalCode -> 63 (Hit::accented(false, true, k)).
//              Output level per region, fitted to the service-manual check
//              figures at MULTI OUT, knobs centred, normal hit (research/02 §7):
//              CH 0.96 Vpp, OH 1.2 Vpp, with the factory ROM, on the full-band
//              output (a 192 kHz render): the manual's scope saw the hardware's
//              image band (to ~48 kHz), which a 44.1/48 kHz host cannot carry,
//              so raw Vpp there is ~9 % (CH) / ~11 % (OH) lower; the level in
//              the band every rate carries (<= 16 kHz) is the same at every rate.
//              Control smoothing 0.22 ms (R453 · C156 integrator).
//   ASSUMED    DAC coupling corner 3.4 Hz (DAC source resistance unknown);
//              DAC bit-weight errors ±2 % (MSB) .. ±0.5 % (LSB) at variance 1;
//              clock ±8 % spread at variance 1.
//
// Extensions (not on the original panel): TUNE per hat (±12 semitones, as a
// clock-rate change: the stock R492 at 0), Source/Color/Density (ROM, the
// synthetic metal source, a user image), choke off (two independent copies of
// the circuit, one per region: a region restarts the copy already playing it,
// so turning choke off never cuts a ringing hat, and turning it on makes the
// next hat hit cut whatever still rings, as the single circuit would). With
// choke on, CH and OH tune/source settings apply to whichever region is
// playing; decay is the circuit's (both knobs).
//
// Not modelled: the converter's exact transistor-level law (its saturation at
// very high accent voltages), the DAC's switching glitches (settle ≪ 1 µs),
// oscillator jitter, the counter's ripple delays, the output stage's
// asymmetry (the hardware's PCM voices are positive-peak dominant).

#pragma once

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

class HiHat {
public:
    static constexpr bool kFreeRunning = false;

    // FITTED constants (see the header comment).
    static constexpr double kAccentSpanClosedDb = 6.8;
    static constexpr double kAccentSpanOpenDb = 7.2;
    static constexpr double kLevelClosed = 0.4996;   // volts per (gain x DAC unit): CH 0.96 Vpp
    static constexpr double kLevelOpen = 0.5621;     // OH 1.2 Vpp
    static constexpr double kC135 = 1.0e-6;
    static constexpr double kTriggerHoldSeconds = 0.002;
    static constexpr double kControlTau = 220.0e3 * 1000.0e-12;   // R453 · C156
    // Residual decay of the original's companded data over each region,
    // FITTED (see the header comment): -dB · x^power at address fraction x.
    static constexpr double kResidualClosedDb = 40.0, kResidualClosedPow = 1.25;
    static constexpr double kResidualOpenDb = 36.0, kResidualOpenPow = 6.0;

    void prepare(const VoiceSetup& s) noexcept {
        setup_ = s;
        setup_.sampleRate = dsp::clamp(std::isfinite(s.sampleRate) ? s.sampleRate : 48000.0, 8000.0, 768000.0);
        const float var = dsp::clamp(std::isfinite(s.variance) ? s.variance : 0.0f, 0.0f, 1.0f);
        setup_.variance = var;
        const std::uint32_t seed = s.unitSeed;
        dsp::AnalogLowpass2 s1, s2;
        dsp::pcm::reconstructionFilter(seed, kId + 0x10U, var, s1, s2);
        kernel_.build(setup_.sampleRate, dsp::pcm::kernelTaps(s.oversample, setup_.sampleRate), s1, s2);
        dac_.build(seed, kId + 0x20U, var);
        clockScale_ = static_cast<double>(dsp::componentScale(seed, kId + 0x30U, 0.08f, var));
        c135_ = kC135 * static_cast<double>(dsp::componentScale(seed, kId + 0x31U, 0.10f, var));
        const double c134 = (s.revision == Revision::Early ? 10.0e-6 : 0.01e-6) *
                            static_cast<double>(dsp::componentScale(seed, kId + 0x32U, 0.10f, var));
        outHpHz_ = dsp::pcm::outputCouplingHz(c134);
        cCtrl_ = 1.0 - std::exp(-1.0 / (kControlTau * setup_.sampleRate));

        holdSamples_ = static_cast<int>(std::lround(kTriggerHoldSeconds * setup_.sampleRate));
        for (Circuit* c : {&a_, &b_}) {
            c->chain.prepare(setup_.sampleRate, &kernel_, &dac_, dsp::pcm::kDacCouplingHz, outHpHz_, kLevelClosed);
            c->synth.prepare(dsp::hash32(seed, kId + (c == &a_ ? 0x40U : 0x41U)));
        }
        reset();
    }

    void reset() noexcept {
        for (Circuit* c : {&a_, &b_}) {
            c->chain.reset();
            c->u = 0.0;
            c->hold = 0;
            c->gS = 0.0;
            c->residClosed = true;
            c->residBlock = -2;
            c->r0 = c->r1 = 1.0;
            c->closed = true;
            c->synthOn = false;
            c->accLine = AccentDac::volts(AccentDac::kNormalCode);
            c->pitch = 0.0f;
        }
        pendClosed_ = pendOpen_ = false;
        updateCircuit(a_);
        updateCircuit(b_);
    }

    void setParams(const VoiceParams& closed, const VoiceParams& open) noexcept {
        closed_ = dsp::pcm::sanitiseParams(closed, CH);
        open_ = dsp::pcm::sanitiseParams(open, OH);
    }

    void setChoke(bool on) noexcept { choke_ = on; }
    void triggerClosed(const Hit& h) noexcept {
        pendClosed_ = true;
        hitClosed_ = h;
    }
    void triggerOpen(const Hit& h) noexcept {
        pendOpen_ = true;
        hitOpen_ = h;
    }
    void setAccLine(float volts) noexcept {
        const float v = dsp::pcm::sanitiseAcc(volts);
        a_.accLine = v;
        b_.accLine = v;
    }
    void setRoms(const RomImage* factory, const RomImage* user) noexcept {
        factory_ = factory;
        user_ = user;
    }

    void render(float* outClosed, float* outOpen, int n) noexcept {
        if (pendClosed_ || pendOpen_) applyTriggers();
        updateCircuit(a_);
        updateCircuit(b_);
        a_.chain.render(outClosed, outOpen, n, gainFn(a_), false);
        b_.chain.render(outClosed, outOpen, n, gainFn(b_), true);
    }

    bool isActive() const noexcept {
        return a_.chain.isActive() || b_.chain.isActive() || pendClosed_ || pendOpen_;
    }
    int latencySamples() const noexcept { return kernel_.latency(); }
    // The step kernel (the cymbals use it when theirs would be the same).
    const dsp::PcmStepKernel& kernel() const noexcept { return kernel_; }

    // Introspection for tests and the UI.
    double clockHz(bool closedRegion) const noexcept {
        return hatClock(closedRegion ? closed_ : open_, 0.0f);
    }
    double decayTau(bool closedRegion) const noexcept { return tauFor(closedRegion); }

private:
    static constexpr std::uint32_t kId = 0x48480000U;

    struct Circuit {
        dsp::PcmCircuit chain;
        dsp::MetalSynth synth;
        double u = 0.0;       // 1 - V(C135)/V_charge: the envelope
        int hold = 0;         // trigger pulse samples left (C135 held discharged)
        double gS = 0.0;      // VCA control after the converter's smoothing
        double aDecay = 0.0;  // per-sample envelope factor
        double target = 0.0;  // accent x level
        bool residClosed = true;   // which region's residual envelope
        int residBlock = -2;       // the 32-word block r0..r1 belong to (-2: none)
        double r0 = 1.0, r1 = 1.0; // residual at the block's ends
        bool closed = true;
        bool synthOn = false;
        bool restartSynth = false;
        float accLine = AccentDac::volts(AccentDac::kNormalCode);
        float pitch = 0.0f;
    };

    // The residual data envelope at word n of a region (gain).
    static double residual(bool closedRegion, int n) noexcept {
        const auto len = static_cast<double>(
            dsp::pcm::romSpan(closedRegion ? RomRegion::HatClosed : RomRegion::HatOpen).length());
        const double x = dsp::clamp(static_cast<double>(n) / len, 0.0, 1.0);
        const double db = closedRegion ? kResidualClosedDb * std::pow(x, kResidualClosedPow)
                                       : kResidualOpenDb * std::pow(x, kResidualOpenPow);
        return std::exp(-db * 0.11512925464970229);   // ln(10)/20
    }

    // The VCA control per host sample: C135 held discharged during the trigger
    // pulse, then charging (u = e^(-t/tau)); the converter's smoothing on top.
    struct GainFn {
        Circuit* c;
        double cCtrl;
        double operator()(int counter, bool running) const noexcept {
            // the residual envelope, evaluated every 32 words and interpolated
            const int blk = counter >> 5;
            if (blk != c->residBlock) {
                c->r0 = blk == c->residBlock + 1 ? c->r1 : residual(c->residClosed, blk << 5);
                c->r1 = residual(c->residClosed, (blk + 1) << 5);
                c->residBlock = blk;
            }
            const double resid = c->r0 + (c->r1 - c->r0) * static_cast<double>(counter & 31) * (1.0 / 32.0);
            if (c->hold > 0) {
                --c->hold;
                c->u = 1.0;
            } else {
                c->u = dsp::killDenormal(c->u * c->aDecay);
            }
            const double t = running ? c->target * c->u * resid : 0.0;
            c->gS = dsp::killDenormal(c->gS + (t - c->gS) * cCtrl);
            return c->gS;
        }
    };
    GainFn gainFn(Circuit& c) noexcept { return GainFn{&c, cCtrl_}; }

    double hatClock(const VoiceParams& p, float pitch) const noexcept {
        const double st = static_cast<double>(p.k[static_cast<std::size_t>(knob::PcmTune)]) + static_cast<double>(pitch);
        return dsp::pcm::kHatClockHz * clockScale_ * std::exp2(st / 12.0);
    }

    double tauFor(bool closedMode) const noexcept {
        const double dCh = static_cast<double>(closed_.k[static_cast<std::size_t>(knob::PcmDecay)]);
        const double dOh = static_cast<double>(open_.k[static_cast<std::size_t>(knob::PcmDecay)]);
        const double rCh = 10.0e3 + 100.0e3 * dCh;                          // R451 + VR21 (B)
        const double rOh = 100.0e3 + 1.0e6 * dsp::pcm::audioTaper(dOh);    // R452 + VR23 (A)
        const double r = closedMode ? rCh * rOh / (rCh + rOh) : rOh;
        return c135_ * r;
    }

    void applyTriggers() noexcept {
        if (choke_) {
            // One circuit: closed and open on the same sample play open. A second
            // copy still sounding from a choke-off period is cut as well, as the
            // single hardware circuit's retrigger would cut it.
            if (pendOpen_) start(a_, false, hitOpen_);
            else start(a_, true, hitClosed_);
            b_.chain.stop();
        } else {
            // Two independent copies, one per region: a region restarts the copy
            // already playing it, else takes a free one (a_ for closed, b_ for
            // open by preference), so switching choke off never cuts a ringing hat.
            if (pendClosed_) start(circuitFor(true), true, hitClosed_);
            if (pendOpen_) start(circuitFor(false), false, hitOpen_);
        }
        pendClosed_ = pendOpen_ = false;
    }

    Circuit& circuitFor(bool closedRegion) noexcept {
        auto plays = [closedRegion](const Circuit& c) { return c.chain.running() && c.closed == closedRegion; };
        if (plays(a_)) return a_;
        if (plays(b_)) return b_;
        Circuit& pref = closedRegion ? a_ : b_;
        Circuit& other = closedRegion ? b_ : a_;
        if (!pref.chain.running() || other.chain.running()) return pref;
        return other;
    }

    // Each region renders to its own channel: closed -> outClosed, open -> outOpen.
    void start(Circuit& c, bool closedRegion, const Hit& h) noexcept {
        c.closed = closedRegion;
        c.accLine = dsp::pcm::sanitiseAcc(h.acc);
        c.pitch = dsp::pcm::sanitisePitch(h.pitchSemis);
        c.u = 1.0;
        c.hold = holdSamples_;
        c.residClosed = closedRegion;
        c.residBlock = -2;   // not -1: block 0 must not reuse a previous hit's r1
        c.synthOn = false;   // re-evaluated (and the synth restarted) in updateCircuit
        const auto span = dsp::pcm::romSpan(closedRegion ? RomRegion::HatClosed : RomRegion::HatOpen);
        c.chain.start(span.start, span.end, closedRegion ? 0 : 1);
        c.restartSynth = true;
    }

    void updateCircuit(Circuit& c) noexcept {
        const VoiceParams& p = c.closed ? closed_ : open_;
        c.chain.setClockHz(hatClock(p, c.pitch));
        c.aDecay = std::exp(-1.0 / (tauFor(c.closed) * setup_.sampleRate));
        const double level = c.closed ? kLevelClosed : kLevelOpen;
        c.target = (level / kLevelClosed) *
                   dsp::pcm::accentGain(c.accLine, c.closed ? kAccentSpanClosedDb : kAccentSpanOpenDb);
        const PcmSource src = dsp::pcm::sourceOf(p);
        const RomImage* rom = (src == PcmSource::User && user_ != nullptr) ? user_ : factory_;
        c.chain.setRom(rom);   // null: reads mid-scale (silence)
        if (src == PcmSource::Synth) {
            const auto region = c.closed ? RomRegion::HatClosed : RomRegion::HatOpen;
            c.synth.configure(c.closed ? dsp::MetalSynth::Kind::HatClosed : dsp::MetalSynth::Kind::HatOpen,
                              p.k[static_cast<std::size_t>(knob::PcmColor)], p.k[static_cast<std::size_t>(knob::PcmDensity)],
                              dsp::pcm::romBuildRateHz(region), dsp::pcm::romSpan(region).length());
            if (!c.synthOn || c.restartSynth) c.synth.start();
            c.synthOn = true;
            c.chain.setSynth(&c.synth);
        } else {
            c.synthOn = false;
            c.chain.setSynth(nullptr);
        }
        c.restartSynth = false;
    }

    VoiceSetup setup_{};
    dsp::PcmStepKernel kernel_;
    dsp::PcmDac dac_;
    Circuit a_, b_;
    VoiceParams closed_ = defaultParams(CH), open_ = defaultParams(OH);
    const RomImage* factory_ = nullptr;
    const RomImage* user_ = nullptr;
    Hit hitClosed_{}, hitOpen_{};
    bool pendClosed_ = false, pendOpen_ = false;
    bool choke_ = true;
    double clockScale_ = 1.0, c135_ = kC135, outHpHz_ = 723.0, cCtrl_ = 1.0;
    int holdSamples_ = 96;
};

}  // namespace mw909::voices
