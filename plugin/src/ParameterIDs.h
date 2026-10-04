// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Every host-visible parameter id (docs/design/plugin.md §2). The ids are
// serialised in sessions and presets: they are FROZEN. Never rename one; add
// new ids instead (tests/plugin/parameter_ids.txt is the frozen list the
// tests compare the layout against).
//
// Id scheme (lowercase, '_' separated):
//   <v>_<knob id>        a voice's sound control, from kVoiceInfo[v].knobs[k].id
//   <v>_<mixer suffix>   the voice's mixer channel and strip (kVoiceSuffix)
//   fx_<fx>_on|bus|order the placement of one effect instance (kFxIds)
//   fx_<fx>_<field>      one field of that effect's FxParams.h struct
//   fx1_*, fx2_*         the FX bus returns
//   master_*, mcomp_*, lim_*, limiter, soft_clip, output_stage   master chain
//   trig<n>_*            audible trigger outs 1..3
//   everything else      global, sequencer and MIDI settings (constants below)
//
// One deviation from plugin.md §2.1: the mixer channel's drive is
// "<v>_ch_drive", not "<v>_drive", because the bass drum's sound knob "drive"
// (its waveshaper, "BD Shaper") already owns "bd_drive" under the
// "<v>_<knob id>" rule of Voices.h.
//
// <v> is kVoiceInfo[v].id ("bd", "sd", ...). JUCE-free on purpose.

#pragma once

#include <array>
#include <string>
#include <string_view>

#include "mw909/Voices.h"
#include "mw909/fx/FxParams.h"

namespace mw909::plugin::paramid {

// juce::ParameterID version hint of every parameter shipped so far.
inline constexpr int kParameterVersion = 1;

// --- master chain ------------------------------------------------------------------
inline constexpr const char* masterVolume = "master_volume";
inline constexpr const char* outputStage = "output_stage";
inline constexpr const char* masterDrive = "master_drive";
inline constexpr const char* mcompOn = "mcomp_on";
inline constexpr const char* mcompXLow = "mcomp_xlow";
inline constexpr const char* mcompXHigh = "mcomp_xhigh";
inline constexpr const char* limiter = "limiter";
inline constexpr const char* limThresh = "lim_thresh";
inline constexpr const char* limAttack = "lim_attack";
inline constexpr const char* limRelease = "lim_release";
inline constexpr const char* softClip = "soft_clip";

// Multiband compressor bands: "mcomp_<band>_<suffix>".
inline constexpr std::array<std::string_view, 3> kBandIds{{"low", "mid", "high"}};
inline constexpr std::array<std::string_view, 7> kBandSuffix{{"on", "solo", "thresh", "ratio", "attack", "release", "makeup"}};

// --- global, sequencer, MIDI -----------------------------------------------------------
inline constexpr const char* totalAccent = "total_accent";
inline constexpr const char* hatChoke = "hat_choke";
inline constexpr const char* quality = "quality";
inline constexpr const char* variance = "variance";
inline constexpr const char* revision = "revision";
inline constexpr const char* seqMode = "seq_mode";
inline constexpr const char* seqRun = "seq_run";
inline constexpr const char* tempo = "tempo";
inline constexpr const char* swingScale = "swing_scale";
inline constexpr const char* variation = "variation";
inline constexpr const char* autoFill = "auto_fill";
inline constexpr const char* fillSlot = "fill_slot";
inline constexpr const char* playMode = "play_mode";
inline constexpr const char* songTrack = "song_track";
inline constexpr const char* record = "record";
inline constexpr const char* jitter = "jitter";
inline constexpr const char* velocitySens = "velocity_sens";
inline constexpr const char* midiChannel = "midi_channel";
inline constexpr const char* bassChannel = "bass_channel";
inline constexpr const char* midiOut = "midi_out";
inline constexpr const char* midiOutChannel = "midi_out_channel";

// --- per voice: the mixer channel and the strip ------------------------------------------
enum class VoiceParam : int {
    Level = 0, Pan, Spread, Drive, Mute, Solo, Out, Route, SendBus, Send, SendPre,
    EqOn, LowCut, HighCut, Reso,
    Eq1On, Eq1Freq, Eq1Width, Eq1Gain,
    Eq2On, Eq2Freq, Eq2Width, Eq2Gain,
    CompOn, CompPre, CompThresh, CompRatio, CompAttack, CompRelease, CompMakeup,
    kCount
};
inline constexpr int kNumVoiceParams = static_cast<int>(VoiceParam::kCount);

inline constexpr std::array<std::string_view, kNumVoiceParams> kVoiceSuffix{{
    "level", "pan", "spread", "ch_drive", "mute", "solo", "out", "route", "send_bus", "send", "send_pre",
    "eq_on", "lowcut", "highcut", "reso",
    "eq1_on", "eq1_freq", "eq1_width", "eq1_gain",
    "eq2_on", "eq2_freq", "eq2_width", "eq2_gain",
    "comp_on", "comp_pre", "comp_thresh", "comp_ratio", "comp_attack", "comp_release", "comp_makeup",
}};

// "bd_level", "oh_comp_thresh", ...
inline std::string voice(int voiceIndex, VoiceParam p) {
    std::string id(kVoiceInfo[static_cast<std::size_t>(voiceIndex)].id);
    id += '_';
    id += kVoiceSuffix[static_cast<std::size_t>(p)];
    return id;
}

// "bd_tune", "ch_source", ...: knob k of voice v (k < kVoiceInfo[v].numKnobs).
inline std::string knob(int voiceIndex, int knobIndex) {
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voiceIndex)];
    std::string id(info.id);
    id += '_';
    id += info.knobs[static_cast<std::size_t>(knobIndex)].id;
    return id;
}

// --- FX ---------------------------------------------------------------------------------
// Instance ids in FxId order.
inline constexpr std::array<std::string_view, kNumFxInstances> kFxIds{{
    "dist", "eqa", "eqb", "delay", "reverb", "chorus", "crush", "dyna", "dynb", "filter",
}};

// "fx_<fx>_<field>": a placement field ("on", "bus", "order") or an effect field.
inline std::string fx(int instance, std::string_view field) {
    std::string id("fx_");
    id += kFxIds[static_cast<std::size_t>(instance)];
    id += '_';
    id += field;
    return id;
}

// FX bus returns, b = 0 (FX 1) or 1 (FX 2): "fx1_level", "fx2_out".
inline std::string fxBus(int bus, std::string_view field) {
    std::string id("fx");
    id += static_cast<char>('1' + bus);
    id += '_';
    id += field;
    return id;
}
inline constexpr const char* fx1To2 = "fx1_to2";
inline constexpr const char* fx1To2Pre = "fx1_to2_pre";

// --- trigger outs -----------------------------------------------------------------------
inline constexpr std::array<std::string_view, 6> kTrigSuffix{{"on", "source", "route", "out", "pan", "level"}};
// "trig1_on" ... (n = 0..kNumTriggerOuts-1 -> "trig1".."trig3")
inline std::string trig(int n, std::string_view suffix) {
    std::string id("trig");
    id += static_cast<char>('1' + n);
    id += '_';
    id += suffix;
    return id;
}

// "mcomp_low_thresh" ...
inline std::string band(int b, std::string_view suffix) {
    std::string id("mcomp_");
    id += kBandIds[static_cast<std::size_t>(b)];
    id += '_';
    id += suffix;
    return id;
}

// Output choices: Main, Out 2 .. Out 16.
inline constexpr int kNumOutputChoices = kNumBuses;

}  // namespace mw909::plugin::paramid
