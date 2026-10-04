// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// 4-band parametric EQ effect (stereo). Low band: 12 dB/oct low cut with
// resonance (out at 20 Hz) or a low shelf (+-24 dB, resonance shapes the
// shelf's corner, 0 = Butterworth). Two bells (0.5..4 octaves, +-24 dB). High
// band: 12 dB/oct high cut (out at 20 kHz) or a high shelf. Every section is a
// bilinear, pre-warped SVF (fx/FxFilters.h); switching cut <-> shelf
// crossfades the two shapes, sections out of the path cost nothing.

#pragma once

#include <array>

#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxGate.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class ParamEq : public StereoGate<ParamEq> {
public:
    static constexpr double kShelfQMax = 2.0;

    void prepare(double sampleRate, int /*maxBlockSize*/ = 0) noexcept {
        gatePrepare(sampleRate);
        lowCut_.setKind(EqKind::HighPass);
        lowShelf_.setKind(EqKind::LowShelf);
        highCut_.setKind(EqKind::LowPass);
        highShelf_.setKind(EqKind::HighShelf);
        for (auto& b : bell_) b.setKind(EqKind::Bell);
        forEach([&](EqFilter<2>& f) { f.prepare(sampleRate, ctrl_); });
        reset();
        setParams(ParamEqParams{});
    }

    void reset() noexcept {
        gateReset();
        last_.valid = false;
        forEach([](EqFilter<2>& f) { f.clear(); });
        forEach([](EqFilter<2>& f) { f.snap(); });   // silence: no glides pending
    }

    void setParams(const ParamEqParams& p) noexcept {
        if (same(last_, p)) return;   // unchanged: nothing to do (cheap per-block calls)
        const bool wasBypassed = !anyRunning() && bypassLive();
        const double lowHz = sanitize(p.lowHz, 20.0f, 20.0f, 2000.0f);
        const double lowDb = sanitize(p.lowGainDb, 0.0f, -24.0f, 24.0f);
        const double lowRes = sanitize(p.lowRes, 0.0f, 0.0f, 1.0f);
        const double highHz = sanitize(p.highHz, 20000.0f, 500.0f, 20000.0f);
        const double highDb = sanitize(p.highGainDb, 0.0f, -24.0f, 24.0f);
        const double highRes = sanitize(p.highRes, 0.0f, 0.0f, 1.0f);
        lowCut_.setTarget(!p.lowShelf && lowHz > 20.0, lowHz, resonanceToQ(lowRes), 0.0);
        lowShelf_.setTarget(p.lowShelf && lowDb != 0.0, lowHz, resonanceToQ(lowRes, kShelfQMax), lowDb);
        highCut_.setTarget(!p.highShelf && highHz < 20000.0, highHz, resonanceToQ(highRes), 0.0);
        highShelf_.setTarget(p.highShelf && highDb != 0.0, highHz, resonanceToQ(highRes, kShelfQMax), highDb);
        for (std::size_t b = 0; b < 2; ++b) {
            const auto& e = p.mid[b];
            const double f = sanitize(e.freqHz, b == 0 ? 400.0f : 2500.0f, 20.0f, 20000.0f);
            const double q = octavesToQ(sanitize(e.widthOct, 1.0f, 0.5f, 4.0f));
            const double g = sanitize(e.gainDb, 0.0f, -24.0f, 24.0f);
            bell_[b].setTarget(e.on, f, q, g);
        }
        if (fresh_) {
            forEach([](EqFilter<2>& f) { f.snap(); });
        } else if (!awake()) {
            // Asleep in silence: jump. Asleep in bypass with signal passing
            // (every band out): bands switched on fade in from dry.
            if (wasBypassed) forEach([](EqFilter<2>& f) { f.snapParams(); });
            else forEach([](EqFilter<2>& f) { f.snap(); });
        }
        fresh_ = false;
    }

    // Every band out: the input passes untouched (once asleep, at no cost).
    void process(float* l, float* r, int n) noexcept { gateProcess(l, r, n); }
    bool isActive() const noexcept { return awake(); }
    double tailSeconds() const noexcept { return 0.05; }

    // --- StereoGate hooks ---------------------------------------------------
    bool passThrough() const noexcept { return !anyRunning(); }
    void control() noexcept {
        forEach([](EqFilter<2>& f) { f.control(); });
        if (!anyRunning()) {   // the last band finished fading out: bypass from this tick
            forEach([](EqFilter<2>& f) { f.clear(); });
            gateSleep();
        }
    }
    void run(float* l, float* r, int n) noexcept {
        lowCut_.processStereo(l, r, n);
        lowShelf_.processStereo(l, r, n);
        bell_[0].processStereo(l, r, n);
        bell_[1].processStereo(l, r, n);
        highShelf_.processStereo(l, r, n);
        highCut_.processStereo(l, r, n);
    }
    bool quiet() const noexcept {
        double m = 0.0;
        bool settled = true;
        forEachC([&](const EqFilter<2>& f) {
            m += f.stateMagnitude();
            settled = settled && f.settled();
        });
        return m < kSleepFloor && settled;
    }
    void sleepNow() noexcept {
        forEach([](EqFilter<2>& f) {
            f.clear();
            f.snap();
        });
    }
    void onWake() noexcept {}

private:
    bool anyRunning() const noexcept {
        bool any = false;
        forEachC([&](const EqFilter<2>& f) { any = any || f.running(); });
        return any;
    }
    template <typename F>
    void forEach(F&& fn) {
        fn(lowCut_);
        fn(lowShelf_);
        fn(bell_[0]);
        fn(bell_[1]);
        fn(highCut_);
        fn(highShelf_);
    }
    template <typename F>
    void forEachC(F&& fn) const {
        fn(lowCut_);
        fn(lowShelf_);
        fn(bell_[0]);
        fn(bell_[1]);
        fn(highCut_);
        fn(highShelf_);
    }

    bool fresh_ = true;
    EqFilter<2> lowCut_, lowShelf_, highCut_, highShelf_;
    std::array<EqFilter<2>, 2> bell_{};
    LastParams<ParamEqParams> last_;
};

}  // namespace mw909::fx
