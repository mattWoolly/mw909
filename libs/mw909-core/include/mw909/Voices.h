// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The voice roster and the per-voice contract shared by the DSP voices, the
// engine, the sequencer and the plugin shell. This header is the single source
// of truth for voice order and voice controls: pattern tracks, parameter
// blocks, MIDI maps and output routing all index by VoiceId, and the plugin
// builds every per-voice sound parameter from kVoiceInfo[v].knobs.
// Contract details: docs/design/architecture.md §3.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mw909 {

// Panel order of the original machine. The hardware's one hi-hat circuit is
// two VoiceIds (CH, OH) so each has its own channel; the HiHat voice class
// renders both and implements the choke between them.
enum VoiceId : std::uint8_t {
    BD = 0,  // bass drum
    SD,      // snare drum
    LT,      // low tom
    MT,      // mid tom
    HT,      // hi tom
    RS,      // rim shot
    CP,      // hand clap
    CH,      // closed hi-hat (6-bit PCM)
    OH,      // open hi-hat   (6-bit PCM)
    CR,      // crash cymbal  (6-bit PCM)
    RD,      // ride cymbal   (6-bit PCM)
    kNumVoices
};

constexpr bool isSampleVoice(int v) noexcept { return v >= CH && v <= RD; }

// ---------------------------------------------------------------------------
// Dynamics: the accent DAC.
//
// The original machine's CPU writes a 6-bit code per accent DAC (0..5 V,
// RM0621 binary-weighted network, research/03 §3.2) "almost concurrent" with
// a fixed 5 V / 2 ms trigger, and the voltage holds until that DAC is written
// again. RS and CP share one DAC, CR and RD share another. Dynamics therefore
// arrive as a held control voltage, not as a trigger height.
//
// code = clamp(base + (voice accent ? kVoiceAccentCodes : 0)
//                   + (total accent ? round(kTotalAccentCodes * knob) : 0), 0, 63)
// The firmware's codes are undocumented (research/03 §9 Q1); the constants
// below are FITTED to the measured accent gains (research/05 §0.11,
// research/03 §3.2) and owned by the analog-voice work. Voices must behave
// sensibly over the whole 0..5 V range; weak/low-velocity hits below the
// normal level are allowed to be voice-specific near transistor thresholds
// (a circuit may go quiet abruptly where the real one would).
struct AccentDac {
    static constexpr float kVolts = 5.0f;        // full scale (code 63)
    static constexpr int kMaxCode = 63;
    static constexpr int kNormalCode = 24;       // FITTED: a plain step
    static constexpr int kWeakCode = 12;         // extension: a weak step
    static constexpr int kVoiceAccentCodes = 16; // FITTED: per-instrument accent (fixed strength on the hardware)
    static constexpr int kTotalAccentCodes = 39; // FITTED: total accent at knob = 1

    static constexpr float volts(int code) noexcept {
        return kVolts * static_cast<float>(code < 0 ? 0 : (code > kMaxCode ? kMaxCode : code)) /
               static_cast<float>(kMaxCode);
    }
};

// One trigger of a voice circuit. `acc` is the voltage on the voice's accent
// line for this hit (AccentDac::volts of the code, 0..5 V). `pitchSemis` is a
// per-hit pitch offset (chromatic BD from MIDI); sequencer hits use 0.
struct Hit {
    float acc = AccentDac::volts(AccentDac::kNormalCode);
    float pitchSemis = 0.0f;

    static constexpr Hit fromCode(int code) noexcept { return {AccentDac::volts(code), 0.0f}; }
    static constexpr Hit normal() noexcept { return fromCode(AccentDac::kNormalCode); }
    static constexpr Hit weak() noexcept { return fromCode(AccentDac::kWeakCode); }
    // Per-instrument accent and/or total accent at a TOTAL ACCENT knob position 0..1.
    static constexpr Hit accented(bool voiceAccent, bool totalAccent, float totalKnob) noexcept {
        const float k = totalKnob < 0.0f ? 0.0f : (totalKnob > 1.0f ? 1.0f : totalKnob);
        const int code = AccentDac::kNormalCode + (voiceAccent ? AccentDac::kVoiceAccentCodes : 0) +
                         (totalAccent ? static_cast<int>(AccentDac::kTotalAccentCodes * k + 0.5f) : 0);
        return fromCode(code);
    }
};

// ---------------------------------------------------------------------------
// Voice controls.
//
// Every voice has up to kMaxKnobs sound controls, described by kVoiceInfo.
// Values are in the knob's own unit (KnobInfo::min..max). Rules:
//  * A knob the original panel has (`panel`) spans the hardware pot travel
//    0..1 with the hardware taper, so a value maps to a pot position.
//  * An extended knob (not on the panel) reproduces the stock circuit at its
//    default exactly and extends both ways.
//  * Every voice has a TUNE and a DECAY knob (the brief's rule). On the sample
//    voices TUNE changes the ROM clock rate, as the crash/ride pot does.
//  * The plugin's parameter id is "<voice id>_<knob id>", e.g. "bd_attack".
enum class KnobUnit : std::uint8_t { Normalised, Semitones, Choice };

struct KnobInfo {
    std::string_view id;     // "tune" — parameter-id suffix, lowercase, frozen once shipped
    std::string_view name;   // "Tune"
    float min = 0.0f;
    float max = 1.0f;
    float def = 0.5f;
    KnobUnit unit = KnobUnit::Normalised;
    bool panel = false;      // the original panel has this knob
};

inline constexpr int kMaxKnobs = 6;

struct VoiceParams {
    std::array<float, kMaxKnobs> k{};   // indexed by the voice's knob enum below
};

// Knob indices per voice. Order == kVoiceInfo[v].knobs order.
namespace knob {
enum Bd : int { BdTune = 0, BdAttack, BdDecay, BdPitch, BdDepth, BdDrive };
enum Sd : int { SdTune = 0, SdTone, SdSnappy, SdDecay, SdDepth };
enum Tom : int { TomTune = 0, TomDecay, TomDepth, TomSweep, TomNoise };
enum Rs : int { RsTune = 0, RsDecay };
enum Cp : int { CpTune = 0, CpDecay, CpReverb };
// CH, OH, CR, RD share one layout.
enum Pcm : int { PcmTune = 0, PcmDecay, PcmSource, PcmColor, PcmDensity };
}  // namespace knob

// Sample-voice source (knob::PcmSource, KnobUnit::Choice).
enum class PcmSource : std::uint8_t {
    Rom = 0,     // the shipped 6-bit ROM image (built from CC0 recordings, research/02 §9)
    Synth,       // the synthetic metal source, rendered at the ROM clock (Color/Density shape it)
    User,        // a user-imported image (Machine::postUserRom); falls back to Rom when empty
    kCount
};

struct VoiceInfo {
    std::string_view code;       // "BD"
    std::string_view name;       // "Bass Drum"
    std::string_view id;         // "bd" — parameter-id prefix, lowercase
    int numKnobs = 0;
    std::array<KnobInfo, kMaxKnobs> knobs{};
    bool hardwareAccent = false; // per-instrument accent exists on the original (BD SD toms CH)
    bool hardwareFlam = false;   // flam exists on the original (BD SD toms)
};

namespace detail {
inline constexpr KnobInfo kPanelTune{"tune", "Tune", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, true};
inline constexpr KnobInfo kPanelDecay{"decay", "Decay", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, true};
inline constexpr KnobInfo kExtTuneSt{"tune", "Tune", -12.0f, 12.0f, 0.0f, KnobUnit::Semitones, false};
inline constexpr KnobInfo kExtDecay{"decay", "Decay", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false};
inline constexpr KnobInfo kPcmSource{"source", "Source", 0.0f, 2.0f, 0.0f, KnobUnit::Choice, false};
inline constexpr KnobInfo kPcmColor{"color", "Color", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false};
inline constexpr KnobInfo kPcmDensity{"density", "Density", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false};
inline constexpr std::array<KnobInfo, kMaxKnobs> kTomKnobs{{
    kPanelTune,
    kPanelDecay,
    {"depth", "Tune Depth", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false},
    {"sweep", "Tune Decay", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false},
    {"noise", "Noise", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false},
    {},
}};
// CH/OH: no tune on the original (R492 is fixed) but a DECAY knob each;
// CR/RD: a TUNE pot each but no decay knob (research/02 §0).
inline constexpr std::array<KnobInfo, kMaxKnobs> kHatKnobs{{
    kExtTuneSt, kPanelDecay, kPcmSource, kPcmColor, kPcmDensity, {}}};
inline constexpr std::array<KnobInfo, kMaxKnobs> kCymKnobs{{
    kPanelTune, kExtDecay, kPcmSource, kPcmColor, kPcmDensity, {}}};
}  // namespace detail

inline constexpr std::array<VoiceInfo, kNumVoices> kVoiceInfo{{
    {"BD", "Bass Drum", "bd", 6, {{
         detail::kPanelTune,                                                     // pitch-sweep time (research/05 §0.3)
         {"attack", "Attack", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, true},     // click level
         detail::kPanelDecay,
         {"pitch", "Pitch", -12.0f, 12.0f, 0.0f, KnobUnit::Semitones, false},    // base frequency
         {"depth", "Tune Depth", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false}, // sweep amount
         {"drive", "Shaper", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false},     // waveshaper drive (the R52 mod)
     }}, true, true},
    {"SD", "Snare Drum", "sd", 5, {{
         detail::kPanelTune,
         {"tone", "Tone", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, true},
         {"snappy", "Snappy", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, true},
         detail::kExtDecay,
         {"depth", "Tune Depth", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false},
         {},
     }}, true, true},
    {"LT", "Low Tom", "lt", 5, detail::kTomKnobs, true, true},
    {"MT", "Mid Tom", "mt", 5, detail::kTomKnobs, true, true},
    {"HT", "Hi Tom", "ht", 5, detail::kTomKnobs, true, true},
    {"RS", "Rim Shot", "rs", 2, {{detail::kExtTuneSt, detail::kExtDecay, {}, {}, {}, {}}}, false, false},
    {"CP", "Hand Clap", "cp", 3, {{
         detail::kExtTuneSt,
         detail::kExtDecay,
         {"reverb", "Reverb", 0.0f, 1.0f, 0.5f, KnobUnit::Normalised, false},
         {}, {}, {},
     }}, false, false},
    {"CH", "Closed Hi-Hat", "ch", 5, detail::kHatKnobs, true, false},
    {"OH", "Open Hi-Hat", "oh", 5, detail::kHatKnobs, false, false},
    {"CR", "Crash Cymbal", "cr", 5, detail::kCymKnobs, false, false},
    {"RD", "Ride Cymbal", "rd", 5, detail::kCymKnobs, false, false},
}};

constexpr VoiceParams defaultParams(int v) noexcept {
    VoiceParams p{};
    const auto& info = kVoiceInfo[static_cast<std::size_t>(v)];
    for (int i = 0; i < kMaxKnobs; ++i) p.k[static_cast<std::size_t>(i)] = info.knobs[static_cast<std::size_t>(i)].def;
    return p;
}

// Fixed at prepare time. Voices must not allocate: prepare() may be called on
// the audio thread when quality, the unit seed or the hardware revision changes.
enum class Revision : std::uint8_t {
    Late = 0,   // serial >= 426700: every service-note change applied (default)
    Early       // first production: BD C9 0.22u, RS R417 12k, HH C134 10u, tom C54/R194, no output caps
};

struct VoiceSetup {
    double sampleRate = 48000.0;  // rate of the buffers passed to render()
    int oversample = 1;           // internal oversampling the voice may use (1, 2 or 4)
    std::uint32_t unitSeed = 0;   // component-tolerance seed ("serial number")
    float variance = 0.0f;        // 0 = ideal nominal parts, 1 = full tolerance spread
    Revision revision = Revision::Late;
};

// Voice output scale: voices render VOLTS at their individual (MULTI) output
// with their LEVEL pot at centre, i.e. 1.0f == 1 V. That is how the service
// manual's check figures were taken ("all knobs set to center"), so the
// peak-to-peak tables in research/01 and research/02 §7 calibrate a voice
// directly, in Vpp. The channel LEVEL (Engine, ChannelParams::level) scales
// from there. The engine converts volts to digital full scale with
// kDigitalPerVolt.
inline constexpr float kDigitalPerVolt = 0.1f;   // 10 V peak == 0 dBFS (headroom for accented hits)

}  // namespace mw909
