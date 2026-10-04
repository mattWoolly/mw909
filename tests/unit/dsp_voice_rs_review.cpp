// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Independent review checks for the rim shot (RS). Self-contained: no shared
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

#include "mw909/voices/RimShot.h"

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
using mw909::voices::RimShot;

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

std::vector<float> hit(double fs, float tuneSemis, float decay, int code, double seconds) {
    static RimShot v;
    VoiceSetup s;
    s.sampleRate = fs;
    v.prepare(s);
    VoiceParams p = mw909::defaultParams(mw909::RS);
    p.k[mw909::knob::RsTune] = tuneSemis;
    p.k[mw909::knob::RsDecay] = decay;
    v.setParams(p);
    std::vector<float> out(static_cast<std::size_t>(seconds * fs));
    v.trigger(Hit::fromCode(code));
    v.render(out.data(), static_cast<int>(out.size()));
    return out;
}

}  // namespace

TEST_CASE("RS review: prepare, setParams, trigger and render never allocate", "[rs][review]") {
    static RimShot v;
    static float out[4096];
    std::mt19937_64 r(5);
    gAllocs = 0;
    for (double fs : {22050.0, 44100.0, 96000.0, 384000.0}) {
        VoiceSetup s;
        s.sampleRate = fs;
        s.variance = 1.0f;
        s.unitSeed = static_cast<std::uint32_t>(r());
        s.revision = fs > 50000.0 ? mw909::Revision::Early : mw909::Revision::Late;
        gCount = true;
        v.prepare(s);
        for (int b = 0; b < 200; ++b) {
            VoiceParams p{};
            for (auto& k : p.k) k = r() % 5 == 0 ? hostile(r) : static_cast<float>(uni(r) * 30.0 - 15.0);
            v.setParams(p);
            if (b % 5 == 0) v.trigger(Hit{r() % 4 == 0 ? hostile(r) : mw909::AccentDac::volts(static_cast<int>(r() % 64U)), 0.0f});
            if (b % 7 == 0) v.setAccLine(hostile(r));
            v.render(out, 1 + static_cast<int>(r() % 4096U));
        }
        gCount = false;
    }
    CHECK(gAllocs.load() == 0);
}

TEST_CASE("RS review: hostile controls at 22.05..384 kHz with random blocks stay finite and bounded", "[rs][review][fuzz]") {
    std::mt19937_64 r(78);
    static float out[8192];
    for (int run = 0; run < 6; ++run) {
        static RimShot v;
        VoiceSetup s;
        s.sampleRate = 22050.0 + uni(r) * (384000.0 - 22050.0);
        s.variance = static_cast<float>(uni(r));
        s.unitSeed = static_cast<std::uint32_t>(r());
        v.prepare(s);
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
            v.render(out, n);
            for (int i = 0; i < n; ++i) {
                if (!std::isfinite(out[i])) ++bad;
                else peak = std::max(peak, static_cast<double>(std::abs(out[i])));
            }
            done += n;
        }
        INFO("fs " << s.sampleRate << " peak " << peak << " V");
        CHECK(bad == 0);
        CHECK(peak < 10.0);
    }
}

TEST_CASE("RS review: random partitions with mid-run events are bit-identical", "[rs][review][determinism]") {
    const double fs = 96000.0;
    const long n = static_cast<long>(fs * 3.0);
    auto run = [&](std::uint64_t blockSeed) {
        static RimShot v;
        VoiceSetup s;
        s.sampleRate = fs;
        s.variance = 0.6f;
        s.unitSeed = 7U;
        v.prepare(s);
        std::vector<float> out(static_cast<std::size_t>(n));
        std::mt19937_64 br(blockSeed), ev(11);
        const long every = 4801;
        long i = 0;
        while (i < n) {
            if (i % every == 0) {
                VoiceParams p{};
                p.k[0] = static_cast<float>(uni(ev) * 24.0 - 12.0);
                p.k[1] = static_cast<float>(uni(ev));
                v.setParams(p);
                if (ev() % 2 == 0) v.trigger(Hit::fromCode(static_cast<int>(ev() % 64U)));
                if (ev() % 3 == 0) v.setAccLine(static_cast<float>(uni(ev) * 5.0));
            }
            long len = blockSeed == 0 ? 1 : 1 + static_cast<long>(br() % 8192U);
            len = std::min({len, n - i, (i / every + 1) * every - i});
            v.render(out.data() + i, static_cast<int>(len));
            i += len;
        }
        return out;
    };
    const auto a = run(0), b = run(3), c = run(9);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0);
}

// architecture §3.6: isActive() goes false once output and state are below about
// -120 dBFS (1e-5 V at kDigitalPerVolt 0.1). Review finding: the sleep test
// looks at the instantaneous |y|, so the voice can sleep at a zero crossing of
// a tail still ringing at ~1.9e-5 V (-115 dBFS).
TEST_CASE("RS review: sleeps only once the tail is below -120 dBFS", "[rs][review][!mayfail]") {
    for (float tune : {0.0f, 12.0f}) {
        for (float decay : {0.0f, 0.5f, 1.0f}) {
            RimShot v;
            VoiceSetup s;
            s.sampleRate = 48000.0;
            v.prepare(s);
            VoiceParams p{};
            p.k[mw909::knob::RsTune] = tune;
            p.k[mw909::knob::RsDecay] = decay;
            v.setParams(p);
            v.trigger(Hit::fromCode(63));
            std::vector<float> y;
            for (int i = 0; i < 48000 * 3 && v.isActive(); ++i) {
                float o;
                v.render(&o, 1);
                y.push_back(o);
            }
            REQUIRE(!v.isActive());
            double last = 0.0;
            for (std::size_t i = y.size() > 240 ? y.size() - 240 : 0; i < y.size(); ++i) last = std::max(last, static_cast<double>(std::abs(y[i])));
            INFO("Tune " << tune << " Decay " << decay << ": slept at " << static_cast<double>(y.size()) / 48.0 << " ms, max |y| in the last 5 ms " << last << " V");
            CHECK(last < 1.0e-5);
        }
    }
}

// Review finding: past ~25 ms the recording keeps a slow tail (-25.5 / -27.5 /
// -33.3 dB at 40 / 50 / 60 ms, 20 ms r.m.s.; it carries a low-frequency
// component that also makes the hit negative-peak dominant), while the model
// falls to -41.5 / -53.1 / -76.9 dB.
TEST_CASE("RS review: the tail after 30 ms follows the recording", "[rs][review][reference][!mayfail]") {
    const std::string dir = refDir();
    if (dir.empty()) SKIP("reference recordings not present");
    std::vector<double> r;
    double rfs = 0.0;
    REQUIRE(readWav(dir + "/RimShot/RimShot909-shot1.wav", r, rfs));
    double dc = 0.0;
    const auto tail = static_cast<std::size_t>(0.05 * rfs);
    for (std::size_t i = r.size() - tail; i < r.size(); ++i) dc += r[i] / static_cast<double>(tail);
    for (double& v : r) v -= dc;
    const auto m = hit(48000.0, 0.0f, 0.5f, mw909::AccentDac::kNormalCode, 0.3);
    const std::vector<double> ts = {0.04, 0.05, 0.06};
    const auto er = envDb(r, rfs, 0.02, ts), em = envDb(std::vector<double>(m.begin(), m.end()), 48000.0, 0.02, ts);
    for (std::size_t k = 0; k < ts.size(); ++k) {
        INFO("at " << ts[k] * 1e3 << " ms: model " << em[k] << " dB, recording " << er[k] << " dB");
        CHECK(std::abs(em[k] - er[k]) < 6.0);
    }
}
