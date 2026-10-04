// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Stereo delay effect.
//
//  * Time: free (1..1000 ms) or tempo-synced (1/32..1/1, straight / dotted /
//    triplet, of the bpm handed to setParams). The line holds kMaxSeconds
//    (10 s: 1/1 dotted down to 36 BPM, 1/1 down to 24 BPM; it costs no more
//    memory than 6 s, the buffer being a power of two). A synced time longer
//    than that is halved until it fits, so the echo stays on the beat grid
//    (e.g. 1/1 dotted at 30 BPM, 12 s, plays as 6 s = 1/2 dotted). Time
//    changes glide (a tape-like pitch bend, ~60 ms), never jump.
//  * Fractional read with 4-point Hermite interpolation: the echo lands at
//    exactly time * fs samples (sub-sample accurate).
//  * Feedback 0..0.98 through an optional loop filter (off / 12 dB low-pass,
//    band-pass, high-pass with resonance). The filter is normalised to unity
//    peak gain, so resonance never pushes the loop over unity; a soft safety
//    clip above +3.5 dBFS keeps extreme settings bounded.
//  * Spread -1..1: 0 = two independent channels (dual mono); |spread| blends
//    toward a full ping-pong (input summed to one side, the feedback crossing
//    L <-> R every repeat). Positive spread puts the first echo on the left,
//    negative on the right. The feedback matrix [[1-a, a], [a, 1-a]] has
//    eigenvalues 1 and 1-2a, so any spread is as stable as the plain loop.
//  * out = dry + mix (echo - dry).

#pragma once

#include <array>
#include <cmath>

#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class Delay : public StereoGate<Delay> {
public:
    static constexpr double kMaxSeconds = 10.0;   // 1/1 dotted at 36 BPM

    // Allocates the delay lines (non-realtime).
    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) {
        fs_ = sampleRate;
        gatePrepare(sampleRate);
        const int maxD = static_cast<int>(std::ceil(kMaxSeconds * sampleRate)) + 8;
        for (auto& line : line_) line.allocate(maxD);
        time_.setTime(0.060, sampleRate);
        time_.setEps(1.0e-4);
        for (auto* s : {&fb_, &mix_, &spread_}) {
            s->setTime(kDefaultSmoothSeconds, sampleRate);
            s->setEps(1.0e-7);
        }
        filter_.prepare(sampleRate, ctrl_);
        reset();
        setParams(DelayParams{}, 120.0);
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        for (auto& line : line_) line.clear();
        filter_.clear();
        sinceLoud_ = 0;
        snapAll();   // silence: no glides pending
    }

    void setParams(const DelayParams& p, double bpm) noexcept {
        if (same(last_, p, bpm)) return;   // unchanged: nothing to do (cheap per-block calls)
        double seconds;
        if (p.sync) {
            const auto div = sanitizeEnum(p.div, SyncDiv::D1_8, static_cast<int>(SyncDiv::kCount));
            const auto kind = sanitizeEnum(p.kind, SyncKind::Dotted, 3);
            seconds = syncSeconds(div, kind, bpm);
            while (seconds > kMaxSeconds) seconds *= 0.5;   // too long for the line: half the division
        } else {
            seconds = sanitize(p.timeMs, 250.0f, 1.0f, 1000.0f) * 0.001;
        }
        seconds = dsp::clamp(seconds, 0.001, kMaxSeconds);
        time_.setTarget(seconds * fs_);
        fb_.setTarget(sanitize(p.feedback, 0.35f, 0.0f, 0.98f));
        spread_.setTarget(sanitize(p.spread, 0.0f, -1.0f, 1.0f));
        mix_.setTarget(sanitize(p.mix, 1.0f, 0.0f, 1.0f));
        wantType_ = sanitizeEnum(p.loopFilter, FilterType::Off, 4);
        cutoff_ = sanitize(p.cutoffHz, 3000.0f, 20.0f, 20000.0f);
        q_ = resonanceToQ(sanitize(p.reso, 0.0f, 0.0f, 1.0f));
        applyFilter();
        if (fresh_ || !awake()) snapAll();
        fresh_ = false;
    }

    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }

    // Time for the echoes to fall 120 dB (from full scale to the sleep floor).
    double tailSeconds() const noexcept {
        const double t = time_.target() / fs_;
        const double fb = fb_.target();
        if (fb < 1.0e-6) return t;
        return t * (1.0 + std::log(1.0e-6) / std::log(fb));
    }
    double timeSamples() const noexcept { return time_.value(); }

    // --- StereoGate hooks ---------------------------------------------------
    void control() noexcept {
        // Loop filter type change: fade the old type out, switch, fade in.
        if (type_ != wantType_ && filter_.fade() == 0.0 && !filter_.running()) {
            type_ = wantType_;
            filter_.clear();
            applyFilter();
        }
        filter_.control();
        norm_ = (type_ == FilterType::LowPass || type_ == FilterType::HighPass)
                    ? 1.0 / resonantPeakGain(filter_.currentQ())
                    : 1.0;
    }
    void run(float* l, float* r, int n) noexcept {
        int lastLoud = -1;   // index of the last sample written above the floor
        const bool filt = filter_.running();
        for (int i = 0; i < n; ++i) {
            const double d = time_.tick();
            const double fb = fb_.tick();
            const double mix = mix_.tick();
            const double sp = spread_.tick();
            const double a = std::abs(sp);
            const double inL = l[i], inR = r[i];
            const double dl = line_[0].readHermite(d);
            const double dr = line_[1].readHermite(d);
            double fl = dl, fr = dr;
            if (filt) {
                const double w = filter_.step();
                if (w > 0.0) {   // at 0 the filter is out of the loop (and keeps no state)
                    fl = dl + w * (filter_.tick(0, dl) * norm_ - dl);
                    fr = dr + w * (filter_.tick(1, dr) * norm_ - dr);
                }
            }
            const double m = 0.5 * (inL + inR);
            double wl, wr;
            if (sp >= 0.0) {   // input to the left, first echo left
                wl = (1.0 - a) * inL + a * m + fb * ((1.0 - a) * fl + a * fr);
                wr = (1.0 - a) * inR + fb * ((1.0 - a) * fr + a * fl);
            } else {
                wl = (1.0 - a) * inL + fb * ((1.0 - a) * fl + a * fr);
                wr = (1.0 - a) * inR + a * m + fb * ((1.0 - a) * fr + a * fl);
            }
            const float sl = kneeClip(static_cast<float>(wl), 1.5f, 3.0f);
            const float sr = kneeClip(static_cast<float>(wr), 1.5f, 3.0f);
            line_[0].write(sl);
            line_[1].write(sr);
            if (std::max(std::abs(sl), std::abs(sr)) > kSleepFloor) lastLoud = i;
            l[i] = static_cast<float>(inL + mix * (dl - inL));
            r[i] = static_cast<float>(inR + mix * (dr - inR));
        }
        // Samples since the last loud write, exact whatever the chunking.
        sinceLoud_ = lastLoud >= 0 ? n - 1 - lastLoud : sinceLoud_ + n;
    }
    bool quiet() const noexcept {
        return sinceLoud_ > static_cast<long>(time_.value()) + 8 && time_.settled() && filter_.stateMagnitude() < kSleepFloor;
    }
    void sleepNow() noexcept {
        for (auto& line : line_) line.clear();
        filter_.clear();
        snapAll();
        sinceLoud_ = 0;
    }
    void onWake() noexcept { sinceLoud_ = 0; }

private:
    void applyFilter() noexcept {
        EqKind k = EqKind::LowPass;
        if (type_ == FilterType::BandPass) k = EqKind::BandPass;
        if (type_ == FilterType::HighPass) k = EqKind::HighPass;
        filter_.setKind(k);
        // Engaged only while the running type is the wanted one.
        filter_.setTarget(type_ != FilterType::Off && type_ == wantType_, cutoff_, q_, 0.0);
    }
    void snapAll() noexcept {
        time_.snap();
        fb_.snap();
        mix_.snap();
        spread_.snap();
        type_ = wantType_;
        applyFilter();
        filter_.snap();
        norm_ = (type_ == FilterType::LowPass || type_ == FilterType::HighPass) ? 1.0 / resonantPeakGain(q_) : 1.0;
    }

    double fs_ = 48000.0;
    bool fresh_ = true;
    std::array<DelayLine, 2> line_{};
    Smoothed time_, fb_, mix_, spread_;
    EqFilter<2> filter_;
    FilterType type_ = FilterType::Off, wantType_ = FilterType::Off;
    double cutoff_ = 3000.0, q_ = 0.70710678118654752, norm_ = 1.0;
    long sinceLoud_ = 0;
    LastParams<DelayParams> last_;
};

}  // namespace mw909::fx
