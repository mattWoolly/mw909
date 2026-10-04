// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Rim shot (RS). A block model of the original circuit, simulated in volts
// (research/01 §6, research/05 §2.5; designators from the voicing-board
// schematic):
//
//   TRIG -> C111 0.018u: the edge, through D89 and R396 22k / R395 22k, is a
//           2.2 V kick decaying in 0.8 ms (C111 x 44k) into the (+) inputs of
//           three bridged-T resonators; it also turns on Q63 (R399 22k) for
//           about a millisecond, which lets Q64 pass the accent line (buffer
//           IC48b, x3.128) into C119.
//   F1     IC48a: C113 = C112 0.01u, R394 2.2k to ground, R407 470k bridge:
//          495 Hz, Q 7.3 (dsp/BridgedT.h, the sibling 808 voice's resonator).
//   F2     IC49a: C116 = C115 0.027u, R411 2.2k, R414 330k: 219 Hz, Q 6.1.
//   F3     IC49b: C118 = C117 0.0047u, R404 2.2k, R416 470k: 1053 Hz, Q 7.3.
//          The schematic annotates 30 / 15 / 30 Vpp at the op-amp outputs
//          (rail clipping, modelled); at the fitted kick (below) the model
//          swings 10.6 / 6.7 / 11.7 Vpp and never reaches the rails.
//   Sum    R408 12k, R415 12k, R417 3.3k (12k before the service change) into
//          the anti-parallel D91/D92 to ground: a hard clipper at ~+-0.6 V,
//          which hides each band until the stronger ones decay and makes the
//          intermodulation lines (measured 723, 1288, 1975 Hz)
//          (dsp/DiodeClipper.h, the resonator outputs as a Thevenin source).
//   VCA    R423 12k / R418 680 into Q65's emitter; its base from C119 0.047u
//          (charged through R401 2.7k) through R403 1M: tau 47 ms.
//   Output IC50a, LEVEL VR19 100k (B) || C120 220p (14.5 kHz at the centre),
//          then IC50b: unity Sallen-Key high-pass C121 = C122 0.01u, R420
//          220k, R422 22k (229 Hz, Q 1.58) with gain 2.
//
// Schematic values: everything above. FITTED (and to what; struct
// RimShotFit holds them):
//  * Resonator frequencies trimmed to the measured 481 / 222 / 1019 Hz (the
//    parts give 495 / 219 / 1053 Hz, within capacitor tolerance; the Qs are
//    kept).
//  * kickVolts 0.5: the edge into the resonators, to the measured envelope
//    (3 ms r.m.s. windows: -0.3 / -1.3 / -3.6 / -8.9 / -12.4 / -17.9 /
//    -29.1 dB from 0 to 30 ms). This is a stand-in, not a derivation: the
//    parts give a 2.2 V step (R399 / Q63's base load on C111 shortens the
//    edge to ~0.26 ms but does not lower it), and 2.2 V with 0.26 ms
//    reproduces the schematic's 30 / 15 / 30 Vpp annotation, but then the
//    clipper stays saturated ~10 ms too long against the recordings (-6.6 dB
//    at 15 ms, measured -12.2). The 0.5 V kick compensates an envelope
//    mismatch whose cause (the clipper's or VCA's load, most likely) is not
//    identified.
//  * vcaSat 0.8: Q65's saturation, to the TOTAL ACCENT span (+3.9 dB,
//    research/05 §0.11); accent changes the level only (measured: the same
//    partials and decays at accent 0 and 100).
//  * outputGain 17.3: 0.98 Vpp for a normal hit: the hand clap's 2 Vpp
//    trimmer target and the reference unit's RS/CP ratio (-6.24 dB at equal
//    part volume, 11 shots).
//  * evenOrder 0 (available): the recordings are negative-peak dominant
//    (+/- 0.53) after the output high-pass, which an even-order term in the
//    VCA does not reproduce (the high-pass removes what it adds); left out.
//
// Not modelled: the trigger-out pulse stretcher (Q66/Q67, the engine's
// trigger out), op-amp slew, and the recordings' late tail and its
// low-frequency part: below 120 Hz the recordings show an excursion of
// -14 % of Vpp at ~4 ms and then a persistent ~2 % (a 59 Hz line at -39 dB
// in the 30-70 ms spectrum), and their 20 ms r.m.s. at 50 / 60 ms reads -28
// / -30 dB re the peak where the model reads -43 / -55 dB. Its likely source
// is Q65's base-current / collector feedthrough of the C119 envelope into
// IC50a and through the output high-pass (it would also account for part of
// the negative-peak dominance); it is not identified well enough to model.
//
// Extended controls (Voices.h): Tune shifts the three resonators (semitones);
// Decay scales their ring time and the VCA envelope (x0.25..x4, 0.5 stock).
// RS takes no noise (the circuit has none).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mw909/Voices.h"
#include "mw909/dsp/BridgedT.h"
#include "mw909/dsp/DiodeClipper.h"
#include "mw909/dsp/Filters.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/TransistorVca.h"
#include "mw909/dsp/VoiceControls.h"

namespace mw909::voices {

// The fitted constants of RimShot (see its header comment).
struct RimShotFit {
    double trim1 = 481.0 / 494.9;     // F1 frequency re the parts
    double trim2 = 222.0 / 218.8;     // F2
    double trim3 = 1019.0 / 1053.1;   // F3
    double kickVolts = 0.5;           // the edge into the resonators (derived 2.2 V before R399/Q63's load)
    double evenOrder = 0.0;           // the VCA's asymmetry (emitter swing ~1.2 Vt)
    double vcaSat = 0.8;              // Q65 saturation (base drive re 100k)
    double outputGain = 17.3;
};

class RimShot {
public:
    static constexpr bool kFreeRunning = false;
    using Fit = RimShotFit;
    static constexpr Fit kFit{};
    void setFit(const Fit& f) noexcept { fit_ = f; }  // calibration tooling only; prepare() after
    const Fit& fit() const noexcept { return fit_; }

    // ---- schematic values -------------------------------------------------
    static constexpr double kKickVolts = (5.0 - 0.6) * 22.0e3 / 44.0e3;  // 2.2 V
    static constexpr double kKickSeconds = 0.018e-6 * 44.0e3;             // 0.8 ms
    static constexpr double kGateSeconds = 1.0e-3;                         // Q63 on-time (~the kick)
    struct Res {
        double c, rShunt, rBridge;
    };
    static constexpr Res kRes[3] = {{0.01e-6, 2.2e3, 470.0e3}, {0.027e-6, 2.2e3, 330.0e3}, {0.0047e-6, 2.2e3, 470.0e3}};
    static constexpr double kR408 = 12.0e3, kR415 = 12.0e3, kR417Late = 3.3e3, kR417Early = 12.0e3;
    static constexpr double kRload = 12.0e3 + 680.0, kEmitter = 680.0 / 12680.0;
    static constexpr double kDiodeIs = 2.5e-9, kDiodeNvt = 1.75 * 0.02585;
    static constexpr double kC119 = 0.047e-6, kR401 = 2.7e3, kR403 = 1.0e6;
    static constexpr double kLevelLpHz = 1.0 / (dsp::kTwoPi * 50.0e3 * 220.0e-12);  // VR19 at the centre
    static constexpr double kHpHz = 228.8, kHpQ = 1.58, kHpGain = 2.0;
    static constexpr double kRailVolts = 13.5;

    void prepare(const VoiceSetup& raw) noexcept {
        const VoiceSetup s = dsp::saneSetup(raw);
        fs_ = s.sampleRate;
        dt_ = 1.0 / fs_;
        auto part = [&s](std::uint32_t id, double nominal, float tol) {
            return nominal * static_cast<double>(dsp::componentScale(s.unitSeed, kPartBase + id, tol, s.variance));
        };
        const double trims[3] = {fit_.trim1, fit_.trim2, fit_.trim3};
        for (int i = 0; i < 3; ++i) {
            const auto k = static_cast<std::size_t>(i);
            // A common scale of both capacitors moves f and keeps Q.
            const double c = kRes[k].c / trims[k] * part(static_cast<std::uint32_t>(i), 1.0, 0.05f);
            base_[k].a = 1.0 / (kRes[k].rBridge * c);
            base_[k].b = (1.0 / kRes[k].rShunt) / c;
            base_[k].d = 2.0 / (kRes[k].rBridge * c);
            bt_[k].prepare(fs_);
        }
        r417_ = s.revision == Revision::Early ? kR417Early : kR417Late;
        gIn_ = 1.0 / kR408 + 1.0 / kR415 + 1.0 / r417_;
        c119_ = part(3, kC119, 0.10f);
        clip_.set(1.0 / kR408 + 1.0 / kR415 + 1.0 / r417_, 1.0 / kRload, kDiodeIs, kDiodeNvt);
        nEnv_.setCap(c119_);
        kickDecay_ = std::exp(-dt_ / kKickSeconds);
        levelLp_.setCutoff(kLevelLpHz, fs_);
        hp_.setFreqQ(kHpHz, kHpQ, fs_);
        haveParams_ = false;
        setParams(params_);
        reset();
    }

    void reset() noexcept {
        for (auto& b : bt_) b.reset();
        clip_.reset();
        nEnv_.reset();
        levelLp_.reset();
        hp_.reset();
        kick_ = 0.0;
        gateLeft_ = 0.0;
        pending_ = false;
        active_ = false;
    }

    void setParams(const VoiceParams& raw) noexcept {
        const VoiceParams p = dsp::saneParams(raw, RS);
        if (haveParams_ && p.k == params_.k) return;
        params_ = p;
        haveParams_ = true;
        using namespace knob;
        tune_ = dsp::semitonesToRatio(static_cast<double>(p.k[RsTune]));
        decay_ = dsp::timeKnobScale(p.k[RsDecay]);
        gBase_ = 1.0 / (kR403 * decay_);
        for (int i = 0; i < 3; ++i) {
            const auto k = static_cast<std::size_t>(i);
            // Tune scales the capacitors (a, b by the ratio); Decay the ring
            // time (d) at constant frequency.
            res_[k].a = base_[k].a * tune_;
            res_[k].b = base_[k].b * tune_;
            res_[k].d = base_[k].d * tune_ / decay_;
            step_[k] = bt_[k].step(res_[k].a * res_[k].b, res_[k].d);
        }
    }

    void trigger(const Hit& hit) noexcept {
        acc_ = std::isfinite(hit.acc) ? hit.acc : 0.0f;
        pending_ = true;
        active_ = true;
    }

    // RS shares its accent DAC with the hand clap. The line is read only while
    // Q63 conducts (about a millisecond after the edge).
    void setAccLine(float volts) noexcept {
        if (std::isfinite(volts)) acc_ = volts;
    }

    void render(float* out, int n) noexcept {
        if (!active_) {
            std::fill(out, out + n, 0.0f);
            return;
        }
        for (int i = 0; i < n; ++i) {
            out[i] = static_cast<float>(tick());
            if (!active_) {
                std::fill(out + i + 1, out + n, 0.0f);
                return;
            }
        }
    }

    bool isActive() const noexcept { return active_; }
    int latencySamples() const noexcept { return 0; }

private:
    static constexpr std::uint32_t kPartBase = 0xB500U;
    static constexpr double kSleepVolts = 1.0e-7;

    struct Coef {
        double a = 0.0, b = 0.0, d = 0.0;
    };

    double tick() noexcept {
        if (pending_) {
            pending_ = false;
            // The edge recharges C111: the kick is restored, not added.
            kick_ = fit_.kickVolts;
            gateLeft_ = kGateSeconds;
        } else {
            kick_ *= kickDecay_;
        }
        const bool gate = gateLeft_ > 0.0;
        if (gate) gateLeft_ -= dt_;

        // ---- resonators -----------------------------------------------------
        double sum = 0.0;
        const double rs[3] = {kR408, kR415, r417_};
        for (int i = 0; i < 3; ++i) {
            const auto k = static_cast<std::size_t>(i);
            bt_[k].tick(step_[k], res_[k].a, res_[k].b, res_[k].b * kick_);
            bt_[k].clampOutput(kRailVolts, kick_);
            sum += (kick_ + bt_[k].u()) / rs[k];
        }
        const double gIn = gIn_;
        const double node = clip_.process(sum / gIn);

        // ---- VCA envelope ------------------------------------------------------
        nEnv_.begin();
        if (gate) {
            const double vbuf = dsp::accentBufferVolts(static_cast<double>(acc_));
            nEnv_.chargeFrom(kR401, std::max(0.0, vbuf - 0.1));
        }
        nEnv_.toBaseConductance(gBase_);
        nEnv_.step(dt_);
        const double g = dsp::vcaGain(nEnv_.drive() * (dsp::kBaseRefOhms / kR403), fit_.vcaSat);

        // ---- output ------------------------------------------------------------------
        const double w = node * (1.0 / 0.6);
        const double lvl = levelLp_.lowpass(-g * node * kEmitter * (1.0 + fit_.evenOrder * w));
        hp_.tick(lvl);
        const double y = fit_.outputGain * kHpGain * hp_.highpass();

        // ---- sleep -------------------------------------------------------------
        // An amplitude bound, not the instantaneous output (which a ringing
        // tail crosses zero with): the resonators' ringing amplitudes from
        // their energies (Cauchy-Schwarz over the three, the clipper only
        // lowers the node), through the VCA, the high-pass's peak gain (< 2)
        // and the output filters' own states.
        if (!gate && kick_ < 1.0e-6) {
            double e = 0.0;
            for (int i = 0; i < 3; ++i) {
                const auto k = static_cast<std::size_t>(i);
                const double ru = bt_[k].u(), rw = bt_[k].w();
                e += (ru * ru + res_[k].b / res_[k].a * rw * rw) / (rs[k] * rs[k]);
            }
            const double node2 = 3.0 * e / (gIn * gIn);  // >= (node amplitude)^2
            const double scale = fit_.outputGain * kHpGain;
            const double cell = 2.0 * scale * g * kEmitter;
            const double rest = scale * (2.0 * std::abs(lvl) + hp_.stateMagnitude());
            if (rest < 0.5 * kSleepVolts && cell * cell * node2 < 0.25 * kSleepVolts * kSleepVolts) {
                reset();
                return 0.0;
            }
        }
        return y;
    }

    Fit fit_ = kFit;
    double fs_ = 48000.0, dt_ = 1.0 / 48000.0;
    VoiceParams params_ = defaultParams(RS);
    bool haveParams_ = false;
    Coef base_[3], res_[3];
    dsp::BridgedT::Step step_[3];
    double r417_ = kR417Late, c119_ = kC119, tune_ = 1.0, decay_ = 1.0, kickDecay_ = 0.0;
    double gIn_ = 1.0 / kR408 + 1.0 / kR415 + 1.0 / kR417Late, gBase_ = 1.0 / kR403;   // per prepare / setParams

    dsp::BridgedT bt_[3];
    dsp::DiodePairClipper clip_;
    dsp::EnvNode nEnv_;
    dsp::OnePole levelLp_;
    dsp::Svf hp_;

    float acc_ = AccentDac::volts(AccentDac::kNormalCode);
    double kick_ = 0.0, gateLeft_ = 0.0;
    bool pending_ = false, active_ = false;
};

}  // namespace mw909::voices
