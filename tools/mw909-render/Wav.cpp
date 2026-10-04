// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace mw909::tools {

namespace {

void put16(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    put16(b, v & 0xFFFFU);
    put16(b, (v >> 16) & 0xFFFFU);
}

void putTag(std::vector<std::uint8_t>& b, const char* tag)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(tag[i]));
}

std::uint32_t get16(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

std::uint32_t get32(const std::uint8_t* p)
{
    return get16(p) | (get16(p + 2) << 16);
}

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

} // namespace

bool parseSampleFormat(const std::string& text, SampleFormat& out)
{
    if (text == "16" || text == "pcm16" || text == "s16")
        out = SampleFormat::Pcm16;
    else if (text == "24" || text == "pcm24" || text == "s24")
        out = SampleFormat::Pcm24;
    else if (text == "32f" || text == "f32" || text == "float" || text == "float32")
        out = SampleFormat::Float32;
    else
        return false;
    return true;
}

const char* sampleFormatName(SampleFormat format)
{
    switch (format)
    {
        case SampleFormat::Pcm16:
            return "16-bit PCM";
        case SampleFormat::Pcm24:
            return "24-bit PCM";
        case SampleFormat::Float32:
            return "32-bit float";
    }
    return "?";
}

double lsbOf(int bitsPerSample, bool isFloat)
{
    if (isFloat || bitsPerSample <= 1)
        return 0.0;
    return 1.0 / std::ldexp(1.0, bitsPerSample - 1);
}

bool writeWav(const std::string& path, const std::vector<const float*>& channels, int numFrames,
              int sampleRate, SampleFormat format, WriteStats* stats, std::string& error)
{
    const auto numChannels = static_cast<std::uint32_t>(channels.size());
    if (numChannels == 0 || numFrames < 0 || sampleRate <= 0)
    {
        error = "invalid WAV parameters";
        return false;
    }

    const bool isFloat = format == SampleFormat::Float32;
    const std::uint32_t bytesPerSample =
        format == SampleFormat::Pcm16 ? 2U : (format == SampleFormat::Pcm24 ? 3U : 4U);
    const std::uint32_t blockAlign = bytesPerSample * numChannels;
    // fmt chunk: 16 bytes for PCM, 18 (cbSize = 0) plus a fact chunk for float.
    const std::uint32_t fmtSize = isFloat ? 18U : 16U;
    const std::uint32_t factBytes = isFloat ? 12U : 0U;
    // Sizes in 64 bits first: a RIFF size field is 32 bits, so a file whose
    // RIFF size does not fit is refused rather than written with a wrapped
    // (corrupt) header.
    const std::uint64_t dataBytes64 = static_cast<std::uint64_t>(blockAlign) * static_cast<std::uint64_t>(numFrames);
    const std::uint64_t riffSize64 = 4U + (8U + fmtSize) + factBytes + (8U + dataBytes64) + (dataBytes64 & 1U);
    if (riffSize64 > 0xFFFFFFFFULL || blockAlign > 0xFFFFU)
    {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "%llu bytes of audio do not fit a WAV file (4 GiB limit); render less (--length / "
                      "--measures), fewer channels or a smaller format",
                      static_cast<unsigned long long>(dataBytes64));
        error = buf;
        return false;
    }
    const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);
    const auto riffSize = static_cast<std::uint32_t>(riffSize64);

    std::vector<std::uint8_t> b;
    b.reserve(64 + static_cast<std::size_t>(dataBytes));

    putTag(b, "RIFF");
    put32(b, riffSize);
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    put32(b, fmtSize);
    put16(b, isFloat ? kFormatFloat : kFormatPcm);
    put16(b, numChannels);
    put32(b, static_cast<std::uint32_t>(sampleRate));
    put32(b, static_cast<std::uint32_t>(sampleRate) * blockAlign);
    put16(b, blockAlign);
    put16(b, bytesPerSample * 8U);
    if (isFloat)
    {
        put16(b, 0);
        putTag(b, "fact");
        put32(b, 4);
        put32(b, static_cast<std::uint32_t>(numFrames));
    }
    putTag(b, "data");
    put32(b, dataBytes);

    WriteStats local;
    const double scale = format == SampleFormat::Pcm16 ? 32768.0 : 8388608.0;
    const double maxInt = scale - 1.0;

    for (int i = 0; i < numFrames; ++i)
    {
        for (std::uint32_t c = 0; c < numChannels; ++c)
        {
            const float x = channels[c][i];
            local.peak = std::max(local.peak, std::abs(x));
            if (isFloat)
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &x, sizeof bits);
                put32(b, bits);
                continue;
            }
            double q = std::round(static_cast<double>(x) * scale);
            if (q > maxInt)
            {
                q = maxInt;
                ++local.clipped;
            }
            else if (q < -scale)
            {
                q = -scale;
                ++local.clipped;
            }
            else if (!std::isfinite(q))
            {
                q = 0.0;
                ++local.clipped;
            }
            const auto v = static_cast<std::uint32_t>(static_cast<std::int32_t>(q));
            b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
            b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
            if (format == SampleFormat::Pcm24)
                b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
        }
    }
    if ((dataBytes & 1U) != 0U)
        b.push_back(0);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        error = "cannot open '" + path + "' for writing";
        return false;
    }
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f)
    {
        error = "write failed for '" + path + "'";
        return false;
    }
    if (stats != nullptr)
        *stats = local;
    return true;
}

bool readWav(const std::string& path, WavData& out, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0
        || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
    {
        error = "'" + path + "' is not a RIFF/WAVE file";
        return false;
    }

    std::uint16_t formatTag = 0;
    int channels = 0, rate = 0, bits = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;

    std::size_t pos = 12;
    while (pos + 8 <= b.size())
    {
        const std::uint8_t* chunk = b.data() + pos;
        const std::size_t size = get32(chunk + 4);
        const std::size_t body = pos + 8;
        const std::size_t avail = std::min(size, b.size() - body);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && avail >= 16)
        {
            formatTag = static_cast<std::uint16_t>(get16(chunk + 8));
            channels = static_cast<int>(get16(chunk + 10));
            rate = static_cast<int>(get32(chunk + 12));
            bits = static_cast<int>(get16(chunk + 22));
            if (formatTag == kFormatExtensible && avail >= 26)
                formatTag = static_cast<std::uint16_t>(
                    get16(chunk + 8 + 24)); // first 2 bytes of the subformat GUID
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            data = b.data() + body;
            dataSize = avail;
        }
        pos = body + size + (size & 1U);
    }

    if (channels <= 0 || rate <= 0 || data == nullptr)
    {
        error = "'" + path + "': missing fmt or data chunk";
        return false;
    }
    const bool isFloat = formatTag == kFormatFloat;
    if (!((formatTag == kFormatPcm && (bits == 16 || bits == 24 || bits == 32))
          || (isFloat && bits == 32)))
    {
        error = "'" + path + "': unsupported format tag " + std::to_string(formatTag) + " / "
                + std::to_string(bits) + " bits";
        return false;
    }

    const std::size_t bytesPerSample = static_cast<std::size_t>(bits / 8);
    const std::size_t count = dataSize / bytesPerSample;
    const std::size_t frames = count / static_cast<std::size_t>(channels);
    out.channels = channels;
    out.sampleRate = rate;
    out.bitsPerSample = bits;
    out.isFloat = isFloat;
    out.samples.assign(frames * static_cast<std::size_t>(channels), 0.0f);

    const double scale = std::ldexp(1.0, bits - 1);
    for (std::size_t i = 0; i < out.samples.size(); ++i)
    {
        const std::uint8_t* p = data + i * bytesPerSample;
        if (isFloat)
        {
            const std::uint32_t v = get32(p);
            float x = 0.0f;
            std::memcpy(&x, &v, sizeof x);
            out.samples[i] = x;
            continue;
        }
        std::int32_t v = 0;
        if (bits == 16)
            v = static_cast<std::int16_t>(get16(p));
        else if (bits == 24)
        {
            std::uint32_t u = static_cast<std::uint32_t>(p[0])
                              | (static_cast<std::uint32_t>(p[1]) << 8)
                              | (static_cast<std::uint32_t>(p[2]) << 16);
            if ((u & 0x800000U) != 0U)
                u |= 0xFF000000U;
            v = static_cast<std::int32_t>(u);
        }
        else
            v = static_cast<std::int32_t>(get32(p));
        out.samples[i] = static_cast<float>(static_cast<double>(v) / scale);
    }
    return true;
}

} // namespace mw909::tools
