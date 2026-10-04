// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "mw909/fx/MasterChain.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace mw909::fx {

void MasterChain::prepare(double sampleRate, int /*maxBlockSize*/) {
    fs_ = sampleRate;
    ctrl_ = controlInterval(sampleRate);
    for (int c = 0; c < 2; ++c) {
        modernHp_[static_cast<std::size_t>(c)].setCutoff(kModernHpfHz, sampleRate);
        origHp_[static_cast<std::size_t>(c)].setCutoff(kOriginalHpfHz, sampleRate);
        origLp_[static_cast<std::size_t>(c)].setCutoff(kOriginalLpfHz, sampleRate);
    }
    for (auto& w : stageW_) w.setTime(0.010, sampleRate);
    lpfW_.setTime(0.010, sampleRate);
    compW_.setTime(0.010, sampleRate);
    for (auto& w : bandOn_) w.setTime(0.010, sampleRate);
    limW_.setTime(0.010, sampleRate);
    clipW_.setTime(0.010, sampleRate);
    for (auto* s : {&drive_, &limThrS_, &volume_}) {
        s->setTime(kDefaultSmoothSeconds, sampleRate);
        s->setEps(1.0e-7);
    }
    for (std::size_t b = 0; b < 3; ++b) {
        bandMakeup_[b].setTime(kDefaultSmoothSeconds, sampleRate);
        bandMakeup_[b].setEps(1.0e-7);
        bandSolo_[b].setTime(0.010, sampleRate);
        bandSolo_[b].setEps(1.0e-7);
        comp_[b].prepare(sampleRate);
    }
    const double tick = sampleRate / static_cast<double>(ctrl_);
    for (auto* s : {&logLo_, &logHi_}) {
        s->setTime(0.030, tick);
        s->setEps(1.0e-5);
    }
    fresh_ = true;
    setParams(MasterParams{}, Revision::Late);
    reset();
    fresh_ = true;   // the caller's first parameters apply at once
}

void MasterChain::reset() noexcept {
    for (auto& f : modernHp_) f.reset();
    for (auto& f : origHp_) f.reset();
    for (auto& f : origLp_) f.reset();
    xLo_.clear();
    xHi_.clear();
    for (auto& c : comp_) c.clear();
    limGain_ = 1.0;
    ctrlLeft_ = 0;
    last_.valid = false;
    snapAll();
}

// Every glide and crossfade jumps to its target.
void MasterChain::snapAll() noexcept {
    for (auto& w : stageW_) w.snap();
    lpfW_.snap();
    drive_.snap();
    compW_.snap();
    logLo_.snap();
    logHi_.snap();
    for (std::size_t b = 0; b < 3; ++b) {
        bandOn_[b].snap();
        bandMakeup_[b].snap();
        bandSolo_[b].snap();
    }
    limW_.snap();
    limThrS_.snap();
    clipW_.snap();
    volume_.snap();
    xLo_.setFreq(std::exp2(logLo_.value()), fs_);
    xHi_.setFreq(std::exp2(logHi_.value()), fs_);
}

void MasterChain::setParams(const MasterParams& p, Revision revision) noexcept {
    if (same(last_, p, static_cast<double>(revision))) return;   // unchanged (per-block calls)
    const auto stage = sanitizeEnum(p.stage, OutputStage::Modern, 3);
    for (int s = 0; s < 3; ++s) stageW_[static_cast<std::size_t>(s)].setTarget(static_cast<int>(stage) == s ? 1.0 : 0.0);
    lpfW_.setTarget(revision == Revision::Late ? 1.0 : 0.0);
    drive_.setTarget(sanitize(p.drive, 0.0f, 0.0f, 1.0f) * kMaxDriveDb);

    compW_.setTarget(p.compOn ? 1.0 : 0.0);
    const double lo = sanitize(p.xoverLowHz, 200.0f, 40.0f, 1000.0f);
    double hi = sanitize(p.xoverHighHz, 3000.0f, 1000.0f, 12000.0f);
    hi = std::max(hi, lo * 1.25);   // keep the upper split above the lower one
    logLo_.setTarget(std::log2(lo));
    logHi_.setTarget(std::log2(hi));
    bool anySolo = false;
    for (const auto& b : p.band) anySolo = anySolo || b.solo;
    for (std::size_t b = 0; b < 3; ++b) {
        const auto& cb = p.band[b];
        comp_[b].setParams(sanitize(cb.thresholdDb, 0.0f, -48.0f, 0.0f), sanitize(cb.ratio, 2.0f, 1.0f, 20.0f),
                           sanitize(cb.attackMs, 10.0f, 1.0f, 500.0f), 0.0,
                           sanitize(cb.releaseMs, 150.0f, 1.0f, 500.0f), kCompKneeDb);
        bandOn_[b].setTarget(cb.on ? 1.0 : 0.0);
        bandMakeup_[b].setTarget(dsp::dbToGain(sanitize(cb.makeupDb, 0.0f, 0.0f, 24.0f)));
        bandSolo_[b].setTarget(!anySolo || cb.solo ? 1.0 : 0.0);
    }

    limW_.setTarget(p.limiterOn ? 1.0 : 0.0);
    limThrS_.setTarget(dsp::dbToGain(sanitize(p.limThresholdDb, 0.0f, -24.0f, 0.0f)));
    limAtt_ = dsp::expDecayCoeff(sanitize(p.limAttackMs, 1.0f, 0.1f, 10.0f) * 0.001, fs_);
    limRel_ = dsp::expDecayCoeff(sanitize(p.limReleaseMs, 100.0f, 10.0f, 1000.0f) * 0.001, fs_);
    clipW_.setTarget(p.softClip ? 1.0 : 0.0);
    volume_.setTarget(dsp::dbToGain(sanitize(p.volumeDb, 0.0f, -60.0f, 12.0f)));

    if (fresh_) {
        snapAll();
        fresh_ = false;
    }
}

void MasterChain::control() noexcept {
    // Keep decaying filter state out of the denormal range even where FTZ/DAZ
    // is not set (tests, tools); flushed to exact zero, it lets the chain rest.
    for (std::size_t c = 0; c < 2; ++c) {
        modernHp_[c].flushTiny();
        origHp_[c].flushTiny();
        origLp_[c].flushTiny();
    }
    xLo_.flushTiny();
    xHi_.flushTiny();
    if (!logLo_.settled()) xLo_.setFreq(std::exp2(logLo_.tick()), fs_);
    if (!logHi_.settled()) xHi_.setFreq(std::exp2(logHi_.tick()), fs_);
}

// Everything isActive() checks except the limiter's gain.
bool MasterChain::restBesidesLimiter() const noexcept {
    for (const auto& w : stageW_)
        if (!w.settled()) return false;
    if (!lpfW_.settled() || !drive_.settled() || !compW_.settled() || !logLo_.settled() || !logHi_.settled() ||
        !limW_.settled() || !limThrS_.settled() || !clipW_.settled() || !volume_.settled())
        return false;
    for (std::size_t b = 0; b < 3; ++b)
        if (!bandOn_[b].settled() || !bandMakeup_[b].settled() || !bandSolo_[b].settled() || !comp_[b].idle())
            return false;
    for (std::size_t c = 0; c < 2; ++c)
        if (modernHp_[c].state() != 0.0 || origHp_[c].state() != 0.0 || origLp_[c].state() != 0.0) return false;
    return xLo_.stateMagnitude() == 0.0 && xHi_.stateMagnitude() == 0.0;
}

bool MasterChain::isActive() const noexcept { return limGain_ != 1.0 || !restBesidesLimiter(); }

// What runLimiter() does to the limiter's gain over n samples of exact zeros
// when every other state is at rest (restBesidesLimiter()): the stages before
// it turn zeros into +0 and change nothing, so its input peak is 0 and the
// target gain is 1. The same recursion and snap, sample by sample; every glide
// it reads is settled, so ticking them is a no-op.
void MasterChain::releaseLimiter(int n) noexcept {
    if (limW_.value() <= 0.0) {   // settled out: runLimiter() pins the gain to unity
        if (n > 0) limGain_ = 1.0;
        return;
    }
    for (int k = 0; k < n && limGain_ != 1.0; ++k) {
        const double gt = 1.0;
        limGain_ = gt + (limGain_ - gt) * (gt < limGain_ ? limAtt_ : limRel_);
        if (gt == 1.0 && limGain_ > 1.0 - 1.0e-9) limGain_ = 1.0;
    }
}

// What process() does to the control grid over n samples, without control():
// at rest control() changes nothing (zero states flush to zero, the crossover
// glides are settled), so only the grid position has to move on.
void MasterChain::skipSilence(int n) noexcept {
    if (n <= 0) return;
    if (n <= ctrlLeft_) {
        ctrlLeft_ -= n;
    } else {
        const int k = n - ctrlLeft_;   // samples after the next tick is due
        ctrlLeft_ = ctrl_ - 1 - (k - 1) % ctrl_;
    }
}

void MasterChain::process(float* left, float* right, int n) noexcept {
    // The last stage: one non-finite (or absurd) sample from upstream must not
    // poison the filter states for the rest of the session. NaN/Inf -> +0,
    // anything beyond +-kInputLimit (+120 dBFS) is clamped, and -0 becomes +0,
    // so silence is +0 whether the chain runs or rests. Done on the bits, with
    // an OR of the results as the silence test: integer selects and an integer
    // reduction, so the loop vectorises (float compares against a NaN and a
    // float max reduction keep GCC from it).
    constexpr std::uint32_t kLimitBits = std::bit_cast<std::uint32_t>(kInputLimit);
    std::uint32_t bits = 0;
    for (float* io : {left, right})
        for (int k = 0; k < n; ++k) {
            const std::uint32_t u = std::bit_cast<std::uint32_t>(io[k]);
            const std::uint32_t a = u & 0x7fffffffu;
            const std::uint32_t mag = a >= 0x7f800000u ? 0u : (a > kLimitBits ? kLimitBits : a);
            const std::uint32_t y = mag | (mag != 0u ? (u & 0x80000000u) : 0u);
            io[k] = std::bit_cast<float>(y);
            bits |= y;
        }
    // At rest, silence in is silence out with no state change: skip the work.
    // If only the limiter is still releasing, run just its gain recursion:
    // every other stage would turn the zeros into +0 and change nothing.
    if (bits == 0 && restBesidesLimiter()) {
        if (limGain_ != 1.0) releaseLimiter(n);
        skipSilence(n);
        return;
    }
    int i = 0;
    while (i < n) {
        if (ctrlLeft_ == 0) {
            control();
            ctrlLeft_ = ctrl_;
        }
        const int m = std::min(n - i, ctrlLeft_);
        float* l = left + i;
        float* r = right + i;
        runStage(l, r, m);
        runComp(l, r, m);
        runLimiter(l, r, m);
        i += m;
        ctrlLeft_ -= m;
    }
}

void MasterChain::runStage(float* l, float* r, int n) noexcept {
    auto& wOff = stageW_[0];
    auto& wMod = stageW_[1];
    auto& wOrg = stageW_[2];
    const bool settled = wOff.settled() && wMod.settled() && wOrg.settled() && lpfW_.settled() && drive_.settled();
    if (settled && wOff.value() == 1.0) return;   // Off: nothing at all
    float* io[2] = {l, r};
    const bool driveSettled = drive_.settled();
    double pre = std::pow(10.0, drive_.value() / 20.0);
    double post = 1.0 / pre;
    for (int k = 0; k < n; ++k) {
        const double off = wOff.tick(), mod = wMod.tick(), org = wOrg.tick(), lpf = lpfW_.tick();
        if (!driveSettled) {
            pre = std::pow(10.0, drive_.tick() / 20.0);
            post = 1.0 / pre;
        }
        for (std::size_t c = 0; c < 2; ++c) {
            const double x = io[c][k];
            double y = off * x;
            if (mod > 0.0) {
                const double h = modernHp_[c].highpass(x);
                y += mod * post * kneeClip(static_cast<float>(h * pre), kRailLinear, kRail);
            }
            if (org > 0.0) {
                const double h = origHp_[c].highpass(x);
                const double s = post * kneeClip(static_cast<float>(h * pre), kRailLinear, kRail);
                const double lp = origLp_[c].lowpass(s);
                y += org * (s + lpf * (lp - s));
            }
            io[c][k] = static_cast<float>(y);
        }
    }
    if (wMod.settled() && wMod.value() == 0.0) {
        modernHp_[0].reset();
        modernHp_[1].reset();
    }
    if (wOrg.settled() && wOrg.value() == 0.0) {
        for (auto& f : origHp_) f.reset();
        for (auto& f : origLp_) f.reset();
    }
}

void MasterChain::runComp(float* l, float* r, int n) noexcept {
    if (compW_.settled() && compW_.value() == 0.0) {
        // Out: the per-band glides wait at their targets, so the state the
        // compressor comes back with never depends on where a block ended.
        for (std::size_t b = 0; b < 3; ++b) {
            bandOn_[b].snap();
            bandMakeup_[b].snap();
            bandSolo_[b].snap();
        }
        return;
    }
    for (int k = 0; k < n; ++k) {
        const double w = compW_.tick();
        double band[3][2];
        for (int c = 0; c < 2; ++c) {
            const double x = c == 0 ? l[k] : r[k];
            double low, rest, mid, high;
            xLo_.split(c, x, low, rest);
            xHi_.split(c, rest, mid, high);
            band[0][c] = xHi_.allpass(c, low);
            band[1][c] = mid;
            band[2][c] = high;
        }
        double yl = 0.0, yr = 0.0;
        for (std::size_t b = 0; b < 3; ++b) {
            const double on = bandOn_[b].tick();
            const double mk = bandMakeup_[b].tick();
            const double solo = bandSolo_[b].tick();
            // The detector tracks its band even while the band is out, so it
            // comes back in with the reduction the material calls for (no
            // make-up overshoot), and in silence it releases to rest.
            const float level = static_cast<float>(std::max(std::abs(band[b][0]), std::abs(band[b][1])));
            const double gr = comp_[b].tick(level);
            const double g = on > 0.0 ? 1.0 + on * (gr * mk - 1.0) : 1.0;
            yl += solo * g * band[b][0];
            yr += solo * g * band[b][1];
        }
        l[k] = static_cast<float>(w >= 1.0 ? yl : l[k] + w * (yl - l[k]));
        r[k] = static_cast<float>(w >= 1.0 ? yr : r[k] + w * (yr - r[k]));
    }
    if (compW_.settled() && compW_.value() == 0.0) {
        xLo_.clear();
        xHi_.clear();
        for (auto& c : comp_) c.clear();
        for (std::size_t b = 0; b < 3; ++b) {
            bandOn_[b].snap();
            bandMakeup_[b].snap();
            bandSolo_[b].snap();
        }
    }
}

void MasterChain::runLimiter(float* l, float* r, int n) noexcept {
    const bool limOff = limW_.settled() && limW_.value() == 0.0;
    const bool clipOff = clipW_.settled() && clipW_.value() == 0.0;
    for (int k = 0; k < n; ++k) {
        double xl = l[k], xr = r[k];
        const double wl = limOff ? 0.0 : limW_.tick();
        if (wl > 0.0) {
            const double w = wl;
            const double thr = limThrS_.tick();
            const double peak = std::max(std::abs(xl), std::abs(xr));
            const double gt = peak > thr ? thr / peak : 1.0;
            limGain_ = gt + (limGain_ - gt) * (gt < limGain_ ? limAtt_ : limRel_);
            // The release toward unity would stall a few ulps short of it in
            // double; land on it exactly so the chain can come to rest.
            if (gt == 1.0 && limGain_ > 1.0 - 1.0e-9) limGain_ = 1.0;
            const double yl = thr * kneeClip(static_cast<float>(xl * limGain_ / thr), kLimiterKnee, 1.0f);
            const double yr = thr * kneeClip(static_cast<float>(xr * limGain_ / thr), kLimiterKnee, 1.0f);
            xl = w >= 1.0 ? yl : xl + w * (yl - xl);
            xr = w >= 1.0 ? yr : xr + w * (yr - xr);
        } else {
            limGain_ = 1.0;
            limThrS_.snap();
        }
        const double wc = clipOff ? 0.0 : clipW_.tick();
        if (wc > 0.0) {
            const double cl = kneeClip(static_cast<float>(xl), kSoftClipLinear, 1.0f);
            const double cr = kneeClip(static_cast<float>(xr), kSoftClipLinear, 1.0f);
            xl = wc >= 1.0 ? cl : xl + wc * (cl - xl);
            xr = wc >= 1.0 ? cr : xr + wc * (cr - xr);
        }
        const double v = volume_.tick();
        l[k] = static_cast<float>(xl * v);
        r[k] = static_cast<float>(xr * v);
    }
}

}  // namespace mw909::fx
