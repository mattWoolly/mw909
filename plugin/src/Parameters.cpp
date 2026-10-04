// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "Parameters.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace mw909::plugin {

namespace {

constexpr std::size_t at(int i) noexcept { return static_cast<std::size_t>(i); }

// --- accessor pairs ---------------------------------------------------------------
// Each macro expands to the two accessors of a table row: a setter that writes
// the (already sanitised) value into MachineParams and a getter that reads it
// back. `m` is the MachineParams and `i` the row's index inside `expr`.

#define MW_FLOAT(expr)                                                                \
    [](MachineParams& m, int i, float value) { (void)i; expr = value; },                       \
    [](const MachineParams& m, int i) -> float { (void)i; return static_cast<float>(expr); }

#define MW_DOUBLE(expr)                                                               \
    [](MachineParams& m, int i, float value) { (void)i; expr = static_cast<double>(value); },  \
    [](const MachineParams& m, int i) -> float { (void)i; return static_cast<float>(expr); }

#define MW_BOOL(expr)                                                                 \
    [](MachineParams& m, int i, float value) { (void)i; expr = value >= 0.5f; },               \
    [](const MachineParams& m, int i) -> float { (void)i; return (expr) ? 1.0f : 0.0f; }

#define MW_ENUM(expr, Type)                                                           \
    [](MachineParams& m, int i, float value) { (void)i; expr = static_cast<Type>(static_cast<int>(value)); }, \
    [](const MachineParams& m, int i) -> float { (void)i; return static_cast<float>(static_cast<int>(expr)); }

// A 1-based field shown as a 0-based choice (MIDI out channel 1..16).
#define MW_ONE_BASED(expr)                                                            \
    [](MachineParams& m, int i, float value) { (void)i; expr = static_cast<std::uint8_t>(static_cast<int>(value) + 1); }, \
    [](const MachineParams& m, int i) -> float { (void)i; return static_cast<float>(static_cast<int>(expr) - 1); }

// A voice reference that may be kNoSource: choice 0 = Off, 1.. = BD..
#define MW_SOURCE(expr)                                                               \
    [](MachineParams& m, int i, float value) {                                         \
        (void)i;                                                                       \
        const int c = static_cast<int>(value);                                         \
        expr = c <= 0 ? kNoSource : static_cast<std::uint8_t>(c - 1);                  \
    },                                                                                 \
    [](const MachineParams& m, int i) -> float {                                       \
        (void)i;                                                                       \
        const int s = static_cast<int>(expr);                                          \
        return (s >= kNumVoices) ? 0.0f : static_cast<float>(s + 1);                   \
    }

using Setter = void (*)(MachineParams&, int, float);
using Getter = float (*)(const MachineParams&, int);

// --- row builders -------------------------------------------------------------------

ParamDef floatRow(std::string id, std::string name, float low, float high, Setter set, Getter get,
                  std::string label = {}, int decimals = 2, double step = 0.0, ParamMap map = ParamMap::Linear) {
    ParamDef d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.kind = ParamKind::Float;
    d.low = low;
    d.high = high;
    d.step = step;
    d.map = map;
    d.label = std::move(label);
    d.decimals = decimals;
    d.set = set;
    d.get = get;
    return d;
}

ParamDef boolRow(std::string id, std::string name, Setter set, Getter get) {
    ParamDef d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.kind = ParamKind::Toggle;
    d.low = 0.0f;
    d.high = 1.0f;
    d.set = set;
    d.get = get;
    return d;
}

ParamDef choiceRow(std::string id, std::string name, std::vector<std::string> choices, Setter set, Getter get) {
    ParamDef d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.kind = ParamKind::Choice;
    d.low = 0.0f;
    d.high = static_cast<float>(choices.size() - 1);
    d.choices = std::move(choices);
    d.set = set;
    d.get = get;
    return d;
}

ParamDef intRow(std::string id, std::string name, int low, int high, Setter set, Getter get) {
    ParamDef d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.kind = ParamKind::Int;
    d.low = static_cast<float>(low);
    d.high = static_cast<float>(high);
    d.set = set;
    d.get = get;
    return d;
}

// --- choice lists ---------------------------------------------------------------------

std::vector<std::string> outputChoices() {
    std::vector<std::string> c{"Main"};
    for (int i = 2; i <= kNumBuses; ++i) c.push_back("Out " + std::to_string(i));
    return c;
}

std::vector<std::string> channelChoices(const char* first) {
    std::vector<std::string> c{first};
    for (int i = 1; i <= 16; ++i) c.push_back(std::to_string(i));
    return c;
}

std::vector<std::string> voiceChoices(bool withOff) {
    std::vector<std::string> c;
    if (withOff) c.emplace_back("Off");
    for (const auto& info : kVoiceInfo) c.emplace_back(info.code);
    return c;
}

const std::vector<std::string> kRouteChoices{"Output", "FX 1", "FX 2"};
const std::vector<std::string> kSendBusChoices{"Off", "FX 1", "FX 2"};
const std::vector<std::string> kFxBusChoices{"FX 1", "FX 2"};
const std::vector<std::string> kSyncDivChoices{"1/32", "1/16", "1/8", "1/4", "1/2", "1/1"};
const std::vector<std::string> kSyncKindChoices{"Straight", "Dotted", "Triplet"};
const std::vector<std::string> kFilterTypeChoices{"Off", "Low Pass", "Band Pass", "High Pass"};
const std::vector<std::string> kLfoWaveChoices{"Sine", "Triangle", "Saw", "Square", "Random"};
const std::vector<std::string> kModSourceChoices{"LFO", "Envelope"};
const std::vector<std::string> kDynamicsModeChoices{"Compressor", "VCA"};
const std::vector<std::string> kPcmSourceChoices{"ROM", "Synth", "User"};

constexpr std::array<const char*, kNumFxInstances> kFxNames{
    {"Distortion", "EQ A", "EQ B", "Delay", "Reverb", "Chorus", "Bitcrusher", "Dynamics A", "Dynamics B", "Filter"}};

// Default bus of each instance (plugin.md §2.2): dist, eqa, delay, dyna and
// filter on FX 1, the rest on FX 2.
constexpr std::array<std::uint8_t, kNumFxInstances> kDefaultFxBus{{0, 0, 1, 0, 1, 1, 1, 0, 1, 0}};

// --- the table ------------------------------------------------------------------------

void addVoiceRows(std::vector<ParamDef>& rows, int v) {
    const auto& info = kVoiceInfo[at(v)];
    const std::string code(info.code);
    const std::string group = "voice_" + std::string(info.id);
    const std::string groupName(info.name);
    const auto vid = [v](paramid::VoiceParam p) { return paramid::voice(v, p); };
    const auto name = [&code](const char* what) { return code + " " + what; };
    std::vector<ParamDef> r;

    // Sound: one row per knob of kVoiceInfo.
    for (int k = 0; k < info.numKnobs; ++k) {
        const KnobInfo& knob = info.knobs[at(k)];
        ParamDef d;
        if (knob.unit == KnobUnit::Choice) {
            d = choiceRow(paramid::knob(v, k), code + " " + std::string(knob.name), kPcmSourceChoices,
                          MW_FLOAT(m.engine.voice[at(i / kMaxKnobs)].k[at(i % kMaxKnobs)]));
        } else if (knob.unit == KnobUnit::Semitones) {
            d = floatRow(paramid::knob(v, k), code + " " + std::string(knob.name), knob.min, knob.max,
                         MW_FLOAT(m.engine.voice[at(i / kMaxKnobs)].k[at(i % kMaxKnobs)]), "st", 2, 0.01);
        } else {
            d = floatRow(paramid::knob(v, k), code + " " + std::string(knob.name), knob.min, knob.max,
                         MW_FLOAT(m.engine.voice[at(i / kMaxKnobs)].k[at(i % kMaxKnobs)]), {}, 3);
        }
        d.index = v * kMaxKnobs + k;
        d.kit = true;
        r.push_back(std::move(d));
    }

    // Mixer channel.
    using VP = paramid::VoiceParam;
    std::vector<ParamDef> mixer;
    mixer.push_back(floatRow(vid(VP::Level), name("Level"), 0.0f, 1.0f, MW_FLOAT(m.engine.channel[at(i)].level), {}, 3));
    mixer.push_back(floatRow(vid(VP::Pan), name("Pan"), -1.0f, 1.0f, MW_FLOAT(m.engine.channel[at(i)].pan), {}, 2));
    mixer.push_back(floatRow(vid(VP::Spread), name("Spread"), 0.0f, 1.0f, MW_FLOAT(m.engine.channel[at(i)].spread), {}, 3));
    mixer.push_back(floatRow(vid(VP::Drive), name("Drive"), 0.0f, 1.0f, MW_FLOAT(m.engine.channel[at(i)].drive), {}, 3));
    mixer.push_back(boolRow(vid(VP::Mute), name("Mute"), MW_BOOL(m.mute[at(i)])));
    mixer.push_back(boolRow(vid(VP::Solo), name("Solo"), MW_BOOL(m.solo[at(i)])));
    mixer.push_back(choiceRow(vid(VP::Out), name("Output"), outputChoices(),
                              MW_ENUM(m.engine.channel[at(i)].output, std::uint8_t)));
    mixer.push_back(choiceRow(vid(VP::Route), name("Route"), kRouteChoices,
                              MW_ENUM(m.engine.channel[at(i)].route, ChannelRoute)));
    mixer.push_back(choiceRow(vid(VP::SendBus), name("Send Bus"), kSendBusChoices,
                              MW_ENUM(m.engine.channel[at(i)].sendBus, std::uint8_t)));
    mixer.push_back(floatRow(vid(VP::Send), name("Send"), 0.0f, 1.0f, MW_FLOAT(m.engine.channel[at(i)].send), {}, 3));
    mixer.push_back(boolRow(vid(VP::SendPre), name("Send Pre"), MW_BOOL(m.engine.channel[at(i)].sendPre)));

    // Strip: Filter/EQ.
    mixer.push_back(boolRow(vid(VP::EqOn), name("EQ On"), MW_BOOL(m.engine.strip[at(i)].eqOn)));
    mixer.push_back(floatRow(vid(VP::LowCut), name("Low Cut"), 20.0f, 2000.0f, MW_FLOAT(m.engine.strip[at(i)].lowCutHz),
                             "Hz", 1, 0.0, ParamMap::Log));
    mixer.push_back(floatRow(vid(VP::HighCut), name("High Cut"), 500.0f, 20000.0f,
                             MW_FLOAT(m.engine.strip[at(i)].highCutHz), "Hz", 0, 0.0, ParamMap::Log));
    mixer.push_back(floatRow(vid(VP::Reso), name("Resonance"), 0.0f, 1.0f, MW_FLOAT(m.engine.strip[at(i)].resonance), {}, 3));
    mixer.push_back(boolRow(vid(VP::Eq1On), name("EQ1 On"), MW_BOOL(m.engine.strip[at(i)].band[0].on)));
    mixer.push_back(floatRow(vid(VP::Eq1Freq), name("EQ1 Freq"), 20.0f, 20000.0f,
                             MW_FLOAT(m.engine.strip[at(i)].band[0].freqHz), "Hz", 0, 0.0, ParamMap::Log));
    mixer.push_back(floatRow(vid(VP::Eq1Width), name("EQ1 Width"), 0.5f, 4.0f,
                             MW_FLOAT(m.engine.strip[at(i)].band[0].widthOct), "oct", 2));
    mixer.push_back(floatRow(vid(VP::Eq1Gain), name("EQ1 Gain"), -24.0f, 24.0f,
                             MW_FLOAT(m.engine.strip[at(i)].band[0].gainDb), "dB", 2, 0.01));
    mixer.push_back(boolRow(vid(VP::Eq2On), name("EQ2 On"), MW_BOOL(m.engine.strip[at(i)].band[1].on)));
    mixer.push_back(floatRow(vid(VP::Eq2Freq), name("EQ2 Freq"), 20.0f, 20000.0f,
                             MW_FLOAT(m.engine.strip[at(i)].band[1].freqHz), "Hz", 0, 0.0, ParamMap::Log));
    mixer.push_back(floatRow(vid(VP::Eq2Width), name("EQ2 Width"), 0.5f, 4.0f,
                             MW_FLOAT(m.engine.strip[at(i)].band[1].widthOct), "oct", 2));
    mixer.push_back(floatRow(vid(VP::Eq2Gain), name("EQ2 Gain"), -24.0f, 24.0f,
                             MW_FLOAT(m.engine.strip[at(i)].band[1].gainDb), "dB", 2, 0.01));

    // Strip: compressor.
    mixer.push_back(boolRow(vid(VP::CompOn), name("Comp On"), MW_BOOL(m.engine.strip[at(i)].compOn)));
    mixer.push_back(boolRow(vid(VP::CompPre), name("Comp Pre EQ"), MW_BOOL(m.engine.strip[at(i)].compPreEq)));
    mixer.push_back(floatRow(vid(VP::CompThresh), name("Comp Threshold"), -48.0f, 0.0f,
                             MW_FLOAT(m.engine.strip[at(i)].compThresholdDb), "dB", 2, 0.01));
    mixer.push_back(floatRow(vid(VP::CompRatio), name("Comp Ratio"), 1.0f, 20.0f,
                             MW_FLOAT(m.engine.strip[at(i)].compRatio), ":1", 2, 0.01));
    mixer.push_back(floatRow(vid(VP::CompAttack), name("Comp Attack"), 1.0f, 500.0f,
                             MW_FLOAT(m.engine.strip[at(i)].compAttackMs), "ms", 1, 0.0, ParamMap::Log));
    mixer.push_back(floatRow(vid(VP::CompRelease), name("Comp Release"), 1.0f, 500.0f,
                             MW_FLOAT(m.engine.strip[at(i)].compReleaseMs), "ms", 1, 0.0, ParamMap::Log));
    mixer.push_back(floatRow(vid(VP::CompMakeup), name("Comp Makeup"), 0.0f, 24.0f,
                             MW_FLOAT(m.engine.strip[at(i)].compMakeupDb), "dB", 2, 0.01));

    for (auto& d : mixer) {
        d.index = v;
        d.kit = d.id != vid(VP::Mute) && d.id != vid(VP::Solo);
        r.push_back(std::move(d));
    }
    for (auto& d : r) {
        d.groupId = group;
        d.groupName = groupName;
        rows.push_back(std::move(d));
    }
}

void addFxRows(std::vector<ParamDef>& rows) {
    std::vector<ParamDef> r;

    // Bus returns.
    for (int b = 0; b < kNumFxBuses; ++b) {
        auto d = floatRow(paramid::fxBus(b, "level"), "FX " + std::to_string(b + 1) + " Level", -60.0f, 12.0f,
                          MW_FLOAT(m.engine.fx.bus[at(i)].levelDb), "dB", 2, 0.01);
        d.index = b;
        r.push_back(std::move(d));
    }
    for (int b = 0; b < kNumFxBuses; ++b) {
        auto d = choiceRow(paramid::fxBus(b, "out"), "FX " + std::to_string(b + 1) + " Output", outputChoices(),
                           MW_ENUM(m.engine.fx.bus[at(i)].output, std::uint8_t));
        d.index = b;
        r.push_back(std::move(d));
    }
    r.push_back(floatRow(paramid::fx1To2, "FX 1 to FX 2", 0.0f, 1.0f, MW_FLOAT(m.engine.fx.bus[0].toBus2), {}, 3));
    r.push_back(boolRow(paramid::fx1To2Pre, "FX 1 to FX 2 Pre", MW_BOOL(m.engine.fx.bus[0].toBus2Pre)));

    // Placement of every instance.
    for (int f = 0; f < kNumFxInstances; ++f) {
        const std::string fxName = kFxNames[at(f)];
        auto on = boolRow(paramid::fx(f, "on"), fxName + " On", MW_BOOL(m.engine.fx.place[at(i)].on));
        auto bus = choiceRow(paramid::fx(f, "bus"), fxName + " Bus", kFxBusChoices,
                             MW_ENUM(m.engine.fx.place[at(i)].bus, std::uint8_t));
        auto order = intRow(paramid::fx(f, "order"), fxName + " Order", 0, kNumFxInstances - 1,
                            MW_ENUM(m.engine.fx.place[at(i)].order, std::uint8_t));
        for (auto* d : {&on, &bus, &order}) {
            d->index = f;
            r.push_back(std::move(*d));
        }
    }

    // Effect parameters, instance by instance (FxParams.h field order).
    const auto add = [&r](int instance, std::string field, std::string fieldName, ParamDef d, int index = 0) {
        d.id = paramid::fx(instance, field);
        d.name = std::string(kFxNames[at(instance)]) + " " + fieldName;
        d.index = index;
        r.push_back(std::move(d));
    };
    const int dist = static_cast<int>(FxId::Distortion);
    add(dist, "preamp", "Preamp", floatRow({}, {}, 0.0f, 36.0f, MW_FLOAT(m.engine.fx.distortion.preampDb), "dB", 2, 0.01));
    add(dist, "threshold", "Threshold",
        floatRow({}, {}, -36.0f, 0.0f, MW_FLOAT(m.engine.fx.distortion.thresholdDb), "dB", 2, 0.01));
    add(dist, "knee", "Knee", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.distortion.knee), {}, 3));
    add(dist, "split", "Split",
        floatRow({}, {}, 0.0f, 2000.0f, MW_FLOAT(m.engine.fx.distortion.splitHz), "Hz", 1, 0.0, ParamMap::Cubic));
    add(dist, "low_out", "Low Out",
        floatRow({}, {}, -60.0f, 12.0f, MW_FLOAT(m.engine.fx.distortion.lowOutDb), "dB", 2, 0.01));
    add(dist, "clip_out", "Clip Out",
        floatRow({}, {}, -60.0f, 12.0f, MW_FLOAT(m.engine.fx.distortion.clipOutDb), "dB", 2, 0.01));
    add(dist, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.distortion.mix), {}, 3));

    for (int e = 0; e < 2; ++e) {
        const int fx = static_cast<int>(e == 0 ? FxId::EqA : FxId::EqB);
        add(fx, "low_shelf", "Low Shelf", boolRow({}, {}, MW_BOOL(m.engine.fx.eq[at(i)].lowShelf)), e);
        add(fx, "low_freq", "Low Freq",
            floatRow({}, {}, 20.0f, 2000.0f, MW_FLOAT(m.engine.fx.eq[at(i)].lowHz), "Hz", 1, 0.0, ParamMap::Log), e);
        add(fx, "low_gain", "Low Gain",
            floatRow({}, {}, -24.0f, 24.0f, MW_FLOAT(m.engine.fx.eq[at(i)].lowGainDb), "dB", 2, 0.01), e);
        add(fx, "low_res", "Low Resonance", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.eq[at(i)].lowRes), {}, 3), e);
        add(fx, "mid1_on", "Mid 1 On", boolRow({}, {}, MW_BOOL(m.engine.fx.eq[at(i)].mid[0].on)), e);
        add(fx, "mid1_freq", "Mid 1 Freq",
            floatRow({}, {}, 20.0f, 20000.0f, MW_FLOAT(m.engine.fx.eq[at(i)].mid[0].freqHz), "Hz", 0, 0.0, ParamMap::Log), e);
        add(fx, "mid1_width", "Mid 1 Width",
            floatRow({}, {}, 0.5f, 4.0f, MW_FLOAT(m.engine.fx.eq[at(i)].mid[0].widthOct), "oct", 2), e);
        add(fx, "mid1_gain", "Mid 1 Gain",
            floatRow({}, {}, -24.0f, 24.0f, MW_FLOAT(m.engine.fx.eq[at(i)].mid[0].gainDb), "dB", 2, 0.01), e);
        add(fx, "mid2_on", "Mid 2 On", boolRow({}, {}, MW_BOOL(m.engine.fx.eq[at(i)].mid[1].on)), e);
        add(fx, "mid2_freq", "Mid 2 Freq",
            floatRow({}, {}, 20.0f, 20000.0f, MW_FLOAT(m.engine.fx.eq[at(i)].mid[1].freqHz), "Hz", 0, 0.0, ParamMap::Log), e);
        add(fx, "mid2_width", "Mid 2 Width",
            floatRow({}, {}, 0.5f, 4.0f, MW_FLOAT(m.engine.fx.eq[at(i)].mid[1].widthOct), "oct", 2), e);
        add(fx, "mid2_gain", "Mid 2 Gain",
            floatRow({}, {}, -24.0f, 24.0f, MW_FLOAT(m.engine.fx.eq[at(i)].mid[1].gainDb), "dB", 2, 0.01), e);
        add(fx, "high_shelf", "High Shelf", boolRow({}, {}, MW_BOOL(m.engine.fx.eq[at(i)].highShelf)), e);
        add(fx, "high_freq", "High Freq",
            floatRow({}, {}, 500.0f, 20000.0f, MW_FLOAT(m.engine.fx.eq[at(i)].highHz), "Hz", 0, 0.0, ParamMap::Log), e);
        add(fx, "high_gain", "High Gain",
            floatRow({}, {}, -24.0f, 24.0f, MW_FLOAT(m.engine.fx.eq[at(i)].highGainDb), "dB", 2, 0.01), e);
        add(fx, "high_res", "High Resonance",
            floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.eq[at(i)].highRes), {}, 3), e);
    }

    const int delay = static_cast<int>(FxId::Delay);
    add(delay, "sync", "Sync", boolRow({}, {}, MW_BOOL(m.engine.fx.delay.sync)));
    add(delay, "time", "Time",
        floatRow({}, {}, 1.0f, 1000.0f, MW_FLOAT(m.engine.fx.delay.timeMs), "ms", 1, 0.0, ParamMap::Log));
    add(delay, "div", "Division", choiceRow({}, {}, kSyncDivChoices, MW_ENUM(m.engine.fx.delay.div, SyncDiv)));
    add(delay, "kind", "Timing", choiceRow({}, {}, kSyncKindChoices, MW_ENUM(m.engine.fx.delay.kind, SyncKind)));
    add(delay, "feedback", "Feedback", floatRow({}, {}, 0.0f, 0.98f, MW_FLOAT(m.engine.fx.delay.feedback), {}, 3));
    add(delay, "spread", "Spread", floatRow({}, {}, -1.0f, 1.0f, MW_FLOAT(m.engine.fx.delay.spread), {}, 2));
    add(delay, "loop_filter", "Loop Filter",
        choiceRow({}, {}, kFilterTypeChoices, MW_ENUM(m.engine.fx.delay.loopFilter, FilterType)));
    add(delay, "cutoff", "Cutoff",
        floatRow({}, {}, 20.0f, 20000.0f, MW_FLOAT(m.engine.fx.delay.cutoffHz), "Hz", 0, 0.0, ParamMap::Log));
    add(delay, "reso", "Resonance", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.delay.reso), {}, 3));
    add(delay, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.delay.mix), {}, 3));

    const int reverb = static_cast<int>(FxId::Reverb);
    add(reverb, "predelay", "Predelay", floatRow({}, {}, 0.0f, 250.0f, MW_FLOAT(m.engine.fx.reverb.predelayMs), "ms", 1));
    add(reverb, "early_size", "Early Size", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.earlySize), {}, 3));
    add(reverb, "early_diffusion", "Early Diffusion",
        floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.earlyDiffusion), {}, 3));
    add(reverb, "early_color", "Early Color", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.earlyColor), {}, 3));
    add(reverb, "late_size", "Late Size", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.lateSize), {}, 3));
    add(reverb, "late_diffusion", "Late Diffusion",
        floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.lateDiffusion), {}, 3));
    add(reverb, "late_color", "Late Color", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.lateColor), {}, 3));
    add(reverb, "decay", "Decay", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.decay), {}, 3));
    add(reverb, "modulation", "Modulation", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.modulation), {}, 3));
    add(reverb, "early_late", "Early/Late", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.earlyLate), {}, 3));
    add(reverb, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.reverb.mix), {}, 3));

    const int chorus = static_cast<int>(FxId::Chorus);
    add(chorus, "offset", "Offset", floatRow({}, {}, 1.0f, 30.0f, MW_FLOAT(m.engine.fx.chorus.offsetMs), "ms", 2));
    add(chorus, "depth", "Depth", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.chorus.depth), {}, 3));
    add(chorus, "rate", "Rate",
        floatRow({}, {}, 0.02f, 10.0f, MW_FLOAT(m.engine.fx.chorus.rateHz), "Hz", 3, 0.0, ParamMap::Log));
    add(chorus, "feedback", "Feedback", floatRow({}, {}, -0.95f, 0.95f, MW_FLOAT(m.engine.fx.chorus.feedback), {}, 3));
    add(chorus, "spatial", "Spatial", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.chorus.spatial), {}, 3));
    add(chorus, "low_cut", "Low Cut",
        floatRow({}, {}, 20.0f, 20000.0f, MW_FLOAT(m.engine.fx.chorus.lowCutHz), "Hz", 0, 0.0, ParamMap::Log));
    add(chorus, "high_cut", "High Cut",
        floatRow({}, {}, 20.0f, 20000.0f, MW_FLOAT(m.engine.fx.chorus.highCutHz), "Hz", 0, 0.0, ParamMap::Log));
    add(chorus, "fatness", "Fatness", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.chorus.fatness), {}, 3));
    add(chorus, "gain", "Gain", floatRow({}, {}, -24.0f, 24.0f, MW_FLOAT(m.engine.fx.chorus.gainDb), "dB", 2, 0.01));
    add(chorus, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.chorus.mix), {}, 3));

    const int crush = static_cast<int>(FxId::Bitcrusher);
    add(crush, "bits", "Bits", floatRow({}, {}, 1.0f, 24.0f, MW_FLOAT(m.engine.fx.bitcrusher.bits), {}, 2));
    add(crush, "dither", "Dither", boolRow({}, {}, MW_BOOL(m.engine.fx.bitcrusher.dither)));
    add(crush, "rate", "Rate",
        floatRow({}, {}, 44.0f, 44100.0f, MW_FLOAT(m.engine.fx.bitcrusher.rateHz), "Hz", 0, 0.0, ParamMap::Log));
    add(crush, "jitter", "Jitter", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.bitcrusher.jitter), {}, 3));
    add(crush, "approx_filter", "Approx Filter",
        floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.bitcrusher.approxFilter), {}, 3));
    add(crush, "images_filter", "Images Filter",
        floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.bitcrusher.imagesFilter), {}, 3));
    add(crush, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.bitcrusher.mix), {}, 3));

    for (int e = 0; e < 2; ++e) {
        const int fx = static_cast<int>(e == 0 ? FxId::DynamicsA : FxId::DynamicsB);
        add(fx, "mode", "Mode", choiceRow({}, {}, kDynamicsModeChoices, MW_ENUM(m.engine.fx.dynamics[at(i)].mode, DynamicsMode)), e);
        add(fx, "threshold", "Threshold",
            floatRow({}, {}, -48.0f, 0.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].thresholdDb), "dB", 2, 0.01), e);
        add(fx, "ratio", "Ratio", floatRow({}, {}, 1.0f, 20.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].ratio), ":1", 2, 0.01), e);
        add(fx, "attack", "Attack",
            floatRow({}, {}, 1.0f, 500.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].attackMs), "ms", 1, 0.0, ParamMap::Log), e);
        add(fx, "hold", "Hold", floatRow({}, {}, 0.0f, 500.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].holdMs), "ms", 1), e);
        add(fx, "release", "Release",
            floatRow({}, {}, 1.0f, 500.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].releaseMs), "ms", 1, 0.0, ParamMap::Log), e);
        add(fx, "makeup", "Makeup",
            floatRow({}, {}, 0.0f, 24.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].makeupDb), "dB", 2, 0.01), e);
        add(fx, "sidechain", "Sidechain", choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.dynamics[at(i)].sidechain)), e);
        add(fx, "trigger1", "Trigger 1",
            choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.dynamics[at(i)].triggers[0])), e);
        add(fx, "trigger2", "Trigger 2",
            choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.dynamics[at(i)].triggers[1])), e);
        add(fx, "trigger3", "Trigger 3",
            choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.dynamics[at(i)].triggers[2])), e);
        add(fx, "vca_depth", "VCA Depth",
            floatRow({}, {}, -1.0f, 1.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].vcaDepth), {}, 2), e);
        add(fx, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.dynamics[at(i)].mix), {}, 3), e);
    }

    const int filter = static_cast<int>(FxId::Filter);
    add(filter, "type", "Type", choiceRow({}, {}, kFilterTypeChoices, MW_ENUM(m.engine.fx.filter.type, FilterType)));
    add(filter, "cutoff", "Cutoff",
        floatRow({}, {}, 20.0f, 20000.0f, MW_FLOAT(m.engine.fx.filter.cutoffHz), "Hz", 0, 0.0, ParamMap::Log));
    add(filter, "reso", "Resonance", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.filter.reso), {}, 3));
    add(filter, "source", "Mod Source", choiceRow({}, {}, kModSourceChoices, MW_ENUM(m.engine.fx.filter.source, ModSource)));
    add(filter, "lfo_sync", "LFO Sync", boolRow({}, {}, MW_BOOL(m.engine.fx.filter.lfoSync)));
    add(filter, "lfo_rate", "LFO Rate",
        floatRow({}, {}, 0.01f, 20.0f, MW_FLOAT(m.engine.fx.filter.lfoRateHz), "Hz", 3, 0.0, ParamMap::Log));
    add(filter, "lfo_div", "LFO Division", choiceRow({}, {}, kSyncDivChoices, MW_ENUM(m.engine.fx.filter.lfoDiv, SyncDiv)));
    add(filter, "lfo_wave", "LFO Wave", choiceRow({}, {}, kLfoWaveChoices, MW_ENUM(m.engine.fx.filter.lfoWave, LfoWave)));
    add(filter, "lfo_spread", "LFO Spread", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.filter.lfoSpread), {}, 3));
    add(filter, "env_attack", "Env Attack",
        floatRow({}, {}, 1.0f, 500.0f, MW_FLOAT(m.engine.fx.filter.envAttackMs), "ms", 1, 0.0, ParamMap::Log));
    add(filter, "env_hold", "Env Hold", floatRow({}, {}, 0.0f, 500.0f, MW_FLOAT(m.engine.fx.filter.envHoldMs), "ms", 1));
    add(filter, "env_release", "Env Release",
        floatRow({}, {}, 1.0f, 500.0f, MW_FLOAT(m.engine.fx.filter.envReleaseMs), "ms", 1, 0.0, ParamMap::Log));
    add(filter, "env_trigger1", "Env Trigger 1",
        choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.filter.envTriggers[0])));
    add(filter, "env_trigger2", "Env Trigger 2",
        choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.filter.envTriggers[1])));
    add(filter, "env_trigger3", "Env Trigger 3",
        choiceRow({}, {}, voiceChoices(true), MW_SOURCE(m.engine.fx.filter.envTriggers[2])));
    add(filter, "mod_amount", "Mod Amount", floatRow({}, {}, -1.0f, 1.0f, MW_FLOAT(m.engine.fx.filter.modAmount), {}, 3));
    add(filter, "mix", "Mix", floatRow({}, {}, 0.0f, 1.0f, MW_FLOAT(m.engine.fx.filter.mix), {}, 3));

    for (auto& d : r) {
        d.kit = true;
        d.groupId = "fx";
        d.groupName = "FX";
        rows.push_back(std::move(d));
    }
}

void addMasterRows(std::vector<ParamDef>& rows) {
    std::vector<ParamDef> r;
    // 0.01 dB grid: 0 dB has no exact image under a float normalised value on
    // a -60..+12 range; the grid makes the default (and every readout) exact.
    r.push_back(floatRow(paramid::masterVolume, "Master Volume", -60.0f, 12.0f, MW_FLOAT(m.engine.master.volumeDb),
                         "dB", 2, 0.01));
    r.push_back(choiceRow(paramid::outputStage, "Output Stage", {"Off", "Modern", "Original"},
                          MW_ENUM(m.engine.master.stage, OutputStage)));
    r.push_back(floatRow(paramid::masterDrive, "Master Drive", 0.0f, 1.0f, MW_FLOAT(m.engine.master.drive), {}, 3));
    r.push_back(boolRow(paramid::mcompOn, "Multiband Comp On", MW_BOOL(m.engine.master.compOn)));
    r.push_back(floatRow(paramid::mcompXLow, "Multiband Comp Low X", 40.0f, 1000.0f, MW_FLOAT(m.engine.master.xoverLowHz),
                         "Hz", 0, 0.0, ParamMap::Log));
    r.push_back(floatRow(paramid::mcompXHigh, "Multiband Comp High X", 1000.0f, 12000.0f,
                         MW_FLOAT(m.engine.master.xoverHighHz), "Hz", 0, 0.0, ParamMap::Log));
    for (int b = 0; b < 3; ++b) {
        static constexpr std::array<const char*, 3> kBandNames{{"Low", "Mid", "High"}};
        const std::string prefix = std::string("Multiband ") + kBandNames[at(b)] + " ";
        std::vector<ParamDef> band;
        band.push_back(boolRow(paramid::band(b, "on"), prefix + "On", MW_BOOL(m.engine.master.band[at(i)].on)));
        band.push_back(boolRow(paramid::band(b, "solo"), prefix + "Solo", MW_BOOL(m.engine.master.band[at(i)].solo)));
        band.push_back(floatRow(paramid::band(b, "thresh"), prefix + "Threshold", -48.0f, 0.0f,
                                MW_FLOAT(m.engine.master.band[at(i)].thresholdDb), "dB", 2, 0.01));
        band.push_back(floatRow(paramid::band(b, "ratio"), prefix + "Ratio", 1.0f, 20.0f,
                                MW_FLOAT(m.engine.master.band[at(i)].ratio), ":1", 2, 0.01));
        band.push_back(floatRow(paramid::band(b, "attack"), prefix + "Attack", 1.0f, 500.0f,
                                MW_FLOAT(m.engine.master.band[at(i)].attackMs), "ms", 1, 0.0, ParamMap::Log));
        band.push_back(floatRow(paramid::band(b, "release"), prefix + "Release", 1.0f, 500.0f,
                                MW_FLOAT(m.engine.master.band[at(i)].releaseMs), "ms", 1, 0.0, ParamMap::Log));
        band.push_back(floatRow(paramid::band(b, "makeup"), prefix + "Makeup", 0.0f, 24.0f,
                                MW_FLOAT(m.engine.master.band[at(i)].makeupDb), "dB", 2, 0.01));
        for (auto& d : band) {
            d.index = b;
            r.push_back(std::move(d));
        }
    }
    r.push_back(boolRow(paramid::limiter, "Limiter", MW_BOOL(m.engine.master.limiterOn)));
    r.push_back(floatRow(paramid::limThresh, "Limiter Threshold", -24.0f, 0.0f, MW_FLOAT(m.engine.master.limThresholdDb),
                         "dB", 2, 0.01));
    r.push_back(floatRow(paramid::limAttack, "Limiter Attack", 0.1f, 10.0f, MW_FLOAT(m.engine.master.limAttackMs), "ms",
                         2, 0.0, ParamMap::Log));
    r.push_back(floatRow(paramid::limRelease, "Limiter Release", 10.0f, 1000.0f, MW_FLOAT(m.engine.master.limReleaseMs),
                         "ms", 1, 0.0, ParamMap::Log));
    r.push_back(boolRow(paramid::softClip, "Soft Clip", MW_BOOL(m.engine.master.softClip)));
    for (auto& d : r) {
        d.kit = true;
        d.groupId = "master";
        d.groupName = "Master";
        rows.push_back(std::move(d));
    }
}

void addTriggerRows(std::vector<ParamDef>& rows) {
    for (int n = 0; n < kNumTriggerOuts; ++n) {
        const std::string prefix = "Trigger " + std::to_string(n + 1) + " ";
        std::vector<ParamDef> r;
        r.push_back(boolRow(paramid::trig(n, "on"), prefix + "On", MW_BOOL(m.engine.trig[at(i)].on)));
        r.push_back(choiceRow(paramid::trig(n, "source"), prefix + "Source", voiceChoices(false),
                              MW_ENUM(m.engine.trig[at(i)].source, std::uint8_t)));
        r.push_back(choiceRow(paramid::trig(n, "route"), prefix + "Route", kRouteChoices,
                              MW_ENUM(m.engine.trig[at(i)].route, ChannelRoute)));
        r.push_back(choiceRow(paramid::trig(n, "out"), prefix + "Output", outputChoices(),
                              MW_ENUM(m.engine.trig[at(i)].output, std::uint8_t)));
        r.push_back(floatRow(paramid::trig(n, "pan"), prefix + "Pan", -1.0f, 1.0f, MW_FLOAT(m.engine.trig[at(i)].pan), {}, 2));
        r.push_back(floatRow(paramid::trig(n, "level"), prefix + "Level", 0.0f, 1.0f, MW_FLOAT(m.engine.trig[at(i)].level),
                             {}, 3));
        for (auto& d : r) {
            d.index = n;
            d.kit = true;
            d.groupId = "trigger";
            d.groupName = "Trigger Outs";
            rows.push_back(std::move(d));
        }
    }
}

void addGlobalRows(std::vector<ParamDef>& rows) {
    std::vector<ParamDef> global;
    {
        auto d = floatRow(paramid::totalAccent, "Total Accent", 0.0f, 1.0f, MW_FLOAT(m.seq.totalAccent), {}, 3);
        d.kit = true;
        global.push_back(std::move(d));
    }
    global.push_back(boolRow(paramid::hatChoke, "Hat Choke", MW_BOOL(m.engine.hatChoke)));
    // Quality, variance and the revision rebuild the voices when they change
    // (which silences them): settings, not automation targets.
    {
        auto d = choiceRow(paramid::quality, "Quality", {"Eco", "Standard", "High"}, MW_ENUM(m.quality, Quality));
        d.automatable = false;
        global.push_back(std::move(d));
    }
    {
        auto d = floatRow(paramid::variance, "Analog Variance", 0.0f, 1.0f, MW_FLOAT(m.variance), {}, 3);
        d.automatable = false;
        global.push_back(std::move(d));
    }
    {
        auto d = choiceRow(paramid::revision, "Unit Revision", {"Late", "Early"}, MW_ENUM(m.revision, Revision));
        d.automatable = false;
        d.kit = true;
        global.push_back(std::move(d));
    }
    for (auto& d : global) {
        d.groupId = "global";
        d.groupName = "Global";
        rows.push_back(std::move(d));
    }

    std::vector<ParamDef> sequencer;
    sequencer.push_back(choiceRow(paramid::seqMode, "Sequencer Mode", {"Off", "Host Sync", "Internal", "Host Note"},
                                  MW_ENUM(m.seq.mode, seq::Mode)));
    sequencer.push_back(boolRow(paramid::seqRun, "Run", MW_BOOL(m.seq.run)));
    sequencer.push_back(floatRow(paramid::tempo, "Tempo", 20.0f, 400.0f, MW_DOUBLE(m.seq.internalBpm), "BPM", 1, 0.1));
    sequencer.push_back(floatRow(paramid::swingScale, "Shuffle Depth", 0.0f, 1.0f, MW_FLOAT(m.seq.swingScale), {}, 3));
    sequencer.push_back(
        choiceRow(paramid::variation, "Variation", {"A", "AB", "B"}, MW_ENUM(m.seq.variation, seq::VariationMode)));
    sequencer.push_back(choiceRow(paramid::autoFill, "Auto Fill", {"Manual", "16", "12", "8", "4", "2"},
                                  MW_ENUM(m.seq.autoFill, seq::AutoFill)));
    {
        std::vector<std::string> slots;
        for (int i = 1; i <= seq::kPatternsPerGroup; ++i) slots.push_back(std::to_string(i));
        sequencer.push_back(choiceRow(paramid::fillSlot, "Fill Pattern", slots, MW_ENUM(m.seq.fillSlot, std::uint8_t)));
    }
    sequencer.push_back(
        choiceRow(paramid::playMode, "Play Mode", {"Pattern", "Chain", "Track"}, MW_ENUM(m.seq.play, seq::PlayMode)));
    sequencer.push_back(choiceRow(paramid::songTrack, "Track",
                                  {"I-1", "I-2", "I-3", "I-4", "II-1", "II-2", "II-3", "II-4"},
                                  MW_ENUM(m.seq.songTrack, std::uint8_t)));
    sequencer.push_back(boolRow(paramid::record, "Tap Write", MW_BOOL(m.seq.record)));
    sequencer.push_back(boolRow(paramid::jitter, "Timing Jitter", MW_BOOL(m.seq.jitter)));
    for (auto& d : sequencer) {
        d.groupId = "sequencer";
        d.groupName = "Sequencer";
        rows.push_back(std::move(d));
    }

    std::vector<ParamDef> midi;
    midi.push_back(boolRow(paramid::velocitySens, "Velocity Sensitive", MW_BOOL(m.midi.velocitySensitive)));
    midi.push_back(choiceRow(paramid::midiChannel, "MIDI Channel", channelChoices("Omni"),
                             MW_ENUM(m.midi.receiveChannel, std::uint8_t)));
    midi.push_back(choiceRow(paramid::bassChannel, "Bass Channel", channelChoices("Off"),
                             MW_ENUM(m.midi.bassChannel, std::uint8_t)));
    midi.push_back(boolRow(paramid::midiOut, "MIDI Out", MW_BOOL(m.midi.midiOut)));
    {
        std::vector<std::string> channels;
        for (int i = 1; i <= 16; ++i) channels.push_back(std::to_string(i));
        midi.push_back(choiceRow(paramid::midiOutChannel, "MIDI Out Channel", channels, MW_ONE_BASED(m.midi.outChannel)));
    }
    for (auto& d : midi) {
        d.groupId = "midi";
        d.groupName = "MIDI";
        rows.push_back(std::move(d));
    }
}

#undef MW_FLOAT
#undef MW_DOUBLE
#undef MW_BOOL
#undef MW_ENUM
#undef MW_ONE_BASED
#undef MW_SOURCE

std::vector<ParamDef> buildDefs() {
    std::vector<ParamDef> rows;
    rows.reserve(640);
    for (int v = 0; v < kNumVoices; ++v) addVoiceRows(rows, v);
    addFxRows(rows);
    addMasterRows(rows);
    addTriggerRows(rows);
    addGlobalRows(rows);

    const MachineParams defaults = defaultMachineParams();
    for (auto& d : rows) {
        jassert(d.set != nullptr && d.get != nullptr);
        // A curved range gets a decimal grid two decades below its smallest
        // non-zero value (20..20000 Hz: 0.1 Hz; 1..500 ms: 0.01 ms; a 0..2000
        // cubic: 0.1). Without one, round numbers such as a 200 Hz default
        // have no exact image under a float normalised value.
        if (d.kind == ParamKind::Float && d.step <= 0.0 && d.map != ParamMap::Linear) {
            const double reference = d.map == ParamMap::Log ? static_cast<double>(d.low) : static_cast<double>(d.high) * 1.0e-2;
            d.step = std::pow(10.0, std::floor(std::log10(reference) + 1.0e-6) - 2.0);  // 0.01f counts as 1e-2
        }
        d.def = std::clamp(d.get(defaults, d.index), d.low, d.high);
    }
    return rows;
}

juce::String formatValue(float value, int decimals) {
    // Never show "-0.00".
    const juce::String text(static_cast<double>(value), decimals);
    return text.containsOnly("-0.") ? text.removeCharacters("-") : text;
}

}  // namespace

// --- defaults ---------------------------------------------------------------------------

MachineParams defaultMachineParams() {
    MachineParams m{};
    for (int v = 0; v < kNumVoices; ++v) m.engine.voice[at(v)] = defaultParams(v);
    for (int f = 0; f < kNumFxInstances; ++f) {
        m.engine.fx.place[at(f)].on = false;
        m.engine.fx.place[at(f)].bus = kDefaultFxBus[at(f)];
        m.engine.fx.place[at(f)].order = static_cast<std::uint8_t>(f);
    }
    return m;
}

// --- tables ------------------------------------------------------------------------------

const std::vector<ParamDef>& parameterDefs() {
    static const std::vector<ParamDef> defs = buildDefs();
    return defs;
}

const std::vector<std::string>& allParameterIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (const auto& d : parameterDefs()) out.push_back(d.id);
        return out;
    }();
    return ids;
}

int parameterIndex(const std::string& id) {
    static const std::unordered_map<std::string, int> index = [] {
        std::unordered_map<std::string, int> out;
        const auto& defs = parameterDefs();
        for (std::size_t i = 0; i < defs.size(); ++i) out.emplace(defs[i].id, static_cast<int>(i));
        return out;
    }();
    const auto it = index.find(id);
    return it == index.end() ? -1 : it->second;
}

const std::vector<std::string>& kitParameterIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> out;
        for (const auto& d : parameterDefs())
            if (d.kit) out.push_back(d.id);
        return out;
    }();
    return ids;
}

// --- ranges --------------------------------------------------------------------------------

juce::NormalisableRange<float> makeExactRange(float low, float high, double step, ParamMap map) {
    const double lo = static_cast<double>(low);
    const double hi = static_cast<double>(high);
    const auto fromD = [map, lo, hi](double n) {
        switch (map) {
            case ParamMap::Log: return lo * std::exp(n * std::log(hi / lo));
            case ParamMap::Cubic: return lo + (hi - lo) * n * n * n;
            case ParamMap::Linear: break;
        }
        return lo + (hi - lo) * n;
    };
    const auto toD = [map, lo, hi](double v) {
        double n = 0.0;
        switch (map) {
            case ParamMap::Log: n = v > 0.0 ? std::log(v / lo) / std::log(hi / lo) : 0.0; break;
            case ParamMap::Cubic: n = std::cbrt((v - lo) / (hi - lo)); break;
            case ParamMap::Linear: n = (v - lo) / (hi - lo); break;
        }
        return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
    };
    // On the grid (when there is one) and inside the range. The mapping itself
    // lands on the grid, so every path to a real value (JUCE's APVTS reads a
    // parameter's default through the bare range, without snapping) agrees.
    const auto snapD = [step, low, high](double v) {
        if (step > 0.0) v = static_cast<double>(low) + step * std::floor((v - static_cast<double>(low)) / step + 0.5);
        const auto f = static_cast<float>(v);
        return f < low ? low : (f > high ? high : f);
    };
    const auto from01 = [fromD, snapD](float, float, float n) { return snapD(fromD(static_cast<double>(n))); };
    const auto to01 = [fromD, toD, snapD](float, float, float v) {
        const float n = static_cast<float>(toD(static_cast<double>(v)));
        const auto hits = [&fromD, &snapD, v](float candidate) {
            return candidate >= 0.0f && candidate <= 1.0f &&
                   juce::exactlyEqual(snapD(fromD(static_cast<double>(candidate))), v);
        };
        if (hits(n)) return n;
        // The rounded image of n may miss v by a float step where the mapping
        // is steep; the normalised value v came from is next door.
        float up = n, down = n;
        for (int i = 0; i < 16; ++i) {
            up = std::nextafter(up, 2.0f);
            if (hits(up)) return up;
            down = std::nextafter(down, -1.0f);
            if (hits(down)) return down;
        }
        return n;
    };
    const auto snap = [snapD](float, float, float v) { return snapD(static_cast<double>(v)); };
    juce::NormalisableRange<float> range(low, high, from01, to01, snap);
    range.interval = static_cast<float>(step);
    return range;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    std::unique_ptr<juce::AudioProcessorParameterGroup> group;
    std::string groupId;

    const auto flush = [&] {
        if (group != nullptr) layout.add(std::move(group));
    };

    for (const auto& d : parameterDefs()) {
        if (group == nullptr || d.groupId != groupId) {
            flush();
            groupId = d.groupId;
            group = std::make_unique<juce::AudioProcessorParameterGroup>(juce::String(d.groupId),
                                                                         juce::String(d.groupName), " | ");
        }
        const juce::ParameterID pid{juce::String(d.id), paramid::kParameterVersion};
        const juce::String name(d.name);
        switch (d.kind) {
            case ParamKind::Float: {
                const int decimals = d.decimals;
                group->addChild(std::make_unique<juce::AudioParameterFloat>(
                    pid, name, makeExactRange(d.low, d.high, d.step, d.map), d.def,
                    juce::AudioParameterFloatAttributes()
                        .withLabel(juce::String(d.label))
                        .withAutomatable(d.automatable)
                        .withStringFromValueFunction(
                            [decimals](float v, int) { return formatValue(v, decimals); })));
                break;
            }
            case ParamKind::Toggle:
                group->addChild(std::make_unique<BoolParameter>(
                    pid, name, d.def >= 0.5f, juce::AudioParameterBoolAttributes().withAutomatable(d.automatable)));
                break;
            case ParamKind::Choice: {
                juce::StringArray choices;
                for (const auto& c : d.choices) choices.add(juce::String(c));
                group->addChild(std::make_unique<juce::AudioParameterChoice>(
                    pid, name, choices, static_cast<int>(d.def),
                    juce::AudioParameterChoiceAttributes().withAutomatable(d.automatable)));
                break;
            }
            case ParamKind::Int:
                group->addChild(std::make_unique<juce::AudioParameterInt>(
                    pid, name, static_cast<int>(d.low), static_cast<int>(d.high), static_cast<int>(d.def),
                    juce::AudioParameterIntAttributes().withAutomatable(d.automatable)));
                break;
        }
    }
    flush();
    return layout;
}

// --- the per-block bridge -----------------------------------------------------------------------

Parameters::Parameters(juce::AudioProcessorValueTreeState& apvts) {
    const auto& defs = parameterDefs();
    raw_.reserve(defs.size());
    for (const auto& d : defs) {
        auto* p = apvts.getRawParameterValue(juce::String(d.id));
        jassert(p != nullptr);
        raw_.push_back(p);
    }
}

void Parameters::snapshot(MachineParams& out) const noexcept {
    const auto& defs = parameterDefs();
    const std::size_t n = std::min(defs.size(), raw_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const ParamDef& d = defs[i];
        float v = raw_[i]->load(std::memory_order_relaxed);
        if (!std::isfinite(v)) v = d.def;
        v = v < d.low ? d.low : (v > d.high ? d.high : v);
        if (d.kind != ParamKind::Float) v = std::floor(v + 0.5f);
        d.set(out, d.index, v);
    }
}

std::vector<std::pair<std::string, float>> Parameters::kitValues(const factory::Kit& kit) {
    MachineParams m = defaultMachineParams();
    m.engine.voice = kit.voice;
    m.engine.channel = kit.channel;
    m.engine.strip = kit.strip;
    m.engine.fx = kit.fx;
    // An effect the kit leaves off keeps its default slot (the factory data
    // leaves the placement of unused instances zeroed). Where it sits does not
    // change the sound while it is off, and the stock kit then matches the
    // parameter defaults exactly (hosts check that a new instance reports
    // its declared defaults).
    const MachineParams defaults = defaultMachineParams();
    for (std::size_t f = 0; f < m.engine.fx.place.size(); ++f)
        if (!m.engine.fx.place[f].on) m.engine.fx.place[f] = defaults.engine.fx.place[f];
    m.engine.trig = kit.trig;
    m.engine.master = kit.master;
    m.seq.totalAccent = kit.totalAccent;
    m.revision = kit.revision;

    std::vector<std::pair<std::string, float>> values;
    values.reserve(kitParameterIds().size());
    for (const auto& d : parameterDefs())
        if (d.kit) values.emplace_back(d.id, d.get(m, d.index));
    return values;
}

std::vector<std::pair<std::string, float>> Parameters::presetValues(const factory::Preset& preset) {
    MachineParams m = defaultMachineParams();
    m.seq.internalBpm = static_cast<double>(preset.tempo);
    m.seq.swingScale = preset.swingScale;
    m.seq.variation = preset.variation;
    m.seq.play = preset.play;
    m.seq.songTrack = preset.songTrack;
    m.seq.autoFill = preset.autoFill;

    std::vector<std::pair<std::string, float>> values;
    for (const char* id : {paramid::tempo, paramid::swingScale, paramid::variation, paramid::playMode,
                           paramid::songTrack, paramid::autoFill}) {
        const int i = parameterIndex(id);
        if (i < 0) continue;
        const ParamDef& d = parameterDefs()[at(i)];
        values.emplace_back(d.id, d.get(m, d.index));
    }
    return values;
}

}  // namespace mw909::plugin
