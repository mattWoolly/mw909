// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Independent review checks for the hand clap (CP). Self-contained: no shared
// measurement helpers. Checks tagged [!mayfail] document defects found in
// review; they report but do not fail the run until the voice is fixed.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "mw909/voices/HandClap.h"
#include "mw909/dsp/Filters.h"
#include "mw909/dsp/Noise.h"

namespace {
std::atomic<long> gAllocs{0};
std::atomic<bool> gCount{false};
}  // namespace

void* operator new(std::size_t n) {
    if (gCount.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n != 0 ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using mw909::Hit;
using mw909::VoiceParams;
using mw909::VoiceSetup;
using mw909::voices::HandClap;

namespace {

float hostile(std::mt19937_64& r) {
    constexpr float c[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), 1.0e-40f, -1.0e-40f, 1.0e30f, -1.0e30f, 0.0f,
                           std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    return c[r() % (sizeof(c) / sizeof(c[0]))];
}
double uni(std::mt19937_64& r) { return std::uniform_real_distribution<double>(0.0, 1.0)(r); }

// dB of a centred moving r.m.s. (window `win` s) at times `ts` after the onset
// (first sample above 10 % of the peak), re the envelope's maximum.
std::vector<double> envDb(const std::vector<double>& x0, double fs, double win, const std::vector<double>& ts) {
    double pk = 0.0;
    for (double v : x0) pk = std::max(pk, std::abs(v));
    std::size_t on = 0;
    while (on < x0.size() && std::abs(x0[on]) < 0.1 * pk) ++on;
    const std::vector<double> x(x0.begin() + static_cast<std::ptrdiff_t>(on), x0.end());
    const auto w = static_cast<std::size_t>(win * fs);
    std::vector<double> c(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) c[i + 1] = c[i] + x[i] * x[i];
    std::vector<double> e(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t a = i >= w / 2 ? i - w / 2 : 0, b = std::min(x.size(), i + w / 2 + 1);
        e[i] = std::sqrt((c[b] - c[a]) / static_cast<double>(b - a));
    }
    const double em = *std::max_element(e.begin(), e.end());
    std::vector<double> out;
    for (double t : ts) {
        const auto i = static_cast<std::size_t>(t * fs);
        out.push_back(i < e.size() ? 20.0 * std::log10(e[i] / em + 1e-30) : -300.0);
    }
    return out;
}

// Minimal RIFF/WAVE reader (PCM 16/24/32 or float 32), first channel.
bool readWav(const std::string& path, std::vector<double>& x, double& fs) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
    auto u16 = [&d](std::size_t o) { return static_cast<unsigned>(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&d](std::size_t o) {
        return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
               (static_cast<std::uint32_t>(d[o + 2]) << 16) | (static_cast<std::uint32_t>(d[o + 3]) << 24);
    };
    unsigned fmt = 0, ch = 1, bits = 0;
    std::size_t o = 12;
    while (o + 8 <= d.size()) {
        const std::uint32_t len = u32(o + 4);
        if (std::memcmp(d.data() + o, "fmt ", 4) == 0) {
            fmt = u16(o + 8);
            ch = u16(o + 10);
            fs = static_cast<double>(u32(o + 12));
            bits = u16(o + 22);
            if (fmt == 0xFFFE) fmt = u16(o + 32);
        } else if (std::memcmp(d.data() + o, "data", 4) == 0) {
            const std::size_t bps = bits / 8, frame = bps * ch, n = std::min<std::size_t>(len, d.size() - o - 8) / frame;
            x.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t p = o + 8 + i * frame;
                double v = 0.0;
                if (fmt == 3 && bits == 32) {
                    float fl;
                    std::memcpy(&fl, d.data() + p, 4);
                    v = fl;
                } else if (bits == 16) {
                    v = static_cast<std::int16_t>(u16(p)) / 32768.0;
                } else if (bits == 24) {
                    const std::int32_t s = static_cast<std::int32_t>((d[p] << 8) | (d[p + 1] << 16) | (d[p + 2] << 24)) >> 8;
                    v = s / 8388608.0;
                } else if (bits == 32) {
                    v = static_cast<std::int32_t>(u32(p)) / 2147483648.0;
                }
                x[i] = v;
            }
            return n > 0;
        }
        o += 8 + len + (len & 1U);
    }
    return false;
}

// The reference set: the folder under the local reference directory that holds
// the per-voice folders (not named here). Empty when absent.
std::string refDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = MW909_REF_SAMPLES_DIR;
    if (!fs::is_directory(root, ec)) return {};
    for (const auto& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && fs::is_directory(e.path() / "RimShot", ec) && fs::is_directory(e.path() / "HandClap", ec))
            return e.path().string();
    return {};
}

std::vector<float> hit(double fs, int code, float reverb, std::uint32_t seed, double seconds) {
    static HandClap v;
    VoiceSetup s;
    s.sampleRate = fs;
    v.prepare(s);
    VoiceParams p = mw909::defaultParams(mw909::CP);
    p.k[mw909::knob::CpReverb] = reverb;
    v.setParams(p);
    const auto n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> noise(n), out(n);
    mw909::dsp::WhiteNoise bus;
    bus.prepare(fs);
    bus.seed(seed);
    bus.fill(noise.data(), static_cast<int>(n));
    v.trigger(Hit::fromCode(code));
    v.render(out.data(), noise.data(), static_cast<int>(n));
    return out;
}

// Power of the 0.5-5 kHz band (two second-order sections) in a centred 10 ms
// window at times ts after the onset (20 % of the peak), re the band's maximum
// in the first 40 ms.
std::vector<double> tailPower(const std::vector<double>& x0, double fs, const std::vector<double>& ts) {
    double pk = 0.0;
    for (double v : x0) pk = std::max(pk, std::abs(v));
    std::size_t on = 0;
    while (on < x0.size() && std::abs(x0[on]) < 0.2 * pk) ++on;
    mw909::dsp::Svf hp, lp;
    hp.setFreqQ(500.0, 0.7071, fs);
    lp.setFreqQ(5000.0, 0.7071, fs);
    std::vector<double> y(x0.size() - on);
    for (std::size_t i = on; i < x0.size(); ++i) {
        hp.tick(x0[i]);
        lp.tick(hp.highpass());
        y[i - on] = lp.lowpass();
    }
    const auto w = static_cast<std::size_t>(0.01 * fs);
    std::vector<double> c(y.size() + 1, 0.0);
    for (std::size_t i = 0; i < y.size(); ++i) c[i + 1] = c[i] + y[i] * y[i];
    auto pw = [&](std::size_t i) {
        const std::size_t a = i >= w / 2 ? i - w / 2 : 0, b = std::min(y.size(), i + w / 2 + 1);
        return (c[b] - c[a]) / static_cast<double>(b - a);
    };
    double m = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.04 * fs); ++i) m = std::max(m, pw(i));
    std::vector<double> out;
    for (double t : ts) out.push_back(pw(static_cast<std::size_t>(t * fs)) / m);
    return out;
}

}  // namespace

TEST_CASE("CP review: prepare, setParams, trigger and render never allocate", "[cp][review]") {
    static HandClap v;
    static float out[4096], noise[4096];
    std::mt19937_64 r(5);
    mw909::dsp::WhiteNoise bus;
    gAllocs = 0;
    for (double fs : {22050.0, 44100.0, 96000.0, 384000.0}) {
        VoiceSetup s;
        s.sampleRate = fs;
        s.variance = 1.0f;
        s.unitSeed = static_cast<std::uint32_t>(r());
        bus.prepare(fs);
        gCount = true;
        v.prepare(s);
        for (int b = 0; b < 200; ++b) {
            VoiceParams p{};
            for (auto& k : p.k) k = r() % 5 == 0 ? hostile(r) : static_cast<float>(uni(r) * 30.0 - 15.0);
            v.setParams(p);
            if (b % 5 == 0) v.trigger(Hit{r() % 4 == 0 ? hostile(r) : mw909::AccentDac::volts(static_cast<int>(r() % 64U)), 0.0f});
            if (b % 7 == 0) v.setAccLine(hostile(r));
            gCount = false;
            bus.fill(noise, 4096);
            gCount = true;
            v.render(out, noise, 1 + static_cast<int>(r() % 4096U));
        }
        gCount = false;
    }
    CHECK(gAllocs.load() == 0);
}

TEST_CASE("CP review: hostile controls at 22.05..384 kHz with random blocks stay finite and bounded", "[cp][review][fuzz]") {
    std::mt19937_64 r(79);
    static float out[8192], noise[8192];
    for (int run = 0; run < 6; ++run) {
        static HandClap v;
        VoiceSetup s;
        s.sampleRate = 22050.0 + uni(r) * (384000.0 - 22050.0);
        s.variance = static_cast<float>(uni(r));
        s.unitSeed = static_cast<std::uint32_t>(r());
        v.prepare(s);
        mw909::dsp::WhiteNoise bus;
        bus.prepare(s.sampleRate);
        bus.seed(static_cast<std::uint32_t>(r()) | 1U);
        double peak = 0.0;
        long bad = 0, done = 0;
        const long total = static_cast<long>(s.sampleRate * 4.0);
        while (done < total) {
            VoiceParams p{};
            for (auto& k : p.k) k = r() % 3 == 0 ? hostile(r) : static_cast<float>(uni(r) * 24.0 - 12.0);
            v.setParams(p);
            if (r() % 3 == 0) v.trigger(Hit{r() % 3 == 0 ? hostile(r) : mw909::AccentDac::volts(static_cast<int>(r() % 64U)), 0.0f});
            if (r() % 5 == 0) v.setAccLine(r() % 2 == 0 ? hostile(r) : static_cast<float>(uni(r) * 5.0));
            const int n = r() % 2 == 0 ? 1 + static_cast<int>(r() % 64U) : 1 + static_cast<int>(r() % 8192U);
            bus.fill(noise, n);
            v.render(out, noise, n);
            for (int i = 0; i < n; ++i) {
                if (!std::isfinite(out[i])) ++bad;
                else peak = std::max(peak, static_cast<double>(std::abs(out[i])));
            }
            done += n;
        }
        INFO("fs " << s.sampleRate << " peak " << peak << " V");
        CHECK(bad == 0);
        CHECK(peak < 20.0);
    }
}

TEST_CASE("CP review: random partitions with mid-run events are bit-identical", "[cp][review][determinism]") {
    const double fs = 44100.0;
    const long n = static_cast<long>(fs * 4.0);
    std::vector<float> noise(static_cast<std::size_t>(n));
    mw909::dsp::WhiteNoise bus;
    bus.prepare(fs);
    bus.seed(4242U);
    bus.fill(noise.data(), static_cast<int>(n));
    auto run = [&](std::uint64_t blockSeed) {
        static HandClap v;
        VoiceSetup s;
        s.sampleRate = fs;
        s.variance = 0.6f;
        s.unitSeed = 7U;
        v.prepare(s);
        std::vector<float> out(static_cast<std::size_t>(n));
        std::mt19937_64 br(blockSeed), ev(11);
        const long every = 7001;
        long i = 0;
        while (i < n) {
            if (i % every == 0) {
                VoiceParams p{};
                p.k[0] = static_cast<float>(uni(ev) * 24.0 - 12.0);
                p.k[1] = static_cast<float>(uni(ev));
                p.k[2] = static_cast<float>(uni(ev));
                v.setParams(p);
                if (ev() % 2 == 0) v.trigger(Hit::fromCode(static_cast<int>(ev() % 64U)));
                if (ev() % 3 == 0) v.setAccLine(static_cast<float>(uni(ev) * 5.0));
            }
            long len = blockSeed == 0 ? 1 : 1 + static_cast<long>(br() % 8192U);
            len = std::min({len, n - i, (i / every + 1) * every - i});
            v.render(out.data() + i, noise.data() + i, static_cast<int>(len));
            i += len;
        }
        return out;
    };
    const auto a = run(0), b = run(3), c = run(9);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
}

// research/05 §2.6: the reverb tail falls at about -96 dB/s (tau ~90 ms); the
// recordings stay exponential to -50 dB at 500 ms (-74 dB/s from 300 to 500 ms).
// Review finding: the 10 MOhm leak in dsp/TransistorVca.h drains C59/C61 below
// Q37's base threshold at ~330 ms and the tail collapses (-198 dB/s; -84 dB at
// 500 ms). Without the leak the model follows the recordings within 1 dB.
TEST_CASE("CP review: the reverb tail stays exponential to 500 ms", "[cp][review][!mayfail]") {
    double p3 = 0.0, p5 = 0.0;
    for (std::uint32_t h = 0; h < 12; ++h) {
        const auto x = hit(48000.0, mw909::AccentDac::kNormalCode, 0.5f, 70U + h, 0.6);
        const auto p = tailPower(std::vector<double>(x.begin(), x.end()), 48000.0, {0.3, 0.5});
        p3 += p[0];
        p5 += p[1];
    }
    const double slope = 10.0 * std::log10(p5 / p3) / 0.2;
    INFO("tail slope 300-500 ms: " << slope << " dB/s (recordings ~ -74, research/05 ~ -96)");
    CHECK(slope > -150.0);
}

TEST_CASE("CP review: the tail at 300 / 400 / 500 ms follows the recordings", "[cp][review][reference][!mayfail]") {
    const std::string dir = refDir();
    if (dir.empty()) SKIP("reference recordings not present");
    const std::vector<double> ts = {0.3, 0.4, 0.5};
    std::vector<double> ref(ts.size(), 0.0), mod(ts.size(), 0.0);
    int nr = 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir + "/HandClap", ec)) {
        std::vector<double> r;
        double rfs = 0.0;
        if (!readWav(e.path().string(), r, rfs)) continue;
        const auto p = tailPower(r, rfs, ts);
        for (std::size_t k = 0; k < ts.size(); ++k) ref[k] += p[k];
        ++nr;
    }
    REQUIRE(nr > 0);
    for (std::uint32_t h = 0; h < 12; ++h) {
        const auto x = hit(48000.0, mw909::AccentDac::kNormalCode, 0.5f, 170U + h, 0.6);
        const auto p = tailPower(std::vector<double>(x.begin(), x.end()), 48000.0, ts);
        for (std::size_t k = 0; k < ts.size(); ++k) mod[k] += p[k];
    }
    for (std::size_t k = 0; k < ts.size(); ++k) {
        const double rd = 10.0 * std::log10(ref[k] / nr), md = 10.0 * std::log10(mod[k] / 12.0);
        INFO("at " << ts[k] * 1e3 << " ms: model " << md << " dB, recordings " << rd << " dB");
        CHECK(std::abs(md - rd) < 4.0);
    }
}

// The trimmer target is 2 Vpp at the multi-out (LEVEL centred); the brief asks
// for level within +-3 % across 44.1..192 kHz. Review finding: the mean Vpp is
// 2.00 at 48 kHz but 2.34 at 192 kHz (+17 %): the burst path has content up to
// the host Nyquist, so the calibrated quantity depends on the host rate.
TEST_CASE("CP review: the 2 Vpp calibration holds at 192 kHz", "[cp][review][!mayfail]") {
    auto meanVpp = [](double fs) {
        double s = 0.0;
        for (std::uint32_t h = 0; h < 16; ++h) {
            const auto x = hit(fs, mw909::AccentDac::kNormalCode, 0.5f, 900U + h, 0.25);
            s += static_cast<double>(*std::max_element(x.begin(), x.end()) - *std::min_element(x.begin(), x.end()));
        }
        return s / 16.0;
    };
    const double v48 = meanVpp(48000.0), v192 = meanVpp(192000.0);
    INFO("mean Vpp 48 kHz " << v48 << ", 192 kHz " << v192);
    CHECK(std::abs(v192 / v48 - 1.0) < 0.05);
}

// architecture §3.3: feed the accent into the circuit, "never as a plain
// post-gain". Review finding: with the reverb path off, an accented hit is the
// normal hit times 1.84695 at every sample (burst gain x (V_acc/V_normal)^0.75):
// accent changes neither the burst depth nor its shape.
TEST_CASE("CP review: accent is not a plain post-gain on the burst path", "[cp][review][!mayfail]") {
    const auto a = hit(48000.0, mw909::AccentDac::kNormalCode, 0.0f, 5U, 0.1);
    const auto b = hit(48000.0, 63, 0.0f, 5U, 0.1);
    double pk = 0.0;
    for (float v : a) pk = std::max(pk, static_cast<double>(std::abs(v)));
    double lo = 1e30, hi = -1e30;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i]) < 1e-3 * pk) continue;
        const double q = static_cast<double>(b[i]) / static_cast<double>(a[i]);
        lo = std::min(lo, q);
        hi = std::max(hi, q);
    }
    INFO("accented / normal, sample by sample: " << lo << " .. " << hi);
    CHECK(hi / lo > 1.01);
}
