// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// HiHat (CH + OH): clock and tune, decay, level and accent against the
// service-manual check figures (research/02 §7), choke semantics
// (architecture §3.4), images and fold-back, robustness, determinism,
// retrigger, partition independence, sleep/wake, the reference-recording
// statistics (skipped when the local recordings are absent) and CPU.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "PcmTestUtil.h"
#include "mw909/dsp/PcmClock.h"

using namespace mw909;
using namespace pcmtest;

namespace {

constexpr double kHatClock = dsp::pcm::kHatClockHz;

VoiceParams hatParams(int voice, float decay, float tune = 0.0f) {
    VoiceParams p = defaultParams(voice);
    p.k[knob::PcmDecay] = decay;
    p.k[knob::PcmTune] = tune;
    return p;
}

struct Pair {
    std::vector<float> closed, open;
};

// Drive the HiHat directly: fn(hat, sampleIndex) is called before each sample.
template <class Fn>
Pair renderPair(double fs, double seconds, const VoiceParams& pc, const VoiceParams& po, Fn&& fn, bool choke = true,
                Revision rev = Revision::Late) {
    voices::HiHat h;
    h.prepare(setupFor(fs, 2, 0U, 0.0f, rev));
    h.setRoms(&factoryRom(RomSlot::Hat), nullptr);
    h.setParams(pc, po);
    h.setChoke(choke);
    const auto n = static_cast<std::size_t>(seconds * fs);
    Pair p{std::vector<float>(n), std::vector<float>(n)};
    std::size_t pos = 0;
    while (pos < n) {
        std::size_t next = fn(h, pos);   // returns the next sample at which fn wants control
        if (next <= pos || next > n) next = n;
        h.render(p.closed.data() + pos, p.open.data() + pos, static_cast<int>(next - pos));
        pos = next;
    }
    return p;
}

double bandVpp(const std::vector<float>& x, double fs) { return bandLimitedVpp(x, fs, 16000.0); }

}  // namespace

TEST_CASE("hats: clock 31.9 kHz, region lengths, tune as a clock-rate change", "[hats][pitch]") {
    for (double fs : kRates) {
        // OH region: 24576 words; CH region: 8192 words (decays at max so the end is visible)
        const auto oh = oneHit<OpenHat>(fs, hatParams(OH, 1.0f), Hit::normal(), 1.2);
        const auto ch = oneHit<ClosedHat>(fs, hatParams(CH, 1.0f), Hit::normal(), 0.5);
        // (the closed hat's residual envelope takes it ~70 dB down by its end)
        const double tOh = endTime(oh, fs), tCh = endTime(ch, fs, 1.0e-6);
        INFO("fs " << fs << " OH ends " << tOh << " s, CH ends " << tCh << " s");
        REQUIRE(std::abs(tOh * kHatClock / 24576.0 - 1.0) < 0.003);
        REQUIRE(std::abs(tCh * kHatClock / 8192.0 - 1.0) < 0.006);
        for (float st : {-12.0f, 7.0f, 12.0f}) {
            const auto x = oneHit<OpenHat>(fs, hatParams(OH, 1.0f, st), Hit::normal(), 1.8);
            const double expect = 24576.0 / (kHatClock * std::exp2(st / 12.0));
            INFO("tune " << st << " st: ends " << endTime(x, fs) << " s, expected " << expect);
            REQUIRE(std::abs(endTime(x, fs) / expect - 1.0) < 0.003);
        }
        // a per-hit pitch offset moves the clock too
        const auto up = oneHit<OpenHat>(fs, hatParams(OH, 1.0f), Hit{Hit::normal().acc, 12.0f}, 0.6);
        REQUIRE(std::abs(endTime(up, fs) / (24576.0 / (2.0 * kHatClock)) - 1.0) < 0.003);
    }
    // the ZOH's sinc null sits exactly at the clock (research/05 §5.1 method)
    const auto oh = oneHit<OpenHat>(96000.0, hatParams(OH, 1.0f), Hit::normal(), 0.8);
    const double null = zohNullHz(oh, 96000.0, 25000.0, 40000.0, 0.02, 0.75);
    INFO("ZOH null at " << null << " Hz");
    REQUIRE(std::abs(null / kHatClock - 1.0) < 0.01);
    voices::HiHat h;
    h.prepare(setupFor(48000.0));
    REQUIRE(h.clockHz(true) == kHatClock);
}

TEST_CASE("hats: decay to -20 dB matches the service manual, at every rate", "[hats][decay]") {
    // research/02 §7, knobs centred: CH about 100 ms (no accent; 65 ms in the
    // accented photo, a 65..100 ms band), OH about 0.29 s to -14 dB and 0.5 s to -20 dB.
    double ref20Ch = 0.0, ref20Oh = 0.0;
    for (double fs : {48000.0, 44100.0, 96000.0, 192000.0}) {
        const auto ch = bandLimit(oneHit<ClosedHat>(fs, defaultParams(CH), Hit::normal(), 0.4), fs, 16000.0);
        const auto cha = bandLimit(oneHit<ClosedHat>(fs, defaultParams(CH), fullAccent(), 0.4), fs, 16000.0);
        const auto oh = bandLimit(oneHit<OpenHat>(fs, defaultParams(OH), Hit::normal(), 1.0), fs, 16000.0);
        const double c20 = timeToDb(ch, fs, -20.0), ca20 = timeToDb(cha, fs, -20.0);
        const double o14 = timeToDb(oh, fs, -14.0), o20 = timeToDb(oh, fs, -20.0);
        INFO("fs " << fs << ": CH -20 dB " << c20 << " ms (accent " << ca20 << "), OH -14/-20 dB " << o14 << "/" << o20 << " ms");
        REQUIRE(c20 > 65.0);
        REQUIRE(c20 < 120.0);
        REQUIRE(ca20 > 65.0);
        REQUIRE(ca20 < 120.0);
        // +-20 %: the two drawn OH points are 1.72x apart, an exponential gives 1.43x,
        // so no decay is within the drawings' +-10 % of both (research/02 §7)
        REQUIRE(std::abs(o14 / 290.0 - 1.0) < 0.20);
        REQUIRE(std::abs(o20 / 500.0 - 1.0) < 0.20);
        // across rates: the steadier r.m.s.-envelope measure, +-5 %
        const double rc = rmsTimeToDb(ch, fs, -20.0), ro = rmsTimeToDb(oh, fs, -20.0);
        if (fs == 48000.0) {
            ref20Ch = rc;
            ref20Oh = ro;
        } else {
            INFO("r.m.s. -20 dB: CH " << rc << " (48k " << ref20Ch << "), OH " << ro << " (48k " << ref20Oh << ")");
            REQUIRE(std::abs(rc / ref20Ch - 1.0) < 0.05);
            REQUIRE(std::abs(ro / ref20Oh - 1.0) < 0.05);
        }
    }
    // DECAY knobs. CH: the measured knob law (research/05 §5.6, first-crossing
    // r.m.s. -20 dB at DECAY 0/25/50/75/100 %: 20/54/65/79/88 ms), within the
    // 5 ms window's resolution at DECAY 0 and +-15 % elsewhere.
    const double fs = 96000.0;
    const double chTarget[] = {20.0, 54.0, 65.0, 79.0, 88.0};
    for (int i = 0; i < 5; ++i) {
        const float d = 0.25f * static_cast<float>(i);
        const double t = rmsTimeToDb(oneHit<ClosedHat>(fs, hatParams(CH, d), Hit::normal(), 0.4), fs, -20.0);
        INFO("CH DECAY " << d << ": -20 dB at " << t << " ms (measured " << chTarget[i] << ")");
        REQUIRE(std::abs(t - chTarget[i]) <= std::max(5.0, 0.15 * chTarget[i]));
    }
    // OH: grows with its knob, and even at DECAY 1 (tau 1.1 s) it is far below
    // -20 dB when the counter stops at 770 ms (no audible cut).
    const auto o0 = oneHit<OpenHat>(fs, hatParams(OH, 0.0f), Hit::normal(), 1.0);
    const auto o1 = oneHit<OpenHat>(fs, hatParams(OH, 1.0f), Hit::normal(), 1.0);
    const double t0 = rmsTimeToDb(o0, fs, -20.0), t1 = rmsTimeToDb(o1, fs, -20.0);
    const double stopLevel = toDb(rms(o1, fs, 0.74, 0.765) / rms(o1, fs, 0.0, 0.02));
    INFO("OH -20 dB at DECAY 0: " << t0 << " ms, DECAY 1: " << t1 << " ms; DECAY 1 level before the counter stop " << stopLevel << " dB");
    REQUIRE(t0 < 300.0);
    REQUIRE(t1 > 1.4 * t0);
    REQUIRE(t1 < 720.0);
    REQUIRE(stopLevel < -30.0);
}

TEST_CASE("hats: the CH decay moves slightly with the OH knob (paralleled paths)", "[hats][decay]") {
    voices::HiHat h;
    h.prepare(setupFor(48000.0));
    h.setParams(hatParams(CH, 0.5f), hatParams(OH, 0.0f));
    const double tauLo = h.decayTau(true);
    h.setParams(hatParams(CH, 0.5f), hatParams(OH, 1.0f));
    const double tauHi = h.decayTau(true);
    INFO("CH tau with OH knob 0: " << tauLo * 1e3 << " ms, OH knob 1: " << tauHi * 1e3 << " ms");
    REQUIRE(tauHi > tauLo * 1.1);
    REQUIRE(tauHi < tauLo * 1.6);
    // the OH decay does not depend on the CH knob
    h.setParams(hatParams(CH, 0.0f), hatParams(OH, 0.5f));
    const double o1 = h.decayTau(false);
    h.setParams(hatParams(CH, 1.0f), hatParams(OH, 0.5f));
    REQUIRE(h.decayTau(false) == o1);
}

TEST_CASE("hats: Vpp at MULTI OUT and the accent law, at every rate", "[hats][level]") {
    // research/02 §7: CH 0.96 Vpp normal, OH 1.2 Vpp; research/05 §0.11 accent spans.
    // The manual's scope figures are full bandwidth, and the hardware output
    // carries strong images up to ~48 kHz (research/05 §0.8), so the levels are
    // calibrated on the full-band output: a 192 kHz render (96 kHz within 3 %).
    // At 44.1/48 kHz the host Nyquist removes part of the image band, so raw
    // Vpp is lower there; levels are compared across rates in the band every
    // rate carries (<= 16 kHz).
    const double fs0 = 192000.0;
    const double ch = vpp(oneHit<ClosedHat>(fs0, defaultParams(CH), Hit::normal(), 0.3));
    const double cha = vpp(oneHit<ClosedHat>(fs0, defaultParams(CH), fullAccent(), 0.3));
    const double oh = vpp(oneHit<OpenHat>(fs0, defaultParams(OH), Hit::normal(), 0.3));
    const double oha = vpp(oneHit<OpenHat>(fs0, defaultParams(OH), fullAccent(), 0.3));
    const double chMid = vpp(oneHit<ClosedHat>(fs0, defaultParams(CH), Hit::fromCode(49), 0.3));
    INFO("CH " << ch << " Vpp, accent " << cha << " (" << toDb(cha / ch) << " dB), code 49: " << chMid << "; OH " << oh
               << " Vpp, accent " << oha << " (" << toDb(oha / oh) << " dB)");
    REQUIRE(std::abs(ch / 0.96 - 1.0) < 0.03);
    REQUIRE(std::abs(oh / 1.2 - 1.0) < 0.03);
    REQUIRE(std::abs(toDb(cha / ch) - 6.8) < 0.5);
    REQUIRE(std::abs(toDb(oha / oh) - 7.2) < 0.5);
    REQUIRE(std::abs(chMid / 1.6 - 1.0) < 0.07);   // the manual's "with accent" photo = accent code ~49
    // full band at 96 kHz (the reconstruction filter, not the host, sets the band)
    for (int os : {1, 2, 4}) {
        const double c96 = vpp(oneHit<ClosedHat>(96000.0, defaultParams(CH), Hit::normal(), 0.3, os));
        const double o96 = vpp(oneHit<OpenHat>(96000.0, defaultParams(OH), Hit::normal(), 0.3, os));
        const double c192 = vpp(oneHit<ClosedHat>(fs0, defaultParams(CH), Hit::normal(), 0.3, os));
        INFO("oversample " << os << ": CH 96k " << c96 << ", 192k " << c192 << ", OH 96k " << o96);
        REQUIRE(std::abs(c96 / ch - 1.0) < 0.03);
        REQUIRE(std::abs(c192 / ch - 1.0) < 0.03);
        REQUIRE(std::abs(o96 / oh - 1.0) < 0.03);
    }
    // a weak hit is quieter, a 0 V line quieter still
    const double weak = vpp(oneHit<ClosedHat>(fs0, defaultParams(CH), Hit::weak(), 0.3));
    REQUIRE(weak < ch);
    REQUIRE(vpp(oneHit<ClosedHat>(fs0, defaultParams(CH), Hit::fromCode(0), 0.3)) < weak);
    // the same level in the band every host rate carries
    const double refCh = bandVpp(oneHit<ClosedHat>(fs0, defaultParams(CH), Hit::normal(), 0.3), fs0);
    const double refOh = bandVpp(oneHit<OpenHat>(fs0, defaultParams(OH), fullAccent(), 0.3), fs0);
    for (double fs : kRates) {
        const double c = bandVpp(oneHit<ClosedHat>(fs, defaultParams(CH), Hit::normal(), 0.3), fs);
        const double o = bandVpp(oneHit<OpenHat>(fs, defaultParams(OH), fullAccent(), 0.3), fs);
        INFO("fs " << fs << " band-limited Vpp CH " << c << " (192k " << refCh << "), OH accent " << o << " (192k " << refOh << ")");
        REQUIRE(std::abs(c / refCh - 1.0) < 0.03);
        REQUIRE(std::abs(o / refOh - 1.0) < 0.03);
    }
}

TEST_CASE("hats: the TOTAL ACCENT knob sweep", "[hats][level]") {
    // research/05 §10.3: TOTAL ACCENT 0 -> 100 % with the total-accent step on
    // raises the hat by +6.8 dB (CH) / +7.2 dB (OH), roughly linearly in dB, and
    // at 0 % it matches the no-accent hit. (Those recordings are the TOTAL ACCENT
    // step: the open hat has no per-instrument accent on the hardware, and the
    // fixed-strength per-instrument accent would not match the no-accent hit
    // at 0 %.) Hit::accented(false, true, k) is that condition.
    const double fs = 96000.0;
    for (int open = 0; open < 2; ++open) {
        auto level = [&](Hit h) {
            return open ? vpp(oneHit<OpenHat>(fs, defaultParams(OH), h, 0.2)) : vpp(oneHit<ClosedHat>(fs, defaultParams(CH), h, 0.2));
        };
        const double none = level(Hit::normal());
        std::vector<double> db;
        for (int k = 0; k <= 10; ++k) db.push_back(toDb(level(Hit::accented(false, true, 0.1f * static_cast<float>(k))) / none));
        const double span = open ? 7.2 : 6.8;
        INFO((open ? "OH" : "CH") << ": TA 0 " << db[0] << " dB, 50 % " << db[5] << " dB, 100 % " << db[10] << " dB");
        REQUIRE(std::abs(db[0]) < 0.5);
        REQUIRE(std::abs(db[10] - span) < 0.5);
        for (int k = 1; k <= 10; ++k) REQUIRE(db[static_cast<std::size_t>(k)] >= db[static_cast<std::size_t>(k - 1)]);
        // roughly linear in dB: the midpoint within 1 dB of half the span
        REQUIRE(std::abs(db[5] - 0.5 * span) < 1.0);
    }
}

TEST_CASE("hats: choke, simultaneous CH+OH, choke off", "[hats][choke]") {
    const double fs = 48000.0;
    const auto gap = static_cast<std::size_t>(0.1 * fs);
    const VoiceParams pc = defaultParams(CH), po = hatParams(OH, 1.0f);
    // OH, then CH at 100 ms: the open hat is cut, the closed one plays
    const Pair cut = renderPair(fs, 0.5, pc, po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
        if (pos == 0) {
            h.triggerOpen(Hit::normal());
            return gap;
        }
        if (pos == gap) h.triggerClosed(Hit::normal());
        return 0;
    });
    const double before = rms(cut.open, fs, 0.05, 0.1);
    const double after = rms(cut.open, fs, 0.103, 0.3);
    const double closedBefore = rms(cut.closed, fs, 0.0, 0.1);
    const double closedAfter = rms(cut.closed, fs, 0.1, 0.2);
    INFO("open before " << before << " after " << after << "; closed before " << closedBefore << " after " << closedAfter);
    REQUIRE(after < before * 1.0e-3);
    REQUIRE(closedBefore == 0.0);
    REQUIRE(closedAfter > 0.05);
    // an OH hit cuts a ringing CH the same way
    const Pair cut2 = renderPair(fs, 0.4, hatParams(CH, 1.0f), po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
        if (pos == 0) {
            h.triggerClosed(Hit::normal());
            return static_cast<std::size_t>(0.03 * fs);
        }
        h.triggerOpen(Hit::normal());
        return 0;
    });
    REQUIRE(rms(cut2.closed, fs, 0.034, 0.2) < rms(cut2.closed, fs, 0.0, 0.03) * 1.0e-3);
    REQUIRE(rms(cut2.open, fs, 0.03, 0.2) > 0.05);

    // CH and OH on the same sample play open, in either call order, identical to OH alone
    const Pair ohOnly = renderPair(fs, 0.4, pc, po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
        if (pos == 0) h.triggerOpen(Hit::normal());
        return 0;
    });
    for (int order = 0; order < 2; ++order) {
        const Pair both = renderPair(fs, 0.4, pc, po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
            if (pos == 0) {
                if (order == 0) {
                    h.triggerClosed(Hit::normal());
                    h.triggerOpen(Hit::normal());
                } else {
                    h.triggerOpen(Hit::normal());
                    h.triggerClosed(Hit::normal());
                }
            }
            return 0;
        });
        REQUIRE(both.open == ohOnly.open);
        for (float v : both.closed) REQUIRE(v == 0.0f);
    }

    // choke off: two independent circuits, each identical to its own hit
    const Pair chOnly = renderPair(fs, 0.4, pc, po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
        if (pos == 0) h.triggerClosed(Hit::normal());
        return 0;
    });
    const Pair free = renderPair(
        fs, 0.4, pc, po,
        [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
            if (pos == 0) {
                h.triggerClosed(Hit::normal());
                h.triggerOpen(Hit::normal());
            }
            return 0;
        },
        false);
    REQUIRE(free.open == ohOnly.open);
    REQUIRE(free.closed == chOnly.closed);
}

TEST_CASE("hats: switching choke never leaves a hat the hardware would have cut, nor cuts a free one", "[hats][choke]") {
    const double fs = 48000.0;
    const auto at = static_cast<std::size_t>(0.1 * fs);
    const VoiceParams pc = defaultParams(CH), po = hatParams(OH, 1.0f);
    // OH started with choke off, then choke on and a CH hit: the one circuit's
    // retrigger cuts the open hat
    const Pair a = renderPair(
        fs, 0.4, pc, po,
        [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
            if (pos == 0) {
                h.triggerOpen(Hit::normal());
                return at;
            }
            h.setChoke(true);
            h.triggerClosed(Hit::normal());
            return 0;
        },
        false);
    INFO("open hat after the CH hit: " << rms(a.open, fs, 0.104, 0.3) << " (before " << rms(a.open, fs, 0.05, 0.1) << ")");
    REQUIRE(rms(a.open, fs, 0.104, 0.3) < 1.0e-3 * rms(a.open, fs, 0.05, 0.1));
    REQUIRE(rms(a.closed, fs, 0.1, 0.2) > 0.05);
    // OH started with choke on, then choke off and a CH hit: two independent
    // copies, the open hat keeps ringing exactly as it would alone
    const Pair alone = renderPair(fs, 0.4, pc, po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
        if (pos == 0) h.triggerOpen(Hit::normal());
        return 0;
    });
    const Pair b = renderPair(fs, 0.4, pc, po, [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
        if (pos == 0) {
            h.triggerOpen(Hit::normal());
            return at;
        }
        h.setChoke(false);
        h.triggerClosed(Hit::normal());
        return 0;
    });
    REQUIRE(b.open == alone.open);
    REQUIRE(rms(b.closed, fs, 0.1, 0.2) > 0.05);
}

TEST_CASE("hats: Revision::Early keeps the low end (C134 10 µF instead of 0.01 µF)", "[hats][revision]") {
    const double fs = 48000.0;
    auto low = [&](Revision rev) {
        const Pair p = renderPair(
            fs, 0.5, defaultParams(CH), hatParams(OH, 1.0f),
            [&](voices::HiHat& h, std::size_t pos) -> std::size_t {
                if (pos == 0) h.triggerOpen(Hit::normal());
                return 0;
            },
            true, rev);
        const auto s = spectrumOf(p.open, fs, 0.0, 0.45);
        return s.energy(20.0, 400.0) / s.energy(400.0, 20000.0);
    };
    const double late = low(Revision::Late), early = low(Revision::Early);
    INFO("energy below 400 Hz / above: late " << late << ", early " << early);
    REQUIRE(early > 4.0 * late);
}

TEST_CASE("hats: the hardware's images are present, fold-back is rejected", "[hats][images]") {
    // images: the 16..24 kHz band at 48 kHz (the original carries it about 9 dB below
    // its strongest band, research/05 §10.2) and 24..48 kHz at 96 kHz
    {
        const double fs = 48000.0;
        const auto x = oneHit<ClosedHat>(fs, defaultParams(CH), Hit::normal(), 0.3);
        const auto s = spectrumOf(x, fs, 0.0, 0.25);
        const double strongest = std::max({s.energy(2000, 5000), s.energy(5000, 8000), s.energy(8000, 12000)});
        const double img = 10.0 * std::log10(s.energy(16000, 23900) / strongest);
        INFO("16..24 kHz band " << img << " dB re the strongest band");
        REQUIRE(img > -15.0);
        REQUIRE(img < -2.0);
    }
    {
        const double fs = 96000.0;
        const auto x = oneHit<ClosedHat>(fs, defaultParams(CH), Hit::normal(), 0.3);
        const auto s = spectrumOf(x, fs, 0.0, 0.25);
        const double strongest = std::max({s.energy(2000, 5000), s.energy(5000, 8000), s.energy(8000, 12000)});
        const double img = 10.0 * std::log10(s.energy(24000, 47000) / strongest);
        INFO("24..48 kHz band " << img << " dB re the strongest band");
        REQUIRE(img > -25.0);
        REQUIRE(img < -5.0);
    }
    // fold-back: a periodic user image (period 7 words) has lines only at
    // multiples of clock/7; anything else up to 20 kHz (0.45 fs at 44.1 kHz,
    // 0.30 fs at high rates) is aliasing.
    RomImage user{};
    user.data.fill(kRomMidScale);
    for (int i = 0; i < kHatOpenEnd; ++i) {
        const double v = std::sin(2.0 * dsp::kPi * (i % 7) / 7.0) + 0.3 * std::sin(4.0 * dsp::kPi * (i % 7) / 7.0);
        user.data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(32 + std::lround(23.0 * v));
    }
    const double f0 = kHatClock / 7.0;
    for (double fs : kRates) {
        voices::HiHat h;
        h.prepare(setupFor(fs));
        h.setRoms(&factoryRom(RomSlot::Hat), &user);
        VoiceParams po = hatParams(OH, 1.0f);
        po.k[knob::PcmSource] = 2.0f;
        h.setParams(defaultParams(CH), po);
        h.triggerOpen(Hit::normal());
        std::vector<float> a(static_cast<std::size_t>(0.75 * fs)), b(a.size());
        h.render(a.data(), b.data(), static_cast<int>(a.size()));
        const auto s = spectrumOf(b, fs, 0.05, 0.7, 20.0);
        double ref = 0.0, worst = 0.0, at = 0.0;
        for (std::size_t i = 1; i < s.power.size(); ++i) {
            const double f = static_cast<double>(i) * s.binHz;
            const double m = std::round(f / f0);
            const bool onGrid = std::abs(f - m * f0) < 60.0;
            if (onGrid) ref = std::max(ref, s.power[i]);
            else if (f > 1000.0 && f < std::max(std::min(20000.0, 0.45 * fs), 0.30 * fs) && s.power[i] > worst) {
                worst = s.power[i];
                at = f;
            }
        }
        INFO("fs " << fs << ": worst off-grid line " << 10.0 * std::log10(worst / ref) << " dB at " << at << " Hz");
        REQUIRE(10.0 * std::log10(worst / ref) < -90.0);
    }
}

TEST_CASE("hats: robustness, NaN safety, determinism", "[hats][robust]") {
    for (double fs : kRates) {
        checkRobust<ClosedHat>(fs, 10.0);
        checkRobust<OpenHat>(fs, 10.0);
        checkNanSafe<ClosedHat>(fs);
        checkNanSafe<OpenHat>(fs);
    }
    checkDeterminism<ClosedHat>(0.4);
    checkDeterminism<OpenHat>(0.6);
}

TEST_CASE("hats: retrigger carries state, partition independence, sleep and wake", "[hats][state]") {
    checkRetrigger<ClosedHat>(0.03, 0.3);
    checkRetrigger<OpenHat>(0.12, 0.6);
    for (double fs : kRates) {
        checkPartition<ClosedHat>(fs, 0.6);
        checkPartition<OpenHat>(fs, 0.6);
        checkSleepWake<ClosedHat>(fs, 5.0);
        checkSleepWake<OpenHat>(fs, 5.0);
    }
    // the latency is the kernel's half length (dsp::pcm::kernelTaps)
    REQUIRE(ClosedHat{}.latency() == 0);
    ClosedHat a;
    struct L {
        double fs;
        int os, latency;
    };
    for (const L& l : {L{44100.0, 1, 32}, L{48000.0, 1, 24}, L{48000.0, 2, 24}, L{48000.0, 4, 32}, L{96000.0, 1, 16},
                       L{96000.0, 2, 24}, L{96000.0, 4, 24}, L{192000.0, 1, 8}, L{192000.0, 2, 16}}) {
        a.prepare(setupFor(l.fs, l.os));
        INFO("fs " << l.fs << " oversample " << l.os);
        REQUIRE(a.latency() == l.latency);
        REQUIRE(a.latency() <= dsp::pcm::maxKernelLatency(l.fs));
    }
}

TEST_CASE("hats: statistics against the reference recordings", "[hats][reference]") {
    const std::string chRef = findRef("ClosedHat", "ClosedHat909-decay050.wav");
    const std::string ch0Ref = findRef("ClosedHat", "ClosedHat909-decay000.wav");
    const std::string ch1Ref = findRef("ClosedHat", "ClosedHat909-decay100.wav");
    const std::string ohRef = findRef("OpenHat", "OpenHat-accent000-decay100.wav");
    if (chRef.empty() || ch0Ref.empty() || ch1Ref.empty() || ohRef.empty()) SKIP("reference recordings not present");
    const double fs = 96000.0;
    struct Row {
        const char* name;
        std::string path;
        std::vector<float> model;
        std::vector<float> rec{};
        double recFs = 0.0;
    };
    Row rows[] = {{"CH decay 50", chRef, oneHit<ClosedHat>(fs, hatParams(CH, 0.5f), Hit::normal(), 0.5)},
                  {"CH decay 0", ch0Ref, oneHit<ClosedHat>(fs, hatParams(CH, 0.0f), Hit::normal(), 0.5)},
                  {"CH decay 100", ch1Ref, oneHit<ClosedHat>(fs, hatParams(CH, 1.0f), Hit::normal(), 0.5)},
                  {"OH decay 100", ohRef, oneHit<OpenHat>(fs, hatParams(OH, 1.0f), Hit::normal(), 1.0)}};
    for (Row& r : rows) {
        Wav w;
        REQUIRE(readWav(r.path, w));
        removeTailDc(w.x, w.fs);
        r.rec = w.x;
        r.recFs = w.fs;
        const double nRef = zohNullHz(w.x, w.fs, 25000.0, 40000.0, 0.0, 0.25);
        const double nMod = zohNullHz(r.model, fs, 25000.0, 40000.0, 0.0, 0.25);
        const auto sr = spectrumOf(w.x, w.fs, 0.0, 0.25), sm = spectrumOf(r.model, fs, 0.0, 0.25);
        const double cr = sr.centroid(100.0, 20000.0), cm = sm.centroid(100.0, 20000.0);
        auto imgDb = [](const Spectrum& s) {
            const double top = std::max({s.energy(2000, 5000), s.energy(5000, 8000), s.energy(8000, 12000)});
            return 10.0 * std::log10(s.energy(16000, 24000) / top);
        };
        std::printf("[reference] %-13s clock null %6.0f / %6.0f Hz | centroid %5.0f / %5.0f Hz | 16-24k %5.1f / %5.1f dB | "
                    "-10/-20 dB %4.0f/%4.0f vs %4.0f/%4.0f ms (model / original)\n",
                    r.name, nMod, nRef, cm, cr, imgDb(sm), imgDb(sr), rmsTimeToDb(r.model, fs, -10.0),
                    rmsTimeToDb(r.model, fs, -20.0), rmsTimeToDb(w.x, w.fs, -10.0), rmsTimeToDb(w.x, w.fs, -20.0));
        if (std::string(r.name) == "CH decay 0") continue;   // too short for the 0.25 s spectral statistics
        REQUIRE(std::abs(nMod / nRef - 1.0) < 0.02);
        REQUIRE(std::abs(cm / cr - 1.0) < 0.25);
        REQUIRE(std::abs(imgDb(sm) - imgDb(sr)) < 6.0);
    }
    // The CH DECAY knob law (first-crossing r.m.s. -20 dB): centre within 15 %,
    // and the knob's range as the original's (ratios to the centre within 15 %
    // at DECAY 100 and 35 % at DECAY 0, where 5 ms windows quantise ~20 ms).
    auto t20 = [&](const Row& r, bool model) { return model ? rmsTimeToDb(r.model, fs, -20.0) : rmsTimeToDb(r.rec, r.recFs, -20.0); };
    const double mid = t20(rows[0], true), midRec = t20(rows[0], false);
    const double lo = t20(rows[1], true) / mid, loRec = t20(rows[1], false) / midRec;
    const double hi = t20(rows[2], true) / mid, hiRec = t20(rows[2], false) / midRec;
    INFO("CH -20 dB: centre " << mid << " / " << midRec << " ms; DECAY 0 ratio " << lo << " / " << loRec
                              << "; DECAY 100 ratio " << hi << " / " << hiRec << " (model / original)");
    REQUIRE(std::abs(mid / midRec - 1.0) < 0.15);
    REQUIRE(std::abs(lo / loRec - 1.0) < 0.35);
    REQUIRE(std::abs(hi / hiRec - 1.0) < 0.15);
    // The OH at DECAY 100 is NOT asserted against this recording: it reaches
    // -20 dB at ~0.25 s, shorter than the service manual's OH with the knobs
    // centred (~0.49 s), which the brief calibrates to. The two sources
    // conflict; the model follows the manual at the centre and only makes
    // DECAY 100 reach well below -20 dB before the counter stop (decay test).
}

TEST_CASE("hats: CPU per sample", "[hats][cpu]") {
    for (double fs : {48000.0, 96000.0}) {
        const double ch = benchNsPerSample<ClosedHat>(fs, defaultParams(CH));
        const double oh = benchNsPerSample<OpenHat>(fs, defaultParams(OH));
        VoiceParams syn = defaultParams(OH);
        syn.k[knob::PcmSource] = 1.0f;
        const double ohSyn = benchNsPerSample<OpenHat>(fs, syn);
        std::printf("[cpu] hats at %.0f Hz: CH %.1f ns/sample, OH %.1f ns/sample, OH synth %.1f ns/sample\n", fs, ch, oh, ohSyn);
#ifdef NDEBUG
        REQUIRE(ch < 1000.0);
        REQUIRE(oh < 1000.0);
        REQUIRE(ohSyn < 2000.0);
#endif
    }
}
