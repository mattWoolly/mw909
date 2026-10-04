// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The anti-parallel diode pair to ground that rounds every analog VCO of the
// original machine (BD D10/D11, SD D65/D66 and D69/D70, the tom VCOs) and
// limits the rim shot's resonator sum (D91/D92), research/01 §1.3, §3.5, §6.3:
//
//   v --R_in--+--> u
//             |
//        D ||| D (anti-parallel, Shockley: 2 Is sinh(u / nVt))
//             |
//            R_load (the VCA input divider) to ground
//
//   (v - u) G_in = u G_load + 2 Is sinh(u / nVt)
//
// solved for u by Newton's method, warm-started from the previous sample (the
// drive is a low-frequency triangle, so 2..3 iterations converge to 1e-9 V).
// The iteration stops on convergence, so the result is a deterministic
// function of (input, previous output). DiodeClipperBank holds tables for a
// range of input resistances, for a clipper whose drive is a knob (the BD's
// Shaper). Header-only, allocation-free.

#pragma once

#include <cmath>
#include <cstddef>

#include "mw909/dsp/Math.h"

namespace mw909::dsp {

class DiodePairClipper {
public:
    // gIn, gLoad in siemens; saturation current in amperes; nVt in volts.
    void set(double gIn, double gLoad, double saturationAmps, double nVt) noexcept {
        gIn_ = std::max(gIn, 1.0e-12);
        gLoad_ = std::max(gLoad, 0.0);
        twoIs_ = 2.0 * std::max(saturationAmps, 1.0e-18);
        invNvt_ = 1.0 / std::max(nVt, 1.0e-3);
    }
    void setInputConductance(double gIn) noexcept { gIn_ = std::max(gIn, 1.0e-12); }
    void reset() noexcept { u_ = 0.0; }

    // Node voltage u for input voltage v.
    double process(double v) noexcept {
        if (!std::isfinite(v)) v = 0.0;
        const double i = gIn_ * v;
        const double g = gIn_ + gLoad_;
        double u = u_;
        // The diodes cannot sit far beyond a volt; a warm start outside that
        // (first sample after a reset with a huge drive) is pulled in first.
        const double lim = 30.0 / invNvt_;
        u = clamp(u, -lim, lim);
        for (int it = 0; it < 10; ++it) {
            const double e = std::exp(u * invNvt_);
            const double ie = 1.0 / e;
            const double f = g * u + 0.5 * twoIs_ * (e - ie) - i;
            const double df = g + 0.5 * twoIs_ * invNvt_ * (e + ie);
            double du = f / df;
            // Damped: never jump more than 4 nVt per step (the exponential
            // makes undamped Newton overshoot from far below the knee).
            const double maxStep = 4.0 / invNvt_;
            du = clamp(du, -maxStep, maxStep);
            u -= du;
            if (std::abs(du) < 1.0e-8) break;  // 10 nV on a ~0.6 V node
        }
        u_ = u;
        return u;
    }

    double last() const noexcept { return u_; }
    // Warm start for the next process() (a bank's fallback solver).
    void prime(double u) noexcept { u_ = std::isfinite(u) ? u : 0.0; }

    // Tabulate u(v) over [-vMax, vMax] for the present conductances, so that
    // processTable() costs one interpolation (the voices' fixed clippers).
    // Allocation-free; about 1k Newton solves (~100 us). The table is a pure
    // function of (conductances, diode, vMax): a call that repeats the
    // previous one's arguments keeps it and returns at once, so a voice's
    // re-prepare on the audio thread (seed, variance, quality, revision: none
    // of which reach these tables) does not pay for it again.
    void buildTable(double vMax) noexcept {
        vMax = std::max(vMax, 1.0e-3);
        if (haveTable_ && vMax == vMax_ && gIn_ == builtGIn_ && gLoad_ == builtGLoad_ && twoIs_ == builtTwoIs_ &&
            invNvt_ == builtInvNvt_)
            return;
        vMax_ = vMax;
        builtGIn_ = gIn_;
        builtGLoad_ = gLoad_;
        builtTwoIs_ = twoIs_;
        builtInvNvt_ = invNvt_;
        const double save = u_;
        u_ = 0.0;
        // From the centre outwards so every solve is warm-started.
        const int mid = kTable / 2;
        for (int i = mid; i <= kTable; ++i) table_[static_cast<std::size_t>(i)] = process(vAt(i));
        u_ = 0.0;
        for (int i = mid; i >= 0; --i) table_[static_cast<std::size_t>(i)] = process(vAt(i));
        u_ = save;
        scale_ = static_cast<double>(kTable) / (2.0 * vMax_);
        haveTable_ = true;
    }
    bool hasTable() const noexcept { return haveTable_; }
    double tableRange() const noexcept { return haveTable_ ? vMax_ : 0.0; }
    // The table's value at |v| < tableRange() (no state change).
    double lookup(double v) const noexcept {
        const double p = (v + vMax_) * scale_;
        const auto i = std::min(static_cast<std::size_t>(p), static_cast<std::size_t>(kTable - 1));
        const double f = p - static_cast<double>(i);
        return table_[i] + (table_[i + 1] - table_[i]) * f;
    }
    double processTable(double v) noexcept { return processTableOf(*this, v); }
    // processTable() with the table of `owner`, a clipper set to the same
    // conductances and diode whose table covers the same range: a voice with
    // several identical clippers keeps one table (one set of cache lines).
    // This clipper's own state still warm-starts its solve beyond the table.
    double processTableOf(const DiodePairClipper& owner, double v) noexcept {
        if (!(std::abs(v) < owner.vMax_)) return process(v);  // beyond the table (and NaN): solve
        const double p = (v + owner.vMax_) * owner.scale_;
        const auto i = std::min(static_cast<std::size_t>(p), static_cast<std::size_t>(kTable - 1));
        const double f = p - static_cast<double>(i);
        u_ = owner.table_[i] + (owner.table_[i + 1] - owner.table_[i]) * f;
        return u_;
    }

private:
    static constexpr int kTable = 1024;
    double vAt(int i) const noexcept { return -vMax_ + 2.0 * vMax_ * static_cast<double>(i) / kTable; }
    double table_[kTable + 1] = {};
    double vMax_ = 1.0, scale_ = 1.0;
    double builtGIn_ = 0.0, builtGLoad_ = 0.0, builtTwoIs_ = 0.0, builtInvNvt_ = 0.0;   // the table's arguments
    bool haveTable_ = false;

    double gIn_ = 1.0e-4, gLoad_ = 0.0, twoIs_ = 5.0e-9, invNvt_ = 1.0 / 0.045;
    double u_ = 0.0;
};

// N tabulated clippers whose input conductances are spaced evenly in log
// from gInLo to gInHi, all built in prepare() (about N x 100 us the first
// time; a re-prepare with the same arguments keeps them), so that the drive
// can move on the audio thread for the cost of two table reads. At a
// bank position p in [0, N-1] (and the matching exact conductance gIn) the
// node voltage is interpolated between the two neighbouring tables and, off
// the grid points, polished by one Newton step at gIn: worst error ~0.1 mV on
// a 0.5 V node for N = 9 over a 16:1 range (4 mV without the polish). On a
// grid point it is the table itself.
template <int N>
class DiodeClipperBank {
    static_assert(N >= 2);

public:
    void build(double gInLo, double gInHi, double gLoad, double saturationAmps, double nVt, double vMax) noexcept {
        gLoad_ = std::max(gLoad, 0.0);
        twoIs_ = 2.0 * std::max(saturationAmps, 1.0e-18);
        invNvt_ = 1.0 / std::max(nVt, 1.0e-3);
        for (int k = 0; k < N; ++k) {
            const double f = static_cast<double>(k) / (N - 1);
            const double g = gInLo * std::pow(gInHi / gInLo, f);
            DiodePairClipper& c = bank_[static_cast<std::size_t>(k)];
            c.set(g, gLoad_, saturationAmps, nVt);
            c.buildTable(vMax);
        }
        solver_ = bank_[0];
    }
    void reset() noexcept { u_ = 0.0; }

    // Node voltage for input v at bank position p, whose input conductance is gIn.
    double process(double v, double p, double gIn) noexcept {
        p = std::isfinite(p) ? clamp(p, 0.0, static_cast<double>(N - 1)) : 0.0;
        const auto i = std::min(static_cast<std::size_t>(p), static_cast<std::size_t>(N - 2));
        const double f = p - static_cast<double>(i);
        if (!(std::abs(v) < bank_[i].tableRange())) {  // beyond the tables (and NaN): solve
            solver_.set(gIn, gLoad_, 0.5 * twoIs_, 1.0 / invNvt_);
            solver_.prime(u_);
            u_ = solver_.process(v);
            return u_;
        }
        if (f == 0.0) {
            u_ = bank_[i].lookup(v);
            return u_;
        }
        double u = bank_[i].lookup(v) * (1.0 - f) + bank_[i + 1].lookup(v) * f;
        // One Newton step on g u + 2 Is sinh(u / nVt) = gIn v at the exact gIn.
        const double e = std::exp(clamp(u * invNvt_, -30.0, 30.0)), ie = 1.0 / e;
        const double g = gIn + gLoad_;
        u -= (g * u + 0.5 * twoIs_ * (e - ie) - gIn * v) / (g + 0.5 * twoIs_ * invNvt_ * (e + ie));
        u_ = u;
        return u;
    }
    double last() const noexcept { return u_; }

private:
    DiodePairClipper bank_[N];
    DiodePairClipper solver_;
    double gLoad_ = 0.0, twoIs_ = 5.0e-9, invNvt_ = 1.0 / 0.045;
    double u_ = 0.0;
};

}  // namespace mw909::dsp
