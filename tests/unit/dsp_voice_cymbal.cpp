// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Cymbal (CR, RD): the measured clock law and TUNE, the address-driven decay
// (it spans the sample and stretches with TUNE), level and accent against the
// service-manual figures (research/02 §5.3, §7), the shared accent line, the
// decay extension, user-ROM swaps, robustness, determinism, retrigger,
// partition independence, sleep/wake, the shared step kernel, reference
// statistics and CPU.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "PcmTestUtil.h"
#include "mw909/dsp/PcmClock.h"

using namespace mw909;
using namespace pcmtest;

namespace {

VoiceParams cymParams(int voice, float tune, float decay = 0.5f) {
    VoiceParams p = defaultParams(voice);
    p.k[knob::PcmTune] = tune;
    p.k[knob::PcmDecay] = decay;
    return p;
}

template <class A>
std::vector<float> hit(double fs, const VoiceParams& p, Hit h = Hit::normal(), double seconds = 1.3) {
    return oneHit<A>(fs, p, h, seconds);
}

}  // namespace

TEST_CASE("cymbal: clock law f = 56.6k/(2 - k), 32768 words, clocks above the host rate", "[cymbal][pitch]") {
    // research/05 §5.1 (measured): 28.3 kHz at TUNE 0, 37.7 at centre, 56.6 at max.
    REQUIRE(std::abs(dsp::pcm::cymbalClockHz(0.0) - 28300.0) < 1.0);
    REQUIRE(std::abs(dsp::pcm::cymbalClockHz(0.5) - 37733.3) < 1.0);
    REQUIRE(std::abs(dsp::pcm::cymbalClockHz(1.0) - 56600.0) < 1.0);
    for (double fs : kRates) {
        for (float k : {0.0f, 0.3f, 0.5f, 0.8f, 1.0f}) {
            const double clock = dsp::pcm::cymbalClockHz(k);
            for (int rd = 0; rd < 2; ++rd) {
                const auto x = rd ? hit<Ride>(fs, cymParams(RD, k, 1.0f), Hit::normal(), 1.4)
                                  : hit<Crash>(fs, cymParams(CR, k, 1.0f), Hit::normal(), 1.4);
                const double t = endTime(x, fs);
                INFO((rd ? "ride" : "crash") << " fs " << fs << " tune " << k << ": ends at " << t << " s, expected "
                                             << kRomWords / clock);
                REQUIRE(std::abs(t * clock / kRomWords - 1.0) < 0.004);
            }
        }
    }
    // the sinc null of the ZOH at the clock (96 kHz render, clocks below 48 kHz)
    for (float k : {0.0f, 0.5f}) {
        const auto x = hit<Crash>(96000.0, cymParams(CR, k, 1.0f), Hit::normal(), 1.0);
        const double clock = dsp::pcm::cymbalClockHz(k);
        const double null = zohNullHz(x, 96000.0, 0.8 * clock, 1.2 * clock, 0.0, 0.8);
        INFO("tune " << k << ": null at " << null << " Hz, clock " << clock);
        REQUIRE(std::abs(null / clock - 1.0) < 0.01);
    }
    // pitch offset per hit
    const auto up = hit<Crash>(48000.0, cymParams(CR, 0.0f, 1.0f), Hit{Hit::normal().acc, 12.0f}, 1.0);
    REQUIRE(std::abs(endTime(up, 48000.0) * 2.0 * 28300.0 / kRomWords - 1.0) < 0.004);
}

TEST_CASE("cymbal: the decay is locked to the address, shaped as the original measures", "[cymbal][decay]") {
    // research/05 §10.2 (an original unit, first-crossing r.m.s.): crash -10/-20/-30
    // dB at 354/607/776 ms (TUNE 50), 439/792/1016 (TUNE 0), 239/457/545 (TUNE 100);
    // ride 194/564/761, 252/662/979, 149/381/525. The service manual's crash
    // drawing (a plateau, -14 dB only at 0.89 of the sample, research/02 §5.3)
    // conflicts with this measurement; the measurement is followed and the
    // manual's figures are printed for reference.
    voices::Cymbal c;
    c.prepare(setupFor(48000.0), CR);
    c.setParams(defaultParams(CR));
    double prev = 1.0;
    for (int st = 0; st < 64; ++st) {   // a staircase that only falls
        REQUIRE(c.envelopeAt(st) <= prev);
        prev = c.envelopeAt(st);
    }
    REQUIRE(toDb(c.envelopeAt(63)) < -35.0);   // the recordings end about 40 dB down
    c.prepare(setupFor(48000.0), RD);
    c.setParams(defaultParams(RD));
    REQUIRE(toDb(c.envelopeAt(63)) < -35.0);

    struct Target {
        float tune;
        double crash[3], ride[3];
    };
    const Target targets[] = {{0.0f, {439, 792, 1016}, {252, 662, 979}},
                              {0.5f, {354, 607, 776}, {194, 564, 761}},
                              {1.0f, {239, 457, 545}, {149, 381, 525}}};
    const double dbs[] = {-10.0, -20.0, -30.0};
    for (const Target& t : targets) {
        const double fs = 96000.0;
        const auto cr = hit<Crash>(fs, cymParams(CR, t.tune), Hit::normal(), 1.4);
        const auto rd = hit<Ride>(fs, cymParams(RD, t.tune), Hit::normal(), 1.4);
        for (int i = 0; i < 3; ++i) {
            const double mc = rmsTimeToDb(cr, fs, dbs[i]), mr = rmsTimeToDb(rd, fs, dbs[i]);
            INFO("TUNE " << t.tune << " " << dbs[i] << " dB: crash " << mc << " ms (original " << t.crash[i] << "), ride " << mr
                         << " ms (original " << t.ride[i] << ")");
            // +-12 %, the ride's -10 dB point at TUNE 100 +-25 % (its early
            // envelope is the recorded content's, not the law's)
            REQUIRE(std::abs(mc / t.crash[i] - 1.0) < 0.12);
            REQUIRE(std::abs(mr / t.ride[i] - 1.0) < (t.tune == 1.0f && i == 0 ? 0.25 : 0.12));
        }
    }
    {
        const double fs = 48000.0, len = kRomWords / dsp::pcm::kCymbalClockCentreHz;
        const auto cr = bandLimit(hit<Crash>(fs, defaultParams(CR)), fs, 16000.0);
        const auto rd = bandLimit(hit<Ride>(fs, defaultParams(RD)), fs, 16000.0);
        std::printf("[cymbal] service-manual drawing vs model (fraction of the sample): crash -14 dB 0.89 vs %.2f; "
                    "ride -14/-20 dB 0.30/0.49 vs %.2f/%.2f\n",
                    timeToDb(cr, fs, -14.0) / 1000.0 / len, timeToDb(rd, fs, -14.0) / 1000.0 / len,
                    timeToDb(rd, fs, -20.0) / 1000.0 / len);
    }

    // the same decay at every host rate (+-5 %, in the band every rate carries;
    // 20 ms windows, so a dip of the content near the threshold cannot decide it)
    double refCr = 0.0, refRd = 0.0;
    for (double fs : {48000.0, 44100.0, 96000.0, 192000.0}) {
        const auto cr = bandLimit(hit<Crash>(fs, defaultParams(CR)), fs, 16000.0);
        const auto rd = bandLimit(hit<Ride>(fs, defaultParams(RD)), fs, 16000.0);
        const double rc = rmsTimeToDb(cr, fs, -20.0, 0.02), rr = rmsTimeToDb(rd, fs, -20.0, 0.02);
        if (fs == 48000.0) {
            refCr = rc;
            refRd = rr;
        } else {
            INFO("fs " << fs << ": -20 dB crash " << rc << " (48k " << refCr << "), ride " << rr << " (48k " << refRd << ")");
            REQUIRE(std::abs(rc / refCr - 1.0) < 0.05);
            REQUIRE(std::abs(rr / refRd - 1.0) < 0.05);
        }
    }
    // the decay stretches with TUNE by the clock ratio (2.0 from TUNE 0 to max)
    const double fs = 48000.0;
    const double slow = rmsTimeToDb(hit<Crash>(fs, cymParams(CR, 0.0f), Hit::normal(), 1.5), fs, -10.0);
    const double fast = rmsTimeToDb(hit<Crash>(fs, cymParams(CR, 1.0f), Hit::normal(), 1.5), fs, -10.0);
    INFO("crash -10 dB at TUNE 0: " << slow << " ms, TUNE max: " << fast << " ms");
    REQUIRE(std::abs(slow / fast / 2.0 - 1.0) < 0.08);
}

TEST_CASE("cymbal: Decay extension: stock at 0.5, longer above, shorter below", "[cymbal][decay]") {
    const double fs = 48000.0;
    const auto stock = hit<Crash>(fs, defaultParams(CR), Hit::normal(), 0.5);
    REQUIRE(hit<Crash>(fs, cymParams(CR, 0.5f, 0.5f), Hit::normal(), 0.5) == stock);
    const double t0 = rmsTimeToDb(hit<Ride>(fs, cymParams(RD, 0.5f, 0.0f)), fs, -10.0);
    const double t5 = rmsTimeToDb(hit<Ride>(fs, cymParams(RD, 0.5f, 0.5f)), fs, -10.0);
    const double t1 = rmsTimeToDb(hit<Ride>(fs, cymParams(RD, 0.5f, 1.0f)), fs, -10.0);
    INFO("ride -10 dB: decay 0 " << t0 << ", 0.5 " << t5 << ", 1 " << t1 << " ms");
    REQUIRE(t0 < 0.8 * t5);
    REQUIRE(t1 > 1.5 * t5);
}

TEST_CASE("cymbal: Vpp at MULTI OUT and the accent law, at every rate", "[cymbal][level]") {
    // research/02 §7: crash 2.3 Vpp, ride 2.2 Vpp; research/05 §0.11: +7.9 / +7.7 dB.
    // Calibrated on the full-band output (192 kHz; 96 kHz within 3 %), as the
    // manual's scope saw it; across rates compared in the band every rate carries.
    const double fs0 = 192000.0;
    const double cr = vpp(hit<Crash>(fs0, defaultParams(CR), Hit::normal(), 0.4));
    const double cra = vpp(hit<Crash>(fs0, defaultParams(CR), fullAccent(), 0.4));
    const double rd = vpp(hit<Ride>(fs0, defaultParams(RD), Hit::normal(), 0.4));
    const double rda = vpp(hit<Ride>(fs0, defaultParams(RD), fullAccent(), 0.4));
    INFO("crash " << cr << " Vpp (+" << toDb(cra / cr) << " dB accented), ride " << rd << " Vpp (+" << toDb(rda / rd) << " dB)");
    REQUIRE(std::abs(cr / 2.3 - 1.0) < 0.03);
    REQUIRE(std::abs(rd / 2.2 - 1.0) < 0.03);
    REQUIRE(std::abs(toDb(cra / cr) - 7.9) < 0.5);
    REQUIRE(std::abs(toDb(rda / rd) - 7.7) < 0.5);
    REQUIRE(vpp(hit<Crash>(fs0, defaultParams(CR), Hit::weak(), 0.4)) < cr);
    for (int os : {1, 2, 4}) {
        const double c96 = vpp(oneHit<Crash>(96000.0, defaultParams(CR), Hit::normal(), 0.4, os));
        const double r96 = vpp(oneHit<Ride>(96000.0, defaultParams(RD), Hit::normal(), 0.4, os));
        const double r192 = vpp(oneHit<Ride>(fs0, defaultParams(RD), Hit::normal(), 0.4, os));
        INFO("oversample " << os << ": crash 96k " << c96 << ", ride 96k " << r96 << ", ride 192k " << r192);
        REQUIRE(std::abs(c96 / cr - 1.0) < 0.03);
        REQUIRE(std::abs(r96 / rd - 1.0) < 0.03);
        REQUIRE(std::abs(r192 / rd - 1.0) < 0.03);
    }
    // TOTAL ACCENT sweep (the total-accent step, research/05 §10.3): 0 % = no
    // accent, monotone, roughly linear in dB
    for (int ride = 0; ride < 2; ++ride) {
        auto level = [&](Hit h) {
            return ride ? vpp(hit<Ride>(fs0, defaultParams(RD), h, 0.2)) : vpp(hit<Crash>(fs0, defaultParams(CR), h, 0.2));
        };
        const double none = level(Hit::normal());
        const double d0 = toDb(level(Hit::accented(false, true, 0.0f)) / none);
        const double d5 = toDb(level(Hit::accented(false, true, 0.5f)) / none);
        const double d10 = toDb(level(Hit::accented(false, true, 1.0f)) / none);
        const double span = ride ? 7.7 : 7.9;
        INFO((ride ? "ride" : "crash") << " TA 0/50/100 %: " << d0 << " / " << d5 << " / " << d10 << " dB");
        REQUIRE(std::abs(d0) < 0.5);
        REQUIRE(std::abs(d10 - span) < 0.5);
        REQUIRE(std::abs(d5 - 0.5 * span) < 1.0);
    }
    const double refCr = bandLimitedVpp(hit<Crash>(fs0, defaultParams(CR), Hit::normal(), 0.4), fs0, 16000.0);
    const double refRd = bandLimitedVpp(hit<Ride>(fs0, defaultParams(RD), fullAccent(), 0.4), fs0, 16000.0);
    for (double fs : kRates) {
        const double c = bandLimitedVpp(hit<Crash>(fs, defaultParams(CR), Hit::normal(), 0.4), fs, 16000.0);
        const double r = bandLimitedVpp(hit<Ride>(fs, defaultParams(RD), fullAccent(), 0.4), fs, 16000.0);
        INFO("fs " << fs << ": band-limited Vpp crash " << c << " (192k " << refCr << "), ride accent " << r << " (192k " << refRd << ")");
        REQUIRE(std::abs(c / refCr - 1.0) < 0.03);
        REQUIRE(std::abs(r / refRd - 1.0) < 0.03);
    }
}

TEST_CASE("cymbal: the shared accent line is read continuously", "[cymbal][accent]") {
    // The ride fires with full accent while the crash rings: the engine rewrites
    // the crash's accent line, and the crash gets louder from then on.
    const double fs = 48000.0;
    auto run = [&](bool rewrite) {
        Crash a;
        a.prepare(setupFor(fs));
        a.setParams(defaultParams(CR));
        a.trigger(Hit::normal());
        std::vector<float> out(static_cast<std::size_t>(0.6 * fs));
        const auto at = static_cast<std::size_t>(0.3 * fs);
        a.render(out.data(), static_cast<int>(at));
        if (rewrite) a.voice.setAccLine(AccentDac::volts(AccentDac::kMaxCode));
        a.render(out.data() + at, static_cast<int>(out.size() - at));
        return out;
    };
    const auto plain = run(false), lifted = run(true);
    REQUIRE(rms(lifted, fs, 0.0, 0.3) == rms(plain, fs, 0.0, 0.3));
    const double gain = toDb(rms(lifted, fs, 0.31, 0.6) / rms(plain, fs, 0.31, 0.6));
    INFO("level after the line rewrite: +" << gain << " dB");
    REQUIRE(std::abs(gain - 7.9) < 0.5);
}

TEST_CASE("cymbal: a user ROM swapped in mid-ring is read from the next clock edge", "[cymbal][rom]") {
    const double fs = 48000.0;
    RomImage user{};
    user.data.fill(kRomMidScale);
    for (int i = 0; i < kRomWords; ++i) user.data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((i / 8) % 2 ? 50 : 14);
    auto run = [&](bool swap) {
        voices::Cymbal c;
        c.prepare(setupFor(fs), RD);
        c.setRoms(&factoryRom(RomSlot::Ride), nullptr);
        VoiceParams p = defaultParams(RD);
        p.k[knob::PcmSource] = 2.0f;   // User: falls back to the factory image while empty
        c.setParams(p);
        c.trigger(Hit::normal());
        std::vector<float> out(static_cast<std::size_t>(0.4 * fs));
        const auto at = static_cast<std::size_t>(0.2 * fs);
        c.render(out.data(), static_cast<int>(at));
        if (swap) c.setRoms(&factoryRom(RomSlot::Ride), &user);
        c.render(out.data() + at, static_cast<int>(out.size() - at));
        return out;
    };
    const auto a = run(false), b = run(true);
    REQUIRE(rms(a, fs, 0.0, 0.2) == rms(b, fs, 0.0, 0.2));
    REQUIRE(a != b);
    REQUIRE(allFinite(b));
    // the square-wave image: clock/16 dominates after the swap
    const auto s = spectrumOf(b, fs, 0.21, 0.4);
    const double f0 = dsp::pcm::kCymbalClockCentreHz / 16.0;
    REQUIRE(s.energy(f0 - 30.0, f0 + 30.0) > 0.2 * s.energy(100.0, 20000.0));
}

TEST_CASE("cymbal: robustness, NaN safety, determinism", "[cymbal][robust]") {
    for (double fs : kRates) {
        checkRobust<Crash>(fs, 10.0);
        checkRobust<Ride>(fs, 10.0);
        checkNanSafe<Crash>(fs);
        checkNanSafe<Ride>(fs);
    }
    checkDeterminism<Crash>(1.0);
    checkDeterminism<Ride>(1.0);
}

TEST_CASE("cymbal: retrigger carries state, partition independence, sleep and wake", "[cymbal][state]") {
    checkRetrigger<Crash>(0.2, 1.0);
    checkRetrigger<Ride>(0.1, 1.0);
    for (double fs : kRates) {
        checkPartition<Crash>(fs, 0.8);
        checkPartition<Ride>(fs, 0.8);
        checkSleepWake<Crash>(fs, 8.0);
        checkSleepWake<Ride>(fs, 8.0);
    }
}

TEST_CASE("cymbal: a shared step kernel is used only while it is the one this voice needs", "[cymbal][prepare]") {
    // The engine passes the hat's kernel to the crash and the crash's to the
    // ride; at variance 0 all three filters are the same and the table is shared.
    auto hat = std::make_unique<voices::HiHat>();
    auto cr = std::make_unique<voices::Cymbal>();
    auto rd = std::make_unique<voices::Cymbal>();
    const VoiceSetup nominal = setupFor(48000.0, 2, 7, 0.0f);
    hat->prepare(nominal);
    cr->prepare(nominal, CR, &hat->kernel());
    rd->prepare(nominal, RD, &cr->kernel());
    REQUIRE(&cr->kernel() == &hat->kernel());
    REQUIRE(&rd->kernel() == &hat->kernel());
    REQUIRE(cr->kernelCurrent());
    REQUIRE(rd->kernelCurrent());
    // Re-preparing the hat alone with other filters changes the shared table
    // under the cymbals: kernelCurrent() (asserted by the engine and by debug
    // renders) reports it.
    hat->prepare(setupFor(48000.0, 2, 7, 1.0f));
    REQUIRE_FALSE(cr->kernelCurrent());
    REQUIRE_FALSE(rd->kernelCurrent());
    // With variance the filters differ: each cymbal builds its own.
    const VoiceSetup spread = setupFor(48000.0, 2, 7, 1.0f);
    cr->prepare(spread, CR, &hat->kernel());
    rd->prepare(spread, RD, &cr->kernel());
    REQUIRE(&cr->kernel() != &hat->kernel());
    REQUIRE(&rd->kernel() != &cr->kernel());
    REQUIRE(cr->kernelCurrent());
    REQUIRE(rd->kernelCurrent());
}

TEST_CASE("cymbal: statistics against the reference recordings", "[cymbal][reference]") {
    const char* tunes[] = {"000", "050", "100"};
    std::string crRef[3], rdRef[3];
    for (int i = 0; i < 3; ++i) {
        crRef[i] = findRef("Crash", std::string("Crash909-tune") + tunes[i] + ".wav");
        rdRef[i] = findRef("Ride", std::string("Ride909-tune") + tunes[i] + ".wav");
        if (crRef[i].empty() || rdRef[i].empty()) SKIP("reference recordings not present");
    }
    const double fs = 96000.0;
    for (int rd = 0; rd < 2; ++rd) {
        double model20[3], orig20[3];
        for (int i = 0; i < 3; ++i) {
            const float tune = 0.5f * static_cast<float>(i);
            const auto m = rd ? hit<Ride>(fs, cymParams(RD, tune), Hit::normal(), 1.4) : hit<Crash>(fs, cymParams(CR, tune), Hit::normal(), 1.4);
            Wav w;
            REQUIRE(readWav(rd ? rdRef[i] : crRef[i], w));
            removeTailDc(w.x, w.fs);
            const double clock = dsp::pcm::cymbalClockHz(tune);
            model20[i] = rmsTimeToDb(m, fs, -20.0);
            orig20[i] = rmsTimeToDb(w.x, w.fs, -20.0);
            std::printf("[reference] %-5s tune %s | -10/-20/-30 dB %4.0f/%4.0f/%4.0f vs %4.0f/%4.0f/%4.0f ms (model / original)\n",
                        rd ? "ride" : "crash", tunes[i], rmsTimeToDb(m, fs, -10.0), model20[i], rmsTimeToDb(m, fs, -30.0),
                        rmsTimeToDb(w.x, w.fs, -10.0), orig20[i], rmsTimeToDb(w.x, w.fs, -30.0));
            if (i == 1) {
                // clock null and spectral centroid at TUNE 50 (clock below the 48 kHz recording Nyquist)
                const double nRef = zohNullHz(w.x, w.fs, 0.8 * clock, 1.2 * clock, 0.0, 0.5);
                const double nMod = zohNullHz(m, fs, 0.8 * clock, 1.2 * clock, 0.0, 0.5);
                const auto sr = spectrumOf(w.x, w.fs, 0.0, 0.3), sm = spectrumOf(m, fs, 0.0, 0.3);
                const double cr = sr.centroid(100.0, 20000.0), cm = sm.centroid(100.0, 20000.0);
                std::printf("[reference] %-5s tune 050 | clock null %6.0f / %6.0f Hz | centroid %5.0f / %5.0f Hz\n",
                            rd ? "ride" : "crash", nMod, nRef, cm, cr);
                REQUIRE(std::abs(nMod / nRef - 1.0) < 0.03);
                REQUIRE(std::abs(cm / cr - 1.0) < 0.25);
            }
        }
        // decay to -20 dB: within 15 % at TUNE 50, and its stretch with TUNE
        // (ratios to TUNE 50) within 12 %
        INFO((rd ? "ride" : "crash") << " -20 dB at TUNE 0/50/100: " << model20[0] << "/" << model20[1] << "/" << model20[2]
                                     << " ms vs " << orig20[0] << "/" << orig20[1] << "/" << orig20[2] << " ms");
        REQUIRE(std::abs(model20[1] / orig20[1] - 1.0) < 0.15);
        REQUIRE(std::abs((model20[0] / model20[1]) / (orig20[0] / orig20[1]) - 1.0) < 0.12);
        REQUIRE(std::abs((model20[2] / model20[1]) / (orig20[2] / orig20[1]) - 1.0) < 0.12);
    }
}

TEST_CASE("cymbal: CPU per sample", "[cymbal][cpu]") {
    for (double fs : {48000.0, 96000.0}) {
        const double cr = benchNsPerSample<Crash>(fs, defaultParams(CR));
        const double rdMax = benchNsPerSample<Ride>(fs, cymParams(RD, 1.0f));
        VoiceParams syn = defaultParams(CR);
        syn.k[knob::PcmSource] = 1.0f;
        const double crSyn = benchNsPerSample<Crash>(fs, syn);
        std::printf("[cpu] cymbal at %.0f Hz: crash %.1f ns/sample, ride at max TUNE (56.6 kHz clock) %.1f ns/sample, "
                    "crash synth %.1f ns/sample\n",
                    fs, cr, rdMax, crSyn);
#ifdef NDEBUG
        REQUIRE(cr < 1000.0);
        REQUIRE(rdMax < 1500.0);
        REQUIRE(crSyn < 2500.0);
#endif
    }
}
