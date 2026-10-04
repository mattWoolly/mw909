// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// mw909-rombuild <sources-dir> <output.cpp>
//
// Reads the four CC0 source recordings (hat_open.wav, hat_closed.wav,
// crash.wav, ride.wav; provenance in assets/rom/PROVENANCE.md), turns each into
// 6-bit ROM content with mw909::romFromAudio, and writes the factory images as
// a C++ source implementing mw909::factoryRom(). The output is deterministic:
// the same sources give the same bytes on the same toolchain.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mw909/dsp/Rom.h"

namespace {

struct Audio {
    std::vector<float> x;
    double fs = 0.0;
    int channels = 0;
    int bits = 0;
};

// RIFF/WAVE reader: PCM 16/24/32-bit and IEEE float 32-bit, any channel
// count (mixed to mono by averaging).
bool readWav(const std::string& path, Audio& a, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open";
        return false;
    }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
               (static_cast<std::uint32_t>(d[o + 2]) << 16) | (static_cast<std::uint32_t>(d[o + 3]) << 24);
    };
    auto u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8); };
    if (d.size() < 12 || std::string(d.begin(), d.begin() + 4) != "RIFF" || std::string(d.begin() + 8, d.begin() + 12) != "WAVE") {
        err = "not a RIFF/WAVE file";
        return false;
    }
    std::uint32_t format = 0;
    std::size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const std::string id(d.begin() + static_cast<std::ptrdiff_t>(pos), d.begin() + static_cast<std::ptrdiff_t>(pos + 4));
        const std::size_t size = u32(pos + 4);
        if (id == "fmt " && pos + 24 <= d.size()) {
            format = u16(pos + 8);
            a.channels = static_cast<int>(u16(pos + 10));
            a.fs = static_cast<double>(u32(pos + 12));
            a.bits = static_cast<int>(u16(pos + 22));
            if (format == 0xFFFEU && size >= 40) format = u16(pos + 32);   // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (id == "data") {
            if (a.channels <= 0 || a.fs <= 0.0) {
                err = "data before fmt";
                return false;
            }
            const int bytes = a.bits / 8;
            const bool pcm = format == 1U && (a.bits == 16 || a.bits == 24 || a.bits == 32);
            const bool flt = format == 3U && a.bits == 32;
            if (!pcm && !flt) {
                err = "unsupported sample format";
                return false;
            }
            const std::size_t frame = static_cast<std::size_t>(bytes * a.channels);
            const std::size_t end = std::min(d.size(), pos + 8 + size);
            for (std::size_t i = pos + 8; i + frame <= end; i += frame) {
                double sum = 0.0;
                for (int c = 0; c < a.channels; ++c) {
                    const std::size_t o = i + static_cast<std::size_t>(c * bytes);
                    double v = 0.0;
                    if (flt) {
                        const std::uint32_t b = u32(o);
                        float fv;
                        std::memcpy(&fv, &b, 4);
                        v = static_cast<double>(fv);
                    } else if (a.bits == 16) {
                        v = static_cast<double>(static_cast<std::int16_t>(u16(o))) / 32768.0;
                    } else if (a.bits == 24) {
                        const auto s = static_cast<std::int32_t>((static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
                                                       (static_cast<std::uint32_t>(d[o + 2]) << 16))
                                                      << 8) >>
                            8;
                        v = static_cast<double>(s) / 8388608.0;
                    } else {
                        v = static_cast<double>(static_cast<std::int32_t>(u32(o))) / 2147483648.0;
                    }
                    sum += v;
                }
                a.x.push_back(static_cast<float>(sum / a.channels));
            }
            return !a.x.empty();
        }
        pos += 8 + size + (size & 1U);
    }
    err = "no data chunk";
    return false;
}

// Per-source voicing EQ (RBJ biquads at the source rate), applied before
// romFromAudio. The CC0 recordings are orchestral cymbals, darker and fuller
// in the low mids than the cymbals the original machine's data came from; the
// EQ moves their band balance towards the original's published statistics
// (research/05 §10.2 band levels; statistics only). Documented in PROVENANCE.md.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad highpass(double f, double q, double fs) {
    const double w = 2.0 * 3.14159265358979323846 * f / fs, al = std::sin(w) / (2.0 * q), c = std::cos(w), a0 = 1.0 + al;
    Biquad b;
    b.b0 = (1.0 + c) / 2.0 / a0;
    b.b1 = -(1.0 + c) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0 * c / a0;
    b.a2 = (1.0 - al) / a0;
    return b;
}

Biquad highShelf(double f, double gainDb, double fs) {
    const double A = std::pow(10.0, gainDb / 40.0), w = 2.0 * 3.14159265358979323846 * f / fs;
    const double c = std::cos(w), al = std::sin(w) / 2.0 * std::sqrt(2.0), sq = 2.0 * std::sqrt(A) * al;
    const double a0 = (A + 1.0) - (A - 1.0) * c + sq;
    Biquad b;
    b.b0 = A * ((A + 1.0) + (A - 1.0) * c + sq) / a0;
    b.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c) / a0;
    b.b2 = A * ((A + 1.0) + (A - 1.0) * c - sq) / a0;
    b.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * c) / a0;
    b.a2 = ((A + 1.0) - (A - 1.0) * c - sq) / a0;
    return b;
}

struct Voicing {
    double hpHz, hpQ, shelfHz, shelfDb;   // hpHz 0: no high-pass; shelfDb 0: no shelf
};

void applyVoicing(Audio& a, const Voicing& v) {
    Biquad hp = highpass(v.hpHz > 0.0 ? v.hpHz : 10.0, v.hpQ, a.fs), sh = highShelf(v.shelfHz, v.shelfDb, a.fs);
    for (float& s : a.x) {
        double y = static_cast<double>(s);
        if (v.hpHz > 0.0) y = hp.run(y);
        if (v.shelfDb != 0.0) y = sh.run(y);
        s = static_cast<float>(y);
    }
}

std::uint32_t fnv1a(const mw909::RomImage& r) {
    std::uint32_t h = 2166136261U;
    for (std::uint8_t v : r.data) {
        h ^= v;
        h *= 16777619U;
    }
    return h;
}

void writeImage(std::ostream& o, const char* name, const mw909::RomImage& r) {
    o << "constexpr RomImage " << name << "{{{\n";
    for (int i = 0; i < mw909::kRomWords; ++i) {
        if (i % 32 == 0) o << "    ";
        o << static_cast<int>(r.data[static_cast<std::size_t>(i)]);
        if (i + 1 < mw909::kRomWords) o << (i % 32 == 31 ? ",\n" : ",");
    }
    o << "\n}}};\n\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: mw909-rombuild <sources-dir> <output.cpp>\n";
        return 2;
    }
    const std::string dir = argv[1];
    struct Job {
        const char* file;
        mw909::RomRegion region;
        mw909::RomImage* image;
        Voicing eq;
    };
    mw909::RomImage hat{}, crash{}, ride{};
    hat.data.fill(mw909::kRomMidScale);
    crash.data.fill(mw909::kRomMidScale);
    ride.data.fill(mw909::kRomMidScale);
    const Job jobs[] = {
        {"hat_open.wav", mw909::RomRegion::HatOpen, &hat, {800.0, 0.5, 6000.0, 0.0}},
        {"hat_closed.wav", mw909::RomRegion::HatClosed, &hat, {0.0, 0.707, 6000.0, 0.0}},
        {"crash.wav", mw909::RomRegion::Crash, &crash, {1000.0, 0.707, 6000.0, 4.0}},
        {"ride.wav", mw909::RomRegion::Ride, &ride, {1000.0, 0.707, 6000.0, 0.0}},
    };
    std::ostringstream notes;
    for (const Job& j : jobs) {
        Audio a;
        std::string err;
        const std::string path = dir + "/" + j.file;
        if (!readWav(path, a, err)) {
            std::cerr << "mw909-rombuild: " << path << ": " << err << "\n";
            return 1;
        }
        applyVoicing(a, j.eq);
        if (!mw909::romFromAudio(a.x.data(), static_cast<int>(a.x.size()), a.fs, j.region, *j.image)) {
            std::cerr << "mw909-rombuild: " << path << ": romFromAudio rejected the input\n";
            return 1;
        }
        notes << "//   " << j.file << ": " << a.x.size() << " samples, " << static_cast<int>(a.fs) << " Hz, " << a.bits
              << "-bit, " << a.channels << " ch; EQ high-pass " << j.eq.hpHz << " Hz Q " << j.eq.hpQ << ", shelf "
              << j.eq.shelfDb << " dB at " << j.eq.shelfHz << " Hz\n";
        std::cout << j.file << ": " << a.x.size() << " samples at " << a.fs << " Hz\n";
    }

    std::ofstream o(argv[2], std::ios::binary);
    if (!o) {
        std::cerr << "mw909-rombuild: cannot write " << argv[2] << "\n";
        return 1;
    }
    char sums[160];
    std::snprintf(sums, sizeof sums, "//   FNV-1a: hat %08x, crash %08x, ride %08x\n", fnv1a(hat), fnv1a(crash), fnv1a(ride));
    o << "// SPDX-License-Identifier: AGPL-3.0-or-later\n"
         "// mw909 \xE2\x80\x94 classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.\n"
         "//\n"
         "// GENERATED FILE: do not edit. The factory 6-bit ROM images (mw909/dsp/Rom.h),\n"
         "// built by tools/rombuild from the CC0 recordings in assets/rom/sources/\n"
         "// (provenance, licences and checksums: assets/rom/PROVENANCE.md) with\n"
         "// mw909::romFromAudio (src/rom/RomBuild.cpp). No data of the original machine.\n"
         "// To regenerate after changing a source or romFromAudio:\n"
         "//   cmake --build <build-dir> --target mw909-rombuild\n"
         "//   <build-dir>/tools/rombuild/mw909-rombuild assets/rom/sources libs/mw909-core/src/rom/RomData.cpp\n"
         "// Inputs:\n"
      << notes.str() << sums
      << "\n#include \"mw909/dsp/Rom.h\"\n\nnamespace mw909 {\n\nnamespace {\n\n";
    writeImage(o, "kHat", hat);
    writeImage(o, "kCrash", crash);
    writeImage(o, "kRide", ride);
    o << "}  // namespace\n\n"
         "const RomImage& factoryRom(RomSlot slot) noexcept {\n"
         "    switch (slot) {\n"
         "        case RomSlot::Crash: return kCrash;\n"
         "        case RomSlot::Ride: return kRide;\n"
         "        case RomSlot::Hat:\n"
         "        case RomSlot::kCount: break;\n"
         "    }\n"
         "    return kHat;\n"
         "}\n\n"
         "}  // namespace mw909\n";
    std::cout << sums;
    return o ? 0 : 1;
}
