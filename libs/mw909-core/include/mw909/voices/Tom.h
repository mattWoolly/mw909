// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Low, mid and high tom (LT, MT, HT): one class, the tom chosen in
// prepare(setup, which). A block model of the original circuit, simulated in
// volts (research/01 §5, research/05 §2.4; designators for LT, then MT / HT):
//
//   TRIG 2 ms -> Q16: the +15 V pulse Q15 (resets the three VCOs, charges the
//                pitch capacitors and the VCO-1 overdrive timer, starts the
//                tom-noise tick) and Q17: the accent line (buffer IC18b x3.128,
//                with R118 220k to -15 V: -0.54 V) gated by the trigger.
//   Rails      TUNE VR10 (10k B; R113 68k, R112 10k): wiper 1.705..3.409 V.
//              VDD = wiper (IC14a); VSS = -(wiper + V_C17 R64/R61 + V_C16
//              R64/R60) (IC14b): C17 0.047u / R61 470k (22.1 ms) and C16 0.1u
//              / R60 2.2M (220 ms), charged by the pulse. Only VSS moves, so
//              the three triangles are skewed early in the note (even
//              harmonics) and f = f_s 2 / (1 + VDD/|VSS|) (the rail law).
//   VCOs       C18 22n / C19 33n / C20 12n (MT 18n/27n/10n, HT 15n/22n/8.2n)
//              with 47k, comparators at V_sat/2, reset through 10k
//              (dsp/TriangleVco.h with separate up and down slopes).
//   Rounding   each VCO -> 10k -> anti-parallel diodes -> 2.2k + 47 -> its
//              VCA's emitter. VCO-1's 10k is bypassed by 1k + Q18 while C24
//              0.01u (C38 6.8n, C101 5.6n), charged by the pulse, drains
//              through R101 1M into Q18's base: near-square first 20-30 ms.
//   VCAs       VCO-1 (Q19): ENV1 C22 0.22u (MT/HT 0.15u) through R107 470k,
//              plus ENV2 through R108 220k. VCO-2 (Q20): ENV2 through R109
//              220k. ENV2 is a two-capacitor network: C21 0.68u (MT/HT 0.47u)
//              charged by the accent, discharged by DECAY VR11 (500k B) +
//              R111 56k, feeding C23 0.68u (0.47u) through R100 100k: VCO-2
//              swells in. VCO-3 (Q21): ENV3 C25 0.056u (HT 0.047u) through
//              R110 470k.
//   Tom noise  noise -> R190 22k / C161 4.7n -> Sallen-Key high-pass C51 =
//              C52 4.7n, R191 10k, R192 220k (IC27b), solved as one loaded
//              network: gain 1.174, low-pass 1311 Hz, high-pass 782 Hz Q 1.134
//              (peak 904 Hz, +1.6 dB). Two routes into IC27a (R193 22k): Q33
//              (through C53 0.047u into R195 1k / R199 47: a 3.2 kHz
//              high-pass), a VCA on C54 4.7n (2.2n early units) charged by
//              the pulse through R200 100k (the onset tick), and R194 100k (47k early)
//              directly (a steady component). IC27a -> R114 22k -> VCO-3's
//              emitter node, so ENV3 gates it. (The hardware has one tom-noise
//              circuit for the three toms; each instance here has its own
//              copy, which differs only when two toms ring within ~25 ms.)
//   Output     IC18a, LEVEL VR12 50k (B).
//
// VCO-1's VCA base is fed from both ENV1 (R107) and ENV2 (R108): it sits at
// their Thevenin source less the base drop, so once ENV2 has drained below
// the base it SINKS ENV1's current and VCO-1 cuts off while ENV1 still holds
// a volt or two. That is the measured "cliff" at short DECAY settings and
// why DECAY lengthens VCO-1 as well as the VCO-2 bloom
// (dsp::twoSourceBase).
//
// Schematic values: everything above. FITTED (and to what; kFits[LT/MT/HT]):
//  * f_s = K (wiper + V0): K = 35.28 / 40.74 / 49.21 Hz/V, V0 = -0.088 /
//    +0.056 / -0.023 V, to the asymptotic VCO-1 pitch fitted with the rail
//    law (research/01 F3: LT 57.0 / 85.8 / 117.2, MT 71.9 / 106.6 / 141.3,
//    HT 82.8 / 125.4 / 166.7 Hz at TUNE 0 / 50 / 100; the parts give 4-8 %
//    more). The triangle peak follows from K. VCO-2 / VCO-3 ratios 0.679 /
//    1.88 (LT), 0.673 / 1.77 (MT), 0.675 / 1.95 (HT), measured (the caps give
//    0.667 / 1.83, 0.667 / 1.80, 0.682 / 1.83).
//  * Pitch envelope at the derived 3.02 V (22.1 ms) and 0.645 V (220 ms) on
//    VSS (the slow term at full weight, research/01 F3). VCO-1 against the
//    recordings (spectral peak from 10 % of the peak, DECAY 100): 2-27 ms
//    within 4 % except LT TUNE 0 (+19 %: the model's fast term is too big
//    there; the measured k_a is 25-45 % under the derivation, research/01
//    §5.3); 30-60 ms within 4 %; 60-120 ms within 1 %. (A review with its
//    window aligned differently read up to +-6.6 % at 30-60 ms.)
//  * The envelopes, jointly per tom, to the reference unit's VCO-1 / VCO-2 /
//    VCO-3 levels (spectral peaks re the hit's peak in seven windows from 20
//    to 600 ms) at TUNE 50 and DECAY 0 / 20 / 40 / 60 / 80 / 100, down to
//    the recordings' floor (-63 dB LT, -76 dB MT / HT): rms error 1.5 / 2.0
//    / 1.9 dB (LT / MT / HT). The fitted values are mostly near the parts
//    (the earlier fit to t(-20 dB) alone needed C22 x1.27 and VCO-2 x1.3-1.6;
//    LT's ENV2 capacitor now fits 39 % large):
//    env1Scale 0.93 / 0.90 / 0.85 (x C22 / C36 / C99), env2Scale 1.39 /
//    0.99 / 1.12 (x C23 / C37 / C100), aScale 0.92 / 1.01 / 1.08 (x C21 /
//    C35 / C98), decayScale 1.02 (x VR11), vco2Trim 1.03 / 0.91 / 0.84,
//    env3Scale 0.98 / 0.88 / 0.85 and vco3Trim 1.30 / 1.39 / 1.42. Two
//    departures from the schematic as read:
//    - decayEnd 0.32 / 0.34 / 0.32 V: the DECAY path (VR11 + R111) acts as
//      if it ended ~0.33 V above ground. With it ending at ground, C_A drags
//      ENV2 under the base threshold at every DECAY setting and both VCOs
//      fall off a cliff at -25..-30 dB (MT at DECAY 100: -52 dB at 400 ms,
//      measured -38.5); the recordings show that cliff only at short DECAY,
//      and at DECAY 100 both VCOs ring out exponentially to -65 dB. What
//      holds the path up (a junction at microamp currents?) is UNVERIFIED.
//    - leak3 60 MOhm on ENV3: below the base knee the measured VCO-3 keeps
//      falling at ~-170 dB/s (MT / HT: -67 / -83 / -102 dB at 250 / 350 /
//      450 ms), where a node that drains only into a base would hang (its
//      drive falls as 1/t). LT's floor hides it; the same value is used.
//    The 20 ms r.m.s. tail (above 30 Hz) then follows the recordings within
//    6 dB to their floor at DECAY 50 and 100 (e.g. MT at DECAY 100: -18 /
//    -30 / -41 / -55 / -62 dB at 200..600 ms, measured -18 / -27 / -39 /
//    -53 / -68); at DECAY 0 the cliff comes ~10 ms early (MT / HT -50 / -53
//    dB at 200 ms, measured -43 / -47).
//    With these, t(-20 dB) over DECAY 0..100 (full-band Hilbert envelope,
//    20 ms; measured LT 180..315, MT 129..219, HT 125..229 ms) stays within
//    10 % (LT 196..311, MT 127..219, HT 128..240 ms).
//  * vcaSat 2.91 / 3.82 / 3.16 (base drive re 100k): the cells' saturation,
//    to the TOTAL ACCENT spans +7.3 / +7.7 / +7.4 dB (research/05 §0.11).
//    The curve's shape is not matched: at 50 % the model reads +5.6..5.8 dB
//    where the recordings read +4.1..4.4 (+1.4..1.6 dB), and at 10 % +1.7
//    against 0.0..0.4: the measured curve has a dead first 10 % that the
//    linear knob-to-code mapping of AccentDac cannot express.
//  * outputGain 30.74 / 29.24 / 31.02: 0.95 / 1.05 / 1.06 Vpp (LT / MT /
//    HT) for a normal hit with the knobs centred: the hand clap's 2 Vpp
//    trimmer target and the reference unit's tom/CP ratios (-6.49 / -5.58 /
//    -5.48 dB at equal part volume). The service notes' LT drawing reads
//    ~1.2 Vpp (+-30 %): the model is 2 dB under it, inside its reading.
//  * Q18's overdrive: its share of the bypass follows its base current
//    (active region), interpolating the diode curve between the plain 10k
//    and the full 1k bypass; the switch polarity and law are UNVERIFIED.
//
// Not fitted: the tom-noise level (the R114 / R194 ratios as derived; Q33's
// route against the R194 route, tickGain 1, is an assumption) was not fitted
// separately to the recordings, whose onset noise is masked by VCO-3.
// Not modelled: the cross-coupling of the shared tom-noise circuit between
// toms (see above), Q34's function on C54 (UNVERIFIED), op-amp slew, the
// multi-out RC, and the original's negative-peak dominance (recordings +/-
// 0.71 / 0.64 / 0.62 at knobs centred, model 0.85 / 0.92 / 0.88; its
// mechanism is not identified).
//
// Extended controls (Voices.h): Tune Depth scales the pitch envelope (both
// terms; 0.5 stock), Tune Decay its time constants (x0.25..x4; 0.5 stock),
// Noise the tom-noise level (0 none, 0.5 stock, 1 double).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mw909/Voices.h"
#include "mw909/dsp/DiodeClipper.h"
#include "mw909/dsp/Filters.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/TransistorVca.h"
#include "mw909/dsp/TriangleVco.h"
#include "mw909/dsp/VoiceControls.h"

namespace mw909::voices {

// The fitted constants of one tom (see Tom.h's header comment).
struct TomFit {
    double hzPerVolt = 35.0;   // f_s = K (wiper + V0)
    double offsetVolts = 0.0;  // V0
    double ratio2 = 0.679;     // VCO-2 / VCO-1
    double ratio3 = 1.88;      // VCO-3 / VCO-1
    double fastVolts = 3.02;   // fast pitch term on VSS at the end of the trigger
    double slowVolts = 0.645;  // slow term
    double env1Scale = 1.0;    // x C22
    double env2Scale = 1.0;    // x C21 and C23
    double env3Scale = 1.0;    // x C25
    double vcaSat = 6.0;       // VCA saturation (base drive re 100k, volts)
    double vco2Trim = 1.0;     // VCO-2 level re derived
    double vco3Trim = 1.0;     // VCO-3 level re derived
    double noiseGain = 1.0;    // tom noise into VCO-3's node re derived
    double tickGain = 1.0;     // Q33 route re the R194 route
    double outputGain = 10.0;
    double aScale = 1.0;       // x C21 (the DECAY capacitor)
    double decayScale = 1.0;   // x the DECAY pot's resistance
    double leak1 = 0.0;        // ENV1 node leak, ohms (0: none)
    double leak2 = 0.0;        // ENV2 (C23) node leak
    double leak3 = 0.0;        // ENV3 node leak
    double decayEnd = 0.0;     // voltage the DECAY path ends at (schematic: ground)
};

class Tom {
public:
    static constexpr bool kFreeRunning = false;
    using Fit = TomFit;

    // Per-tom parts (LT, MT, HT).
    struct Parts {
        double c1, c2, c3;      // VCO integrator caps
        double cOver;           // overdrive timer cap
        double cEnv1, cA, cEnv2, cEnv3;
    };
    static constexpr Parts kParts[3] = {
        {22.0e-9, 33.0e-9, 12.0e-9, 0.01e-6, 0.22e-6, 0.68e-6, 0.68e-6, 0.056e-6},
        {18.0e-9, 27.0e-9, 10.0e-9, 0.0068e-6, 0.15e-6, 0.47e-6, 0.47e-6, 0.056e-6},
        {15.0e-9, 22.0e-9, 8.2e-9, 0.0056e-6, 0.15e-6, 0.47e-6, 0.47e-6, 0.047e-6},
    };
    static constexpr Fit kFits[3] = {
        {.hzPerVolt = 35.28, .offsetVolts = -0.088, .ratio2 = 0.679, .ratio3 = 1.88, .fastVolts = 3.02, .slowVolts = 0.645,
         .env1Scale = 0.931, .env2Scale = 1.394, .env3Scale = 0.983, .vcaSat = 2.91, .vco2Trim = 1.031, .vco3Trim = 1.304,
         .noiseGain = 1.0, .tickGain = 1.0, .outputGain = 30.74, .aScale = 0.923, .decayScale = 1.022, .leak1 = 0.0,
         .leak2 = 0.0, .leak3 = 60.0e6, .decayEnd = 0.324},
        {.hzPerVolt = 40.74, .offsetVolts = 0.056, .ratio2 = 0.673, .ratio3 = 1.77, .fastVolts = 3.02, .slowVolts = 0.645,
         .env1Scale = 0.900, .env2Scale = 0.994, .env3Scale = 0.877, .vcaSat = 3.82, .vco2Trim = 0.911, .vco3Trim = 1.388,
         .noiseGain = 1.0, .tickGain = 1.0, .outputGain = 29.24, .aScale = 1.011, .decayScale = 1.011, .leak1 = 0.0,
         .leak2 = 0.0, .leak3 = 60.0e6, .decayEnd = 0.339},
        {.hzPerVolt = 49.21, .offsetVolts = -0.023, .ratio2 = 0.675, .ratio3 = 1.95, .fastVolts = 3.02, .slowVolts = 0.645,
         .env1Scale = 0.848, .env2Scale = 1.117, .env3Scale = 0.845, .vcaSat = 3.16, .vco2Trim = 0.842, .vco3Trim = 1.415,
         .noiseGain = 1.0, .tickGain = 1.0, .outputGain = 31.02, .aScale = 1.083, .decayScale = 1.022, .leak1 = 0.0,
         .leak2 = 0.0, .leak3 = 60.0e6, .decayEnd = 0.319},
    };
    void setFit(const Fit& f) noexcept {  // calibration tooling only; prepare() after
        fit_ = f;
        customFit_ = true;
    }
    const Fit& fit() const noexcept { return fit_; }

    // ---- schematic values -------------------------------------------------
    static constexpr double kTriggerSeconds = 2.0e-3;
    static constexpr double kRint = 47.0e3, kRreset = 10.0e3;
    static constexpr double kRin = 10.0e3, kRbypass = 1.0e3, kRload = 2.2e3 + 47.0, kRe = 47.0;
    static constexpr double kDiodeIs = 2.5e-9, kDiodeNvt = 1.75 * 0.02585;
    static constexpr double kR64 = 100.0e3, kR61 = 470.0e3, kC17 = 0.047e-6, kR60 = 2.2e6, kC16 = 0.1e-6;
    static constexpr double kROver = 1.0e6;
    static constexpr double kR107 = 470.0e3, kR108 = 220.0e3, kR109 = 220.0e3, kR110 = 470.0e3;
    static constexpr double kR100 = 100.0e3, kR111 = 56.0e3, kVR11 = 500.0e3;
    static constexpr double kChargeOhms = 100.0;  // Q17's emitter resistor (R96)
    static constexpr double kTnGain = 1.1739, kTnLpHz = 1311.2, kTnHpHz = 782.2, kTnHpQ = 1.134;
    static constexpr double kR193 = 22.0e3, kR194Late = 100.0e3, kR194Early = 47.0e3;
    static constexpr double kC54Late = 4.7e-9, kC54Early = 2.2e-9, kR200 = 100.0e3;
    static constexpr double kTnEmitter = 47.0 / 1047.0;  // C53 -> R195 1k / R199 47
    static constexpr double kC53HpHz = 1.0 / (dsp::kTwoPi * 1047.0 * 0.047e-6);  // 3.23 kHz
    static constexpr double kR114 = 22.0e3;
    static constexpr double kPulseRailVolts = 14.4;
    static constexpr double kNoiseVolts = 2.435;  // see BassDrum.h

    void prepare(const VoiceSetup& raw, VoiceId which) noexcept {
        which_ = which == MT ? 1 : (which == HT ? 2 : 0);
        voice_ = which == MT ? MT : (which == HT ? HT : LT);
        if (!customFit_) fit_ = kFits[which_];
        const VoiceSetup s = dsp::saneSetup(raw);
        fs_ = s.sampleRate;
        dt_ = 1.0 / fs_;
        const std::uint32_t base = kPartBase + 0x20U * static_cast<std::uint32_t>(which_);
        auto part = [&s, base](std::uint32_t id, double nominal, float tol) {
            return nominal * static_cast<double>(dsp::componentScale(s.unitSeed, base + id, tol, s.variance));
        };
        const Parts& p = kParts[which_];
        k_ = fit_.hzPerVolt * part(0, 1.0, 0.03f);
        r2_ = fit_.ratio2 * part(1, 1.0, 0.02f);
        r3_ = fit_.ratio3 * part(2, 1.0, 0.02f);
        cEnv1_ = part(3, p.cEnv1 * fit_.env1Scale, 0.10f);
        cA_ = part(4, p.cA * fit_.aScale, 0.10f);
        cEnv2_ = part(5, p.cEnv2 * fit_.env2Scale, 0.10f);
        cEnv3_ = part(6, p.cEnv3 * fit_.env3Scale, 0.10f);
        c17_ = part(7, kC17, 0.10f);
        c16_ = part(8, kC16, 0.10f);
        cOver_ = part(9, p.cOver, 0.10f);
        early_ = s.revision == Revision::Early;
        // The VCO threshold follows the pitch calibration (f = V / (4 V_th R C)).
        vth_ = 1.0 / (4.0 * fit_.hzPerVolt * kRint * p.c1);
        vco1_.prepare(kRreset * p.c1 * fs_);
        vco2_.prepare(kRreset * p.c2 * fs_);
        vco3_.prepare(kRreset * p.c3 * fs_);
        // The three VCOs' clippers are the same circuit: VCO-2 and VCO-3 read
        // clip1_'s table (their own state warm-starts a solve beyond it).
        clip1_.set(1.0 / kRin, 1.0 / kRload, kDiodeIs, kDiodeNvt);
        clip2_.set(1.0 / kRin, 1.0 / kRload, kDiodeIs, kDiodeNvt);
        clip3_.set(1.0 / kRin, 1.0 / kRload, kDiodeIs, kDiodeNvt);
        clip1_.buildTable(1.05 * vth_);
        clipOver_.set(1.0 / kRin + 1.0 / kRbypass, 1.0 / kRload, kDiodeIs, kDiodeNvt);
        clipOver_.buildTable(1.05 * vth_);
        nOver_.setCap(cOver_);
        nEnv1_.setCap(cEnv1_);
        nA_.setCap(cA_);
        nEnv1_.setLeak(fit_.leak1);
        nEnv2_.setLeak(fit_.leak2);
        nEnv3_.setLeak(fit_.leak3);
        nEnv2_.setCap(cEnv2_);
        nEnv3_.setCap(cEnv3_);
        n54_.setCap(early_ ? kC54Early : kC54Late);
        tnLp_.setCutoff(kTnLpHz, fs_);
        tnHp_.setFreqQ(kTnHpHz, kTnHpQ, fs_);
        c53_.setCutoff(kC53HpHz, fs_);
        noiseKnob_.setTime(0.005, fs_);
        haveParams_ = false;
        setParams(params_);
        reset();
    }
    void prepare(const VoiceSetup& s) noexcept { prepare(s, voice_); }

    void reset() noexcept {
        vco1_.reset();
        vco2_.reset();
        vco3_.reset();
        clip1_.reset();
        clipOver_.reset();
        clip2_.reset();
        clip3_.reset();
        nOver_.reset();
        nEnv1_.reset();
        nA_.reset();
        nEnv2_.reset();
        nEnv3_.reset();
        n54_.reset();
        tnLp_.reset();
        tnHp_.reset();
        c53_.reset();
        noiseKnob_.snap(noiseTarget_);
        fast_ = slow_ = 0.0;
        pulseLeft_ = 0.0;
        pending_ = false;
        active_ = false;
    }

    void setParams(const VoiceParams& raw) noexcept {
        const VoiceParams p = dsp::saneParams(raw, voice_);
        if (haveParams_ && p.k == params_.k) return;
        params_ = p;
        haveParams_ = true;
        using namespace knob;
        wiper_ = 15.0 * (10.0e3 + 10.0e3 * static_cast<double>(p.k[TomTune])) / 88.0e3;
        decayOhms_ = kR111 + kVR11 * fit_.decayScale * static_cast<double>(p.k[TomDecay]);
        gDecay_ = 1.0 / decayOhms_;
        depth_ = dsp::amountKnobScale(p.k[TomDepth]);
        const double sweep = dsp::timeKnobScale(p.k[TomSweep]);
        fastDecay_ = std::exp(-dt_ / (kR61 * c17_ * sweep));
        slowDecay_ = std::exp(-dt_ / (kR60 * c16_ * sweep));
        noiseTarget_ = static_cast<float>(dsp::amountKnobScale(p.k[TomNoise]));
        noiseKnob_.setTarget(noiseTarget_);
        if (!active_) noiseKnob_.snap(noiseTarget_);
    }

    void trigger(const Hit& hit) noexcept {
        acc_ = std::isfinite(hit.acc) ? hit.acc : 0.0f;
        semis_ = dsp::saneSemis(hit.pitchSemis);
        pending_ = true;
        active_ = true;
    }

    void setAccLine(float volts) noexcept {
        if (std::isfinite(volts)) acc_ = volts;
    }

    void render(float* out, const float* noise, int n) noexcept {
        if (!active_) {
            std::fill(out, out + n, 0.0f);
            return;
        }
        for (int i = 0; i < n; ++i) {
            out[i] = static_cast<float>(tick(static_cast<double>(noise[i])));
            if (!active_) {
                std::fill(out + i + 1, out + n, 0.0f);
                return;
            }
        }
    }

    bool isActive() const noexcept { return active_; }
    int latencySamples() const noexcept { return 0; }

private:
    static constexpr std::uint32_t kPartBase = 0xB200U;
    static constexpr double kSleepVolts = 1.0e-7;

    double tick(double noise) noexcept {
        // ---- trigger --------------------------------------------------------
        bool pulse = false;
        if (pending_) {
            pending_ = false;
            pulseLeft_ = kTriggerSeconds;
            vco1_.hold();
            vco2_.hold();
            vco3_.hold();
            pitch_ = dsp::semitonesToRatio(semis_);
        }
        if (pulseLeft_ > 0.0) {
            pulse = true;
            if (pulseLeft_ <= dt_) {
                const double frac = pulseLeft_ / dt_;
                vco1_.release(frac, 1);
                vco2_.release(frac, 1);
                vco3_.release(frac, 1);
                pulseLeft_ = 0.0;
            } else {
                pulseLeft_ -= dt_;
            }
        }
        // The accent line is read while the trigger is open.
        const double gated = pulse ? dsp::chargeThroughJunctions(
                                         dsp::accentBufferVolts(static_cast<double>(acc_), dsp::analog::kTomBufferOffset) - 0.1,
                                         dsp::analog::kDiodeVolts)
                                   : 0.0;

        // ---- rails and VCOs -----------------------------------------------------
        if (pulse) {
            // C17 / C16 are charged within the trigger (diodes, small R);
            // the fitted terms are their contributions to VSS at its end.
            fast_ = fit_.fastVolts;
            slow_ = fit_.slowVolts;
        } else {
            fast_ *= fastDecay_;
            slow_ *= slowDecay_;
        }
        const double vdd = wiper_ + fit_.offsetVolts;
        const double vss = vdd + depth_ * (fast_ + slow_);
        const double s0 = 4.0 * k_ * pitch_ * vdd * dt_;  // symmetric slope at f_s
        const double down = s0 * vss / vdd;
        const double t1 = vco1_.tick(s0, down);
        const double t2 = vco2_.tick(s0 * r2_, down * r2_);
        // VCO-3 and the tom noise reach the output only through ENV3's VCA;
        // once it has shut (< -140 dB) they are not computed until the next
        // trigger, which resets VCO-3 anyway.
        const bool path3 = pulse || nEnv3_.v() > 0.2;
        const double t3 = path3 ? vco3_.tick(s0 * r3_, down * r3_) : 0.0;

        // VCO-1 overdrive: Q18 bypasses the 10k with 1k while C24 drains.
        double u1;
        if (pulse || nOver_.v() > 0.2) {
            nOver_.begin();
            if (pulse) nOver_.chargeFrom(kChargeOhms, kPulseRailVolts - dsp::analog::kDiodeVolts);
            nOver_.toBase(kROver);
            nOver_.step(dt_);
            // Q18 conducts in its active region: its share of the drive
            // follows its base current, from the full 1k bypass down to none.
            const double over = std::min(1.0, nOver_.drive() / (kPulseRailVolts - 1.2));
            const double plain = clip1_.processTable(vth_ * t1);
            u1 = plain + (clipOver_.processTable(vth_ * t1) - plain) * over;
        } else {
            if (nOver_.v() != 0.0) nOver_.reset();
            u1 = clip1_.processTable(vth_ * t1);
        }
        const double u2 = clip2_.processTableOf(clip1_, vth_ * t2);
        const double u3 = path3 ? clip3_.processTableOf(clip1_, vth_ * t3) : 0.0;

        // ---- envelopes -------------------------------------------------------------
        const dsp::TwoSourceBase b19 = dsp::twoSourceBase(nEnv1_.v(), kR107, nEnv2_.v(), kR108);
        nEnv1_.begin();
        if (pulse) nEnv1_.chargeFrom(kChargeOhms, gated);
        nEnv1_.toSource(kR107, b19.vBase);
        nEnv1_.tail(b19.drive);
        nA_.begin();
        if (pulse) nA_.chargeFrom(kChargeOhms, gated);
        nA_.toConductance(gDecay_, fit_.decayEnd);
        nA_.toNode(kR100, nEnv2_.v());
        nEnv2_.begin();
        nEnv2_.toNode(kR100, nA_.v());
        nEnv2_.toSource(kR108, b19.vBase);
        nEnv2_.toBase(kR109);
        if (path3) {
            nEnv3_.begin();
            if (pulse) nEnv3_.chargeFrom(kChargeOhms, gated);
            nEnv3_.toBase(kR110);
        }
        dsp::EnvNode::step2(nEnv1_, nA_, dt_);
        if (path3) {
            dsp::EnvNode::step2(nEnv2_, nEnv3_, dt_);
        } else {
            nEnv2_.step(dt_);
            if (nEnv3_.v() != 0.0) nEnv3_.reset();
        }
        if (pulse || n54_.v() > 0.2) {
            n54_.begin();
            n54_.toSource(kR200, pulse ? kPulseRailVolts - dsp::analog::kDiodeVolts : 0.0);
            n54_.toBase(kR200);
            n54_.step(dt_);
        } else if (n54_.v() != 0.0) {
            n54_.reset();
        }

        // ---- tom noise ------------------------------------------------------------------
        double tn = 0.0, tnTick = 0.0;
        if (path3) {
            tnHp_.tick(tnLp_.lowpass(kTnGain * kNoiseVolts * noise));
            tn = tnHp_.highpass();
            tnTick = c53_.highpass(tn);  // C53 into R195 + R199
        }
        // IC27a: -R193 x (Q33's collector current + IC27b's output / R194).
        // Q33's current is referred to the R194 route by tickGain.
        const double g33 = n54_.drive() * (dsp::kBaseRefOhms / kR200);
        const double r194 = early_ ? kR194Early : kR194Late;
        const double tomNoise = -(fit_.tickGain * g33 * tnTick * kTnEmitter + tn * kR193 / r194);
        const double noiseIn = static_cast<double>(noiseKnob_.tick()) * fit_.noiseGain * tomNoise * (kRe / kR114);

        // ---- VCAs and output ---------------------------------------------------------------
        const double sat = fit_.vcaSat;
        const double g1 = dsp::vcaGain(b19.drive, sat);
        const double g2 = dsp::vcaGain(nEnv2_.drive() * (dsp::kBaseRefOhms / kR109), sat);
        const double g3 = dsp::vcaGain(nEnv3_.drive() * (dsp::kBaseRefOhms / kR110), sat);
        const double em = kRe / kRload;
        const double y = -fit_.outputGain *
                         (g1 * u1 * em + fit_.vco2Trim * g2 * u2 * em + g3 * (fit_.vco3Trim * u3 * em + noiseIn));

        // ---- sleep ---------------------------------------------------------------------------
        if (!pulse) {
            const double bound = fit_.outputGain * ((g1 + g2 + g3) * 0.03 + g3 * 0.01 * fit_.noiseGain);
            if (bound < kSleepVolts && std::abs(y) < kSleepVolts && nA_.v() < 0.3 && fast_ < 1.0e-3) {
                reset();
                return 0.0;
            }
        }
        return y;
    }

    Fit fit_ = kFits[0];
    bool customFit_ = false;
    int which_ = 0;
    VoiceId voice_ = LT;
    double fs_ = 48000.0, dt_ = 1.0 / 48000.0;
    VoiceParams params_ = defaultParams(LT);
    bool haveParams_ = false, early_ = false;
    double k_ = 35.0, r2_ = 0.679, r3_ = 1.88, vth_ = 7.0;
    double cEnv1_ = 0.22e-6, cA_ = 0.68e-6, cEnv2_ = 0.68e-6, cEnv3_ = 0.056e-6, c17_ = kC17, c16_ = kC16, cOver_ = 0.01e-6;
    double wiper_ = 2.557, decayOhms_ = 306.0e3, gDecay_ = 1.0 / 306.0e3, depth_ = 1.0, fastDecay_ = 0.0, slowDecay_ = 0.0;
    float noiseTarget_ = 1.0f;

    dsp::TriangleVco vco1_, vco2_, vco3_;
    dsp::DiodePairClipper clip1_, clipOver_, clip2_, clip3_;
    dsp::EnvNode nOver_, nEnv1_, nA_, nEnv2_, nEnv3_, n54_;
    dsp::OnePole tnLp_, c53_;
    dsp::Svf tnHp_;
    dsp::Smoother noiseKnob_;

    float acc_ = AccentDac::volts(AccentDac::kNormalCode);
    double semis_ = 0.0, pitch_ = 1.0;
    double fast_ = 0.0, slow_ = 0.0;
    double pulseLeft_ = 0.0;
    bool pending_ = false, active_ = false;
};

}  // namespace mw909::voices
