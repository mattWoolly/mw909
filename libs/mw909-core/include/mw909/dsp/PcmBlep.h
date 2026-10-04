// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Band-limited rendering of a zero-order-hold (stepped) signal at an arbitrary
// clock, at the host rate (research/05 §5.5, "exact-edge BLEP").
//
// The 6-bit DAC of a sample voice holds each value for one clock period. Its
// output is a sum of steps  Σ Δk·u(t − tk)  at the exact (sub-sample) clock
// edges tk, and what the voice must output is that staircase through the
// analog reconstruction filter (two Sallen-Key low-passes, research/02 §6.1)
// and band-limited to the host Nyquist:
//
//     y(n) = Σk Δk · S(n − D − tk),   S = step response of (h_AA * h_LPF)
//
// h_AA is a Kaiser-windowed sinc with its −6 dB point at the host Nyquist
// (β = 9.5, stop band ≥ 95 dB), `taps` host samples long (16, 32, 48 or 64);
// D = taps/2 is the latency. The analog LPF is folded into S, so it costs
// nothing at render time and is exact at every host rate (its 26 kHz corner is
// above the 44.1 kHz Nyquist, so it could not run as an IIR at the host rate).
// The ZOH's sinc droop needs no modelling: it is what a sum of steps is.
// Which length a voice uses depends on the host rate as well as the quality
// (pcm::kernelTaps): every image that would fold back to 20 kHz or below must
// fall in the stop band (≥ 90 dB down); images below the host Nyquist are kept.
//
// S is tabulated at kPhases sub-sample phases over `width()` host samples (the
// AA span plus the analog filter's settling time to −120 dB) and read with
// linear interpolation. Each step costs width() multiply-adds. The table is
// stored phase-major (one row of width() host-sample taps per phase), so a
// step reads two adjacent rows front to back and the multiply-adds run as
// vector arithmetic; time-major, every tap was another cache line.
//
// Building S is cheap enough for prepare() on the audio thread (40..70 µs for
// 32..64 taps at 48 kHz on a 2.8 GHz Xeon; the engine re-prepares the voices
// there when the quality, seed, variance or revision change, and voices whose
// filters agree share one table, see Cymbal::prepare()):
//  * the AA step (the integral of h_AA) depends only on `taps` when time is in
//    host samples, so it is computed once per process at load time
//    (detail::pcmAaStep, src/rom/PcmKernelTables.cpp);
//  * the analog filter is split into its two complex pole modes (partial
//    fractions of the two Sallen-Key sections), each integrated exactly for a
//    piecewise-linear input over the AA span (one complex multiply-add per
//    table point); after the AA span the input is exactly 1 and each mode's
//    response is closed-form, evaluated phase-parallel.
// build() does not allocate (fixed-size table).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "mw909/dsp/Filters.h"
#include "mw909/dsp/Math.h"

namespace mw909::dsp {

// One unity-gain Sallen-Key low-pass section: H(s) = w0² / (s² + s·w0/Q + w0²).
struct AnalogLowpass2 {
    double f0 = 25900.0;
    double q = 0.548;
};

namespace detail {
// The normalised AA step for a kernel of `taps` host samples (16, 32, 48 or 64;
// other values round up), taps*kPhases + 1 points from t = −taps/2 to +taps/2:
// 0 at the first point, exactly 1 at the last. Built at load time; read-only.
const float* pcmAaStep(int taps) noexcept;
}  // namespace detail

class PcmStepKernel {
public:
    static constexpr int kPhases = 256;     // table points per host sample
    static constexpr int kMaxWidth = 128;   // host samples
    static constexpr int kMinTaps = 16;
    static constexpr int kMaxTaps = 64;
    static constexpr double kKaiserBeta = 9.5;   // ≈ 95 dB stop band

    // The kernel lengths that exist (others round up to the next of these).
    static constexpr int roundTaps(int taps) noexcept {
        return taps <= 16 ? 16 : (taps <= 32 ? 32 : (taps <= 48 ? 48 : 64));
    }

    // taps: AA length in host samples (16, 32, 48 or 64). Allocation-free. A
    // call with the same arguments as the previous one returns at once (the
    // engine re-prepares voices for seed changes that leave the nominal
    // filter, and so the table, as it was). A real build costs roughly the
    // table's kPhases * width points of the two-section step response: the
    // largest part of a reconfigure block, and above 48 kHz in 64-sample blocks
    // (three sample voices, variance above 0) more than the 25 % block rule
    // allows; that rule is set at 48 kHz (architecture.md §7).
    void build(double sampleRate, int taps, const AnalogLowpass2& stage1, const AnalogLowpass2& stage2) noexcept {
        if (builtFor(sampleRate, taps, stage1, stage2)) return;
        taps = roundTaps(taps);
        const double fs = clamp(std::isfinite(sampleRate) ? sampleRate : 48000.0, 8000.0, 768000.0);
        builtFs_ = fs;
        builtTaps_ = taps;
        built1_ = stage1;
        built2_ = stage2;
        latency_ = taps / 2;

        // Poles of the two sections in host-sample time units. Q is kept above
        // critical damping (0.51) so the poles stay a distinct complex pair (the
        // stock 0.548 under ±5 % parts never gets below 0.52).
        Mode m1 = modeOf(stage1, fs), m2 = modeOf(stage2, fs);
        // Residues of H(s) = w1² w2² / ((s−p1)(s−p1*)(s−p2)(s−p2*)) at p1 and p2,
        // doubled (h(t) = 2 Re(R1 e^{p1 t}) + 2 Re(R2 e^{p2 t})).
        const double k12 = m1.w2 * m2.w2;
        {
            Cx d = mul(mul(sub(m1.p, conj(m1.p)), sub(m1.p, m2.p)), sub(m1.p, conj(m2.p)));
            m1.g = scale(div(Cx{k12, 0.0}, d), 2.0);
            d = mul(mul(sub(m2.p, conj(m2.p)), sub(m2.p, m1.p)), sub(m2.p, conj(m1.p)));
            m2.g = scale(div(Cx{k12, 0.0}, d), 2.0);
        }

        // Settling of the slower mode to -120 dB: exp(-alpha t) = 1e-6.
        const double alpha = std::min(-m1.p.re, -m2.p.re);   // per host sample
        const int tail = static_cast<int>(std::ceil(13.8 / alpha)) + 2;
        width_ = std::min(taps + tail, kMaxWidth);
        const int n = width_ * kPhases;
        const int aaEnd = taps * kPhases;

        // 1. The AA span: y' = p y + A(t) per mode, exact for A linear between
        //    table points: y(m+1) = E y(m) + b0 A(m) + b1 A(m+1).
        const double dt = 1.0 / kPhases;
        for (Mode* m : {&m1, &m2}) {
            const Cx z = scale(m->p, dt);
            m->e = cexp(z);
            const Cx em1 = sub(m->e, Cx{1.0, 0.0});
            const Cx inv = div(Cx{1.0, 0.0}, m->p);
            m->b1 = sub(div(em1, scale(mul(m->p, m->p), dt)), inv);   // (E-1)/(p² dt) - 1/p
            m->b0 = sub(mul(em1, inv), m->b1);                         // (E-1)/p - b1
        }
        // Each conjugate pole pair as one real biquad on the table grid:
        //   r(m) = a1 r(m-1) - a2 r(m-2) + n2 x(m) + n1 x(m-1) + n0 x(m-2),
        //   r = Re(g y), the two modes in parallel, S = r1 + r2.
        // The two modes run as the two lanes of one 2-wide vector (same
        // operations, different coefficients; element-wise IEEE arithmetic, so
        // each lane is bit-for-bit the scalar recursion). This loop is most of
        // build()'s cost, which the engine pays on the audio thread when a
        // seed or variance change moves the filter: written out as vectors
        // it takes 30..50 % less time than the scalar code the compiler made
        // of two-element arrays.
        const Biquad q1 = biquadOf(m1), q2 = biquadOf(m2);
        const V2 n2{q1.n2, q2.n2}, n1{q1.n1, q2.n1}, n0{q1.n0, q2.n0};
        const V2 a1{q1.a1, q2.a1}, a2{q1.a2, q2.a2};
        const float* a = detail::pcmAaStep(taps);
        V2 xm1{0.0, 0.0}, xm2{0.0, 0.0};   // x(m-1), x(m-2), broadcast
        V2 ra{0.0, 0.0}, rb{0.0, 0.0};     // r(m-1), r(m-2) per mode
        auto advance = [&](double xs) noexcept {
            const V2 x{xs, xs};
            // r(m-1) enters last: one multiply and one add on the recursion's critical path
            const V2 u = (n2 * x + n1 * xm1 + n0 * xm2) - a2 * rb;
            const V2 r = u + a1 * ra;
            rb = ra;
            ra = r;
            xm2 = xm1;
            xm1 = x;
            return r[0] + r[1];
        };
        for (int m = 0; m <= aaEnd; ++m) put(m, static_cast<float>(advance(static_cast<double>(a[m]))));
        advance(1.0);   // m = aaEnd + 1: the input is 1 from here on
        const double r1b = rb[0], r1a = ra[0], r2b = rb[1], r2a = ra[1];
        // The complex mode states at aaEnd from r(aaEnd) = r1b, r(aaEnd + 1) = r1a
        // (input 1 from aaEnd on): r(aaEnd + j) - r∞ = Re(C E^j).
        auto modeState = [](const Mode& md, double rEnd, double rNext) {
            const Cx yInf = neg(div(Cx{1.0, 0.0}, md.p));
            const double rInf = mul(md.g, yInf).re;
            const double d0 = rEnd - rInf, d1 = rNext - rInf;
            return Cx{d0, (d0 * md.e.re - d1) / md.e.im};   // C = g (y(aaEnd) - y∞)
        };
        const Cx cEnd1 = modeState(m1, r1b, r1a), cEnd2 = modeState(m2, r2b, r2a);

        // 2. After the AA span the input is 1: y(j) = y∞ + (y(aaEnd) − y∞) E^j,
        //    y∞ = −1/p. S = Σ Re(g y∞) + Σ Re(g (y(aaEnd) − y∞) E^j), evaluated as
        //    E^j = (E^kPhases)^blk · E^ph with the phase loop independent per point.
        double sInf = 0.0;
        std::array<double, kPhases> f1r{}, f1i{}, f2r{}, f2i{};
        Cx step1{1.0, 0.0}, step2{1.0, 0.0};
        {
            const Cx yInf1 = neg(div(Cx{1.0, 0.0}, m1.p)), yInf2 = neg(div(Cx{1.0, 0.0}, m2.p));
            sInf = mul(m1.g, yInf1).re + mul(m2.g, yInf2).re;
            Cx c1 = cEnd1, c2 = cEnd2;
            for (int ph = 0; ph < kPhases; ++ph) {
                const auto k = static_cast<std::size_t>(ph);
                f1r[k] = c1.re;
                f1i[k] = c1.im;
                f2r[k] = c2.re;
                f2i[k] = c2.im;
                c1 = mul(c1, m1.e);
                c2 = mul(c2, m2.e);
                step1 = mul(step1, m1.e);
                step2 = mul(step2, m2.e);
            }
        }
        // Block b (host sample taps + b) has E^(b kPhases) = G(b); the points
        // are written row by row (phase-major, contiguous in b).
        std::array<double, kMaxWidth + 1> gr1{}, gi1{}, gr2{}, gi2{};
        int blocks = 0;
        {
            Cx g1{1.0, 0.0}, g2{1.0, 0.0};
            for (int base = aaEnd; base <= n; base += kPhases) {
                const auto b = static_cast<std::size_t>(blocks++);
                gr1[b] = g1.re;
                gi1[b] = g1.im;
                gr2[b] = g2.re;
                gi2[b] = g2.im;
                g1 = mul(g1, step1);
                g2 = mul(g2, step2);
            }
        }
        for (int ph = 0; ph < kPhases; ++ph) {
            const auto k = static_cast<std::size_t>(ph);
            float* out = rows_.data() + static_cast<std::size_t>(ph * kRowStride + taps);
            // Point 0 of the first block is the end of the AA span (already
            // written); the last block holds point n alone.
            for (int b = (ph == 0 ? 1 : 0); b < blocks - (ph == 0 ? 0 : 1); ++b) {
                const auto bb = static_cast<std::size_t>(b);
                out[b] = static_cast<float>(sInf + (f1r[k] * gr1[bb] - f1i[k] * gi1[bb]) + (f2r[k] * gr2[bb] - f2i[k] * gi2[bb]));
            }
        }
        // Row kPhases: the phase-0 points one host sample later.
        for (int j = taps; j < width_; ++j) rows_[static_cast<std::size_t>(kPhases * kRowStride + j)] = at((j + 1) * kPhases);
        endValue_ = at(n);
        put(n + 1, endValue_);
    }

    // Whether the table is the one build() would make of these arguments (a
    // voice whose kernel would equal another's uses that one: the sample
    // voices' filters are identical at variance 0).
    bool builtFor(double sampleRate, int taps, const AnalogLowpass2& stage1, const AnalogLowpass2& stage2) const noexcept {
        taps = roundTaps(taps);
        const double fs = clamp(std::isfinite(sampleRate) ? sampleRate : 48000.0, 8000.0, 768000.0);
        return width_ > 0 && fs == builtFs_ && taps == builtTaps_ && stage1.f0 == built1_.f0 && stage1.q == built1_.q &&
               stage2.f0 == built2_.f0 && stage2.q == built2_.q;
    }

    int width() const noexcept { return width_; }
    int latency() const noexcept { return latency_; }
    // S at the end of the table (should be 1 within 1e-5): for tests.
    float endValue() const noexcept { return endValue_; }
    // S(t), t in host samples relative to the step (t = -latency .. width - latency).
    float stepAt(double t) const noexcept {
        const double pos = (t + latency_) * kPhases;
        if (pos <= 0.0) return 0.0f;
        if (pos >= width_ * kPhases) return 1.0f;
        const int i = static_cast<int>(pos);
        const float w = static_cast<float>(pos - i);
        return at(i) + w * (at(i + 1) - at(i));
    }
    // Row `phase` (0..kPhases): S at host-sample offsets j + phase / kPhases,
    // j = 0 .. width() - 1, relative to the kernel's start. Row kPhases is
    // row 0 one host sample later, so a phase and the next are always two
    // rows (PcmStepAccumulator::addStep).
    const float* row(int phase) const noexcept { return rows_.data() + static_cast<std::size_t>(phase) * kRowStride; }

private:
    // Two doubles in one SSE2 / NEON register (GCC and Clang vector extension).
    typedef double V2 __attribute__((vector_size(16)));

    // Minimal complex arithmetic (std::complex's multiply is a library call
    // under strict IEEE semantics).
    struct Cx {
        double re = 0.0, im = 0.0;
    };
    static Cx mul(Cx a, Cx b) noexcept { return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
    static Cx sub(Cx a, Cx b) noexcept { return {a.re - b.re, a.im - b.im}; }
    static Cx neg(Cx a) noexcept { return {-a.re, -a.im}; }
    static Cx conj(Cx a) noexcept { return {a.re, -a.im}; }
    static Cx scale(Cx a, double s) noexcept { return {a.re * s, a.im * s}; }
    static Cx div(Cx a, Cx b) noexcept {
        const double d = b.re * b.re + b.im * b.im;
        return {(a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d};
    }
    static Cx cexp(Cx z) noexcept {
        const double r = std::exp(z.re);
        return {r * std::cos(z.im), r * std::sin(z.im)};
    }
    struct Mode {
        Cx p;           // pole, per host sample
        double w2 = 0;  // w0² of its section
        Cx g;           // 2 x residue
        Cx e, b0, b1;   // per-table-point recursion
    };
    static Mode modeOf(const AnalogLowpass2& s, double fs) noexcept {
        const double f0 = clamp(std::isfinite(s.f0) ? s.f0 : 25900.0, 1000.0, 200000.0);
        const double q = clamp(std::isfinite(s.q) ? s.q : 0.548, 0.51, 20.0);
        const double w = kTwoPi * f0 / fs;
        const double zeta = 0.5 / q;
        Mode m;
        m.p = {-zeta * w, w * std::sqrt(1.0 - zeta * zeta)};
        m.w2 = w * w;
        return m;
    }

    struct Biquad {
        double n2 = 0, n1 = 0, n0 = 0, a1 = 0, a2 = 0;
    };
    // r = Re(g y), y(m+1) = E y(m) + b0 x(m) + b1 x(m+1)  as a real biquad.
    static Biquad biquadOf(const Mode& md) noexcept {
        const Cx gb0 = mul(md.g, md.b0), gb1 = mul(md.g, md.b1), ec = conj(md.e);
        Biquad q;
        q.n2 = gb1.re;
        q.n1 = gb0.re - mul(gb1, ec).re;
        q.n0 = -mul(gb0, ec).re;
        q.a1 = 2.0 * md.e.re;
        q.a2 = md.e.re * md.e.re + md.e.im * md.e.im;
        return q;
    }

    // S at time-major point m = j kPhases + ph (host sample j, phase ph) is
    // rows_[ph][j]; a point at phase 0 is also row kPhases of the sample
    // before. kRowStride keeps every row 16-byte aligned and holds column
    // kMaxWidth (the point after the last tap, read by stepAt()).
    static constexpr int kRowStride = kMaxWidth + 4;
    void put(int m, float v) noexcept {
        const int ph = m % kPhases, j = m / kPhases;
        rows_[static_cast<std::size_t>(ph * kRowStride + j)] = v;
        if (ph == 0 && j > 0) rows_[static_cast<std::size_t>(kPhases * kRowStride + j - 1)] = v;
    }
    float at(int m) const noexcept { return rows_[static_cast<std::size_t>((m % kPhases) * kRowStride + m / kPhases)]; }

    alignas(16) std::array<float, (kPhases + 1) * kRowStride> rows_{};
    int width_ = 0;
    int latency_ = 0;
    float endValue_ = 0.0f;
    double builtFs_ = 0.0;
    int builtTaps_ = 0;
    AnalogLowpass2 built1_{}, built2_{};
};

// Accumulates band-limited steps for one output channel. Sample times are a
// free-running 32-bit host-sample counter (wraps harmlessly: the ring is a
// power of two smaller than 2^32).
class PcmStepAccumulator {
public:
    static constexpr int kRing = 256;
    static_assert(kRing >= PcmStepKernel::kMaxWidth + 2, "ring too small");

    void reset() noexcept {
        ring_.fill(0.0f);
        holdAdd_.fill(0.0);
        hold_ = 0.0;
        busyUntil_ = 0U;
        busy_ = false;
    }

    // A step of height `delta` at host time  (n − 1) + frac,  frac in [0, 1):
    // `n` is the first output sample it can affect.
    void addStep(const PcmStepKernel& k, std::uint32_t n, double frac, float delta) noexcept {
        double pos = (1.0 - frac) * PcmStepKernel::kPhases;
        int i0 = static_cast<int>(pos);
        float w = static_cast<float>(pos - i0);
        if (i0 >= PcmStepKernel::kPhases) {
            i0 = PcmStepKernel::kPhases - 1;
            w = 1.0f;
        }
        const float* ta = k.row(i0);       // S at phase i0 of every tap
        const float* tb = k.row(i0 + 1);   // and at the next phase
        const int width = k.width();
        // The ring positions n .. n + width - 1, in at most two runs (wrap).
        const int start = static_cast<int>(n & kMask);
        const int first = std::min(width, kRing - start);
        mac(ring_.data() + start, ta, tb, w, delta, first);
        mac(ring_.data(), ta + first, tb + first, w, delta, width - first);
        const std::uint32_t end = n + static_cast<std::uint32_t>(width);
        holdAdd_[end & kMask] += static_cast<double>(delta);
        if (!busy_ || static_cast<std::int32_t>(end - busyUntil_) > 0) busyUntil_ = end;
        busy_ = true;
    }

    // Output sample n (call once per sample, in order).
    float tick(std::uint32_t n) noexcept {
        const std::uint32_t i = n & kMask;
        hold_ += holdAdd_[i];
        holdAdd_[i] = 0.0;
        const float y = static_cast<float>(hold_) + ring_[i];
        ring_[i] = 0.0f;
        if (busy_ && n == busyUntil_) busy_ = false;
        return y;
    }

    // No step contribution is pending (the hold value may still be non-zero).
    bool drained() const noexcept { return !busy_; }
    double hold() const noexcept { return hold_; }
    // Only when drained(): snap a hold value that is zero up to rounding.
    void clearHold() noexcept { hold_ = 0.0; }

private:
    static constexpr std::uint32_t kMask = kRing - 1;

    // r[j] += delta (a[j] + w (b[j] - a[j])): independent per j, so it vectorises.
    static void mac(float* __restrict r, const float* __restrict a, const float* __restrict b, float w, float delta,
                    int count) noexcept {
        for (int j = 0; j < count; ++j) r[j] += delta * (a[j] + w * (b[j] - a[j]));
    }

    std::array<float, kRing> ring_{};
    std::array<double, kRing> holdAdd_{};
    double hold_ = 0.0;
    std::uint32_t busyUntil_ = 0U;
    bool busy_ = false;
};

}  // namespace mw909::dsp
