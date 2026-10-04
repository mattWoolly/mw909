// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Hand clap (CP). The original's clap is the sibling 808's multi-clap
// architecture carried over almost part for part (research/01 §7), so this is
// the sibling emulation's proven model with this machine's values, simulated
// in volts:
//
//   noise --> MFB band-pass --+--> 181 Hz HP, /40 --> OTA (BA662A) --> 1.94 kHz HP --+
//                             |        gain = exp(-kappa u): the burst generator      +--> IC28a
//                             +--> 440 Hz LP --> Q37 VCA <-- two-capacitor swell -----+
//
//  * Band-pass: R208 47k in, R209 10k to ground, R207 150k, C42 = C43 4.7n on
//    IC26b: 963 Hz, Q 2.13, centre gain -1.60 (fed by the shared noise; the
//    same bit stream as the snare's snappy).
//  * Burst path: C56 22n with R202 39k + R201 1k (181 Hz high-pass, /40) into
//    the OTA; its output current into R237 82k || C58 1n: a 1.94 kHz
//    first-order high-pass into the summing amp.
//  * Burst generator: C65 0.47u is slammed by Q40 through R223 330 on every
//    comparator firing while the gate (C62 0.027u, R229 1M) is up, and once
//    more when the gate ends; it recovers in between. The exponential
//    converter Q38 sets the OTA gain from it, so every burst jumps up and
//    falls away in dB: u is C65's recovery (0 just after a slam, 1 at rest),
//    gain = (e^(-kappa u) - e^(-kappa)) / (1 - e^(-kappa)).
//  * Reverb path: R204 1k / C57 0.47u (loaded: 440 Hz) -> R205 3.3k / R203 47
//    -> Q37, whose base is fed by C59 0.1u (charged through D52 by the gated
//    accent) -> R233 470k -> C61 0.1u -> R234 470k: it swells in and decays.
//  * IC28a sums both; LEVEL VR20 50k (B). The trimmer TM sets the OTA bias.
//
// Schematic values: everything above. FITTED (and to what; struct
// HandClapFit holds them):
//  * Burst timing: the comparator re-fires when C65's recovery reaches
//    0.31 + 1.0 x (seconds since the trigger) (it rises as the gate capacitor
//    discharges), the gate ends at 31.4 ms; to the measured onsets 0 / 9.8 /
//    21.5 / 31.4 ms (+-0.4 ms, 19 hits, research/05 §2.6; the 9.8 / 11.7 /
//    9.9 ms spacing is not what one RC would give, and the generator's
//    wiring is UNVERIFIED). kappa 14 (the exponential converter's depth over
//    C65's swing), recovery 30 ms in the gate and 0.2 s after it (derived
//    R225 1M x C65 0.47u = 0.47 s), to each burst's ~25 dB fall before the
//    next and to the tail that follows the last.
//  * The reverb envelope's time scale 0.80 x the parts (derived eigen-tau
//    18 / 123 ms; measured tail ~90 ms), reverbGain 0.855 and reverbSat 1.0,
//    to the 1 ms envelope of the 0.5-5 kHz band of the reference unit's nine
//    normal hits (accent 0 and eight shots), power-averaged: -14.9 / -24.4 /
//    -29.1 / -35.1 / -39.6 / -48.1 / -52.9 dB at 60 / 100 / 140 / 200 / 300
//    / 400 / 500 ms (model -15.7 / -24.8 / -28.2 / -32.1 / -40.1 / -48.4 /
//    -53.9). C59 and C61 drain only into Q37's base, so the tail is
//    exponential to below the recordings' floor (dsp/TransistorVca.h; the
//    earlier 10 MOhm leak collapsed it after ~250 ms).
//  * Accent enters the burst generator, not as a post-gain: the accent
//    buffer deepens the sawtooth into the exponential converter, gain =
//    (e^(-kappa u + beta (1 - u)) - e^-kappa) / (1 - e^-kappa) with beta =
//    accentPower x ln(V_acc / V_normal), accentPower 0.85: each burst's
//    peak rises by e^beta and its fall steepens, the rest level is
//    unchanged (research/01 §7.5: the accent reaches only the comparators,
//    whose levels set the swing on C65; the exact law is UNVERIFIED). With
//    the reverb's accent-charged envelope, to the reference unit's accent
//    series: bursts +5.6 dB and tail +6.4 dB r.m.s. at TOTAL ACCENT 100 %
//    (model +5.8 / +6.4; its Vpp span, 2.7 dB, is too noisy to fit).
//  * outputGain 64.1 (the TM trimmer): 2.0 Vpp at the multi-out with LEVEL
//    centred, the service notes' adjustment target, as the mean Vpp of
//    normal hits (64; the reference unit's hits vary over 6 dB in Vpp)
//    below 16 kHz. The late units' multi-out RC is a 15.9 kHz pole, and the
//    burst path (band-pass x 1.94 kHz high-pass) reaches the host Nyquist:
//    raw Vpp is 2.04 / 2.02 / 2.12 / 2.27 V at 44.1 / 48 / 96 / 192 kHz,
//    in-band 2.02 / 1.99 / 1.98 / 2.03 V. The RC itself belongs to the
//    output stage, not modelled here.
//
// Not modelled: the burst generator's comparator wiring (its behaviour is
// fitted, above), D53's clamp on C61 (it never goes negative here), the
// multi-out RC, op-amp slew.
//
// Wake: a sleeping voice is not rendered, so its band-pass (which in the
// hardware always rings on the noise) would start from zero; it is warmed up
// on noise of the same density at the trigger, so the first burst does not
// depend on whether the voice slept.
//
// Extended controls (Voices.h): Tune moves the band-pass (semitones); Decay
// scales the reverb envelope (x0.25..x4, 0.5 stock); Reverb scales the tail
// level ((2 x)^2: 0 none, 0.5 stock, 1 x4).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mw909/Voices.h"
#include "mw909/dsp/Filters.h"
#include "mw909/dsp/Math.h"
#include "mw909/dsp/Noise.h"
#include "mw909/dsp/TransistorVca.h"
#include "mw909/dsp/VoiceControls.h"

namespace mw909::voices {

// The fitted constants of HandClap (see its header comment).
struct HandClapFit {
    double kappa = 14.0;              // exponential-converter depth over the full C65 swing
    double recoverSeconds = 0.030;    // C65 recovery while the gate is up
    double freeSeconds = 0.20;        // and after it
    double refireA = 0.31;            // comparator re-fire level at the trigger (u)
    double refireB = 1.0;             // its rise per second
    double gateSeconds = 0.0314;
    double slamSeconds = 0.4e-3;
    double accentPower = 0.85;        // burst peak ~ (accent buffer / normal)^p (see beta_)
    double reverbScale = 0.80;        // x the C59 / C61 time constants
    double reverbGain = 0.855;        // re the burst path
    double reverbSat = 1.0;           // Q37 saturation (base drive re 100k)
    double outputGain = 64.1;
    double leakOhms = 0.0;            // C59 / C61 node leak (0: none)
};

class HandClap {
public:
    static constexpr bool kFreeRunning = false;
    using Fit = HandClapFit;
    static constexpr Fit kFit{};
    void setFit(const Fit& f) noexcept { fit_ = f; }  // calibration tooling only; prepare() after
    const Fit& fit() const noexcept { return fit_; }

    // ---- schematic values -------------------------------------------------
    static constexpr double kBpHz = 962.9, kBpQ = 2.133, kBpGain = 1.596;
    static constexpr double kOtaHpHz = 180.9, kOtaDiv = 1.0 / 40.0;
    static constexpr double kBurstHpHz = 1941.0;
    static constexpr double kReverbLpHz = 440.0, kReverbEmitter = 47.0 / 3347.0;
    static constexpr double kC59 = 0.1e-6, kR233 = 470.0e3, kC61 = 0.1e-6, kR234 = 470.0e3;
    static constexpr double kTriggerSeconds = 2.0e-3;
    static constexpr double kChargeOhms = 1.0e3;
    static constexpr double kNoiseVolts = 2.435;  // see BassDrum.h
    static constexpr double kWarmSeconds = 4.0e-3;  // > 5 band-pass time constants

    void prepare(const VoiceSetup& raw) noexcept {
        const VoiceSetup s = dsp::saneSetup(raw);
        fs_ = s.sampleRate;
        dt_ = 1.0 / fs_;
        auto part = [&s](std::uint32_t id, double nominal, float tol) {
            return nominal * static_cast<double>(dsp::componentScale(s.unitSeed, kPartBase + id, tol, s.variance));
        };
        bpScale_ = 1.0 / part(0, 1.0, 0.05f);
        c65Scale_ = part(1, 1.0, 0.10f);
        gateScale_ = part(2, 1.0, 0.10f);
        revScale_ = part(3, 1.0, 0.10f);
        trim_ = part(4, 1.0, 0.10f);  // the TM trimmer's setting on this unit
        slam_ = std::exp(-dt_ / (fit_.slamSeconds * 0.36));
        slamSamplesD_ = fit_.slamSeconds;
        recoverGate_ = 1.0 - std::exp(-dt_ / (fit_.recoverSeconds * c65Scale_));
        recoverFree_ = 1.0 - std::exp(-dt_ / (fit_.freeSeconds * c65Scale_));
        otaHp_.setCutoff(kOtaHpHz, fs_);
        burstHp_.setCutoff(kBurstHpHz, fs_);
        revLp_.setCutoff(kReverbLpHz, fs_);
        n59_.setCap(kC59);
        n61_.setCap(kC61);
        n59_.setLeak(fit_.leakOhms);
        n61_.setLeak(fit_.leakOhms);
        // The recordings' tail ends ~-53 dB re the bursts, where Q37's drive
        // is ~-40 dB re its peak: the termination may start at -60 dB
        // (~-73 dB re the bursts), which halves the time to sleep.
        n59_.setTailFloor(1.0e-3);
        n61_.setTailFloor(1.0e-3);
        warm_.prepare(fs_);
        warm_.seed(0xC1A90001U);  // any noise of the bus density will do; not a unit property
        warmSamples_ = static_cast<int>(kWarmSeconds * fs_);
        floor_ = std::exp(-fit_.kappa);
        norm_ = 1.0 / (1.0 - floor_);
        haveParams_ = false;
        setParams(params_);
        reset();
    }

    void reset() noexcept {
        bp_.reset();
        otaHp_.reset();
        burstHp_.reset();
        revLp_.reset();
        n59_.reset();
        n61_.reset();
        u_ = 1.0;
        beta_ = -fit_.kappa;
        sinceTrigger_ = 0.0;
        gate_ = false;
        slamLeft_ = 0.0;
        pulseLeft_ = 0.0;
        pending_ = false;
        active_ = false;
    }

    void setParams(const VoiceParams& raw) noexcept {
        const VoiceParams p = dsp::saneParams(raw, CP);
        if (haveParams_ && p.k == params_.k) return;
        params_ = p;
        haveParams_ = true;
        using namespace knob;
        const double tune = dsp::semitonesToRatio(static_cast<double>(p.k[CpTune]));
        bp_.setFreqQ(kBpHz * bpScale_ * tune, kBpQ, fs_);
        const double decay = dsp::timeKnobScale(p.k[CpDecay]) * fit_.reverbScale * revScale_;
        n59_.setCap(kC59 * decay);
        n61_.setCap(kC61 * decay);
        const double r = dsp::amountKnobScale(p.k[CpReverb]);
        reverbAmount_ = fit_.reverbGain * r * r;
    }

    void trigger(const Hit& hit) noexcept {
        acc_ = std::isfinite(hit.acc) ? hit.acc : 0.0f;
        if (!active_) wake();
        pending_ = true;
        active_ = true;
    }

    // CP shares its accent DAC with the rim shot; the line is read while the
    // trigger is open.
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
    static constexpr std::uint32_t kPartBase = 0xB600U;
    static constexpr double kSleepVolts = 1.0e-7;

    void wake() noexcept {
        for (int i = 0; i < warmSamples_; ++i) bp_.tick(kNoiseVolts * static_cast<double>(warm_.next()));
    }

    double tick(double noise) noexcept {
        if (pending_) {
            pending_ = false;
            pulseLeft_ = kTriggerSeconds;
            sinceTrigger_ = 0.0;
            gate_ = true;
            slamLeft_ = fit_.slamSeconds;
            const double vbuf = dsp::accentBufferVolts(static_cast<double>(acc_));
            const double vn = dsp::accentBufferVolts(static_cast<double>(AccentDac::volts(AccentDac::kNormalCode)));
            // The accent buffer deepens the sawtooth into the exponential
            // converter: beta adds to its swing at the slam (u = 0) and
            // nothing at rest (u = 1), so it raises each burst's peak and
            // steepens its fall (beta >= -kappa keeps the gain >= 0).
            const double ratio = std::max(vbuf, 0.0) / vn;
            beta_ = ratio > 0.0 ? std::max(-fit_.kappa, fit_.accentPower * std::log(ratio)) : -fit_.kappa;
        }
        const bool pulse = pulseLeft_ > 0.0;
        if (pulse) pulseLeft_ -= dt_;

        // ---- burst generator --------------------------------------------------
        if (slamLeft_ > 0.0) {
            slamLeft_ -= dt_;
            u_ *= slam_;
        } else if (gate_) {
            u_ += (1.0 - u_) * recoverGate_;
            if (u_ >= fit_.refireA + fit_.refireB * sinceTrigger_) slamLeft_ = fit_.slamSeconds;
        } else {
            u_ += (1.0 - u_) * recoverFree_;
        }
        sinceTrigger_ += dt_;
        if (gate_ && sinceTrigger_ >= fit_.gateSeconds * gateScale_) {
            gate_ = false;
            slamLeft_ = fit_.slamSeconds;  // the gate's falling edge: the last clap
        }
        const double gBurst = (std::exp(-fit_.kappa * u_ + beta_ * (1.0 - u_)) - floor_) * norm_;

        // ---- reverb envelope: C59 -> R233 -> C61 -> R234 -> Q37 base ----------
        n59_.begin();
        if (pulse) {
            const double vbuf = dsp::accentBufferVolts(static_cast<double>(acc_));
            n59_.chargeFrom(kChargeOhms, dsp::chargeThroughJunctions(vbuf - 0.1, dsp::analog::kDiodeVolts));
        }
        n59_.toNode(kR233, n61_.v());
        n59_.tail(n61_.drive());  // the pair drains only into Q37's base (tail termination)
        n61_.begin();
        n61_.toNode(kR233, n59_.v());
        n61_.toBase(kR234);
        dsp::EnvNode::step2(n59_, n61_, dt_);
        const double gRev = dsp::vcaGain(n61_.drive() * (dsp::kBaseRefOhms / kR234), fit_.reverbSat);

        // ---- audio --------------------------------------------------------------------
        bp_.tick(kNoiseVolts * noise);
        const double band = -kBpGain * bp_.bandpassUnity();
        const double burst = burstHp_.highpass(otaHp_.highpass(band) * kOtaDiv * gBurst);
        const double reverb = revLp_.lowpass(band) * kReverbEmitter * gRev * reverbAmount_;
        const double y = -fit_.outputGain * trim_ * (burst + reverb);

        // ---- sleep -----------------------------------------------------------------------
        if (!gate_ && !pulse && slamLeft_ <= 0.0) {
            const double bound = fit_.outputGain * trim_ * (gBurst * 0.1 + gRev * 0.01 * reverbAmount_ + 1.0e-12);
            if (bound < kSleepVolts && std::abs(y) < kSleepVolts) {
                reset();
                return 0.0;
            }
        }
        return y;
    }

    Fit fit_ = kFit;
    double fs_ = 48000.0, dt_ = 1.0 / 48000.0;
    VoiceParams params_ = defaultParams(CP);
    bool haveParams_ = false;
    double bpScale_ = 1.0, c65Scale_ = 1.0, gateScale_ = 1.0, revScale_ = 1.0, trim_ = 1.0;
    double slam_ = 0.0, slamSamplesD_ = 0.0, recoverGate_ = 0.0, recoverFree_ = 0.0;
    double floor_ = 0.0, norm_ = 1.0, reverbAmount_ = 0.05;
    int warmSamples_ = 192;

    dsp::Svf bp_;
    dsp::OnePole otaHp_, burstHp_, revLp_;
    dsp::EnvNode n59_, n61_;
    dsp::WhiteNoise warm_;

    float acc_ = AccentDac::volts(AccentDac::kNormalCode);
    double u_ = 1.0, beta_ = -14.0, sinceTrigger_ = 0.0;
    double slamLeft_ = 0.0, pulseLeft_ = 0.0;
    bool gate_ = false, pending_ = false, active_ = false;
};

}  // namespace mw909::voices
