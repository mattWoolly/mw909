// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Snare drum (SD). A block model of the original circuit, simulated in volts
// (research/01 §4, research/05 §2.3; designators from the voicing-board
// schematic):
//
//   TRIG 2 ms -> Q39/Q40: the +15 V pulse (resets both VCOs, charges the bend
//                capacitor, mutes the drum VCAs through Q46/Q49) and Q41: the
//                accent line (buffer IC34b, x3.128) gated by the trigger.
//   Node A     TUNE VR6 (10k B) wiper, 1.25..2.50 V (R287 100k, R284 10k),
//              plus the bend: C66 0.01u charged by the pulse through D56 +
//              R247 47k, relaxing through R248 470k + R249 100k. IC35a/b make
//              VDD = +A, VSS = -A for the 4069UB buffers of both VCOs.
//   VCO-1/2    integrators C69 0.01u / C71 0.0068u with R261 / R264 47k,
//              comparators IC37a / IC38a (V_sat/2), reset by Q44 / Q45 through
//              10k (dsp/TriangleVco.h). Diode rounding D65/D66 (R283 10k,
//              R288 1.5k + R289 47) and D69/D70 (R305 10k, R304 2.2k + R303 47).
//   Drum VCAs  ENV3: C83 0.1u charged by the gated accent through D68,
//              R291 220k to C70 4.7n, R290 22k to Q50's base (tau 24.2 ms).
//              ENV2: C82 0.047u through D67, R295 220k to C79 0.01u, R293
//              220k to Q51's base (tau 20.7 ms). Q46 / Q49 hold the C70 / C79
//              nodes at ground during the trigger, so the tone starts at its
//              end, both oscillators in phase.
//   Snappy     noise -> R286 10k / R285 3.3k -> C85 0.015u (R302 220k) ->
//              unity Sallen-Key low-pass IC40a (R301 = R300 = 22k, C81 3.3n,
//              C84 220p). Solved as one network with its source: gain 0.242,
//              high-pass 47.0 Hz, low-pass 8110 Hz Q 1.767 (peak 7.44 kHz).
//              (a) C80 0.022u / R299 4.7k + R298 47: high-pass 1.52 kHz ->
//              Q48 on ENV4: C67 0.47u charged from the accent through Q42/D60
//              for as long as C68 0.0068u / R256 1M keeps Q42 on (~24 ms),
//              then discharging through R254 100k + VR7 TONE (500k B) toward
//              IC34a's -0.1 x accent, and through R297 220k into the base:
//              the VCA cuts off when C67 reaches the threshold.
//              (b) unity Sallen-Key high-pass C75 = C76 2.2n, R276 6.8k,
//              R277 47k (4.05 kHz, Q 1.31) -> Q47 on ENV5: the gated accent
//              through D64 into C73 470p, R280 220k to the base (0.1 ms after
//              the gate: a 2 ms accent-scaled crack).
//              Q47 + Q48 -> IC39b, feedback SNAPPY VR9 (10k B): gain
//              proportional to the knob; C78 0.47u / R294 47k into the mixer.
//   Output     IC40b sums the drum VCAs and the snappy; LEVEL VR8 50k (B).
//
// Schematic values: everything above. FITTED (and to what; struct
// SnareDrumFit holds them):
//  * VCO scale: f1 = K (A + V0), K = 90.64 Hz/V, V0 = 0.045 V, to the
//    measured settled f1 = 117.4 / 173.5 / 230.7 Hz at TUNE 0 / 50 / 100
//    (fact-checked spectral peaks; the parts give 98.5 / 148 / 197 Hz,
//    -15 %). The triangle peak follows from K (5.87 V, derived 6.75 V).
//  * VCO-2 runs at exactly 3:2 of VCO-1 once settled (measured 1.4998..1.5017
//    at every TUNE and TONE; C69/C71 = 1.471). The lock is modelled, not its
//    mechanism (UNVERIFIED, research/01 §4.2): VCO-2's slope is 1.5 x
//    VCO-1's times (1 + 0.055 e^(-t / 30 ms)), fitted to the reference
//    unit's sliding 30 ms window ratio at TUNE 25..100 (TONE 0, SNAPPY 0;
//    windows from 10 / 20 / 30 / 50 ms: measured 1.58 / 1.57 / 1.53 / 1.50 on
//    average, model 1.55 / 1.53 / 1.50 / 1.51; it was 1.52 / 1.50 / 1.49 /
//    1.49 with the earlier 0.05, 12 ms), within what the 6-20 ms zero-crossing
//    cycle frequencies of the two summed oscillators allow (larger or longer
//    excesses move them 10 % off the recordings). The measured ratio holds
//    near 1.58 to ~40 ms and then drops faster than one exponential can;
//    about a third of its excess is VCO-1 dipping 2-3 % under its settled
//    pitch at 15-40 ms, which is not modelled (the bend relaxes
//    monotonically).
//  * Bend: 2.38 V on node A at the end of the trigger, relaxing in 5.1 ms,
//    to the measured +216 Hz on VCO-1 at onset at every TUNE and tau
//    4.8..5.5 ms (derived: 2.0 V, 5.7 ms). With the reference's metric the
//    0-6 / 6-12 / 12-20 / 20-35 ms cycle frequencies agree within 2-9 %.
//  * vco2Trim 0.912: VCO-2 -9.2 dB re VCO-1 (measured -8.9..-9.6, derived
//    -8.4); env3Scale 0.918 (x C83) and env2Scale 0.94 (x C82): VCO-1's and
//    VCO-2's envelope taus 25.1 / 24.3 ms (measured 25.0..25.9 / 21.5..24.8
//    ms with the same metric; derived 24.2 / 20.7 ms, which the cells'
//    saturation lengthens).
//  * drumSat 4.0 / snappySat 2.32 (base drive re 100k): the VCA cells'
//    saturation, to the TOTAL ACCENT span (+4.8 dB, research/05 §0.11;
//    model +5.0, and +3.9 at 50 % where the recordings read +3.3); the
//    snappy cell saturates hard, as reported for Q48 (research/01 §4.5).
//    A per-voice accent (code 40) gives 1.50x Vpp (+3.5 dB). The service
//    notes' drawing reads 2.1-2.2x (+6.4..6.9 dB, +-2 dB) with every knob
//    centred; no code can give that on a unit like the reference one, whose
//    whole TOTAL ACCENT span is +4.8 dB (the model's code 63: 1.81x): the
//    drawing's ratio is taken as the upper end of its tolerance.
//  * ENV4: env4Scale 1.12 (x C67), r254Scale 0.69 (R254 69k), 10 MOhm from
//    C67 and from the hold timer C68 to ground, and an end voltage of ~0 V instead of -0.1 x accent
//    (env4EndScale 0), to the measured TONE cut-off times 99 / 178 / 225 /
//    254 / 289 ms (3-12 kHz band, first -40 dB crossing; model 98 / 175 /
//    225 / 262 / 292 ms). ENV4 ends at a threshold crossing (its discharge
//    path goes to ground), so the 10 MOhm only trims the times; it is the
//    leak all envelope nodes had when this was fitted (removed from the
//    base-only nodes, whose tails it bent: dsp/TransistorVca.h).
//  * snappyGain 0.87: the snappy amplifier re the drum path, to the
//    reference unit's noise/drum band energies at SNAPPY 50 / 100 (-9.4 /
//    -2.7 dB); crackGain 0.25: path (b) re (a), to the measured crack in the
//    first 1.5 ms (-8 dB re the hit's Vpp at SNAPPY 100).
//  * outputGain 29.1: 1.54 Vpp (mean over hits, below 16 kHz) for a normal
//    hit with all knobs centred: the hand clap's 2 Vpp trimmer target and
//    the reference unit's SD/CP ratio (-2.28 dB, equal part volume). The service notes'
//    drawing reads ~1.0 Vpp +-30 % (0.7..1.3): the model is 1.54x, outside
//    it; the recordings were preferred (see BassDrum.h, outputGain).
//    The level is calibrated and checked below 16 kHz (the late units'
//    multi-out RC is a 15.9 kHz pole): the snappy noise reaches the host
//    Nyquist, so the raw Vpp grows with the host rate (+2..4 % at 192 kHz
//    against 48 kHz) while the in-band level does not (within 1 %).
//  * Waveform asymmetry is not reproduced: the recordings are negative-peak
//    dominant (+/- 0.78 at knobs centred), the model is near-symmetric
//    (0.98); its mechanism is not identified.
//
// Not modelled: the leaky Q47/Q48 and C73 residue of some units (single-unit
// reports), Q43's function on the ENV5 node (UNVERIFIED), the D59 clamp
// detail (C67 is held at >= -0.6 V), the op-amps' slew, the multi-out RC.
//
// Extended controls (Voices.h): Decay scales the drum envelopes and ENV4's
// discharge (x0.25..x4, 0.5 stock); Tune Depth scales the bend (0.5 stock).

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

// The fitted constants of SnareDrum (see its header comment).
struct SnareDrumFit {
    double hzPerVolt = 90.64;         // f1 = K (A + V0)
    double offsetVolts = 0.045;       // V0
    double lockExcess = 0.055;        // VCO-2 / (1.5 VCO-1) - 1 at the tone's start
    double lockSeconds = 0.030;
    double bendVolts = 2.38;          // excess on node A at the end of the trigger
    double bendSeconds = 5.1e-3;
    double vco2Trim = 0.912;          // x the derived VCO-2 level (-0.8 dB)
    double env3Scale = 0.918;         // x C83 (VCO-1 VCA time constant)
    double env2Scale = 0.94;          // x C82 (VCO-2)
    double drumSat = 4.0;             // Q50 / Q51 saturation (volts of base drive)
    double snappySat = 2.32;          // Q47 / Q48
    double snappyGain = 0.87;         // IC39b at SNAPPY full, re the drum path
    double env4Scale = 1.12;          // x C67
    double env4EndScale = 0.0;        // x IC34a's -0.1 x accent
    double r254Scale = 0.69;          // x R254 (the TONE path's fixed resistor)
    double env4LeakOhms = 10.0e6;     // C67 and C68 to ground, part of the ENV4 fit
    double crackGain = 0.25;          // path (b) re path (a)
    double outputGain = 29.1;
};

class SnareDrum {
public:
    static constexpr bool kFreeRunning = false;
    using Fit = SnareDrumFit;
    static constexpr Fit kFit{};
    void setFit(const Fit& f) noexcept { fit_ = f; }  // calibration tooling only; prepare() after
    const Fit& fit() const noexcept { return fit_; }

    // ---- schematic values -------------------------------------------------
    static constexpr double kTriggerSeconds = 2.0e-3;
    static constexpr double kC69 = 0.01e-6, kC71 = 0.0068e-6, kRint = 47.0e3, kRreset = 10.0e3;
    static constexpr double kR283 = 10.0e3, kR288 = 1.5e3 + 47.0, kR305 = 10.0e3, kR304 = 2.2e3 + 47.0, kRe = 47.0;
    static constexpr double kDiodeIs = 2.5e-9, kDiodeNvt = 1.75 * 0.02585;
    static constexpr double kC83 = 0.1e-6, kR291 = 220.0e3, kC70 = 4.7e-9, kR290 = 22.0e3;
    static constexpr double kC82 = 0.047e-6, kR295 = 220.0e3, kC79 = 0.01e-6, kR293 = 220.0e3;
    static constexpr double kChargeOhms = 1.0e3;  // Q41 / Q42 into the envelope caps (not drawn; fast)
    static constexpr double kSnapGain = 0.2419, kSnapHpHz = 47.03, kSnapLpHz = 8110.0, kSnapLpQ = 1.767;
    static constexpr double kPathAHpHz = 1524.0, kPathAEmitter = 47.0 / 4747.0;
    static constexpr double kPathBHpHz = 4047.0, kPathBQ = 1.315, kPathBEmitter = 47.0 / 2247.0;
    static constexpr double kC67 = 0.47e-6, kR254 = 100.0e3, kVR7 = 500.0e3, kR297 = 220.0e3;
    static constexpr double kC68 = 0.0068e-6, kR256 = 1.0e6;
    static constexpr double kC73 = 470.0e-12, kR280 = 220.0e3;
    static constexpr double kMixHpHz = 7.2;  // C78 / R294
    static constexpr double kPulseRailVolts = 14.4;
    static constexpr double kNoiseVolts = 2.435;  // see BassDrum.h

    void prepare(const VoiceSetup& raw) noexcept {
        const VoiceSetup s = dsp::saneSetup(raw);
        fs_ = s.sampleRate;
        dt_ = 1.0 / fs_;
        auto part = [&s](std::uint32_t id, double nominal, float tol) {
            return nominal * static_cast<double>(dsp::componentScale(s.unitSeed, kPartBase + id, tol, s.variance));
        };
        k1_ = fit_.hzPerVolt * part(0, 1.0, 0.03f);
        bendTau_ = fit_.bendSeconds * part(1, 1.0, 0.10f);
        c83_ = part(2, kC83 * fit_.env3Scale, 0.10f);
        c82_ = part(3, kC82 * fit_.env2Scale, 0.10f);
        c67_ = part(4, kC67 * fit_.env4Scale, 0.10f);
        vco1_.prepare(kRreset * kC69 * fs_);
        vco2_.prepare(kRreset * kC71 * fs_);
        const double vth = 1.0 / (4.0 * fit_.hzPerVolt * kRint * kC69);  // the triangle peak, volts
        clip1_.set(1.0 / kR283, 1.0 / kR288, kDiodeIs, kDiodeNvt);
        clip2_.set(1.0 / kR305, 1.0 / kR304, kDiodeIs, kDiodeNvt);
        clip1_.buildTable(1.05 * vth);
        clip2_.buildTable(1.05 * vth);
        n70_.setCap(kC70);
        n79_.setCap(kC79);
        n68_.setCap(kC68);
        n73_.setCap(kC73);
        n67_.setLeak(fit_.env4LeakOhms);
        n68_.setLeak(fit_.env4LeakOhms);  // C68, ENV4's hold timer
        snapHp_.setCutoff(kSnapHpHz, fs_);
        snapLp_.setFreqQ(kSnapLpHz, kSnapLpQ, fs_);
        pathA_.setCutoff(kPathAHpHz, fs_);
        pathB_.setFreqQ(kPathBHpHz, kPathBQ, fs_);
        mixHp_.setCutoff(kMixHpHz, fs_);
        snappy_.setTime(0.005, fs_);
        bendDecay_ = std::exp(-dt_ / bendTau_);
        lockDecay_ = std::exp(-dt_ / fit_.lockSeconds);
        haveParams_ = false;
        setParams(params_);
        reset();
    }

    void reset() noexcept {
        vco1_.reset();
        vco2_.reset();
        clip1_.reset();
        clip2_.reset();
        n83_.reset();
        n70_.reset();
        n82_.reset();
        n79_.reset();
        n67_.reset();
        n68_.reset();
        n73_.reset();
        snapHp_.reset();
        snapLp_.reset();
        pathA_.reset();
        pathB_.reset();
        mixHp_.reset();
        snappy_.snap(snappyTarget_);
        bend_ = 0.0;
        lock_ = 0.0;
        vbufHeld_ = 0.0;
        pulseLeft_ = 0.0;
        hold4_ = false;
        pending_ = false;
        active_ = false;
    }

    void setParams(const VoiceParams& raw) noexcept {
        const VoiceParams p = dsp::saneParams(raw, SD);
        if (haveParams_ && p.k == params_.k) return;
        params_ = p;
        haveParams_ = true;
        using namespace knob;
        wiper_ = 15.0 * (10.0e3 + 10.0e3 * static_cast<double>(p.k[SdTune])) / 120.0e3;
        tone_ = kR254 * fit_.r254Scale + kVR7 * static_cast<double>(p.k[SdTone]);
        gTone_ = 1.0 / tone_;
        snappyTarget_ = static_cast<float>(fit_.snappyGain * static_cast<double>(p.k[SdSnappy]));
        const double decay = dsp::timeKnobScale(p.k[SdDecay]);
        n83_.setCap(c83_ * decay);
        n82_.setCap(c82_ * decay);
        n67_.setCap(c67_ * decay);
        depth_ = dsp::amountKnobScale(p.k[SdDepth]);
        snappy_.setTarget(snappyTarget_);
        if (!active_) snappy_.snap(snappyTarget_);
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
    static constexpr std::uint32_t kPartBase = 0xB100U;
    static constexpr double kSleepVolts = 1.0e-7;

    double tick(double noise) noexcept {
        // ---- trigger --------------------------------------------------------
        bool pulse = false;
        if (pending_) {
            pending_ = false;
            pulseLeft_ = kTriggerSeconds;
            vco1_.hold();
            vco2_.hold();
            hold4_ = true;
            pitch_ = dsp::semitonesToRatio(semis_);
        }
        if (pulseLeft_ > 0.0) {
            pulse = true;
            if (pulseLeft_ <= dt_) {
                const double frac = pulseLeft_ / dt_;
                vco1_.release(frac, 1);
                vco2_.release(frac, 1);
                pulseLeft_ = 0.0;
            } else {
                pulseLeft_ -= dt_;
            }
        }
        // The accent line is read while the trigger (and ENV4's hold) is open.
        // The buffer's output and the gated charge are functions of the line
        // alone, which holds still between hits (and DAC rewrites): worked out
        // again only when it moves, not on every sample of the reading.
        const bool reading = pulse || hold4_;
        if (reading) {
            const std::uint32_t key = std::bit_cast<std::uint32_t>(acc_);
            if (!accentRead_ || key != accentKey_) {
                accentRead_ = true;
                accentKey_ = key;
                vbufRead_ = dsp::accentBufferVolts(static_cast<double>(acc_));
                gatedRead_ = dsp::chargeThroughJunctions(vbufRead_ - 0.1, dsp::analog::kDiodeVolts);
            }
            vbufHeld_ = vbufRead_;
        }
        const double vbuf = vbufHeld_;
        const double gated = reading ? gatedRead_ : 0.0;

        // ---- node A, VCOs --------------------------------------------------
        // The bend capacitor is fully charged within the trigger (tau 0.43 ms)
        // and relaxes from its end; the lock settles from the tone's start.
        if (pulse) {
            bend_ = fit_.bendVolts;
            lock_ = fit_.lockExcess;
        } else {
            bend_ *= bendDecay_;
            lock_ *= lockDecay_;
        }
        const double nodeA = wiper_ + depth_ * bend_;
        const double f1 = k1_ * pitch_ * (nodeA + fit_.offsetVolts);
        const double s1 = 4.0 * f1 * dt_;
        const double s2 = 1.5 * s1 * (1.0 + lock_);
        const double vth = 1.0 / (4.0 * fit_.hzPerVolt * kRint * kC69);  // the triangle peak, volts
        const double u1 = clip1_.processTable(vth * vco1_.tick(s1, s1));
        const double u2 = clip2_.processTable(vth * vco2_.tick(s2, s2));

        // ---- drum envelopes ----------------------------------------------------
        n83_.begin();
        if (pulse) n83_.chargeFrom(kChargeOhms, gated);
        n83_.toNode(kR291, n70_.v());
        n83_.tail(n70_.drive());  // the pair drains only into Q50's base (tail termination)
        n70_.begin();
        n70_.toNode(kR291, n83_.v());
        n70_.toBase(kR290);
        dsp::EnvNode::step2(n83_, n70_, dt_);
        if (pulse) n70_.reset();  // Q46 holds the node during the trigger
        n82_.begin();
        if (pulse) n82_.chargeFrom(kChargeOhms, gated);
        n82_.toNode(kR295, n79_.v());
        n82_.tail(n79_.drive());
        n79_.begin();
        n79_.toNode(kR295, n82_.v());
        n79_.toBase(kR293);
        dsp::EnvNode::step2(n82_, n79_, dt_);
        if (pulse) n79_.reset();  // Q49

        // ---- snappy envelopes ------------------------------------------------
        if (hold4_) {  // C68 matters only for the hold; the next pulse recharges it
            n68_.begin();
            if (pulse) n68_.chargeFrom(kChargeOhms, kPulseRailVolts);
            n68_.toBase(kR256);
            n68_.step(dt_);
            if (!pulse && n68_.v() < dsp::analog::kVbe + 0.02) {
                hold4_ = false;
                n68_.reset();
            }
        }
        n67_.begin();
        if (hold4_) n67_.chargeFrom(kChargeOhms, gated);
        n67_.toConductance(gTone_, -0.1 * vbuf * fit_.env4EndScale);
        n67_.toBase(kR297);
        n67_.step(dt_);
        if (n67_.v() < -dsp::analog::kDiodeVolts) n67_.reset(-dsp::analog::kDiodeVolts);  // D59
        if (pulse || n73_.v() > 0.2) {
            n73_.begin();
            if (pulse) n73_.chargeFrom(kChargeOhms, gated);
            n73_.toBase(kR280);
            n73_.step(dt_);
        } else if (n73_.v() != 0.0) {
            n73_.reset();
        }

        // ---- snappy audio -------------------------------------------------------
        // The snappy filters reach the output only through Q47 / Q48; while
        // both are shut (< -140 dB) they are not computed.
        double va = 0.0, vb = 0.0;
        if (pulse || hold4_ || n67_.v() > 0.2 || n73_.v() > 0.2) {
            snapLp_.tick(snapHp_.highpass(kSnapGain * kNoiseVolts * noise));
            const double lp = snapLp_.lowpass();
            va = pathA_.highpass(lp) * kPathAEmitter;
            pathB_.tick(lp);
            vb = pathB_.highpass() * kPathBEmitter;
        }
        const double g48 = dsp::vcaGain(n67_.drive() * (dsp::kBaseRefOhms / kR297), fit_.snappySat);
        const double g47 = dsp::vcaGain(n73_.drive() * (dsp::kBaseRefOhms / kR280), fit_.snappySat);
        const double snap = mixHp_.highpass(static_cast<double>(snappy_.tick()) * (g48 * va + fit_.crackGain * g47 * vb));

        // ---- drum VCAs and output ---------------------------------------------
        const double g50 = pulse ? 0.0 : dsp::vcaGain(n70_.drive() * (dsp::kBaseRefOhms / kR290), fit_.drumSat);
        const double g51 = pulse ? 0.0 : dsp::vcaGain(n79_.drive() * (dsp::kBaseRefOhms / kR293), fit_.drumSat);
        const double drum = g50 * u1 * (kRe / kR288) + fit_.vco2Trim * g51 * u2 * (kRe / kR304);
        const double y = -fit_.outputGain * (drum + snap);

        // ---- sleep ------------------------------------------------------------------
        if (!pulse && !hold4_) {
            const double bound = fit_.outputGain * ((g50 + g51) * 0.03 + (g48 + g47) * 0.02 * (1.0 + fit_.snappyGain));
            if (bound < kSleepVolts && std::abs(y) < kSleepVolts && std::abs(n83_.v()) < 0.3 &&
                std::abs(n82_.v()) < 0.3) {
                reset();
                return 0.0;
            }
        }
        return y;
    }

    Fit fit_ = kFit;
    double fs_ = 48000.0, dt_ = 1.0 / 48000.0;
    VoiceParams params_ = defaultParams(SD);
    bool haveParams_ = false;
    double k1_ = 90.64, bendTau_ = 5.1e-3, c83_ = kC83, c82_ = kC82, c67_ = kC67;
    double wiper_ = 1.875, tone_ = 350.0e3, gTone_ = 1.0 / 350.0e3, depth_ = 1.0;
    double bendDecay_ = 0.0, lockDecay_ = 0.0;
    float snappyTarget_ = 0.5f;

    dsp::TriangleVco vco1_, vco2_;
    dsp::DiodePairClipper clip1_, clip2_;
    dsp::EnvNode n83_, n70_, n82_, n79_, n67_, n68_, n73_;
    dsp::OnePole snapHp_;
    dsp::Svf snapLp_;
    dsp::OnePole pathA_;
    dsp::Svf pathB_;
    dsp::OnePole mixHp_;
    dsp::Smoother snappy_;

    float acc_ = AccentDac::volts(AccentDac::kNormalCode);
    std::uint32_t accentKey_ = 0;   // the accent line vbufRead_ / gatedRead_ were worked out for
    bool accentRead_ = false;
    double vbufRead_ = 0.0, gatedRead_ = 0.0;
    double semis_ = 0.0, pitch_ = 1.0;
    double bend_ = 0.0, lock_ = 0.0, vbufHeld_ = 0.0;
    double pulseLeft_ = 0.0;
    bool hold4_ = false, pending_ = false, active_ = false;
};

}  // namespace mw909::voices
