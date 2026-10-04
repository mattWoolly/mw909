// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The parameter layout (docs/design/plugin.md §2) and the per-block snapshot.
//
// Two independent guards on the frozen ids:
//  * parameter_ids.txt (next to this file) is the frozen layout: every id in
//    host order with its name, kind, range, default, grid and choices, as the
//    host sees it. Any difference fails. A deliberate addition is recorded by
//    re-running with MW909_UPDATE_PARAMETER_SNAPSHOT=1 and reviewing the diff;
//    an existing line must never change.
//  * a hand-written list of plugin.md's values, checked against the layout.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "TestSupport.h"

using namespace plugintest;
using Catch::Approx;

namespace {

std::string kindOf(const juce::RangedAudioParameter& p) {
    if (dynamic_cast<const juce::AudioParameterBool*>(&p) != nullptr) return "bool";
    if (dynamic_cast<const juce::AudioParameterChoice*>(&p) != nullptr) return "choice";
    if (dynamic_cast<const juce::AudioParameterInt*>(&p) != nullptr) return "int";
    return "float";
}

std::string number(double v) {
    std::ostringstream out;
    out.precision(9);
    out << v;
    return out.str();
}

// One line per parameter, as the host sees it.
std::string describe(int index, juce::RangedAudioParameter& p) {
    const auto& range = p.getNormalisableRange();
    std::string line = std::to_string(index) + " | " + p.paramID.toStdString() + " | " + p.getName(128).toStdString() +
                       " | " + kindOf(p) + " | " + number(range.start) + " .. " + number(range.end) +
                       " | default " + number(p.convertFrom0to1(p.getDefaultValue())) + " | step " +
                       number(range.interval) + " | " + p.getLabel().toStdString() + " | " +
                       (p.isAutomatable() ? "auto" : "not-auto");
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&p))
        line += " | " + choice->choices.joinIntoString(",").toStdString();
    // Two points of the curve pin the mapping (linear, log, cubic).
    if (kindOf(p) == "float")
        line += " | n.25 " + number(p.convertFrom0to1(0.25f)) + " n.5 " + number(p.convertFrom0to1(0.5f));
    return line;
}

std::vector<std::string> describeLayout(mw909Processor& p) {
    std::vector<std::string> lines;
    const auto& params = p.getParameters();
    for (int i = 0; i < params.size(); ++i)
        lines.push_back(describe(i, *dynamic_cast<juce::RangedAudioParameter*>(params[i])));
    return lines;
}

juce::File snapshotFile() { return juce::File(MW909_TEST_DATA_DIR).getChildFile("parameter_ids.txt"); }

struct Expected {
    std::string id;
    std::string name;
    std::string kind;
    float low = 0.0f, high = 1.0f, def = 0.0f;
    int numChoices = 0;
};

// plugin.md §2, written out by hand.
std::vector<Expected> handWritten() {
    std::vector<Expected> e;
    for (int v = 0; v < mw909::kNumVoices; ++v) {
        const auto& info = mw909::kVoiceInfo[static_cast<std::size_t>(v)];
        const std::string p(info.id);
        const std::string c(info.code);
        e.push_back({p + "_level", c + " Level", "float", 0.0f, 1.0f, 0.5f});
        e.push_back({p + "_pan", c + " Pan", "float", -1.0f, 1.0f, 0.0f});
        e.push_back({p + "_spread", c + " Spread", "float", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_ch_drive", c + " Drive", "float", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_mute", c + " Mute", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_solo", c + " Solo", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_out", c + " Output", "choice", 0.0f, 15.0f, 0.0f, 16});
        e.push_back({p + "_route", c + " Route", "choice", 0.0f, 2.0f, 0.0f, 3});
        e.push_back({p + "_send_bus", c + " Send Bus", "choice", 0.0f, 2.0f, 0.0f, 3});
        e.push_back({p + "_send", c + " Send", "float", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_send_pre", c + " Send Pre", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_eq_on", c + " EQ On", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_lowcut", c + " Low Cut", "float", 20.0f, 2000.0f, 20.0f});
        e.push_back({p + "_highcut", c + " High Cut", "float", 500.0f, 20000.0f, 20000.0f});
        e.push_back({p + "_reso", c + " Resonance", "float", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_eq1_on", c + " EQ1 On", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_eq1_freq", c + " EQ1 Freq", "float", 20.0f, 20000.0f, 200.0f});
        e.push_back({p + "_eq1_width", c + " EQ1 Width", "float", 0.5f, 4.0f, 1.0f});
        e.push_back({p + "_eq1_gain", c + " EQ1 Gain", "float", -24.0f, 24.0f, 0.0f});
        e.push_back({p + "_eq2_on", c + " EQ2 On", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_eq2_freq", c + " EQ2 Freq", "float", 20.0f, 20000.0f, 3000.0f});
        e.push_back({p + "_eq2_width", c + " EQ2 Width", "float", 0.5f, 4.0f, 1.0f});
        e.push_back({p + "_eq2_gain", c + " EQ2 Gain", "float", -24.0f, 24.0f, 0.0f});
        e.push_back({p + "_comp_on", c + " Comp On", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_comp_pre", c + " Comp Pre EQ", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_comp_thresh", c + " Comp Threshold", "float", -48.0f, 0.0f, 0.0f});
        e.push_back({p + "_comp_ratio", c + " Comp Ratio", "float", 1.0f, 20.0f, 4.0f});
        e.push_back({p + "_comp_attack", c + " Comp Attack", "float", 1.0f, 500.0f, 10.0f});
        e.push_back({p + "_comp_release", c + " Comp Release", "float", 1.0f, 500.0f, 100.0f});
        e.push_back({p + "_comp_makeup", c + " Comp Makeup", "float", 0.0f, 24.0f, 0.0f});
    }
    // Placement: dist, eqa, delay, dyna, filter on FX 1, the rest on FX 2; order = index.
    const char* fx[] = {"dist", "eqa", "eqb", "delay", "reverb", "chorus", "crush", "dyna", "dynb", "filter"};
    const int bus[] = {0, 0, 1, 0, 1, 1, 1, 0, 1, 0};
    for (int i = 0; i < 10; ++i) {
        const std::string f = std::string("fx_") + fx[i];
        e.push_back({f + "_on", "", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({f + "_bus", "", "choice", 0.0f, 1.0f, static_cast<float>(bus[i]), 2});
        e.push_back({f + "_order", "", "int", 0.0f, 9.0f, static_cast<float>(i)});
    }
    e.push_back({"fx1_level", "", "float", -60.0f, 12.0f, 0.0f});
    e.push_back({"fx2_level", "", "float", -60.0f, 12.0f, 0.0f});
    e.push_back({"fx1_out", "", "choice", 0.0f, 15.0f, 0.0f, 16});
    e.push_back({"fx2_out", "", "choice", 0.0f, 15.0f, 0.0f, 16});
    e.push_back({"fx1_to2", "", "float", 0.0f, 1.0f, 0.0f});
    e.push_back({"fx1_to2_pre", "", "bool", 0.0f, 1.0f, 0.0f});
    // A sample of effect fields (the full set is in the snapshot file).
    e.push_back({"fx_dist_preamp", "Distortion Preamp", "float", 0.0f, 36.0f, 0.0f});
    e.push_back({"fx_dist_split", "Distortion Split", "float", 0.0f, 2000.0f, 200.0f});
    e.push_back({"fx_eqa_mid1_freq", "EQ A Mid 1 Freq", "float", 20.0f, 20000.0f, 400.0f});
    e.push_back({"fx_eqb_mid2_freq", "EQ B Mid 2 Freq", "float", 20.0f, 20000.0f, 2500.0f});
    e.push_back({"fx_delay_div", "Delay Division", "choice", 0.0f, 5.0f, 2.0f, 6});
    e.push_back({"fx_delay_kind", "Delay Timing", "choice", 0.0f, 2.0f, 1.0f, 3});
    e.push_back({"fx_delay_feedback", "Delay Feedback", "float", 0.0f, 0.98f, 0.35f});
    e.push_back({"fx_reverb_predelay", "Reverb Predelay", "float", 0.0f, 250.0f, 10.0f});
    e.push_back({"fx_reverb_decay", "Reverb Decay", "float", 0.0f, 1.0f, 0.5f});
    e.push_back({"fx_chorus_rate", "Chorus Rate", "float", 0.02f, 10.0f, 0.5f});
    e.push_back({"fx_crush_bits", "Bitcrusher Bits", "float", 1.0f, 24.0f, 16.0f});
    e.push_back({"fx_crush_rate", "Bitcrusher Rate", "float", 44.0f, 44100.0f, 44100.0f});
    e.push_back({"fx_dyna_sidechain", "Dynamics A Sidechain", "choice", 0.0f, 11.0f, 0.0f, 12});
    e.push_back({"fx_dynb_vca_depth", "Dynamics B VCA Depth", "float", -1.0f, 1.0f, -1.0f});
    e.push_back({"fx_filter_type", "Filter Type", "choice", 0.0f, 3.0f, 1.0f, 4});
    e.push_back({"fx_filter_cutoff", "Filter Cutoff", "float", 20.0f, 20000.0f, 2000.0f});
    e.push_back({"fx_filter_lfo_wave", "Filter LFO Wave", "choice", 0.0f, 4.0f, 0.0f, 5});
    e.push_back({"fx_filter_env_trigger3", "Filter Env Trigger 3", "choice", 0.0f, 11.0f, 0.0f, 12});
    // Master.
    e.push_back({"master_volume", "Master Volume", "float", -60.0f, 12.0f, 0.0f});
    e.push_back({"output_stage", "Output Stage", "choice", 0.0f, 2.0f, 1.0f, 3});
    e.push_back({"master_drive", "Master Drive", "float", 0.0f, 1.0f, 0.0f});
    e.push_back({"mcomp_on", "Multiband Comp On", "bool", 0.0f, 1.0f, 0.0f});
    e.push_back({"mcomp_xlow", "Multiband Comp Low X", "float", 40.0f, 1000.0f, 200.0f});
    e.push_back({"mcomp_xhigh", "Multiband Comp High X", "float", 1000.0f, 12000.0f, 3000.0f});
    for (const char* b : {"low", "mid", "high"}) {
        const std::string m = std::string("mcomp_") + b;
        e.push_back({m + "_on", "", "bool", 0.0f, 1.0f, 1.0f});
        e.push_back({m + "_solo", "", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({m + "_thresh", "", "float", -48.0f, 0.0f, 0.0f});
        e.push_back({m + "_ratio", "", "float", 1.0f, 20.0f, 2.0f});
        e.push_back({m + "_attack", "", "float", 1.0f, 500.0f, 10.0f});
        e.push_back({m + "_release", "", "float", 1.0f, 500.0f, 150.0f});
        e.push_back({m + "_makeup", "", "float", 0.0f, 24.0f, 0.0f});
    }
    e.push_back({"limiter", "Limiter", "bool", 0.0f, 1.0f, 1.0f});
    e.push_back({"lim_thresh", "", "float", -24.0f, 0.0f, 0.0f});
    e.push_back({"lim_attack", "", "float", 0.1f, 10.0f, 1.0f});
    e.push_back({"lim_release", "", "float", 10.0f, 1000.0f, 100.0f});
    e.push_back({"soft_clip", "", "bool", 0.0f, 1.0f, 1.0f});
    for (const char* t : {"trig1", "trig2", "trig3"}) {
        const std::string p = t;
        e.push_back({p + "_on", "", "bool", 0.0f, 1.0f, 0.0f});
        e.push_back({p + "_source", "", "choice", 0.0f, 10.0f, 5.0f, 11});  // RS
        e.push_back({p + "_route", "", "choice", 0.0f, 2.0f, 0.0f, 3});
        e.push_back({p + "_out", "", "choice", 0.0f, 15.0f, 0.0f, 16});
        e.push_back({p + "_pan", "", "float", -1.0f, 1.0f, 0.0f});
        e.push_back({p + "_level", "", "float", 0.0f, 1.0f, 0.5f});
    }
    e.push_back({"total_accent", "Total Accent", "float", 0.0f, 1.0f, 0.7f});
    e.push_back({"hat_choke", "Hat Choke", "bool", 0.0f, 1.0f, 1.0f});
    e.push_back({"quality", "Quality", "choice", 0.0f, 2.0f, 1.0f, 3});
    e.push_back({"variance", "Analog Variance", "float", 0.0f, 1.0f, 0.0f});
    e.push_back({"revision", "Unit Revision", "choice", 0.0f, 1.0f, 0.0f, 2});
    e.push_back({"seq_mode", "Sequencer Mode", "choice", 0.0f, 3.0f, 1.0f, 4});
    e.push_back({"seq_run", "Run", "bool", 0.0f, 1.0f, 0.0f});
    e.push_back({"tempo", "Tempo", "float", 20.0f, 400.0f, 120.0f});
    e.push_back({"swing_scale", "Shuffle Depth", "float", 0.0f, 1.0f, 1.0f});
    e.push_back({"variation", "Variation", "choice", 0.0f, 2.0f, 0.0f, 3});
    e.push_back({"auto_fill", "Auto Fill", "choice", 0.0f, 5.0f, 0.0f, 6});
    e.push_back({"fill_slot", "Fill Pattern", "choice", 0.0f, 15.0f, 15.0f, 16});
    e.push_back({"play_mode", "Play Mode", "choice", 0.0f, 2.0f, 0.0f, 3});
    e.push_back({"song_track", "Track", "choice", 0.0f, 7.0f, 0.0f, 8});
    e.push_back({"record", "Tap Write", "bool", 0.0f, 1.0f, 0.0f});
    e.push_back({"jitter", "Timing Jitter", "bool", 0.0f, 1.0f, 0.0f});
    e.push_back({"velocity_sens", "Velocity Sensitive", "bool", 0.0f, 1.0f, 1.0f});
    e.push_back({"midi_channel", "MIDI Channel", "choice", 0.0f, 16.0f, 0.0f, 17});
    e.push_back({"bass_channel", "Bass Channel", "choice", 0.0f, 16.0f, 0.0f, 17});
    e.push_back({"midi_out", "MIDI Out", "bool", 0.0f, 1.0f, 0.0f});
    e.push_back({"midi_out_channel", "MIDI Out Channel", "choice", 0.0f, 15.0f, 9.0f, 16});
    return e;
}

juce::StringArray choicesOf(mw909Processor& p, const char* id) {
    auto* choice = dynamic_cast<juce::AudioParameterChoice*>(p.apvts().getParameter(id));
    REQUIRE(choice != nullptr);
    return choice->choices;
}

}  // namespace

TEST_CASE("parameters: the layout matches the frozen snapshot file", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    const std::vector<std::string> lines = describeLayout(*p);
    const juce::File file = snapshotFile();

    if (juce::SystemStats::getEnvironmentVariable("MW909_UPDATE_PARAMETER_SNAPSHOT", {}) == "1") {
        juce::String text;
        text << "# SPDX-License-Identifier: AGPL-3.0-or-later\n"
             << "# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.\n"
             << "#\n# The frozen parameter layout (docs/design/plugin.md §2): index | id | name | kind |\n"
             << "# range | default | grid | unit | automatable | choices | two points of the curve.\n"
             << "# Ids never change once shipped. Regenerate with MW909_UPDATE_PARAMETER_SNAPSHOT=1\n"
             << "# only to ADD parameters, and review the diff.\n";
        for (const auto& line : lines) text << juce::String(juce::CharPointer_UTF8(line.c_str())) << "\n";
        REQUIRE(file.replaceWithText(text, false, false, "\n"));
        WARN("parameter snapshot rewritten: " << file.getFullPathName());
    }

    REQUIRE(file.existsAsFile());
    juce::StringArray stored;
    stored.addLines(file.loadFileAsString());
    std::vector<std::string> frozen;
    for (const auto& line : stored)
        if (line.isNotEmpty() && !line.startsWith("#")) frozen.push_back(line.toStdString());

    REQUIRE(frozen.size() == lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        INFO("parameter " << i);
        CHECK(lines[i] == frozen[i]);
    }
}

TEST_CASE("parameters: plugin.md's values, written out by hand", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    const auto& params = p->getParameters();

    // 11 voices x (their knobs + 30 mixer/strip), 156 FX, 32 master, 18 trigger outs, 21 global.
    int knobs = 0;
    for (const auto& info : mw909::kVoiceInfo) knobs += info.numKnobs;
    CHECK(knobs == 51);
    REQUIRE(params.size() == 608);
    REQUIRE(static_cast<int>(mw909::plugin::allParameterIds().size()) == 608);
    REQUIRE(static_cast<int>(mw909::plugin::parameterDefs().size()) == 608);

    std::set<std::string> ids;
    for (int i = 0; i < params.size(); ++i) {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(params[i]);
        REQUIRE(ranged != nullptr);
        CHECK(ids.insert(ranged->paramID.toStdString()).second);  // unique
        CHECK(ranged->getVersionHint() == 1);
        CHECK(mw909::plugin::allParameterIds()[static_cast<std::size_t>(i)] == ranged->paramID.toStdString());
        CHECK(mw909::plugin::parameterIndex(ranged->paramID.toStdString()) == i);
        // Ids are lowercase words joined by underscores.
        CHECK(ranged->paramID.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789_"));
    }

    for (const auto& e : handWritten()) {
        INFO(e.id);
        auto* ranged = p->apvts().getParameter(juce::String(e.id));
        REQUIRE(ranged != nullptr);
        if (!e.name.empty()) CHECK(ranged->getName(128).toStdString() == e.name);
        CHECK(kindOf(*ranged) == e.kind);
        const auto& range = ranged->getNormalisableRange();
        CHECK(range.start == Approx(e.low).margin(1.0e-6));
        CHECK(range.end == Approx(e.high).margin(1.0e-6));
        CHECK(ranged->convertFrom0to1(ranged->getDefaultValue()) == Approx(e.def).epsilon(1.0e-5).margin(1.0e-6));
        if (e.numChoices > 0) CHECK(dynamic_cast<juce::AudioParameterChoice*>(ranged)->choices.size() == e.numChoices);
    }

    // Every sound knob of kVoiceInfo, by the "<voice>_<knob>" rule.
    for (int v = 0; v < mw909::kNumVoices; ++v) {
        const auto& info = mw909::kVoiceInfo[static_cast<std::size_t>(v)];
        for (int k = 0; k < info.numKnobs; ++k) {
            const auto& knob = info.knobs[static_cast<std::size_t>(k)];
            const juce::String id = voiceId(v) + "_" + juce::String(std::string(knob.id));
            INFO(id);
            auto* ranged = p->apvts().getParameter(id);
            REQUIRE(ranged != nullptr);
            CHECK(ranged->getName(128).toStdString() == std::string(info.code) + " " + std::string(knob.name));
            CHECK(ranged->getNormalisableRange().start == Approx(knob.min));
            CHECK(ranged->getNormalisableRange().end == Approx(knob.max));
            CHECK(ranged->convertFrom0to1(ranged->getDefaultValue()) == Approx(knob.def).margin(1.0e-6));
            if (knob.unit == mw909::KnobUnit::Choice)
                CHECK(dynamic_cast<juce::AudioParameterChoice*>(ranged)->choices == juce::StringArray{"ROM", "Synth", "User"});
            if (knob.unit == mw909::KnobUnit::Semitones)
                CHECK(ranged->getNormalisableRange().interval == Approx(0.01f));
        }
    }

    // Choice labels, in the order the core enums use.
    CHECK(choicesOf(*p, "seq_mode") == juce::StringArray{"Off", "Host Sync", "Internal", "Host Note"});
    CHECK(choicesOf(*p, "quality") == juce::StringArray{"Eco", "Standard", "High"});
    CHECK(choicesOf(*p, "revision") == juce::StringArray{"Late", "Early"});
    CHECK(choicesOf(*p, "variation") == juce::StringArray{"A", "AB", "B"});
    CHECK(choicesOf(*p, "auto_fill") == juce::StringArray{"Manual", "16", "12", "8", "4", "2"});
    CHECK(choicesOf(*p, "play_mode") == juce::StringArray{"Pattern", "Chain", "Track"});
    CHECK(choicesOf(*p, "song_track") == juce::StringArray{"I-1", "I-2", "I-3", "I-4", "II-1", "II-2", "II-3", "II-4"});
    CHECK(choicesOf(*p, "output_stage") == juce::StringArray{"Off", "Modern", "Original"});
    CHECK(choicesOf(*p, "bd_route") == juce::StringArray{"Output", "FX 1", "FX 2"});
    CHECK(choicesOf(*p, "bd_send_bus") == juce::StringArray{"Off", "FX 1", "FX 2"});
    CHECK(choicesOf(*p, "fx_reverb_bus") == juce::StringArray{"FX 1", "FX 2"});
    CHECK(choicesOf(*p, "trig2_source") ==
          juce::StringArray{"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CR", "RD"});
    CHECK(choicesOf(*p, "fx_dyna_sidechain")[0] == "Off");
    CHECK(choicesOf(*p, "fx_dyna_sidechain")[11] == "RD");
    const auto outs = choicesOf(*p, "bd_out");
    CHECK(outs[0] == "Main");
    CHECK(outs[1] == "Out 2");
    CHECK(outs[15] == "Out 16");
    const auto fill = choicesOf(*p, "fill_slot");
    CHECK(fill[0] == "1");
    CHECK(fill[15] == "16");
    CHECK(choicesOf(*p, "midi_channel")[0] == "Omni");
    CHECK(choicesOf(*p, "bass_channel")[0] == "Off");
    CHECK(choicesOf(*p, "midi_out_channel")[9] == "10");

    // The voice-rebuilding settings are not automatable.
    for (const char* id : {"quality", "variance", "revision"}) CHECK_FALSE(p->apvts().getParameter(id)->isAutomatable());
}

TEST_CASE("parameters: the defaults describe the engine's own defaults", "[plugin][parameters]") {
    // MachineParams built from the parameter defaults equals the core's
    // default structs, apart from the placement plugin.md sets.
    const mw909::MachineParams d = mw909::plugin::defaultMachineParams();
    const mw909::MachineParams core{};
    CHECK(d.seq.mode == mw909::seq::Mode::HostSync);
    CHECK(d.seq.fillSlot == 15);
    CHECK(d.midi.outChannel == 10);
    CHECK(d.engine.hatChoke);
    for (int f = 0; f < mw909::kNumFxInstances; ++f) {
        INFO("fx " << f);
        CHECK_FALSE(d.engine.fx.place[static_cast<std::size_t>(f)].on);
        CHECK(d.engine.fx.place[static_cast<std::size_t>(f)].order == f);
    }
    CHECK(d.engine.channel[0].level == core.engine.channel[0].level);
    CHECK(d.engine.master.limiterOn == core.engine.master.limiterOn);
    for (int v = 0; v < mw909::kNumVoices; ++v)
        CHECK(d.engine.voice[static_cast<std::size_t>(v)].k == mw909::defaultParams(v).k);
}

TEST_CASE("parameters: a fresh instance holds factory preset 0", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    const auto& preset = mw909::factory::preset(0);

    std::set<std::string> set;
    const auto check = [&](const std::string& id, float value) {
        INFO(id);
        auto& parameter = param(*p, juce::String(id));
        CHECK(raw(*p, juce::String(id)) == Approx(parameter.convertFrom0to1(parameter.convertTo0to1(value))).margin(1.0e-6));
        set.insert(id);
    };
    for (const auto& [id, value] : mw909::plugin::Parameters::kitValues(mw909::factory::kit(preset.kit))) check(id, value);
    for (const auto& [id, value] : mw909::plugin::Parameters::presetValues(preset)) check(id, value);
    CHECK(raw(*p, "tempo") == Approx(preset.tempo).margin(0.051));

    // Everything else sits at its declared default.
    for (const auto& d : mw909::plugin::parameterDefs()) {
        if (set.count(d.id) != 0) continue;
        INFO(d.id);
        auto& parameter = param(*p, juce::String(d.id));
        CHECK(raw(*p, juce::String(d.id)) == parameter.convertFrom0to1(parameter.getDefaultValue()));
    }

    // The pattern memory and selection are the preset's too, and the factory CC map is in place.
    auto bank = std::make_unique<mw909::seq::PatternBank>();
    mw909::factory::buildBank(preset.bank, *bank);
    CHECK(sameBank(p->patterns().bank(), *bank));
    CHECK(p->patterns().selectedPattern() == preset.pattern);
    CHECK(p->getCurrentProgram() == 0);
    CHECK(p->unitSeed() == 909U);
    CHECK(p->midiLearn().getMap() == mw909::plugin::MidiLearn::factoryMap());
    for (int s = 0; s < mw909::kNumRomSlots; ++s) CHECK(p->userRom(static_cast<mw909::RomSlot>(s)).empty());
}

TEST_CASE("parameters: every value a parameter can hold survives normalise and back", "[plugin][parameters]") {
    // State restore depends on this: a saved value goes through the normalised
    // form on its way back in and has to come out bit-identical.
    auto p = makeProcessor(kRate, kBlock, false);
    Rng rng(20261003U);
    for (auto* base : p->getParameters()) {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(base);
        REQUIRE(ranged != nullptr);
        int mismatches = 0;
        const auto probe = [&](float normalised) {
            const float held = ranged->convertFrom0to1(normalised);
            const float again = ranged->convertFrom0to1(ranged->convertTo0to1(held));
            if (std::memcmp(&held, &again, sizeof(float)) != 0) ++mismatches;
        };
        for (int i = 0; i < 6000; ++i) probe(rand01(rng));
        // The steep ends of log and cubic curves, and the extremes.
        for (float n : {0.0f, 1.0f, 1.0e-7f, 1.0e-5f, 1.0e-3f, 0.999999f})
            probe(n);
        for (int i = 0; i < 500; ++i) probe(rand01(rng) * 1.0e-3f);
        INFO(ranged->paramID);
        CHECK(mismatches == 0);
    }
}

TEST_CASE("parameters: switches report exactly 0 or 1 to the host", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    int bools = 0;
    for (auto* base : p->getParameters()) {
        auto* b = dynamic_cast<juce::AudioParameterBool*>(base);
        if (b == nullptr) continue;
        ++bools;
        for (float poked : {0.37f, 0.5f, 0.93f, 0.0f, 0.49f, 1.0f, 0.5000001f}) {
            b->setValueNotifyingHost(poked);
            const float expected = poked >= 0.5f ? 1.0f : 0.0f;
            INFO(b->paramID << " poked " << poked);
            CHECK(static_cast<juce::AudioProcessorParameter*>(b)->getValue() == expected);
            CHECK(raw(*p, b->paramID) == expected);
        }
    }
    CHECK(bools > 100);
}

TEST_CASE("parameters: the block snapshot carries every parameter into MachineParams", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    Rng rng(909U);
    const auto& defs = mw909::plugin::parameterDefs();

    for (int round = 0; round < 25; ++round) {
        randomiseParameters(*p, rng);
        p->setUnitSeed(static_cast<std::uint32_t>(rng()));

        mw909::MachineParams m;
        p->makeSnapshot(m);

        // Generic: every row reads back what the parameter holds.
        for (const auto& d : defs) {
            INFO(d.id);
            const float held = raw(*p, juce::String(d.id));
            const float expected = d.kind == mw909::plugin::ParamKind::Float ? held : std::floor(held + 0.5f);
            CHECK(d.get(m, d.index) == expected);
        }
        CHECK(m.unitSeed == p->unitSeed());

        // And by hand, for the fields whose mapping is not one to one.
        for (int v = 0; v < mw909::kNumVoices; ++v) {
            const auto i = static_cast<std::size_t>(v);
            INFO("voice " << v);
            CHECK(m.engine.channel[i].level == raw(*p, voiceParam(v, "level")));
            CHECK(m.engine.channel[i].drive == raw(*p, voiceParam(v, "ch_drive")));
            CHECK(m.mute[i] == (raw(*p, voiceParam(v, "mute")) >= 0.5f));
            CHECK(static_cast<int>(m.engine.channel[i].output) == static_cast<int>(raw(*p, voiceParam(v, "out"))));
            CHECK(m.engine.channel[i].output < mw909::kNumBuses);
            CHECK(m.engine.strip[i].band[1].freqHz == raw(*p, voiceParam(v, "eq2_freq")));
            CHECK(m.engine.voice[i].k[0] == raw(*p, voiceParam(v, "tune")));
        }
        CHECK(m.engine.voice[mw909::BD].k[mw909::knob::BdDrive] == raw(*p, "bd_drive"));  // the Shaper knob
        CHECK(m.engine.voice[mw909::CR].k[mw909::knob::PcmSource] == raw(*p, "cr_source"));
        CHECK(static_cast<int>(m.midi.outChannel) == static_cast<int>(raw(*p, "midi_out_channel")) + 1);
        CHECK(static_cast<int>(m.midi.receiveChannel) == static_cast<int>(raw(*p, "midi_channel")));
        const int sidechain = static_cast<int>(raw(*p, "fx_dynb_sidechain"));
        CHECK(m.engine.fx.dynamics[1].sidechain == (sidechain == 0 ? mw909::kNoSource : sidechain - 1));
        CHECK(static_cast<int>(m.engine.fx.place[static_cast<std::size_t>(mw909::FxId::Reverb)].bus) ==
              static_cast<int>(raw(*p, "fx_reverb_bus")));
        CHECK(m.seq.internalBpm == static_cast<double>(raw(*p, "tempo")));
        CHECK(m.seq.totalAccent == raw(*p, "total_accent"));
        CHECK(m.engine.trig[2].source == static_cast<std::uint8_t>(raw(*p, "trig3_source")));
        CHECK(m.engine.master.band[2].releaseMs == raw(*p, "mcomp_high_release"));
        CHECK(m.seq.jitterSeed == 0U);  // 0 = the unit seed (the Machine substitutes it)
    }
}

TEST_CASE("parameters: the snapshot sanitises whatever the atomics hold", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    const auto& defs = mw909::plugin::parameterDefs();
    // Hosts cannot put these into a parameter, but the snapshot does not trust
    // the atomics anyway: non-finite -> default, out of range -> clamped.
    for (const float poison : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity(), 1.0e9f, -1.0e9f, 0.4999f}) {
        for (const auto& d : defs) p->apvts().getRawParameterValue(juce::String(d.id))->store(poison);
        mw909::MachineParams m;
        p->makeSnapshot(m);
        for (const auto& d : defs) {
            INFO(d.id << " poisoned with " << poison);
            const float got = d.get(m, d.index);
            CHECK(std::isfinite(got));
            CHECK(got >= d.low);
            CHECK(got <= d.high);
            if (!std::isfinite(poison)) {
                const float expected = d.kind == mw909::plugin::ParamKind::Float ? d.def : std::floor(d.def + 0.5f);
                CHECK(got == expected);
            }
            if (d.kind != mw909::plugin::ParamKind::Float) CHECK(got == std::floor(got));
        }
        CHECK(m.engine.channel[0].output < mw909::kNumBuses);
        CHECK(m.midi.outChannel >= 1);
        CHECK(m.midi.outChannel <= 16);
        CHECK(m.seq.songTrack < mw909::seq::kNumSongTracks);
        CHECK(m.seq.fillSlot < 16);
    }
}

TEST_CASE("parameters: choice indices line up with the core enums", "[plugin][parameters]") {
    auto p = makeProcessor(kRate, kBlock, false);
    mw909::MachineParams m;

    setParam(*p, "seq_mode", 0.0f);
    p->makeSnapshot(m);
    CHECK(m.seq.mode == mw909::seq::Mode::Off);
    setParam(*p, "seq_mode", 3.0f);
    p->makeSnapshot(m);
    CHECK(m.seq.mode == mw909::seq::Mode::HostNote);
    setParam(*p, "auto_fill", 5.0f);
    p->makeSnapshot(m);
    CHECK(m.seq.autoFill == mw909::seq::AutoFill::Every2);
    setParam(*p, "variation", 1.0f);
    p->makeSnapshot(m);
    CHECK(m.seq.variation == mw909::seq::VariationMode::AB);
    setParam(*p, "play_mode", 2.0f);
    p->makeSnapshot(m);
    CHECK(m.seq.play == mw909::seq::PlayMode::Track);
    setParam(*p, "quality", 2.0f);
    p->makeSnapshot(m);
    CHECK(m.quality == mw909::Quality::High);
    setParam(*p, "revision", 1.0f);
    p->makeSnapshot(m);
    CHECK(m.revision == mw909::Revision::Early);
    setParam(*p, "output_stage", 2.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.master.stage == mw909::OutputStage::Original);
    setParam(*p, "ch_out", 15.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.channel[mw909::CH].output == 15);
    setParam(*p, "oh_route", 2.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.channel[mw909::OH].route == mw909::ChannelRoute::FxBus2);
    setParam(*p, "fx_filter_type", 3.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.fx.filter.type == mw909::FilterType::HighPass);
    setParam(*p, "fx_delay_div", 0.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.fx.delay.div == mw909::SyncDiv::D1_32);
    setParam(*p, "fx_dyna_mode", 1.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.fx.dynamics[0].mode == mw909::DynamicsMode::Vca);
    setParam(*p, "fx_dyna_trigger2", 11.0f);
    p->makeSnapshot(m);
    CHECK(m.engine.fx.dynamics[0].triggers[1] == mw909::RD);
    setParam(*p, "bass_channel", 16.0f);
    p->makeSnapshot(m);
    CHECK(m.midi.bassChannel == 16);
    setParam(*p, "song_track", 7.0f);
    p->makeSnapshot(m);
    CHECK(m.seq.songTrack == 7);
}

TEST_CASE("parameters: kit values cover exactly the kit parameters", "[plugin][parameters]") {
    const auto& kitIds = mw909::plugin::kitParameterIds();
    const std::set<std::string> kit(kitIds.begin(), kitIds.end());
    CHECK(kit.count("bd_mute") == 0);
    CHECK(kit.count("ch_solo") == 0);
    CHECK(kit.count("tempo") == 0);
    CHECK(kit.count("seq_mode") == 0);
    CHECK(kit.count("quality") == 0);
    CHECK(kit.count("bd_drive") == 1);
    CHECK(kit.count("rd_comp_makeup") == 1);
    CHECK(kit.count("fx_filter_mix") == 1);
    CHECK(kit.count("trig3_level") == 1);
    CHECK(kit.count("total_accent") == 1);
    CHECK(kit.count("revision") == 1);
    CHECK(kit.count("master_volume") == 1);
    for (int i = 0; i < mw909::factory::numKits(); ++i) {
        const auto values = mw909::plugin::Parameters::kitValues(mw909::factory::kit(i));
        REQUIRE(values.size() == kitIds.size());
        for (std::size_t k = 0; k < values.size(); ++k) {
            CHECK(values[k].first == kitIds[k]);
            CHECK(std::isfinite(values[k].second));
        }
    }
}

TEST_CASE("parameters: a new instance reports its declared defaults", "[plugin][parameters]") {
    // clap-validator's param-default-values: what a fresh instance holds is
    // what each parameter declares as its default (factory preset 0 is built
    // to agree with plugin.md's defaults).
    auto p = makeProcessor(kRate, kBlock, false);
    for (auto* base : p->getParameters()) {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(base);
        INFO(ranged->paramID);
        CHECK(ranged->getValue() == ranged->getDefaultValue());
    }
}
