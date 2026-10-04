// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Stereo algorithmic reverb: pre-delay, early reflections and a late
// feedback-delay-network tail.
//
//   in -> pre-delay (0..250 ms) -+-> early: 8 taps per side over a span of
//                                |   5..100 ms (earlySize), 2 all-pass
//                                |   diffusers (earlyDiffusion), low-pass tone
//                                |   (earlyColor: 1 kHz .. 20 kHz)
//                                +-> late: input tone (lateColor), 4 all-pass
//                                    diffusers per side (lateDiffusion), then an
//                                    8-line FDN with a Hadamard matrix
//   wet = (1 - earlyLate) * early + earlyLate * late;  out = dry + mix (wet - dry)
//
// Late tail: line lengths 32..71 ms scaled by 0.25..1.5 (lateSize). Every
// line has an absorbent first-order shelf (Jot): gain g_i = 10^(-3 L_i / T60)
// (L_i in seconds) below the corner, so the low/mid-band RT60 equals the decay
// setting, 0.2 s .. 10 s (T60 = 0.2 * 50^decay), and g_i^(1/ratio) above it.
// lateColor moves the corner (600 Hz .. 19.2 kHz, bilinear, so the same at
// every rate) and the HF ratio (0.2 .. 1; at 1 nothing is damped).
// Unmodulated lines read at integer lengths (lossless); modulation moves four
// lines by up to +-1.2 ms with slow incommensurate sines, read with Hermite
// interpolation (it breaks up metallic modes); lengths glide on size changes. The
// matrix is orthogonal and every loop gain is below 1, so the network is
// stable for every setting (tested for 60 s at the maximum).

#pragma once

#include <array>
#include <cmath>

#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class Reverb : public StereoGate<Reverb> {
public:
    static constexpr int kLines = 8;
    static constexpr double kMaxPredelay = 0.250;
    static constexpr double kMaxSpanMs = 100.0;
    static constexpr double kModDepthSeconds = 0.0012;

    // Allocates the delay lines (non-realtime).
    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) {
        fs_ = sampleRate;
        gatePrepare(sampleRate);
        const auto ms = [&](double m) { return static_cast<int>(std::ceil(m * 0.001 * sampleRate)); };
        for (auto& d : pre_) d.allocate(ms(kMaxPredelay * 1000.0) + 4);
        for (auto& d : er_) d.allocate(ms(kMaxSpanMs) + 8);
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 2; ++k) {
                erAp_[c][k].len = ms(kErApMs[c][k]);
                erAp_[c][k].line.allocate(erAp_[c][k].len + 2);
            }
            for (int k = 0; k < 4; ++k) {
                lateAp_[c][k].len = ms(kLateApMs[k] * (c == 0 ? 1.0 : kLateApSkew[k]));
                lateAp_[c][k].line.allocate(lateAp_[c][k].len + 2);
            }
        }
        for (int i = 0; i < kLines; ++i) {
            line_[i].allocate(ms(kLineMs[i] * 1.5 + kModDepthSeconds * 1000.0) + 8);
            modInc_[i] = kModRate[i] / sampleRate;
            base_[static_cast<std::size_t>(i)] = kLineMs[i] * 0.001 * sampleRate;
        }
        const double tick = sampleRate / static_cast<double>(ctrl_);
        for (auto* s : {&erSpan_, &lateScale_, &erG_, &lateG_, &erColor_, &lateColor_, &decay_, &mod_}) {
            s->setTime(0.080, tick);
            s->setEps(1.0e-6);
        }
        predelay_.setTime(0.060, sampleRate);
        predelay_.setEps(1.0e-4);
        for (auto* s : {&el_, &mix_}) {
            s->setTime(kDefaultSmoothSeconds, sampleRate);
            s->setEps(1.0e-7);
        }
        reset();
        setParams(ReverbParams{});
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        clearState();
        snapAll();   // silence: no glides pending
    }

    void setParams(const ReverbParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        predelay_.setTarget(sanitize(p.predelayMs, 10.0f, 0.0f, 250.0f) * 0.001 * fs_);
        erSpan_.setTarget((5.0 + 95.0 * sanitize(p.earlySize, 0.5f, 0.0f, 1.0f)) * 0.001 * fs_);
        erG_.setTarget(0.72 * sanitize(p.earlyDiffusion, 0.6f, 0.0f, 1.0f));
        erColor_.setTarget(sanitize(p.earlyColor, 0.5f, 0.0f, 1.0f));
        lateScale_.setTarget(0.25 + 1.25 * sanitize(p.lateSize, 0.5f, 0.0f, 1.0f));
        lateG_.setTarget(0.75 * sanitize(p.lateDiffusion, 0.7f, 0.0f, 1.0f));
        lateColor_.setTarget(sanitize(p.lateColor, 0.5f, 0.0f, 1.0f));
        decay_.setTarget(sanitize(p.decay, 0.5f, 0.0f, 1.0f));
        mod_.setTarget(sanitize(p.modulation, 0.2f, 0.0f, 1.0f));
        el_.setTarget(sanitize(p.earlyLate, 0.5f, 0.0f, 1.0f));
        mix_.setTarget(sanitize(p.mix, 1.0f, 0.0f, 1.0f));
        if (fresh_ || !awake()) snapAll();
        fresh_ = false;
    }

    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }

    static double rt60ForDecay(double decay) noexcept { return 0.2 * std::pow(50.0, dsp::clamp(decay, 0.0, 1.0)); }

    // Time for the tail to fall 120 dB, plus the pre-delay and early span.
    double tailSeconds() const noexcept {
        return 2.0 * rt60ForDecay(decay_.target()) + (predelay_.target() + erSpan_.target()) / fs_;
    }

    // --- StereoGate hooks ---------------------------------------------------
    void control() noexcept {
        mod_.tick();
        erG_.tick();
        lateG_.tick();
        // Early span and late size ramp linearly across the tick.
        const double inv = 1.0 / static_cast<double>(ctrl_);
        const double spanEnd = erSpan_.tick();
        spanInc_ = (spanEnd - span_) * inv;
        const double scaleEnd = lateScale_.tick();
        scaleInc_ = (scaleEnd - scale_) * inv;
        ramp_ = ctrl_;
        spanTarget_ = spanEnd;
        scaleTarget_ = scaleEnd;
        moving_ = spanInc_ != 0.0 || scaleInc_ != 0.0;
        if (!moving_) cacheTaps();
        const bool colorMoved = !erColor_.settled() || !lateColor_.settled();
        erColor_.tick();
        lateColor_.tick();
        if (colorMoved || !decay_.settled() || !lateScale_.settled() || dirty_) {
            decay_.tick();
            updateLoop(scaleEnd);
            updateTone();
            dirty_ = false;
        }
    }

    void run(float* l, float* r, int n) noexcept {
        int lastLoud = -1;   // index of the last sample written above the floor
        const double modDepth = mod_.value() * kModDepthSeconds * fs_;
        const float erg = static_cast<float>(erG_.value());
        const float lg = static_cast<float>(lateG_.value());
        for (int i = 0; i < n; ++i) {
            float loud = 0.0f;
            if (ramp_ > 0) {
                if (--ramp_ == 0) {
                    span_ = spanTarget_;
                    scale_ = scaleTarget_;
                } else {
                    span_ += spanInc_;
                    scale_ += scaleInc_;
                }
            }
            const double pd = predelay_.tick();
            const double el = el_.tick();
            const double mix = mix_.tick();
            const float inL = l[i], inR = r[i];
            pre_[0].write(inL);
            pre_[1].write(inR);
            const float pl = pd < 1.0 ? inL : pre_[0].readLinear(pd);
            const float pr = pd < 1.0 ? inR : pre_[1].readLinear(pd);

            // ---- early reflections
            er_[0].write(pl);
            er_[1].write(pr);
            float e[2] = {0.0f, 0.0f};
            if (moving_) {
                for (int k = 0; k < 8; ++k) {
                    e[0] += kTapGain[0][k] * er_[0].readLinear(1.0 + kTapPos[0][k] * span_);
                    e[1] += kTapGain[1][k] * er_[1].readLinear(1.0 + kTapPos[1][k] * span_);
                }
            } else {
                for (int c = 0; c < 2; ++c)
                    for (int k = 0; k < 8; ++k) {
                        const float a = er_[c].read(tapI_[c][k]), b = er_[c].read(tapI_[c][k] + 1);
                        e[c] += kTapGain[c][k] * (a + (b - a) * tapF_[c][k]);
                    }
            }
            for (int c = 0; c < 2; ++c) {
                e[c] = erAp_[c][0].tick(e[c], erg);
                e[c] = erAp_[c][1].tick(e[c], erg);
                erLp_[c] += erA_ * (e[c] - erLp_[c]);
                e[c] = erLp_[c];
            }

            // ---- late input: tone + diffusion
            float d[2] = {pl, pr};
            for (int c = 0; c < 2; ++c) {
                inLp_[c] += inA_ * (d[c] - inLp_[c]);
                float v = inLp_[c];
                for (int k = 0; k < 4; ++k) v = lateAp_[c][k].tick(v, lg);
                d[c] = v;
            }

            // ---- FDN
            float y[kLines];
            for (int k = 0; k < kLines; ++k) {
                const double len = base_[k] * scale_;
                float rd;
                if (kModRate[k] > 0.0 && modDepth > 0.0)
                    rd = line_[k].readHermite(len + modDepth * fastSinCycles(modPhase_[k]));
                else
                    rd = line_[k].read(moving_ ? static_cast<int>(len + 0.5) : lineI_[k]);   // lossless integer read
                y[k] = rd;
                // Absorbent first-order shelf: DC gain g, high-frequency gain g*q.
                const float v = (rd - dampS_[k]) * dampG_;
                const float lp = v + dampS_[k];
                dampS_[k] = lp + v;
                damp_[k] = gdc_[k] * (q_[k] * rd + (1.0f - q_[k]) * lp);
            }
            float outL = 0.0f, outR = 0.0f;
            for (int k = 0; k < kLines; ++k) {
                outL += kOutL[k] * y[k];
                outR += kOutR[k] * y[k];
            }
            float h[kLines];
            for (int k = 0; k < kLines; ++k) h[k] = damp_[k];
            hadamard8(h);
            for (int k = 0; k < kLines; ++k) {
                const float w = h[k] + kInGain * d[k & 1];
                line_[k].write(w);
                loud = std::max(loud, std::abs(w));
            }
            for (int k = 0; k < kLines; ++k) {   // wrapped per sample: the phase never depends on the chunking
                modPhase_[k] += modInc_[k];
                if (modPhase_[k] >= 1.0) modPhase_[k] -= 1.0;
            }

            const double wl = (1.0 - el) * kErGain * e[0] + el * kLateOut * outL;
            const double wr = (1.0 - el) * kErGain * e[1] + el * kLateOut * outR;
            l[i] = static_cast<float>(inL + mix * (wl - inL));
            r[i] = static_cast<float>(inR + mix * (wr - inR));
            loud = std::max(loud, std::max(std::abs(pl), std::abs(pr)));
            if (loud > kSleepFloor) lastLoud = i;
        }
        // Samples since the last loud sample, exact whatever the chunking.
        sinceLoud_ = lastLoud >= 0 ? n - 1 - lastLoud : sinceLoud_ + n;
    }

    bool quiet() const noexcept {
        const long need = static_cast<long>((kLineMs[kLines - 1] * 1.5 + kMaxSpanMs + 60.0) * 0.001 * fs_ +
                                            predelay_.value()) + 8;
        return sinceLoud_ > need && predelay_.settled();
    }
    void sleepNow() noexcept {
        clearState();
        snapAll();
    }
    void onWake() noexcept {
        sinceLoud_ = 0;
        modPhase_ = {};
    }

private:
    // All-pass diffuser (lattice form): w = x - g z, y = g w + z.
    struct Allpass {
        DelayLine line;
        int len = 1;
        float tick(float x, float g) noexcept {
            const float z = line.read(len);
            const float w = x - g * z;
            line.write(w);
            return g * w + z;
        }
    };

    static void hadamard8(float* h) noexcept {
        for (int s = 1; s < kLines; s <<= 1)
            for (int i = 0; i < kLines; i += 2 * s)
                for (int j = i; j < i + s; ++j) {
                    const float a = h[j], b = h[j + s];
                    h[j] = a + b;
                    h[j + s] = a - b;
                }
        for (int k = 0; k < kLines; ++k) h[k] *= 0.35355339059327373f;   // 1/sqrt(8)
    }

    // Tap and line positions while the sizes are not gliding.
    void cacheTaps() noexcept {
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 8; ++k) {
                const double pos = 1.0 + kTapPos[c][k] * span_;
                tapI_[c][k] = static_cast<int>(pos);
                tapF_[c][k] = static_cast<float>(pos - static_cast<double>(tapI_[c][k]));
            }
        for (int k = 0; k < kLines; ++k) lineI_[k] = static_cast<int>(base_[k] * scale_ + 0.5);
    }

    void updateLoop(double scale) noexcept {
        const double t60 = rt60ForDecay(decay_.value());
        const double c = lateColor_.value();
        const double ratio = 0.2 + 0.8 * c * c;              // HF decay time / RT60
        const double corner = 600.0 * std::exp2(5.0 * c);    // 600 Hz .. 19.2 kHz
        const double g = dsp::prewarp(std::min(corner, 0.45 * fs_), fs_);
        dampG_ = static_cast<float>(g / (1.0 + g));
        for (int k = 0; k < kLines; ++k) {
            const double lenS = kLineMs[k] * 0.001 * scale;   // seconds
            gdc_[k] = static_cast<float>(std::pow(10.0, -3.0 * lenS / t60));
            q_[k] = static_cast<float>(std::pow(10.0, -3.0 * lenS / t60 * (1.0 / ratio - 1.0)));
        }
    }
    void updateTone() noexcept {
        const auto onePole = [&](double hz) {
            return static_cast<float>(1.0 - std::exp(-dsp::kTwoPi * std::min(hz, 0.45 * fs_) / fs_));
        };
        erA_ = onePole(1000.0 * std::pow(20.0, erColor_.value()));
        inA_ = onePole(1500.0 * std::pow(13.0, lateColor_.value()));
    }
    void clearState() noexcept {
        for (auto& d : pre_) d.clear();
        for (auto& d : er_) d.clear();
        for (auto& c : erAp_)
            for (auto& a : c) a.line.clear();
        for (auto& c : lateAp_)
            for (auto& a : c) a.line.clear();
        for (auto& d : line_) d.clear();
        damp_ = {};
        dampS_ = {};
        erLp_ = {};
        inLp_ = {};
        modPhase_ = {};
        sinceLoud_ = 0;
    }
    void snapAll() noexcept {
        for (auto* s : {&erSpan_, &lateScale_, &erG_, &lateG_, &erColor_, &lateColor_, &decay_, &predelay_, &el_, &mix_, &mod_})
            s->snap();
        span_ = spanTarget_ = erSpan_.value();
        scale_ = scaleTarget_ = lateScale_.value();
        ramp_ = 0;
        moving_ = false;
        cacheTaps();
        updateLoop(scale_);
        updateTone();
        dirty_ = false;
    }

    // Line lengths (ms at size scale 1), mutually prime-ish.
    static constexpr double kLineMs[kLines] = {31.71, 37.11, 40.93, 44.71, 50.29, 56.87, 63.97, 71.31};
    static constexpr double kModRate[kLines] = {0.31, 0.0, 0.47, 0.0, 0.0, 0.73, 0.0, 0.97};
    static constexpr float kOutL[kLines] = {1, 1, -1, -1, 1, -1, 1, -1};
    static constexpr float kOutR[kLines] = {1, -1, 1, -1, -1, -1, 1, 1};
    static constexpr float kInGain = 0.5f;
    static constexpr double kLateOut = 0.25;
    static constexpr double kErGain = 0.55;
    static constexpr double kErApMs[2][2] = {{1.31, 2.83}, {1.73, 3.37}};
    static constexpr double kLateApMs[4] = {4.77, 3.59, 12.73, 9.31};
    static constexpr double kLateApSkew[4] = {1.031, 1.047, 1.023, 1.061};
    static constexpr double kTapPos[2][8] = {{0.043, 0.112, 0.191, 0.287, 0.405, 0.531, 0.702, 0.913},
                                             {0.061, 0.137, 0.222, 0.315, 0.437, 0.583, 0.744, 0.962}};
    static constexpr float kTapGain[2][8] = {{0.84f, -0.71f, 0.62f, -0.52f, 0.45f, -0.37f, 0.30f, -0.24f},
                                             {-0.80f, 0.69f, 0.60f, -0.50f, -0.43f, 0.36f, 0.29f, -0.23f}};

    double fs_ = 48000.0;
    bool fresh_ = true, dirty_ = true;
    Smoothed mod_;
    std::array<DelayLine, 2> pre_{}, er_{};
    std::array<std::array<Allpass, 2>, 2> erAp_{};
    std::array<std::array<Allpass, 4>, 2> lateAp_{};
    std::array<DelayLine, kLines> line_{};
    std::array<float, kLines> damp_{}, dampS_{}, gdc_{}, q_{};
    std::array<double, kLines> base_{};
    float dampG_ = 0.0f;
    std::array<double, kLines> modPhase_{}, modInc_{};
    std::array<float, 2> erLp_{}, inLp_{};
    float erA_ = 1.0f, inA_ = 1.0f;
    Smoothed erSpan_, lateScale_, erG_, lateG_, erColor_, lateColor_, decay_, predelay_, el_, mix_;
    double span_ = 0.0, scale_ = 1.0, spanInc_ = 0.0, scaleInc_ = 0.0, spanTarget_ = 0.0, scaleTarget_ = 1.0;
    int ramp_ = 0;
    bool moving_ = false;
    int tapI_[2][8] = {};
    float tapF_[2][8] = {};
    int lineI_[kLines] = {};
    long sinceLoud_ = 0;
    LastParams<ReverbParams> last_;
};

}  // namespace mw909::fx
