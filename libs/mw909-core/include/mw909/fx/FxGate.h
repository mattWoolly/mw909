// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The block loop every stereo effect shares: control grid, sleep and wake.
//
// An effect derives from StereoGate<Derived> and provides
//     void control() noexcept;                 // once per control tick (slow smoothers)
//     void run(float* l, float* r, int n) noexcept;   // n never crosses a tick
//     bool quiet() const noexcept;             // every output-producing state below the floor
//     void sleepNow() noexcept;                // clear state, snap smoothers
//     void onWake() noexcept;                  // restart modulators from their rest state
// and may hide firstWake() to wake on something other than its input, and
// passThrough() to declare a bypassed state: while it is true and the effect is
// asleep, process() leaves its input untouched (a switched-off effect passes
// the signal dry). control() may call gateSleep() to stop at that tick; the
// rest of the block is then handled as asleep, so the moment an effect goes to
// sleep or into bypass is a sample position counted from its wake-up, never a
// host block boundary. bypassLive() tells whether signal is passing through the
// bypass right now (above the floor within the last control interval; found
// by a backward scan, O(1) while audio flows), so an effect switched back on
// can fade in from dry instead of jumping.
//
// While asleep the input is scanned for the first sample above the floor;
// everything before it is written as zeros, so a sleeping effect costs a
// compare per sample, and skipping the effect entirely while its input is
// silent gives the same output. The control grid restarts at the wake sample,
// so output never depends on the host's block partition.

#pragma once

#include <algorithm>

#include "mw909/fx/FxCommon.h"

namespace mw909::fx {

template <typename D>
class StereoGate {
public:
    bool awake() const noexcept { return awake_; }

    // First sample index in [i, n) that wakes the effect (default: input above
    // the floor). An effect with another input (a sidechain key) hides this.
    int firstWake(const float* l, const float* r, int i, int n) const noexcept {
        return i + firstAbove(l + i, r + i, n - i, kSleepFloor);
    }
    // Bypassed: while asleep, the input passes untouched (default: never).
    bool passThrough() const noexcept { return false; }
    // Asleep in bypass with signal passing (see above).
    bool bypassLive() const noexcept { return !awake_ && passQuiet_ < static_cast<long>(ctrl_); }

protected:
    void gatePrepare(double sampleRate) noexcept {
        ctrl_ = controlInterval(sampleRate);
        awake_ = false;
        ctrlLeft_ = 0;
        passQuiet_ = kQuietLong;
    }
    void gateReset() noexcept {
        awake_ = false;
        ctrlLeft_ = 0;
        passQuiet_ = kQuietLong;
    }
    // Force awake (a trigger arrived: an envelope must run even in silence).
    void gateWake() noexcept {
        if (!awake_) wakeUp();
    }
    // From control(): stop at this tick (the caller has cleared its state).
    void gateSleep() noexcept {
        awake_ = false;
        passQuiet_ = 0;   // it was running: treat the bypassed signal as live
    }

    void gateProcess(float* l, float* r, int n) noexcept {
        D& d = *static_cast<D*>(this);
        int i = 0;
        while (i < n) {
            if (!awake_) {
                if (d.passThrough()) {   // bypassed: the rest passes dry
                    notePassed(l + i, r + i, n - i);
                    return;
                }
                const int j = d.firstWake(l, r, i, n);
                zeroBuf(l + i, j - i);
                zeroBuf(r + i, j - i);
                i = j;
                if (i >= n) break;
                wakeUp();
            }
            if (ctrlLeft_ == 0) {
                d.control();
                ctrlLeft_ = ctrl_;
                if (!awake_) continue;   // control() went to sleep / bypass at this tick
                if (ticksAwake_++ > 0 && inPeak_ < kSleepFloor && outPeak_ < kSleepFloor && d.quiet()) {
                    d.sleepNow();
                    awake_ = false;
                    passQuiet_ = kQuietLong;
                    continue;
                }
                inPeak_ = outPeak_ = 0.0f;
            }
            const int m = std::min(n - i, ctrlLeft_);
            const float pin = std::max(peakAbs(l + i, m), peakAbs(r + i, m));
            inPeak_ = std::max(inPeak_, pin);
            d.run(l + i, r + i, m);
            const float pout = std::max(peakAbs(l + i, m), peakAbs(r + i, m));
            outPeak_ = std::max(outPeak_, pout);
            i += m;
            ctrlLeft_ -= m;
        }
    }

    int ctrl_ = 16;

private:
    static constexpr long kQuietLong = 1L << 30;

    // Track the samples since the bypassed input was last above the floor.
    void notePassed(const float* l, const float* r, int m) noexcept {
        for (int k = m - 1; k >= 0; --k)
            if (std::abs(l[k]) > kSleepFloor || std::abs(r[k]) > kSleepFloor) {
                passQuiet_ = m - 1 - k;
                return;
            }
        passQuiet_ = passQuiet_ + m < kQuietLong ? passQuiet_ + m : kQuietLong;
    }

    void wakeUp() noexcept {
        awake_ = true;
        ctrlLeft_ = 0;
        ticksAwake_ = 0;
        inPeak_ = outPeak_ = 0.0f;
        static_cast<D*>(this)->onWake();
    }

    bool awake_ = false;
    int ctrlLeft_ = 0;
    int ticksAwake_ = 0;
    long passQuiet_ = kQuietLong;
    float inPeak_ = 0.0f, outPeak_ = 0.0f;
};

}  // namespace mw909::fx
