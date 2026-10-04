// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "PatternText.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

namespace mw909::tools {

namespace {

std::string lowerText(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upperText(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Track index for a code (0..10 voices, kAccentTrack = AC), or -1.
int trackFromCode(const std::string& code)
{
    const std::string u = upperText(code);
    if (u == "AC")
        return seq::kAccentTrack;
    for (int v = 0; v < kNumVoices; ++v)
        if (kVoiceInfo[static_cast<std::size_t>(v)].code == u)
            return v;
    return -1;
}

bool parseIntText(const std::string& s, int lo, int hi, int& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v < lo || v > hi)
        return false;
    out = static_cast<int>(v);
    return true;
}

bool parseScale(const std::string& s, seq::Scale& out)
{
    const std::string l = lowerText(s);
    if (l == "16" || l == "16th" || l == "s1")
        out = seq::Scale::Sixteenth;
    else if (l == "32" || l == "32nd" || l == "s2")
        out = seq::Scale::ThirtySecond;
    else if (l == "8t" || l == "s3")
        out = seq::Scale::EighthTriplet;
    else if (l == "16t" || l == "s4")
        out = seq::Scale::SixteenthTriplet;
    else if (l == "8" || l == "8th")
        out = seq::Scale::Eighth;
    else
        return false;
    return true;
}

// "0".."95" or "I-1-1".."II-3-16"; -1 if neither.
int parseAddress(const std::string& text)
{
    int v = 0;
    if (parseIntText(text, 0, seq::kNumPatterns - 1, v))
        return v;
    const std::string u = upperText(text);
    const std::size_t d1 = u.find('-');
    if (d1 == std::string::npos)
        return -1;
    const std::size_t d2 = u.find('-', d1 + 1);
    if (d2 == std::string::npos)
        return -1;
    const std::string bank = u.substr(0, d1);
    int b = -1, g = 0, s = 0;
    if (bank == "I")
        b = 0;
    else if (bank == "II")
        b = 1;
    if (b < 0 || !parseIntText(u.substr(d1 + 1, d2 - d1 - 1), 1, seq::kGroupsPerBank, g)
        || !parseIntText(u.substr(d2 + 1), 1, seq::kPatternsPerGroup, s))
        return -1;
    return seq::patternIndex(b, g - 1, s - 1);
}

std::string addressLabel(int index)
{
    return std::string(seq::bankOf(index) == 0 ? "I" : "II") + "-"
           + std::to_string(seq::groupOf(index) + 1) + "-" + std::to_string(seq::slotOf(index) + 1);
}

// Parses the step tokens of one track line into `cells`. Returns the number of
// steps written, or -1 with `error` set.
int parseSteps(const std::string& text, int track, std::array<seq::Step, seq::kMaxSteps>& cells,
               std::string& error)
{
    int n = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '|')
        {
            ++i;
            continue;
        }
        if (n >= seq::kMaxSteps)
        {
            error = "more than 64 steps";
            return -1;
        }
        seq::Step step;
        switch (c)
        {
            case '-':
            case '.':
                break;
            case 'o':
                step.setLevel(seq::StepLevel::Weak);
                break;
            case 'x':
                step.setLevel(seq::StepLevel::Normal);
                break;
            case 'X':
                step.setLevel(seq::StepLevel::Accent);
                break;
            default:
                error = std::string("unknown step character '") + c + "' (use - . o x X)";
                return -1;
        }
        if (track == seq::kAccentTrack && step.on())
            step.setLevel(seq::StepLevel::Normal);
        ++i;

        if (i < text.size() && text[i] == '(')
        {
            const std::size_t close = text.find(')', i);
            if (close == std::string::npos)
            {
                error = "unterminated '(' modifier";
                return -1;
            }
            const std::string mods = text.substr(i + 1, close - i - 1);
            for (std::size_t k = 0; k < mods.size(); ++k)
            {
                const char m = mods[k];
                if (m == 'f' || m == 'F')
                    step.setFlam(true);
                else if (m >= '1' && m <= '4')
                    step.setSubSteps(m - '0');
                else if ((m == 'p' || m == 'P') && k + 1 < mods.size() && mods[k + 1] >= '0'
                         && mods[k + 1] <= '7')
                    step.setProbabilityCode(mods[++k] - '0');
                else if (m != ' ')
                {
                    error = "bad modifier '" + mods + "' (use f, 1..4, p0..p7)";
                    return -1;
                }
            }
            if (!step.on())
            {
                error = "modifier on an off step";
                return -1;
            }
            i = close + 1;
        }
        cells[static_cast<std::size_t>(n++)] = step;
    }
    return n;
}

std::string stepToken(const seq::Step& s, bool accentRow)
{
    std::string t;
    switch (s.level())
    {
        case seq::StepLevel::Off:
            return "-";
        case seq::StepLevel::Weak:
            t = accentRow ? "x" : "o";
            break;
        case seq::StepLevel::Normal:
            t = "x";
            break;
        case seq::StepLevel::Accent:
            t = accentRow ? "x" : "X";
            break;
    }
    std::string mods;
    if (s.flam())
        mods += 'f';
    if (s.subSteps() > 1)
        mods += static_cast<char>('0' + s.subSteps());
    if (s.probabilityCode() != 0)
    {
        mods += 'p';
        mods += static_cast<char>('0' + s.probabilityCode());
    }
    if (!mods.empty())
        t += "(" + mods + ")";
    return t;
}

// Parses a list of addresses, each optionally "*N" repeated, into `entries`.
// Returns the count, or -1 with `error` set.
template <typename Entries>
int parseEntryList(const std::string& rest, Entries& entries, std::string& error)
{
    std::istringstream ls(rest);
    std::string tok;
    int n = 0;
    while (ls >> tok)
    {
        int repeat = 1;
        const std::size_t star = tok.find('*');
        if (star != std::string::npos)
        {
            if (!parseIntText(tok.substr(star + 1), 1, static_cast<int>(entries.size()), repeat))
            {
                error = "bad repeat in '" + tok + "'";
                return -1;
            }
            tok = tok.substr(0, star);
        }
        const int index = parseAddress(tok);
        if (index < 0)
        {
            error = "'" + tok + "' is not a pattern address (0..95 or I-1-1..II-3-16)";
            return -1;
        }
        for (int k = 0; k < repeat; ++k)
        {
            if (n >= static_cast<int>(entries.size()))
            {
                error = "more than " + std::to_string(entries.size()) + " entries";
                return -1;
            }
            entries[static_cast<std::size_t>(n++)] = static_cast<std::uint8_t>(index);
        }
    }
    return n;
}

} // namespace

const char* scaleText(seq::Scale s)
{
    switch (s)
    {
        case seq::Scale::Sixteenth:
            return "16";
        case seq::Scale::ThirtySecond:
            return "32";
        case seq::Scale::EighthTriplet:
            return "8t";
        case seq::Scale::SixteenthTriplet:
            return "16t";
        case seq::Scale::Eighth:
            return "8";
        case seq::Scale::kNumScales:
            break;
    }
    return "16";
}

bool parsePatternText(const std::string& text, PatternText& out, std::string& error)
{
    auto result = std::make_unique<PatternText>();
    int current = -1; // pattern being edited; -1 = none yet (implies 0 on first use)
    int variation = 0;

    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    const auto fail = [&](const std::string& msg) {
        error = "line " + std::to_string(lineNo) + ": " + msg;
        return false;
    };
    const auto select = [&](int index) {
        current = index;
        variation = 0;
        if (std::find(result->defined.begin(), result->defined.end(), index)
            == result->defined.end())
            result->defined.push_back(index);
    };
    const auto pattern = [&]() -> seq::Pattern& {
        if (current < 0)
            select(0);
        return result->bank.patterns[static_cast<std::size_t>(current)];
    };

    while (std::getline(in, raw))
    {
        ++lineNo;
        const std::size_t hash = raw.find('#');
        std::string line = hash == std::string::npos ? raw : raw.substr(0, hash);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key))
            continue;
        std::string rest;
        std::getline(ls, rest);
        const std::size_t first = rest.find_first_not_of(" \t");
        rest = first == std::string::npos ? std::string() : rest.substr(first);
        const std::string k = lowerText(key);
        int v = 0;

        if (k == "tempo")
        {
            char* end = nullptr;
            const double t = std::strtod(rest.c_str(), &end);
            if (rest.empty() || end == nullptr || *end != '\0' || !(t >= 20.0 && t <= 400.0))
                return fail("tempo must be 20..400 BPM");
            result->tempo = t;
        }
        else if (k == "pattern")
        {
            const int index = parseAddress(rest);
            if (index < 0)
                return fail("pattern needs an address: 0..95 or I-1-1..II-3-16");
            select(index);
        }
        else if (k == "chain")
        {
            std::string e;
            const int n = parseEntryList(rest, result->bank.chain.entries, e);
            if (n < 0)
                return fail("chain: " + e);
            result->bank.chain.length = static_cast<std::uint8_t>(n);
        }
        else if (k == "song")
        {
            std::istringstream ss(rest);
            std::string num;
            ss >> num;
            if (!parseIntText(num, 1, seq::kNumSongTracks, v))
                return fail("song needs a track number 1..8");
            std::string list;
            std::getline(ss, list);
            auto& track = result->bank.tracks[static_cast<std::size_t>(v - 1)];
            std::string e;
            const int n = parseEntryList(list, track.entries, e);
            if (n < 0)
                return fail("song: " + e);
            track.length = static_cast<std::uint16_t>(n);
        }
        else if (k == "name")
        {
            pattern();
            result->names[static_cast<std::size_t>(current)] = rest;
        }
        else if (k == "length")
        {
            if (!parseIntText(rest, 1, seq::kMaxSteps, v))
                return fail("length must be 1..64");
            pattern().length = static_cast<std::uint8_t>(v);
        }
        else if (k == "scale")
        {
            if (!parseScale(rest, pattern().scale))
                return fail("scale must be one of 16 32 8t 16t 8");
        }
        else if (k == "shuffle")
        {
            if (!parseIntText(rest, 0, seq::kMaxShuffleTicks, v))
                return fail("shuffle must be 0..16 ticks (1/96 beat)");
            pattern().shuffle = static_cast<std::uint8_t>(v);
        }
        else if (k == "shufflekey")
        {
            if (!parseIntText(rest, 1, 7, v))
                return fail("shufflekey must be 1..7");
            pattern().shuffle = seq::shuffleTicksForKey(v);
        }
        else if (k == "flam")
        {
            if (!parseIntText(rest, seq::kMinFlamMs, seq::kMaxFlamMs, v))
                return fail("flam must be 1..100 ms");
            pattern().flamMs = static_cast<std::uint8_t>(v);
        }
        else if (k == "flamkey")
        {
            if (!parseIntText(rest, 1, 8, v))
                return fail("flamkey must be 1..8");
            pattern().flamMs = seq::flamMsForKey(v);
        }
        else if (k == "flamgrace")
        {
            if (!parseIntText(rest, 0, 100, v))
                return fail("flamgrace must be 0..100");
            pattern().flamGrace = static_cast<std::uint8_t>(v);
        }
        else if (k == "substeps")
        {
            std::istringstream ss(rest);
            std::string a, b;
            ss >> a >> b;
            int s0 = 0, s1 = 0;
            if (!parseIntText(a, 0, 100, s0) || !parseIntText(b, 0, 100, s1))
                return fail("substeps needs two levels 0..100");
            pattern().subStart = static_cast<std::uint8_t>(s0);
            pattern().subEnd = static_cast<std::uint8_t>(s1);
        }
        else if (k == "tracklength" || k == "trackshuffle")
        {
            std::istringstream ts(rest);
            std::string code, num;
            ts >> code >> num;
            const int track = trackFromCode(code);
            if (track < 0 || track >= kNumVoices)
                return fail(k + " needs a voice code (not AC)");
            if (k == "tracklength")
            {
                if (!parseIntText(num, 1, seq::kMaxSteps, v))
                    return fail("track length must be 1..64");
                pattern().trackLength[static_cast<std::size_t>(track)] = static_cast<std::uint8_t>(v);
            }
            else
            {
                if (!parseIntText(num, 0, seq::kMaxShuffleTicks, v))
                    return fail("track shuffle must be 0..16 ticks");
                pattern().trackShuffle[static_cast<std::size_t>(track)] =
                    static_cast<std::uint8_t>(v + 1);
            }
        }
        else if (k == "variation")
        {
            const std::string u = upperText(rest);
            pattern();
            if (u == "A")
                variation = 0;
            else if (u == "B")
                variation = 1;
            else
                return fail("variation must be A or B");
        }
        else
        {
            const int track = trackFromCode(key);
            if (track < 0)
                return fail("unknown keyword or track '" + key + "'");
            std::array<seq::Step, seq::kMaxSteps> cells{};
            std::string stepError;
            if (parseSteps(rest, track, cells, stepError) < 0)
                return fail(stepError);
            pattern().steps[static_cast<std::size_t>(variation)][static_cast<std::size_t>(track)] =
                cells;
        }
    }

    out = *result;
    return true;
}

bool loadPatternText(const std::string& path, PatternText& out, std::string& error)
{
    std::ifstream f(path);
    if (!f)
    {
        error = "cannot open pattern file '" + path + "'";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!parsePatternText(ss.str(), out, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}

std::string formatPatternText(const seq::Pattern& pattern, int index, const std::string& name,
                              double tempo)
{
    std::ostringstream o;
    if (tempo > 0.0)
        o << "tempo    " << tempo << "\n";
    o << "pattern  " << addressLabel(index) << "\n";
    if (!name.empty())
        o << "name     " << name << "\n";
    o << "length   " << static_cast<int>(pattern.length) << "\n";
    o << "scale    " << scaleText(pattern.scale) << "\n";
    o << "shuffle  " << static_cast<int>(pattern.shuffle) << "\n";
    o << "flam     " << static_cast<int>(pattern.flamMs) << "\n";
    if (pattern.flamGrace != 100)
        o << "flamgrace " << static_cast<int>(pattern.flamGrace) << "\n";
    if (pattern.subStart != 100 || pattern.subEnd != 100)
        o << "substeps " << static_cast<int>(pattern.subStart) << " "
          << static_cast<int>(pattern.subEnd) << "\n";
    for (int v = 0; v < kNumVoices; ++v)
    {
        const auto vi = static_cast<std::size_t>(v);
        if (pattern.trackLength[vi] != 0)
            o << "tracklength " << kVoiceInfo[vi].code << " "
              << static_cast<int>(pattern.trackLength[vi]) << "\n";
        if (pattern.trackShuffle[vi] != 0)
            o << "trackshuffle " << kVoiceInfo[vi].code << " "
              << static_cast<int>(pattern.trackShuffle[vi]) - 1 << "\n";
    }

    for (int var = 0; var < seq::kNumVariations; ++var)
    {
        bool any = false;
        for (int t = 0; t < seq::kNumTracks && !any; ++t)
            for (int s = 0; s < seq::kMaxSteps && !any; ++s)
                any = pattern.at(var, t, s).bits != 0;
        if (var == 1 && !any)
            continue;
        o << "variation " << (var == 0 ? "A" : "B") << "\n";
        for (int t = 0; t < seq::kNumTracks; ++t)
        {
            int last = -1;
            for (int s = 0; s < seq::kMaxSteps; ++s)
                if (pattern.at(var, t, s).bits != 0)
                    last = s;
            if (last < 0)
                continue;
            const int count = std::max(last + 1, pattern.lengthOf(t));
            o << (t == seq::kAccentTrack ? std::string_view("AC")
                                         : kVoiceInfo[static_cast<std::size_t>(t)].code)
              << "  ";
            for (int s = 0; s < count; ++s)
            {
                if (s > 0 && s % 4 == 0)
                    o << ' ';
                o << stepToken(pattern.at(var, t, s), t == seq::kAccentTrack);
            }
            o << "\n";
        }
    }
    return o.str();
}

} // namespace mw909::tools
