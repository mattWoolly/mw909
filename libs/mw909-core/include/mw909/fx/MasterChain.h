// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The master chain on the main bus (individual outs bypass it):
//
//   output stage -> 3-band compressor -> limiter -> soft clip -> volume
//
// Output stage (OutputStage, research/03 §6):
//  * Off: nothing.
//  * Modern: the summing op-amp's saturation only, DC blocked by a first-order
//    10 Hz high-pass.
//  * Original: the hardware main output. The 1 uF coupling capacitors
//    (C153/C154) into the summing amplifier's virtual ground see about 1.22 kOhm
//    of mix resistors in parallel: a first-order high-pass at ~130 Hz on
//    everything in the mix. Then the op-amp, then (Revision::Late, S/N
//    415300 on) the R473 1k + R530 2.2k into C511 0.01 uF network: a
//    first-order low-pass at ~6.6 kHz. (Its fixed -2.4 dB divider is part of
//    the output level calibration and is not applied here.)
//  The op-amp is linear up to 0 dBFS and rounds off toward +6 dBFS (rail).
//  Drive 0..1 raises the level into the op-amp by up to 24 dB and lowers it
//  by the same amount after it, so drive changes the saturation, not the level.
//
// 3-band compressor: Linkwitz-Riley 24 dB/oct crossovers (40..1000 Hz and
// 1..12 kHz, the upper kept above the lower), the low band through the upper
// crossover's all-pass, so the bands sum to a flat magnitude when neutral.
// Per band: on (compressing), solo, threshold, ratio, attack, release,
// make-up; stereo-linked peak detection, 6 dB soft knee.
//
// Limiter: feed-forward, stereo-linked; the gain falls toward threshold/peak
// with the attack time constant and recovers with the release; a soft ceiling
// (knee from -0.5 dB) catches the overshoot of the attack, so the limiter's
// output never exceeds its threshold.
// Soft clip: the diode-like clipper that keeps the output at or below 0 dBFS.
// It is linear to -0.5 dBFS (the same knee as the limiter's ceiling), then
// rounds off with a tanh approach to 0 dBFS. With the defaults (limiter at
// 0 dB, soft clip on) the chain is therefore linear up to -0.5 dBFS; only the
// last half decibel below full scale is shaped.
// Every switch crossfades (10 ms); every level is smoothed.
// Input samples that are NaN/Inf become 0 and anything beyond +120 dBFS is
// clamped, so a bad sample from upstream cannot poison the chain's state.
//
// Rest: isActive() is false once every glide and crossfade is settled, every
// filter state (output stage, crossovers) is exactly zero, no band compressor
// holds gain reduction and the limiter's gain is back at unity. Processing
// silence then produces exact zeros and changes no state, so a block of exact
// zeros (-0 counts as zero) while at rest is skipped outright: process()
// returns after the input scan, and a caller that already knows the block is
// silent calls skipSilence() instead. Skipping only advances the control grid,
// which stays counted from reset(); the output is byte-identical to running
// the block, so it is the same for every block partition. The decaying filter
// states reach exact zero through the control grid's denormal flush (1e-30),
// the limiter's release snaps to unity within 1e-9. When the limiter's gain
// is the only state still moving (a long release after deep limiting), a
// silent block runs just that gain's per-sample recursion, exactly as the full
// chain would. A band compressor's detector keeps tracking its band while the
// band is switched out, so it releases in silence and comes back in without a
// make-up overshoot.

#pragma once

#include <array>

#include "mw909/Voices.h"
#include "mw909/fx/Compressor.h"
#include "mw909/fx/FxCommon.h"
#include "mw909/fx/FxFilters.h"
#include "mw909/fx/FxParams.h"

namespace mw909::fx {

class MasterChain {
public:
    static constexpr double kModernHpfHz = 10.0;
    static constexpr double kOriginalHpfHz = 130.0;
    static constexpr double kOriginalLpfHz = 6570.0;
    static constexpr float kRail = 2.0f;        // op-amp ceiling (+6 dBFS)
    static constexpr float kRailLinear = 1.0f;  // linear up to 0 dBFS
    static constexpr double kMaxDriveDb = 24.0;
    static constexpr float kCompKneeDb = 6.0f;
    static constexpr float kSoftClipLinear = 0.94406088f;   // -0.5 dBFS
    static constexpr float kLimiterKnee = 0.94406088f;      // -0.5 dB below the threshold
    static constexpr float kInputLimit = 1.0e6f;            // +120 dBFS

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParams(const MasterParams& params, Revision revision) noexcept;
    void process(float* left, float* right, int n) noexcept;

    // False while the chain is at rest (see above): silence in, exact zeros out.
    bool isActive() const noexcept;
    // Equivalent to process() on n samples of exact zeros while !isActive()
    // (the caller zero-fills its buffers): advances the control grid only.
    void skipSilence(int n) noexcept;

    float bandReductionDb(int band) const noexcept { return comp_[static_cast<std::size_t>(band % 3)].reductionDb(); }
    float limiterReductionDb() const noexcept { return -dsp::gainToDb(static_cast<float>(limGain_)); }

private:
    void control() noexcept;
    bool restBesidesLimiter() const noexcept;
    void releaseLimiter(int n) noexcept;
    void snapAll() noexcept;
    void runStage(float* l, float* r, int n) noexcept;
    void runComp(float* l, float* r, int n) noexcept;
    void runLimiter(float* l, float* r, int n) noexcept;

    double fs_ = 48000.0;
    int ctrl_ = 16, ctrlLeft_ = 0;
    bool fresh_ = true;

    // Output stage.
    std::array<Fade, 3> stageW_{};   // Off, Modern, Original
    Fade lpfW_;                       // the Late revision's output low-pass
    std::array<OnePoleTpt, 2> modernHp_{}, origHp_{}, origLp_{};
    Smoothed drive_;

    // 3-band compressor.
    Fade compW_;
    Smoothed logLo_, logHi_;
    Lr4Crossover<2> xLo_, xHi_;
    std::array<CompressorCore, 3> comp_{};
    std::array<Fade, 3> bandOn_{};
    std::array<Smoothed, 3> bandMakeup_{}, bandSolo_{};

    // Limiter, soft clip, volume.
    Fade limW_, clipW_;
    double limThr_ = 1.0, limAtt_ = 0.0, limRel_ = 0.0, limGain_ = 1.0;
    Smoothed limThrS_, volume_;
    LastParams<MasterParams> last_;
};

}  // namespace mw909::fx
