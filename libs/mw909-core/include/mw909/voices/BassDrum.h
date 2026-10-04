// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Bass drum (BD). A block model of the original circuit, simulated in volts
// (research/01 §3, research/05 §2.2; designators from the voicing-board
// schematic):
//
//   TRIG 2 ms -> Q1/Q2: the +15 V pulse, and Q5: the accent line gated by the
//                trigger. The accent buffer IC11b (x3.128) feeds Q4/Q5.
//   VCO        integrator IC13a (R59 47k, C14 0.033u) + comparator IC13b
//              (R49 100k / R50 47k); Q11 shorts C14 through R51 2.2k for the
//              2 ms trigger (dsp/TriangleVco.h). The slope is the control
//              voltage of IC12a: V_cv = (R28/R27) 15 V + (R28/R26) V_C9
//              = 1.50 V + 0.4545 V_C9, so f = f_b + df e^(-t/tau).
//   Pitch env  C9 0.33u (0.22u on early units) charged by the +15 V pulse
//              through D7 + R21 2.2k, discharged through R57 22k + VR2 TUNE
//              (100k A) in parallel with R26 330k: tau = 6.8..29.4 ms. The
//              pulse is not accented, so neither is the sweep (measured).
//   Shaper     triangle -> R52 10k -> D10/D11 (anti-parallel, 1SS133) loaded by
//              R53 2.2k + R43 47 -> Q12's emitter (dsp/DiodeClipper.h).
//   ENV1       C8 0.33u, charged from Q4 (the accent buffer) through R35 1.8k
//              and D9, with D8 to the R36 47k / R22 10k divider (2.63 V,
//              8.25k Thevenin: a soft clamp), for as long as the hold timer
//              keeps Q4 on. Hold: C7 0.033u charged from the pulse through
//              R20 6.8k, discharged through R18 470k into Q3. Afterwards C8
//              discharges through R58 47k + VR5 DECAY (1M A) and R54 470k into
//              Q12's base: the thresholded VCA tail.
//   ATTACK     pulse path: Q8 follows the gated accent (R6 22k) and the +15 V
//              pulse (R5 100k); C2 3.3n, R7 100k / R16 39k / C3 4.7n / C4 100p
//              and R15 100k shape its edge into C11 0.47u / R44 22k and node
//              N (R56 330). Noise path: the shared noise through R10 12k /
//              R9 4.7k, C15 0.47u (26 Hz high-pass), R46 4.7k / C13 0.1u
//              (513 Hz low-pass), R45 4.7k into N. N -> VR3 ATTACK (500 B,
//              rheostat, also loading N) -> R55 47 -> Q6's emitter. Q6's base:
//              C12 0.033u, charged by the gated accent through R41 22k
//              (returning to 0 V after the trigger), into R42 470k.
//   Output     Q12 + Q6 collectors into IC11a, LEVEL VR4 100k (B) feedback.
//
// Every RC node is integrated exactly per sample (dsp/TransistorVca.h); the
// VCA cells are linear above the base-emitter threshold with a Shockley knee
// below it. Timing: the tone and the click start at the end of the 2 ms
// trigger (the VCO is held reset during it), as the reference recordings
// show them starting together ~1.5 ms after the first trace of the hit.
//
// Schematic values: everything above. FITTED (and to what; struct
// BassDrumFit holds them):
//  * baseHz 49.5: the VCO threshold becomes 4.88 V (derived 4.32 V), to the
//    settled pitch of two measured units (49.2-49.6 Hz, research/05 §2.2,
//    §7); that triangle peak also reproduces the measured H3/H5/H7 through
//    the 1SS133 pair (Is 2.5 nA, n 1.75; research/01 §3.5).
//  * sweepTrim 1.196 (x R28/R26) and tuneTaper (VR2 fraction 0.0094 /
//    0.0845 / 0.169 / 0.276 / 0.999 at TUNE 0/25/50/75/100 %), to the
//    reference unit's sweep fits (fb + df e^(-t/tau), per-cycle zero
//    crossings, from the onset): df 210-222 Hz, tau 7.6 / 9.7 / 11.8 / 14.4 /
//    29.2 ms (derived from the parts: tau 6.8..29.4 ms, df 216 Hz with a
//    fully charged C9, which the 2 ms pulse does not achieve).
//  * holdVolts 0.88: Q3 lets go when C7 has fallen to it (derived ~1.3 V,
//    45 ms; measured hold 61-63 ms at every accent and DECAY), with C7
//    leaking through 10 MOhm as when it was fitted.
//  * decayTaper (effective VR5 fraction 0.039 / 0.165 / 0.284 / 0.447 /
//    1.299 at DECAY 0/25/50/75/100 %) to the fact-checked t(-20 dB) 98 / 143
//    / 174 / 208 / 299 ms (research/01 F1, means over TUNE 0/50/100). The
//    nominal network runs 17-22 % short and the full-rotation point asks
//    for 1.3 MOhm against the 1 MOhm pot: the shortfall is in the model of
//    the C8 / Q12 network, not the taper (as the fact-check concluded).
//    (Refolded when the envelope nodes' 10 MOhm leak was removed: the same
//    network, the leak now inside these values.)
//  * bodySat 38 (Q12) to the measured TOTAL ACCENT body gain (+6.7 dB,
//    10-30 ms window); clickSat 1.2 (Q6) to the saturating onset (Vpp span
//    +6.2 dB, research/05 §0.11). The span is matched, not the curve: the
//    recordings are flat over the first 10 % and then near-linear in dB
//    (Vpp at 10 / 20 / 40 / 60 %: -0.1 / +1.5 / +3.0 / +5.4 dB), the model
//    is concave (+1.4 / +2.5 / +4.4 / +5.7 dB; its full span +6.9 dB in
//    Vpp at TUNE / ATTACK / DECAY 100). A dead zone cannot come from the
//    linear knob-to-code mapping of AccentDac; it may be firmware.
//  * The click: the measured click (ATTACK 100 minus ATTACK 0) is a unipolar
//    spike at the tone's start, ~0.1 ms rise and ~0.5 ms fall, the polarity
//    of the tone's first half-cycle, with no 3.7 kHz ring; the network as
//    read (a differentiator into a Q 3.1 Sallen-Key) rings and would click
//    at the trigger's start as well. Modelled as the trigger's trailing edge
//    (the voltage Q8 held) decaying in clickSeconds 0.45 ms through the
//    3.74 kHz low-pass at clickLpQ 0.7, clickGain 5.5 into node N: ATTACK
//    0 -> 100 raises the first 3 ms by +2.7 dB (measured +4.3). The edge
//    sense of the network is UNVERIFIED (research/01 §3.6).
//  * evenOrder 0.08: the saturated VCA transistor's even-order term, to the
//    level-independent H2 = -28 dB (research/05 §0.5). H3 / H4 / H5 / H7
//    follow (-25.4 / -45.0 / -39.5 / -39.2 dB, measured -25.7 / -44.7 /
//    -41.0 / -41.0); H6 does not (-76 against -46.5 dB), nor does the
//    original's negative-peak dominance (+/- 0.64; model 0.79): the
//    asymmetric mechanism behind both (an offset before the shaper?) is not
//    identified.
//  * outputGain 12.3: 1.02 Vpp for a normal hit with all knobs centred, from
//    the hand clap's trimmer target (2 Vpp at the multi-out, LEVEL centred:
//    the only numeric level on the board) and the reference unit's BD/CP
//    Vpp ratio at equal part volume (-5.86 dB, the clap averaged over 9
//    hits). This CONFLICTS with the service notes' scope drawing, ~0.61 Vpp
//    +-30 % (0.43..0.79 Vpp): the model is 1.68x (+4.5 dB) the drawing's
//    reading, outside its tolerance. The recordings were preferred: the
//    drawings are hand-drawn, have no graticule and are not on one scale
//    (the clap's burst spacing implies a different px/div, research/01
//    §1.5), whereas the CP trimmer target is a printed number and the
//    recordings' voice-to-voice ratios are measured at one part volume. The
//    snare (1.54 Vpp against ~1.0 drawn) is off the same way.
//  * Accent: a per-voice accent (code 40 against 24) gives 1.67x Vpp (+4.5
//    dB) and 1.75x the ENV1 voltage; the drawing's with/without ratio is
//    2.0x (research/01) or 2.6x (research/03), +-2 dB, taken with every knob
//    centred, so TOTAL ACCENT at 12 o'clock: with the voice accent added
//    (code 60) the model gives 2.2x. AccentDac's codes are not refitted to
//    the forum report of "about 2x" on the envelope (research/03 §3.2,
//    UNVERIFIED, knob setting unknown): kVoiceAccentCodes is shared with the
//    sample voices, and the snare cannot reach its drawn 2.1x at any code
//    (its whole measured TOTAL ACCENT span is +4.8 dB).
//
// Not modelled: the R495 150k early-unit resistor (negligible), the small
// trigger feed-through before the tone, the op-amps' slew and offsets, the
// multi-out 1k / 0.01u RC of late units (an output-stage item).
//
// Extended controls (Voices.h): Pitch moves the base frequency (the R27
// offset, as the classic "pitch" mod: the sweep keeps its size in Hz); Tune
// Depth scales the sweep (R28/R26; 0.5 stock); Shaper scales the drive into
// the diodes (R52 = 10k * 4^(1 - 2 x): 40k .. 2.5k; 0.5 stock), smoothed
// over 5 ms like the other knobs and read from nine tables built in
// prepare() (dsp::DiodeClipperBank: exact at the grid points, the stock one
// included, ~0.1 mV off between them), so automating it costs no more than
// any other knob. Hit::pitchSemis scales the whole VCO (the integrator
// capacitor), for chromatic play.
// Revision::Early: C9 0.22 uF (the TUNE range shortens by 2/3).

#pragma once

#include <algorithm>
#include <bit>
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

// The fitted constants of BassDrum (see its header comment), in one place so
// that calibration tooling can vary them.
struct BassDrumFit {
    double baseHz = 49.5;             // settled pitch; sets the VCO threshold
    double sweepTrim = 1.196;         // x R28/R26
    double tuneTaper[5] = {0.0094, 0.0845, 0.1691, 0.2759, 0.9988};   // VR2 fraction, TUNE 0..100 %
    double decayTaper[5] = {0.0387, 0.1651, 0.2844, 0.4473, 1.2994};  // VR5 fraction, DECAY 0..100 %
    double holdVolts = 0.88;          // C7 voltage at which Q3 lets go
    double bodySat = 38.0;            // Q12 saturation (volts of base drive)
    double clickSat = 1.2;            // Q6 saturation
    double evenOrder = 0.08;          // even-order term of the VCA
    double clickSeconds = 0.45e-3;    // decay of the trailing-edge pulse
    double clickLpQ = 0.7;            // pulse low-pass Q (derived 2.2)
    double clickGain = 5.5;           // pulse path into node N
    double outputGain = 12.3;         // volts out per (base-drive volt x emitter volt)
};

class BassDrum {
public:
    static constexpr bool kFreeRunning = false;

    // ---- schematic values -------------------------------------------------
    static constexpr double kTriggerSeconds = 2.0e-3;  // TRIG pulse [SM p.5]
    static constexpr double kR59 = 47.0e3, kC14 = 0.033e-6, kR51 = 2.2e3;
    static constexpr double kR28 = 150.0e3, kR27 = 1.5e6, kR26 = 330.0e3;
    static constexpr double kR57 = 22.0e3, kVR2 = 100.0e3, kR21 = 2.2e3;
    static constexpr double kC9Late = 0.33e-6, kC9Early = 0.22e-6;
    static constexpr double kR52 = 10.0e3, kR53R43 = 2.247e3, kR43 = 47.0;
    static constexpr double kDiodeIs = 2.5e-9, kDiodeNvt = 1.75 * 0.02585;
    static constexpr double kC8 = 0.33e-6, kR35 = 1.8e3, kR58 = 47.0e3, kVR5 = 1.0e6, kR54 = 470.0e3;
    static constexpr double kClampNodeVolts = 15.0 * 10.0 / 57.0;  // R36 47k / R22 10k
    static constexpr double kClampOhms = 47.0e3 * 10.0e3 / 57.0e3;
    static constexpr double kC7 = 0.033e-6, kR20 = 6.8e3, kR18 = 470.0e3;
    static constexpr double kC12 = 0.033e-6, kR41 = 22.0e3, kR42 = 470.0e3;
    static constexpr double kR6 = 22.0e3, kR5 = 100.0e3;
    static constexpr double kClickLpHz = 3743.0;     // R7 100k / R16 39k / C3 4.7n / C4 100p
    static constexpr double kNoiseDiv = 4.7 / 16.7;  // R9 / (R10 + R9)
    static constexpr double kNoiseHpHz = 25.8, kNoiseLpHz = 513.0;  // C15; R46 + C13
    static constexpr double kR45 = 4.7e3, kR44 = 22.0e3, kR56 = 330.0, kVR3 = 500.0, kR55 = 47.0;
    static constexpr double kPulseRailVolts = 14.4;  // +15 V less Q2's saturation
    static constexpr double kHoldLeakOhms = 10.0e6;  // C7's leak, part of the hold fit (holdVolts)
    // Volts of the 0/15 V noise bit stream per unit of dsp::WhiteNoise: its
    // 2.47e-4 V^2/Hz (research/01 §2.2) over the bus's kDensity^2.
    static constexpr double kNoiseVolts = 2.435;

    // ---- fitted (see the header comment) ------------------------------------
    using Fit = BassDrumFit;
    static constexpr Fit kFit{};
    // Calibration tooling only: replace the fitted constants (call prepare() after).
    void setFit(const Fit& f) noexcept { fit_ = f; }
    const Fit& fit() const noexcept { return fit_; }

    void prepare(const VoiceSetup& raw) noexcept {
        const VoiceSetup s = dsp::saneSetup(raw);
        fs_ = s.sampleRate;
        dt_ = 1.0 / fs_;
        auto part = [&s](std::uint32_t id, double nominal, float tol) {
            return nominal * static_cast<double>(dsp::componentScale(s.unitSeed, kPartBase + id, tol, s.variance));
        };
        c9_ = part(0, s.revision == Revision::Early ? kC9Early : kC9Late, 0.10f);
        c14Scale_ = part(1, 1.0, 0.01f);  // two units agree within 0.3 % (research/05 §7)
        c8_ = part(2, kC8, 0.10f);
        vr5_ = part(3, kVR5, 0.20f);
        vr2_ = part(4, kVR2, 0.20f);
        c7Scale_ = part(5, 1.0, 0.10f);
        holdVolts_ = fit_.holdVolts * part(6, 1.0, 0.05f);
        vth_ = 1.5 / (4.0 * fit_.baseHz * kR59 * kC14);  // the triangle peak, volts
        vco_.prepare(kR51 * kC14 * c14Scale_ * fs_);
        n9_.setCap(c9_);
        n7_.setCap(kC7 * c7Scale_);
        n7_.setLeak(kHoldLeakOhms);
        n8_.setCap(c8_);
        n12_.setCap(kC12);
        q8Fall_ = std::exp(-dt_ / fit_.clickSeconds);
        clickLp_.setFreqQ(kClickLpHz, fit_.clickLpQ, fs_);
        noiseHp_.setCutoff(kNoiseHpHz, fs_);
        noiseLp_.setCutoff(kNoiseLpHz, fs_);
        attack_.setTime(0.005, fs_);
        drive_.setTime(0.005, fs_);
        // The Shaper's R52 range (40k .. 2.5k), every table built here.
        shaper_.build(1.0 / (4.0 * kR52), 4.0 / kR52, 1.0 / kR53R43, kDiodeIs, kDiodeNvt, 1.05 * vth_);
        haveParams_ = false;
        setParams(params_);
        reset();
    }

    void reset() noexcept {
        vco_.reset();
        shaper_.reset();
        n9_.reset();
        n7_.reset();
        n8_.reset();
        n12_.reset();
        clickLp_.reset();
        noiseHp_.reset();
        noiseLp_.reset();
        attack_.snap(attackTarget_);
        snapDrive();
        q8_ = q8Held_ = 0.0;
        q8Armed_ = false;
        pulseLeft_ = 0.0;
        hold_ = false;
        pending_ = false;
        active_ = false;
    }

    void setParams(const VoiceParams& raw) noexcept {
        const VoiceParams p = dsp::saneParams(raw, BD);
        if (haveParams_ && p.k == params_.k) return;
        params_ = p;
        haveParams_ = true;
        using namespace knob;
        const double tune = dsp::knobTable(fit_.tuneTaper, p.k[BdTune]);
        rd9_ = 1.0 / (1.0 / (kR57 + vr2_ * tune) + 1.0 / kR26);
        rd8_ = kR58 + vr5_ * dsp::knobTable(fit_.decayTaper, p.k[BdDecay]);
        g9_ = 1.0 / rd9_;
        g8_ = 1.0 / rd8_;
        // ATTACK: rheostat into R55, minimum resistance at full rotation; it
        // also loads node N.
        const double rv3 = kVR3 * (1.0 - static_cast<double>(p.k[BdAttack]));
        attackTarget_ = static_cast<float>(kR55 / (rv3 + kR55));
        nLoad_ = 1.0 / (1.0 / kR56 + 1.0 / (rv3 + kR55));
        gN_ = 1.0 / kR44 + 1.0 / kR45 + 1.0 / nLoad_;
        offsetVolts_ = (kR28 / kR27) * 15.0 * dsp::semitonesToRatio(static_cast<double>(p.k[BdPitch]));
        sweepGain_ = (kR28 / kR26) * fit_.sweepTrim * dsp::amountKnobScale(p.k[BdDepth]);
        // Shaper: R52 = 10k x 4^(1 - 2 x), smoothed like the other knobs; the
        // tables for its whole range were built in prepare().
        driveTarget_ = p.k[BdDrive];
        drive_.setTarget(driveTarget_);
        driveSettled_ = false;
        attack_.setTarget(attackTarget_);
        if (!active_) {
            attack_.snap(attackTarget_);
            snapDrive();
        }
    }

    void trigger(const Hit& hit) noexcept {
        acc_ = std::isfinite(hit.acc) ? hit.acc : 0.0f;
        semis_ = dsp::saneSemis(hit.pitchSemis);
        pending_ = true;
        active_ = true;
    }

    // The BD's DAC is not shared; kept for the common interface. The line is
    // read while the trigger and the hold are open.
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
    static constexpr std::uint32_t kPartBase = 0xB000U;
    static constexpr int kShaperTables = 9;  // R52 40k .. 2.5k in steps of sqrt(2)
    static constexpr double kSleepVolts = 1.0e-7;

    // The accent buffer and ENV1's charging path are functions of the accent
    // line alone, which holds still between hits (and DAC rewrites): worked
    // out again only when it moves, not on every sample of the trigger and
    // hold.
    void readAccent() noexcept {
        const std::uint32_t key = std::bit_cast<std::uint32_t>(acc_);
        if (accentRead_ && key == accentKey_) return;
        accentRead_ = true;
        accentKey_ = key;
        vbuf_ = dsp::accentBufferVolts(static_cast<double>(acc_));
        // Q4 (saturated, 0.1 V) -> R35 -> node X (D8 to the 2.63 V divider) -> D9 -> C8.
        const double vs = vbuf_ - 0.1;
        const double knee = kClampNodeVolts + dsp::analog::kDiodeVolts;
        const double gC = 1.0 / kClampOhms, gS = 1.0 / kR35;
        const double clamped = (vs * gS + knee * gC) / (gS + gC);
        const double w = dsp::logistic((vs - knee) / 0.05);
        const double ex = vs + (clamped - vs) * w;  // node X while D9 conducts
        const double ohms = 1.0 / (gS + w * gC);
        gCharge8_ = 1.0 / ohms;
        vCharge8_ = ex - dsp::analog::kDiodeVolts;
    }

    // The Shaper's bank position and R52 conductance for a knob value x.
    void setDrive(double x) noexcept {
        drivePos_ = x * (kShaperTables - 1);
        driveG_ = std::exp2(4.0 * x - 2.0) / kR52;
    }
    void snapDrive() noexcept {
        drive_.snap(driveTarget_);
        setDrive(static_cast<double>(driveTarget_));
        driveSettled_ = true;
    }

    double tick(double noise) noexcept {
        // ---- trigger timing ------------------------------------------------
        bool pulse = false;
        if (pending_) {
            pending_ = false;
            pulseLeft_ = kTriggerSeconds;
            vco_.hold();
            hold_ = true;
            hzPerVolt_ = fit_.baseHz / 1.5 * dsp::semitonesToRatio(semis_) / c14Scale_;
        }
        if (pulseLeft_ > 0.0) {
            pulse = true;
            if (pulseLeft_ <= dt_) {
                vco_.release(pulseLeft_ / dt_, 1);
                pulseLeft_ = 0.0;
            } else {
                pulseLeft_ -= dt_;
            }
        }
        // The accent line is read while the trigger and the hold are open.
        if (pulse || hold_) readAccent();
        const double vbuf = (pulse || hold_) ? vbuf_ : 0.0;

        // ---- pitch envelope C9 and VCO ------------------------------------
        n9_.begin();
        if (pulse) n9_.chargeFrom(kR21, kPulseRailVolts - dsp::analog::kDiodeVolts);
        n9_.toConductance(g9_, 0.0);
        n9_.step(dt_);
        const double vcv = offsetVolts_ + sweepGain_ * n9_.v();
        const double slope = 4.0 * hzPerVolt_ * vcv * dt_;
        const double tri = vco_.tick(slope, slope);  // normalised +-1, one sample late
        if (!driveSettled_) {
            drive_.tick();
            if (drive_.settled()) snapDrive();
            else setDrive(static_cast<double>(drive_.value()));
        }
        const double node = shaper_.process(vth_ * tri, drivePos_, driveG_);

        // ---- hold timer C7 and ENV1 C8 --------------------------------------
        if (hold_) {  // after the hold C7 no longer matters: the next pulse recharges it
            n7_.begin();
            if (pulse) n7_.chargeFrom(kR20, kPulseRailVolts);
            n7_.toBase(kR18);
            n7_.step(dt_);
            if (!pulse && n7_.v() < holdVolts_) {
                hold_ = false;
                n7_.reset();
            }
        }

        n8_.begin();
        if (hold_) n8_.chargeFromConductance(gCharge8_, vCharge8_);
        n8_.toConductance(g8_, 0.0);
        n8_.toBase(kR54);
        n8_.step(dt_);

        // ---- click: ENV2 C12, pulse path, noise path -----------------------
        if (pulse || n12_.v() > 0.2) {  // below 0.2 V the click VCA is shut (< -140 dB)
            n12_.begin();
            n12_.toSource(kR41, pulse ? std::max(0.0, vbuf - 0.1) : 0.0);
            n12_.toBase(kR42);
            n12_.step(dt_);
        } else if (n12_.v() != 0.0) {
            n12_.reset();
        }
        // Q8 follows R6 (gated accent) + R5 (+15 V pulse). What reaches node N
        // is the edge at the END of the trigger, at the tone's start: a
        // unipolar pulse of the height Q8 held, decaying in clickSeconds, with
        // the polarity of the tone's first half-cycle (measured, research/01
        // §3.6; the edge sense of the network as read is UNVERIFIED and its
        // leading edge is not seen in the recordings).
        if (pulse) {
            q8Held_ = std::max(0.0, (vbuf / kR6 + kPulseRailVolts / kR5) / (1.0 / kR6 + 1.0 / kR5) - dsp::analog::kVbe);
            q8_ = 0.0;
            q8Armed_ = true;
        } else {
            if (q8Armed_) {
                q8_ = q8Held_;
                q8Armed_ = false;
            }
            q8_ *= q8Fall_;
        }
        clickLp_.tick(q8_);
        const double pulseOut = fit_.clickGain * clickLp_.lowpass();
        // The noise path reaches the output only through Q6.
        const double nlp = n12_.v() > 0.0 ? noiseLp_.lowpass(noiseHp_.highpass(kNoiseVolts * noise * kNoiseDiv)) : 0.0;
        // Node N: two sources (R44, R45) into R56 || the ATTACK path.
        const double gN = gN_;
        const double vN = (pulseOut / kR44 + nlp / kR45) / gN;
        const double ve6 = vN * static_cast<double>(attack_.tick());

        // ---- VCAs and output -------------------------------------------------
        const double g12 = dsp::vcaGain(n8_.drive(), fit_.bodySat);
        const double g6 = dsp::vcaGain(n12_.drive(), fit_.clickSat);
        const double ve12 = node * (kR43 / kR53R43);
        const double w = node * 2.0;  // ~ +-1 at the stock drive
        const double body = g12 * ve12 * (1.0 + fit_.evenOrder * w);
        const double y = -fit_.outputGain * (body + g6 * ve6);

        // ---- sleep -------------------------------------------------------------
        if (!pulse && !hold_) {
            const double bound = fit_.outputGain * (g12 * 0.03 + g6 * 0.05);
            if (bound < kSleepVolts && std::abs(y) < kSleepVolts && n9_.v() < 1.0e-4) {
                reset();
                return 0.0;
            }
        }
        return y;
    }

    Fit fit_ = kFit;
    double fs_ = 48000.0, dt_ = 1.0 / 48000.0;
    VoiceParams params_ = defaultParams(BD);
    bool haveParams_ = false;
    double c9_ = kC9Late, c14Scale_ = 1.0, c8_ = kC8, vr5_ = kVR5, vr2_ = kVR2, c7Scale_ = 1.0;
    double holdVolts_ = 0.88, vth_ = 4.884;
    double rd9_ = 20.6e3, rd8_ = 500.0e3, nLoad_ = 300.0;
    double g9_ = 1.0 / 20.6e3, g8_ = 1.0 / 500.0e3, gN_ = 1.0 / kR44 + 1.0 / kR45 + 1.0 / 300.0;   // set with them
    double offsetVolts_ = 1.5, sweepGain_ = kR28 / kR26;
    double hzPerVolt_ = 33.0, q8Fall_ = 0.0;
    double drivePos_ = 4.0, driveG_ = 1.0 / kR52;
    float attackTarget_ = 0.1f, driveTarget_ = 0.5f;
    bool driveSettled_ = true;

    dsp::TriangleVco vco_;
    dsp::DiodeClipperBank<kShaperTables> shaper_;
    dsp::EnvNode n9_, n7_, n8_, n12_;
    dsp::Svf clickLp_;
    dsp::OnePole noiseHp_, noiseLp_;
    dsp::Smoother attack_, drive_;

    float acc_ = AccentDac::volts(AccentDac::kNormalCode);
    std::uint32_t accentKey_ = 0;   // readAccent(): the line it was worked out for
    bool accentRead_ = false;
    double vbuf_ = 0.0, gCharge8_ = 0.0, vCharge8_ = 0.0;
    double semis_ = 0.0;
    double q8_ = 0.0, q8Held_ = 0.0;
    bool q8Armed_ = false;
    double pulseLeft_ = 0.0;
    bool hold_ = false, pending_ = false, active_ = false;
};

}  // namespace mw909::voices
