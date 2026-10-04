// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Independent review checks of the sample voices (HiHat, Cymbal) and the PCM
// chain, written without the area's own test helpers:
//  * bitwise block-partition independence for random partitions 1..8192 with
//    triggers and parameter changes landing mid-run (Synth source included);
//  * no heap allocation in prepare/reset/setParams/trigger/setAccLine/setRoms/render;
//  * a fuzz over NaN/Inf/denormal/extreme parameters and hits, ROM swaps,
//    choke toggles and audio-thread re-prepares, at 22.05..384 kHz;
//  * contract checks the review found failing, now hard requirements after the
//    fixes: Vpp across host rates (in the common band; full-band calibration),
//    Synth-source hit repeatability, and Eco-quality fold-back up to 20 kHz at
//    44.1 kHz.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <random>
#include <vector>

#include "mw909/Voices.h"
#include "mw909/dsp/PcmBlep.h"
#include "mw909/dsp/PcmCommon.h"
#include "mw909/dsp/Rom.h"
#include "mw909/voices/Cymbal.h"
#include "mw909/voices/HiHat.h"

// ---------------------------------------------------------------------------
// Allocation counter (operator new replaced for this executable; counts only
// while a RealtimeScope is open on the current thread).
// ---------------------------------------------------------------------------
namespace {
thread_local bool tCounting = false;
thread_local long tAllocations = 0;
struct RealtimeScope {
    RealtimeScope() { tCounting = true; }
    ~RealtimeScope() { tCounting = false; }
};
}  // namespace

void* operator new(std::size_t n) {
    if (tCounting) ++tAllocations;
    if (void* p = std::malloc(n != 0U ? n : 1U)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (tCounting) ++tAllocations;
    if (void* p = std::malloc(n != 0U ? n : 1U)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace mw909;

namespace {

VoiceSetup setupAt(double fs, int os = 2) {
    VoiceSetup s;
    s.sampleRate = fs;
    s.oversample = os;
    return s;
}

struct Event {
    long at = 0;
    int kind = 0;   // 0 CH/CR hit, 1 OH/RD hit, 2 params A, 3 params B, 4 accent line
    Hit hit{};
};

std::vector<Event> randomEvents(double fs, long n, unsigned seed) {
    std::mt19937 r(seed);
    std::vector<Event> ev;
    for (long t = 0; t < n; t += 1 + static_cast<long>(r() % static_cast<unsigned>(fs * 0.25))) {
        Event e;
        e.at = t;
        e.kind = static_cast<int>(r() % 5U);
        e.hit = Hit::fromCode(static_cast<int>(r() % 64U));
        ev.push_back(e);
    }
    return ev;
}

template <class NextBlock>
void renderHat(double fs, const std::vector<Event>& ev, long n, NextBlock&& next, std::vector<float>& c, std::vector<float>& o) {
    VoiceParams ca = defaultParams(CH), oa = defaultParams(OH);
    ca.k[knob::PcmTune] = 5.0f;
    ca.k[knob::PcmSource] = 1.0f;   // synth on the closed hat
    oa.k[knob::PcmDecay] = 0.9f;
    voices::HiHat h;
    h.prepare(setupAt(fs));
    h.setRoms(&factoryRom(RomSlot::Hat), nullptr);
    h.setParams(defaultParams(CH), defaultParams(OH));
    c.assign(static_cast<std::size_t>(n), 0.0f);
    o.assign(static_cast<std::size_t>(n), 0.0f);
    long pos = 0;
    std::size_t e = 0;
    while (pos < n) {
        while (e < ev.size() && ev[e].at == pos) {
            const Event& x = ev[e++];
            if (x.kind == 0) h.triggerClosed(x.hit);
            if (x.kind == 1) h.triggerOpen(x.hit);
            if (x.kind == 2) h.setParams(ca, oa);
            if (x.kind == 3) h.setParams(defaultParams(CH), defaultParams(OH));
            if (x.kind == 4) h.setAccLine(x.hit.acc);
        }
        long end = std::min(n, pos + next());
        if (e < ev.size()) end = std::min(end, ev[e].at);
        h.render(c.data() + pos, o.data() + pos, static_cast<int>(end - pos));
        pos = end;
    }
}

template <class NextBlock>
std::vector<float> renderCymbal(double fs, VoiceId which, const std::vector<Event>& ev, long n, NextBlock&& next) {
    VoiceParams a = defaultParams(which);
    a.k[knob::PcmTune] = 0.95f;
    a.k[knob::PcmDecay] = 0.2f;
    a.k[knob::PcmSource] = 1.0f;
    voices::Cymbal v;
    v.prepare(setupAt(fs), which);
    v.setRoms(&factoryRom(which == RD ? RomSlot::Ride : RomSlot::Crash), nullptr);
    v.setParams(defaultParams(which));
    std::vector<float> out(static_cast<std::size_t>(n));
    long pos = 0;
    std::size_t e = 0;
    while (pos < n) {
        while (e < ev.size() && ev[e].at == pos) {
            const Event& x = ev[e++];
            if (x.kind <= 1) v.trigger(x.hit);
            if (x.kind == 2) v.setParams(a);
            if (x.kind == 3) v.setParams(defaultParams(which));
            if (x.kind == 4) v.setAccLine(x.hit.acc);
        }
        long end = std::min(n, pos + next());
        if (e < ev.size()) end = std::min(end, ev[e].at);
        v.render(out.data() + pos, static_cast<int>(end - pos));
        pos = end;
    }
    return out;
}

double rawVpp(const std::vector<float>& x) {
    float lo = 0.0f, hi = 0.0f;
    for (float v : x) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return static_cast<double>(hi - lo);
}

}  // namespace

TEST_CASE("pcm review: random partitions 1..8192 with mid-run events give identical bytes", "[pcm][review]") {
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const auto n = static_cast<long>(fs * 2.5);
        const auto ev = randomEvents(fs, n, static_cast<unsigned>(fs));
        std::mt19937 r(7U);
        auto big = [] { return 8192L; };
        auto one = [] { return 1L; };
        auto rnd = [&] { return 1L + static_cast<long>(r() % 8192U); };
        std::vector<float> c1, o1, c2, o2, c3, o3;
        renderHat(fs, ev, n, big, c1, o1);
        renderHat(fs, ev, n, one, c2, o2);
        renderHat(fs, ev, n, rnd, c3, o3);
        INFO("fs " << fs);
        REQUIRE(c1 == c2);
        REQUIRE(o1 == o2);
        REQUIRE(c1 == c3);
        REQUIRE(o1 == o3);
        for (VoiceId w : {CR, RD}) {
            const auto a = renderCymbal(fs, w, ev, n, big);
            REQUIRE(a == renderCymbal(fs, w, ev, n, one));
            REQUIRE(a == renderCymbal(fs, w, ev, n, rnd));
        }
    }
}

TEST_CASE("pcm review: no heap allocation on the realtime paths", "[pcm][review][realtime]") {
    static voices::HiHat hat;
    static voices::Cymbal crash, ride;
    static RomImage user{};
    std::vector<float> a(4096), b(4096);
    tAllocations = 0;
    {
        RealtimeScope rt;
        for (double fs : {44100.0, 192000.0}) {
            for (int os : {1, 2, 4}) {
                VoiceSetup s = setupAt(fs, os);
                s.variance = 1.0f;
                s.unitSeed = 77U;
                hat.prepare(s);
                crash.prepare(s, CR);
                ride.prepare(s, RD);
                hat.setRoms(&factoryRom(RomSlot::Hat), &user);
                crash.setRoms(&factoryRom(RomSlot::Crash), nullptr);
                ride.setRoms(&factoryRom(RomSlot::Ride), &user);
                for (int src = 0; src < 3; ++src) {
                    VoiceParams pc = defaultParams(CH), po = defaultParams(OH), pr = defaultParams(RD);
                    pc.k[knob::PcmSource] = static_cast<float>(src);
                    po.k[knob::PcmSource] = static_cast<float>(2 - src);
                    pr.k[knob::PcmSource] = static_cast<float>(src);
                    pr.k[knob::PcmColor] = 0.1f * static_cast<float>(src);
                    hat.setParams(pc, po);
                    crash.setParams(pr);
                    ride.setParams(pr);
                    hat.setChoke(src != 1);
                    hat.triggerClosed(Hit::normal());
                    hat.triggerOpen(Hit::fromCode(63));
                    crash.trigger(Hit::normal());
                    ride.trigger(Hit::weak());
                    crash.setAccLine(4.0f);
                    for (int k = 0; k < 8; ++k) {
                        hat.render(a.data(), b.data(), 4096);
                        crash.render(a.data(), 4096);
                        ride.render(b.data(), 1 + k);
                    }
                }
                hat.reset();
                crash.reset();
                ride.reset();
            }
        }
    }
    REQUIRE(tAllocations == 0);
}

TEST_CASE("pcm review: fuzz with non-finite and extreme inputs, ROM swaps and re-prepares", "[pcm][review][fuzz]") {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const float specials[] = {nan, inf, -inf, 1.0e-40f, -1.0e-40f, 1.0e30f, -1.0e30f, 0.0f, 1.0f, -12.0f, 12.0f, 2.0f,
                              std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    static RomImage alt{}, junk{};
    for (int i = 0; i < kRomWords; ++i) alt.data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((i & 1) != 0 ? 63 : 0);
    {
        std::mt19937 r(99U);
        for (auto& v : junk.data) v = static_cast<std::uint8_t>(r() & 0xFFU);   // codes above 63 on purpose
    }
    for (double fs : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        std::mt19937 r(static_cast<unsigned>(fs));
        auto knobValue = [&]() -> float {
            if (r() % 3U == 0U) return specials[r() % (sizeof specials / sizeof specials[0])];
            return std::uniform_real_distribution<float>(-14.0f, 14.0f)(r) * ((r() & 1U) != 0U ? 1.0f : 0.1f);
        };
        auto hitValue = [&]() {
            Hit h;
            h.acc = (r() % 4U == 0U) ? specials[r() % (sizeof specials / sizeof specials[0])]
                                     : std::uniform_real_distribution<float>(-1.0f, 6.0f)(r);
            h.pitchSemis = (r() % 4U == 0U) ? specials[r() % (sizeof specials / sizeof specials[0])]
                                            : std::uniform_real_distribution<float>(-30.0f, 30.0f)(r);
            return h;
        };
        static voices::HiHat hat;
        static voices::Cymbal cym;
        hat.prepare(setupAt(fs));
        cym.prepare(setupAt(fs), CR);
        hat.setRoms(&factoryRom(RomSlot::Hat), nullptr);
        cym.setRoms(&factoryRom(RomSlot::Crash), nullptr);
        std::vector<float> a(8192), b(8192), c(8192);
        const auto total = static_cast<long>(fs * 1.5);
        long pos = 0;
        double worst = 0.0;
        bool finite = true, silentWhenAsleep = true;
        while (pos < total) {
            const int n = (r() % 2U == 0U) ? 1 + static_cast<int>(r() % 64U) : 1 + static_cast<int>(r() % 8192U);
            if (r() % 3U == 0U) {
                VoiceParams p1{}, p2{}, p3{};
                for (std::size_t k = 0; k < static_cast<std::size_t>(kMaxKnobs); ++k) {
                    p1.k[k] = knobValue();
                    p2.k[k] = knobValue();
                    p3.k[k] = knobValue();
                }
                hat.setParams(p1, p2);
                cym.setParams(p3);
            }
            if (r() % 5U == 0U) hat.triggerClosed(hitValue());
            if (r() % 5U == 0U) hat.triggerOpen(hitValue());
            if (r() % 6U == 0U) cym.trigger(hitValue());
            if (r() % 9U == 0U) cym.setAccLine(specials[r() % (sizeof specials / sizeof specials[0])]);
            if (r() % 9U == 0U) hat.setAccLine(specials[r() % (sizeof specials / sizeof specials[0])]);
            if (r() % 15U == 0U) hat.setChoke((r() & 1U) != 0U);
            if (r() % 30U == 0U) {
                const RomImage* u = (r() % 3U == 0U) ? nullptr : ((r() & 1U) != 0U ? &alt : &junk);
                hat.setRoms(&factoryRom(RomSlot::Hat), u);
                cym.setRoms((r() % 4U == 0U) ? nullptr : &factoryRom(RomSlot::Crash), u);
            }
            if (r() % 200U == 0U) {
                VoiceSetup s = setupAt(fs, static_cast<int>(r() % 6U) - 1);
                s.unitSeed = static_cast<std::uint32_t>(r());
                s.variance = (r() & 1U) != 0U ? 1.0f : nan;
                s.revision = (r() & 1U) != 0U ? Revision::Early : Revision::Late;
                hat.prepare(s);
                cym.prepare(s, (r() & 1U) != 0U ? CR : RD);
            }
            const bool hatAwake = hat.isActive(), cymAwake = cym.isActive();
            hat.render(a.data(), b.data(), n);
            cym.render(c.data(), n);
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
                for (float v : {a[i], b[i], c[i]}) {
                    finite = finite && std::isfinite(v);
                    worst = std::max(worst, static_cast<double>(std::abs(v)));
                }
                if (!hatAwake && (a[i] != 0.0f || b[i] != 0.0f)) silentWhenAsleep = false;
                if (!cymAwake && c[i] != 0.0f) silentWhenAsleep = false;
            }
            pos += n;
        }
        INFO("fs " << fs << ": largest |y| " << worst << " V");
        REQUIRE(finite);
        REQUIRE(silentWhenAsleep);
        REQUIRE(worst < 20.0);
    }
}

// Contract §1/§3.7: normal Vpp within ±3 % from 44.1 to 192 kHz. The hardware
// output carries images up to ~48 kHz that only a high host rate can carry, so
// raw Vpp cannot be rate-independent while the images are kept: it is compared
// in the band every rate carries (≤ 16 kHz), and the full-band output (96 and
// 192 kHz, where the reconstruction filter sets the band) carries the
// service-manual calibration. (Originally a [!mayfail] check of raw Vpp; see
// the area report: the contract owner is asked to state the band-limited rule.)
TEST_CASE("pcm review: Vpp the same at every host rate in the common band; full-band calibration", "[pcm][review]") {
    auto hat = [](double fs, bool open) {
        voices::HiHat h;
        h.prepare(setupAt(fs));
        h.setRoms(&factoryRom(RomSlot::Hat), nullptr);
        if (open) h.triggerOpen(Hit::normal());
        else h.triggerClosed(Hit::normal());
        std::vector<float> c(static_cast<std::size_t>(0.4 * fs)), o(c.size());
        h.render(c.data(), o.data(), static_cast<int>(c.size()));
        return open ? o : c;
    };
    auto cym = [](double fs, VoiceId w) {
        voices::Cymbal v;
        v.prepare(setupAt(fs), w);
        v.setRoms(&factoryRom(w == RD ? RomSlot::Ride : RomSlot::Crash), nullptr);
        v.trigger(Hit::normal());
        std::vector<float> y(static_cast<std::size_t>(0.4 * fs));
        v.render(y.data(), static_cast<int>(y.size()));
        return y;
    };
    // band-limited Vpp (FFT brick wall at 16 kHz, 16x interpolated peak search)
    auto bandVpp = [](const std::vector<float>& x, double fs) {
        std::size_t n = 1;
        while (n < 2 * x.size()) n <<= 1;
        std::size_t up = 1;
        while (fs * static_cast<double>(up) < 256000.0) up <<= 1;
        std::vector<std::complex<double>> b(n);
        for (std::size_t i = 0; i < x.size(); ++i) b[i] = x[i];
        auto dft = [](std::vector<std::complex<double>>& a, bool inv) {
            const std::size_t m = a.size();
            for (std::size_t i = 1, j = 0; i < m; ++i) {
                std::size_t bit = m >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                if (i < j) std::swap(a[i], a[j]);
            }
            for (std::size_t len = 2; len <= m; len <<= 1) {
                const double ang = (inv ? 2.0 : -2.0) * dsp::kPi / static_cast<double>(len);
                const std::complex<double> wl(std::cos(ang), std::sin(ang));
                for (std::size_t i = 0; i < m; i += len) {
                    std::complex<double> w(1.0, 0.0);
                    for (std::size_t k = 0; k < len / 2; ++k) {
                        const auto u = a[i + k], v = a[i + k + len / 2] * w;
                        a[i + k] = u + v;
                        a[i + k + len / 2] = u - v;
                        w *= wl;
                    }
                }
            }
        };
        dft(b, false);
        const auto cut = static_cast<std::size_t>(16000.0 / fs * static_cast<double>(n));
        std::vector<std::complex<double>> big(n * up);
        for (std::size_t i = 0; i <= cut; ++i) big[i] = b[i];
        for (std::size_t i = 1; i <= cut; ++i) big[n * up - i] = b[n - i];
        dft(big, true);
        double lo = 0.0, hi = 0.0;
        for (std::size_t i = 0; i < x.size() * up; ++i) {
            const double v = big[i].real() / static_cast<double>(n);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return hi - lo;
    };
    const double c48 = bandVpp(hat(48000.0, false), 48000.0), o48 = bandVpp(hat(48000.0, true), 48000.0);
    const double r48 = bandVpp(cym(48000.0, RD), 48000.0), k48 = bandVpp(cym(48000.0, CR), 48000.0);
    for (double fs : {44100.0, 96000.0, 192000.0}) {
        const double ch = bandVpp(hat(fs, false), fs), oh = bandVpp(hat(fs, true), fs);
        const double cr = bandVpp(cym(fs, CR), fs), rd = bandVpp(cym(fs, RD), fs);
        INFO("fs " << fs << " band-limited: CH " << ch << " (48k " << c48 << "), OH " << oh << " (" << o48 << "), CR " << cr
                   << " (" << k48 << "), RD " << rd << " (" << r48 << ")");
        CHECK(std::abs(ch / c48 - 1.0) < 0.03);
        CHECK(std::abs(oh / o48 - 1.0) < 0.03);
        CHECK(std::abs(cr / k48 - 1.0) < 0.03);
        CHECK(std::abs(rd / r48 - 1.0) < 0.03);
    }
    // full band: 96 vs 192 kHz within 3 %, and 192 kHz on the service-manual figures
    const double hatClosedFull = rawVpp(hat(192000.0, false)), hatOpenFull = rawVpp(hat(192000.0, true));
    const double crashFull = rawVpp(cym(192000.0, CR)), rideFull = rawVpp(cym(192000.0, RD));
    INFO("full band at 192 kHz: CH " << hatClosedFull << ", OH " << hatOpenFull << ", CR " << crashFull << ", RD " << rideFull << " Vpp");
    CHECK(std::abs(rawVpp(hat(96000.0, false)) / hatClosedFull - 1.0) < 0.03);
    CHECK(std::abs(rawVpp(hat(96000.0, true)) / hatOpenFull - 1.0) < 0.03);
    CHECK(std::abs(rawVpp(cym(96000.0, CR)) / crashFull - 1.0) < 0.03);
    CHECK(std::abs(rawVpp(cym(96000.0, RD)) / rideFull - 1.0) < 0.03);
    CHECK(std::abs(hatClosedFull / 0.96 - 1.0) < 0.05);
    CHECK(std::abs(hatOpenFull / 1.2 - 1.0) < 0.05);
    CHECK(std::abs(crashFull / 2.3 - 1.0) < 0.05);
    CHECK(std::abs(rideFull / 2.2 - 1.0) < 0.05);
}

// A Synth hit must not depend on the previous Synth hit: the source ends at
// mid-scale (as a ROM image does), so the held DAC latch does not charge the
// coupling capacitor and the next hit does not start on a decaying offset.
TEST_CASE("pcm review: a Synth-source hit repeats after a finished Synth hit", "[pcm][review]") {
    const double fs = 48000.0;
    for (VoiceId w : {CR, RD}) {
        voices::Cymbal v;
        v.prepare(setupAt(fs), w);
        v.setRoms(&factoryRom(w == RD ? RomSlot::Ride : RomSlot::Crash), nullptr);
        VoiceParams p = defaultParams(w);
        p.k[knob::PcmSource] = 1.0f;
        p.k[knob::PcmColor] = 0.2f;
        v.setParams(p);
        const auto n = static_cast<std::size_t>(3.0 * fs);   // the first hit ends and the voice sleeps
        std::vector<float> first(n), second(n);
        v.trigger(Hit::normal());
        v.render(first.data(), static_cast<int>(n));
        v.trigger(Hit::normal());
        v.render(second.data(), static_cast<int>(n));
        double diff = 0.0;
        for (std::size_t i = 0; i < n; ++i) diff = std::max(diff, static_cast<double>(std::abs(first[i] - second[i])));
        INFO((w == RD ? "ride" : "crash") << ": largest |second - first| " << diff << " V");
        CHECK(diff < 1.0e-3);
    }
    // the hats too (both regions), on the one circuit
    for (int open = 0; open < 2; ++open) {
        voices::HiHat h;
        h.prepare(setupAt(fs));
        h.setRoms(&factoryRom(RomSlot::Hat), nullptr);
        VoiceParams pc = defaultParams(CH), po = defaultParams(OH);
        pc.k[knob::PcmSource] = 1.0f;
        po.k[knob::PcmSource] = 1.0f;
        h.setParams(pc, po);
        const auto n = static_cast<std::size_t>(1.5 * fs);
        std::vector<float> c1(n), o1(n), c2(n), o2(n);
        auto hitIt = [&] {
            if (open) h.triggerOpen(Hit::normal());
            else h.triggerClosed(Hit::normal());
        };
        hitIt();
        h.render(c1.data(), o1.data(), static_cast<int>(n));
        hitIt();
        h.render(c2.data(), o2.data(), static_cast<int>(n));
        double diff = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            diff = std::max({diff, static_cast<double>(std::abs(c1[i] - c2[i])), static_cast<double>(std::abs(o1[i] - o2[i]))});
        INFO((open ? "open" : "closed") << " hat: largest |second - first| " << diff << " V");
        CHECK(diff < 1.0e-3);
    }
}

// Architecture §3.5: fold-back rejected below -90 dB. At 44.1 kHz every
// quality now uses the 64-tap kernel (dsp::pcm::kernelTaps), whose stop band
// starts at 24.1 kHz, so the hat's first image just above Nyquist cannot fold
// into 18..20 kHz. Several tones sweep the folded image across 18.7..19.9 kHz.
TEST_CASE("pcm review: Eco-quality fold-back at 44.1 kHz stays below -90 dB up to 20 kHz", "[pcm][review]") {
    const double fs = 44100.0, clock = 31900.0;
    dsp::AnalogLowpass2 s1, s2;
    dsp::pcm::reconstructionFilter(0U, 0U, 0.0f, s1, s2);
    for (int os : {1, 2, 4}) {
        dsp::PcmStepKernel k;
        k.build(fs, dsp::pcm::kernelTaps(os, fs), s1, s2);
        for (double f0 : {6500.0, 7000.0, 7400.0, 7700.0}) {   // images 25.4/24.9/24.5/24.2 kHz
            dsp::PcmStepAccumulator acc;
            acc.reset();
            const std::size_t skip = 4096, n = 44100;
            std::vector<float> y(skip + n);
            double last = 0.0;
            long step = 0;
            double t = 0.0;
            for (std::size_t i = 0; i < y.size(); ++i) {
                while (t < static_cast<double>(i + 1)) {
                    const double v = std::sin(2.0 * dsp::kPi * f0 * static_cast<double>(step) / clock);
                    acc.addStep(k, static_cast<std::uint32_t>(i) + 1U, t - static_cast<double>(i), static_cast<float>(v - last));
                    last = v;
                    ++step;
                    t = static_cast<double>(step) * fs / clock;
                }
                y[i] = acc.tick(static_cast<std::uint32_t>(i));
            }
            auto amplitude = [&](double f) {
                std::complex<double> s = 0.0;
                double wsum = 0.0;
                for (std::size_t i = 0; i < n; ++i) {
                    const double r = static_cast<double>(i) / static_cast<double>(n - 1);
                    const double w = 0.35875 - 0.48829 * std::cos(2.0 * dsp::kPi * r) + 0.14128 * std::cos(4.0 * dsp::kPi * r) -
                                     0.01168 * std::cos(6.0 * dsp::kPi * r);
                    s += w * static_cast<double>(y[skip + i]) * std::polar(1.0, -2.0 * dsp::kPi * f * static_cast<double>(i) / fs);
                    wsum += w;
                }
                return 2.0 * std::abs(s) / wsum;
            };
            const double aliasDb = 20.0 * std::log10(amplitude(fs - (clock - f0)) / amplitude(f0));
            INFO("oversample " << os << ": fold-back of the " << clock - f0 << " Hz image at " << fs - (clock - f0) << " Hz: "
                               << aliasDb << " dB re the tone");
            CHECK(aliasDb < -90.0);
        }
    }
}
