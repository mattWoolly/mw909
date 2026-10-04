// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Shared plumbing of the mw909-render subcommands: argument reading, value
// parsing, the sound setup (kits, mixes, --set overrides), bus storage, the
// offline Machine driver and the pattern sources.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mw909/Machine.h"

namespace mw909::tools {

// Bad command line: printed with a hint, exit code 2.
struct UsageError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Runtime failure (I/O, invalid file content, a failed check): exit code 1.
struct RunError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// Walks "--name value", "--name=value" and bare flags.
class ArgReader
{
public:
    explicit ArgReader(std::vector<std::string> args)
        : args_(std::move(args))
    {}

    bool more() const { return pos_ < args_.size(); }
    // Next option name (e.g. "--rate"). Throws on a stray positional argument.
    std::string option();
    // Value of the option just read.
    std::string value();
    // The option just read is a flag: it must not carry "=value".
    void flag();
    [[noreturn]] void unknown(const std::string& subcommand) const;
    const std::string& current() const { return current_; }

private:
    std::vector<std::string> args_;
    std::size_t pos_ = 0;
    std::string current_;
    std::string inline_;
    bool hasInline_ = false;
};

std::string lower(std::string s);
double parseDouble(const std::string& option, const std::string& text, double lo, double hi);
int parseInt(const std::string& option, const std::string& text, int lo, int hi);
std::uint32_t parseU32(const std::string& option, const std::string& text);
bool parseBool(const std::string& option, const std::string& text);
std::vector<std::string> splitList(const std::string& text, char sep = ',');

VoiceId parseVoice(const std::string& text);
// Knob index of `id` on voice v (kVoiceInfo[v].knobs), or -1.
int knobIndex(int voice, const std::string& id);
// Parses a knob value: numbers in the knob's range; for KnobUnit::Choice the
// source names rom / synth / user are accepted too.
float parseKnobValue(int voice, int knob, const std::string& option, const std::string& text);
Quality parseQuality(const std::string& text);
const char* qualityName(Quality q);
Revision parseRevision(const std::string& text);
const char* revisionName(Revision r);
seq::VariationMode parseVariationMode(const std::string& text);
seq::AutoFill parseAutoFill(const std::string& text);
seq::PlayMode parsePlayMode(const std::string& text);

// The hardware's pattern address: bank I/II, group 1..3, slot 1..16. Labels
// read "I-1-1" .. "II-3-16"; index = bank*48 + group*16 + slot.
std::string patternLabel(int index);
// "0".."95" or a label; returns -1 if `text` is neither (it may be a name).
int parsePatternAddress(const std::string& text);

// --- sound setup ------------------------------------------------------------------

// The stock machine: every knob at its default, LEVEL at the calibration
// point, centred, no strips, no FX, default master. Independent of the
// factory library.
EngineParams stockEngine();
// A clean master: output stage off, no drive, compressor, limiter or soft clip, 0 dB.
void setCleanMaster(MasterParams& m);
// "typical": the tool's own FX mix (independent of the factory library), used
// by the bench and the golden kit case. Strips (cuts, EQ, compressor) on BD,
// SD, CP, CH and OH; FX bus 1 = delay -> reverb fed by sends, FX bus 2 =
// distortion -> filter carrying CP/RS/LT; master: Original output stage with
// drive, 3-band compressor, limiter and soft clip.
void applyTypicalMix(EngineParams& e);

// Applies one "--set key=value" override (see kSetHelp).
void applySetting(MachineParams& params, const std::string& assignment);
extern const char* const kSetHelp;

// --- buses, output -----------------------------------------------------------------

// Stereo storage for every bus, with a BusBuffers view at an offset.
struct BusStorage
{
    BusStorage(int numSamples, bool allBuses);
    BusBuffers view(int offset);
    std::vector<float>& left(int bus) { return data[static_cast<std::size_t>(bus)][0]; }
    std::vector<float>& right(int bus) { return data[static_cast<std::size_t>(bus)][1]; }

    std::array<std::array<std::vector<float>, 2>, kNumBuses> data;
};

// Throws UsageError when numFrames frames of the given channel count and
// format would not fit a WAV file (4 GiB RIFF limit). Called before
// rendering, so an oversized request fails at once instead of after the render.
void checkWavFits(long long numFrames, int channels, const std::string& formatText);

// Writes a stereo pair to a WAV, mono = (L+R)/2. Frames [first, first+numFrames).
void writeStereoOrMono(const std::string& path, const std::vector<float>& left,
                       const std::vector<float>& right, int first, int numFrames, int sampleRate,
                       int channels, const std::string& formatText, bool quiet);

// Loads a user ROM region from a WAV (any rate, mixed to mono) through
// romFromAudio. Throws RunError on failure.
void loadUserRom(const std::string& path, RomRegion region, RomImage& image);
RomSlot slotOf(RomRegion region);
RomRegion regionForVoice(int voice);

// --- pattern sources -----------------------------------------------------------------

// Where patterns come from: a factory memory, a factory preset, or a text file.
struct PatternSource
{
    std::string memory = "0";   // factory memory, index or name
    std::string pattern;        // index, label or name in that memory ("" = preset's or 0)
    std::string preset;         // factory preset index or name ("" = none)
    std::string textFile;       // pattern text file (may hold several patterns, chain, songs)
};

struct LoadedPatterns
{
    seq::PatternBank bank{};
    int selected = 0;
    std::string name;
    std::string sourceLabel;
    double tempo = 0.0;           // suggested; 0 = none
    // From a preset (else defaults): the kit and sequencer feel.
    bool fromPreset = false;
    int presetKit = 0;
    float presetSwing = 1.0f;
    seq::VariationMode presetVariation = seq::VariationMode::A;
    seq::PlayMode presetPlay = seq::PlayMode::Pattern;
    int presetSongTrack = 0;
    seq::AutoFill presetAutoFill = seq::AutoFill::Manual;
};

// Throws UsageError for an unknown memory/pattern/preset, RunError when a text
// file does not parse.
void loadPatterns(const PatternSource& source, LoadedPatterns& out);
// Pattern-source options shared by `pattern` and `midi`; returns true if consumed.
bool readPatternSourceOption(ArgReader& r, const std::string& opt, PatternSource& src);
// Factory memory / kit / preset by index or (case-insensitive) name.
int resolveMemory(const std::string& text);
int resolveKit(const std::string& text);
int resolvePreset(const std::string& text);

// --- offline driver ----------------------------------------------------------------------

// Hands a user ROM image to a machine (adopted by its next process() call;
// the machine owns and frees the handoff).
void postUserRom(Machine& m, RomSlot slot, const RomImage& image);

int cmdVoice(const std::vector<std::string>& args);
int cmdPattern(const std::vector<std::string>& args);
int cmdMidi(const std::vector<std::string>& args);
int cmdBench(const std::vector<std::string>& args);
int cmdList(const std::vector<std::string>& args);
int cmdCompare(const std::vector<std::string>& args);

} // namespace mw909::tools
