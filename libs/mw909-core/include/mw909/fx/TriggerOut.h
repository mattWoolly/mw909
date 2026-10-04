// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The audible click of the original machine's TRIG OUT jack (research/03 §5.3),
// fired by a voice's hits. On the hardware the rim-shot trigger discharges
// C114 (0.1 uF), which holds the PNP Q67 on while it recharges through R409
// (47k, tau 4.7 ms): the collector sits at about +14.1 V (15 V less the R495
// drop and V_CE(sat)) for the 2 ms trigger plus ~12.8 ms, then Q67 leaves
// saturation and is fully off ~1.7 ms later (pulse 14.6..16.7 ms nominal;
// the manuals print 18 and 20 ms). The jack hangs off the collector through
// R413 (1k) with R412 (22k) to ground and, from S/N 415300, C600 (0.1 uF)
// across the jack:
//
//   rising edge:  C600 charges through R413:            tau = 0.1 ms
//   turn-off:     the collector follows Q67's falling current (linear ramp
//                 over 1.7 ms), then C600 drains through R413 + R412: tau = 2.3 ms
//
// Plugged into an audio input, the pulse passes that input's AC coupling
// (modelled as a first-order 10 Hz high-pass), so what is heard is a sharp
// positive click at the start, a sag across the pulse and a softer negative
// thump when it ends. The level is scaled by kOutputScale (as through a
// -12 dB pad), so the click sits near the voices' own levels: the output is in
// volts like a voice, before kDigitalPerVolt.
//
// Revision (setRevision, default Late): units before S/N 415300 have neither
// C600 nor R495 (a jumper). Without C600 the jack follows the collector
// directly (a sharp edge, and the turn-off ramp straight to 0 V), and without
// the R495 drop the high level is 15 V - V_CE(sat), about +14.75 V.
//
// Deterministic, sample-rate independent (the pulse is timed in seconds and
// every RC is discretised exactly for a held input), and a retrigger while
// the pulse is up restarts its timing without resetting any state. The click
// goes quiet at a sample position (checked per sample once the pulse is
// over), never at a block end.

#pragma once

#include <cmath>

#include "mw909/Voices.h"
#include "mw909/dsp/Math.h"
#include "mw909/fx/FxCommon.h"

namespace mw909::fx {

class TriggerClick {
public:
    static constexpr double kHighVolts = 14.1;
    static constexpr double kHighVoltsEarly = 14.75;    // no R495 drop
    static constexpr double kFullOnSeconds = 0.0148;    // 2 ms TRIG + 12.8 ms
    static constexpr double kTurnOffSeconds = 0.0017;   // Q67 out of saturation -> off
    static constexpr double kTauOn = 1.0e3 * 0.1e-6;    // R413 * C600
    static constexpr double kTauOff = 23.0e3 * 0.1e-6;  // (R413 + R412) * C600
    static constexpr double kCouplingHz = 10.0;
    static constexpr double kOutputScale = 0.25;
    static constexpr double kFloorVolts = 1.0e-5;       // -120 dBFS after kDigitalPerVolt

    void prepare(double sampleRate) noexcept {
        fs_ = sampleRate;
        dt_ = 1.0 / sampleRate;
        applyRevision();
        const double g = dsp::prewarp(kCouplingHz, sampleRate);
        hpG_ = g / (1.0 + g);
        reset();
    }

    void reset() noexcept {
        t_ = kEnd;
        jack_ = 0.0;
        hpS_ = 0.0;
        active_ = false;
    }

    // The hardware revision (Late: C600 and R495 fitted). Takes effect at once;
    // allocation-free, so it may be called on the audio thread.
    void setRevision(Revision revision) noexcept {
        early_ = revision == Revision::Early;
        applyRevision();
    }

    // The source voice fired. The pulse height does not depend on the accent.
    void trigger(const Hit& /*hit*/) noexcept {
        t_ = 0.0;
        active_ = true;
    }

    // Mono, overwrites, volts (kDigitalPerVolt not applied).
    void render(float* out, int n) noexcept {
        if (!active_) {
            for (int i = 0; i < n; ++i) out[i] = 0.0f;
            return;
        }
        for (int i = 0; i < n; ++i) {
            double src = 0.0;
            double a = aOff_;
            if (t_ < kFullOnSeconds) {
                src = high_;
                a = aOn_;
            } else if (t_ < kFullOnSeconds + kTurnOffSeconds) {
                src = high_ * (1.0 - (t_ - kFullOnSeconds) / kTurnOffSeconds);
            } else if (std::abs(jack_) < kFloorVolts && std::abs(hpS_) < kFloorVolts) {
                // The pulse is over and every state is below the floor: silent
                // from this sample on.
                reset();
                for (; i < n; ++i) out[i] = 0.0f;
                return;
            }
            jack_ += (src - jack_) * a;
            // AC coupling: TPT one-pole high-pass.
            const double v = (jack_ - hpS_) * hpG_;
            const double lp = v + hpS_;
            hpS_ = lp + v;
            out[i] = static_cast<float>((jack_ - lp) * kOutputScale);
            if (t_ < kEnd) t_ += dt_;
        }
    }

    bool isActive() const noexcept { return active_; }

private:
    static constexpr double kEnd = 1.0e9;

    void applyRevision() noexcept {
        aOn_ = early_ ? 1.0 : 1.0 - std::exp(-dt_ / kTauOn);
        aOff_ = early_ ? 1.0 : 1.0 - std::exp(-dt_ / kTauOff);
        high_ = early_ ? kHighVoltsEarly : kHighVolts;
    }

    double fs_ = 48000.0, dt_ = 1.0 / 48000.0;
    double aOn_ = 1.0, aOff_ = 1.0, hpG_ = 0.0, high_ = kHighVolts;
    bool early_ = false;
    double t_ = kEnd;
    double jack_ = 0.0, hpS_ = 0.0;
    bool active_ = false;
};

}  // namespace mw909::fx
