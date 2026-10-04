// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.

#include "Common.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "PatternText.h"
#include "Wav.h"
#include "mw909/Factory.h"
#include "mw909/dsp/Rom.h"

namespace mw909::tools {

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// --- ArgReader -----------------------------------------------------------------

std::string ArgReader::option()
{
    const std::string& a = args_[pos_++];
    if (a.size() < 2 || a[0] != '-')
        throw UsageError("unexpected argument '" + a + "'");
    const std::size_t eq = a.find('=');
    if (a.rfind("--", 0) == 0 && eq != std::string::npos)
    {
        current_ = a.substr(0, eq);
        inline_ = a.substr(eq + 1);
        hasInline_ = true;
    }
    else
    {
        current_ = a;
        inline_.clear();
        hasInline_ = false;
    }
    return current_;
}

std::string ArgReader::value()
{
    if (hasInline_)
    {
        hasInline_ = false;
        return inline_;
    }
    if (pos_ >= args_.size())
        throw UsageError("option " + current_ + " needs a value");
    return args_[pos_++];
}

void ArgReader::flag()
{
    if (hasInline_)
        throw UsageError("option " + current_ + " takes no value");
}

void ArgReader::unknown(const std::string& subcommand) const
{
    throw UsageError("unknown option '" + current_ + "' for '" + subcommand + "'");
}

// --- value parsing -----------------------------------------------------------------

double parseDouble(const std::string& option, const std::string& text, double lo, double hi)
{
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (text.empty() || end == nullptr || *end != '\0' || !std::isfinite(v) || v < lo || v > hi)
    {
        char range[96];
        std::snprintf(range, sizeof range, "%g..%g", lo, hi);
        throw UsageError(option + ": '" + text + "' is not a number in " + range);
    }
    return v;
}

int parseInt(const std::string& option, const std::string& text, int lo, int hi)
{
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || end == nullptr || *end != '\0' || v < lo || v > hi)
        throw UsageError(option + ": '" + text + "' is not an integer in " + std::to_string(lo)
                         + ".." + std::to_string(hi));
    return static_cast<int>(v);
}

std::uint32_t parseU32(const std::string& option, const std::string& text)
{
    // Decimal, so a zero-padded serial ("010") is ten, not octal eight; hex
    // only with an explicit 0x prefix.
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const char* digits = text.c_str() + (hex ? 2 : 0);
    char* end = nullptr;
    const unsigned long long v = std::strtoull(digits, &end, hex ? 16 : 10);
    if (text.empty() || !std::isxdigit(static_cast<unsigned char>(digits[0])) || end == nullptr || *end != '\0'
        || v > 0xFFFFFFFFULL)
        throw UsageError(option + ": '" + text + "' is not an unsigned 32-bit integer");
    return static_cast<std::uint32_t>(v);
}

bool parseBool(const std::string& option, const std::string& text)
{
    const std::string l = lower(text);
    if (l == "1" || l == "on" || l == "true" || l == "yes")
        return true;
    if (l == "0" || l == "off" || l == "false" || l == "no")
        return false;
    throw UsageError(option + ": '" + text + "' is not 0/1 (on/off)");
}

std::vector<std::string> splitList(const std::string& text, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
        if (c == sep)
        {
            out.push_back(cur);
            cur.clear();
        }
        else if (c != ' ')
            cur += c;
    }
    out.push_back(cur);
    return out;
}

VoiceId parseVoice(const std::string& text)
{
    const std::string l = lower(text);
    for (int v = 0; v < kNumVoices; ++v)
        if (kVoiceInfo[static_cast<std::size_t>(v)].id == l)
            return static_cast<VoiceId>(v);
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    if (!text.empty() && end != nullptr && *end == '\0' && n >= 0 && n < kNumVoices)
        return static_cast<VoiceId>(n);
    throw UsageError("unknown voice '" + text + "' (use bd sd lt mt ht rs cp ch oh cr rd, or 0..10)");
}

int knobIndex(int voice, const std::string& id)
{
    const auto& info = kVoiceInfo[static_cast<std::size_t>(voice)];
    const std::string l = lower(id);
    for (int k = 0; k < info.numKnobs; ++k)
        if (info.knobs[static_cast<std::size_t>(k)].id == l)
            return k;
    return -1;
}

float parseKnobValue(int voice, int knob, const std::string& option, const std::string& text)
{
    const KnobInfo& ki = kVoiceInfo[static_cast<std::size_t>(voice)].knobs[static_cast<std::size_t>(knob)];
    if (ki.unit == KnobUnit::Choice)
    {
        const std::string l = lower(text);
        if (l == "rom")
            return static_cast<float>(PcmSource::Rom);
        if (l == "synth")
            return static_cast<float>(PcmSource::Synth);
        if (l == "user")
            return static_cast<float>(PcmSource::User);
        return static_cast<float>(parseInt(option, text, static_cast<int>(ki.min), static_cast<int>(ki.max)));
    }
    return static_cast<float>(parseDouble(option, text, static_cast<double>(ki.min), static_cast<double>(ki.max)));
}

Quality parseQuality(const std::string& text)
{
    const std::string l = lower(text);
    if (l == "eco" || l == "0")
        return Quality::Eco;
    if (l == "standard" || l == "std" || l == "1")
        return Quality::Standard;
    if (l == "high" || l == "2")
        return Quality::High;
    throw UsageError("unknown quality '" + text + "' (use eco, standard, high)");
}

const char* qualityName(Quality q)
{
    switch (q)
    {
        case Quality::Eco:
            return "Eco";
        case Quality::Standard:
            return "Standard";
        case Quality::High:
            return "High";
    }
    return "?";
}

Revision parseRevision(const std::string& text)
{
    const std::string l = lower(text);
    if (l == "late" || l == "0")
        return Revision::Late;
    if (l == "early" || l == "1")
        return Revision::Early;
    throw UsageError("unknown revision '" + text + "' (use late, early)");
}

const char* revisionName(Revision r)
{
    return r == Revision::Early ? "Early" : "Late";
}

seq::VariationMode parseVariationMode(const std::string& text)
{
    const std::string l = lower(text);
    if (l == "a")
        return seq::VariationMode::A;
    if (l == "ab")
        return seq::VariationMode::AB;
    if (l == "b")
        return seq::VariationMode::B;
    throw UsageError("--variation: use A, AB or B");
}

seq::AutoFill parseAutoFill(const std::string& text)
{
    switch (parseInt("--auto-fill", text, 0, 16))
    {
        case 0:
            return seq::AutoFill::Manual;
        case 2:
            return seq::AutoFill::Every2;
        case 4:
            return seq::AutoFill::Every4;
        case 8:
            return seq::AutoFill::Every8;
        case 12:
            return seq::AutoFill::Every12;
        case 16:
            return seq::AutoFill::Every16;
        default:
            throw UsageError("--auto-fill: use 0 (manual), 2, 4, 8, 12 or 16");
    }
}

seq::PlayMode parsePlayMode(const std::string& text)
{
    const std::string l = lower(text);
    if (l == "pattern")
        return seq::PlayMode::Pattern;
    if (l == "chain")
        return seq::PlayMode::Chain;
    if (l == "track" || l == "song")
        return seq::PlayMode::Track;
    throw UsageError("--play: use pattern, chain or track");
}

std::string patternLabel(int index)
{
    return std::string(seq::bankOf(index) == 0 ? "I" : "II") + "-"
           + std::to_string(seq::groupOf(index) + 1) + "-" + std::to_string(seq::slotOf(index) + 1);
}

int parsePatternAddress(const std::string& text)
{
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    if (!text.empty() && end != nullptr && *end == '\0')
        return n >= 0 && n < seq::kNumPatterns ? static_cast<int>(n) : -1;
    for (int i = 0; i < seq::kNumPatterns; ++i)
        if (lower(patternLabel(i)) == lower(text))
            return i;
    return -1;
}

// --- sound setup ------------------------------------------------------------------

EngineParams stockEngine()
{
    EngineParams e;
    for (int v = 0; v < kNumVoices; ++v)
    {
        const auto i = static_cast<std::size_t>(v);
        e.voice[i] = defaultParams(v);
        e.channel[i] = ChannelParams{};
        e.strip[i] = StripParams{};
    }
    e.fx = FxParams{};
    e.master = MasterParams{};
    for (auto& t : e.trig)
        t = TriggerOutParams{};
    return e;
}

void setCleanMaster(MasterParams& m)
{
    m = MasterParams{};
    m.stage = OutputStage::Off;
    m.drive = 0.0f;
    m.compOn = false;
    m.limiterOn = false;
    m.softClip = false;
    m.volumeDb = 0.0f;
}

void applyTypicalMix(EngineParams& e)
{
    auto strip = [&](VoiceId v) -> StripParams& { return e.strip[static_cast<std::size_t>(v)]; };
    auto chan = [&](VoiceId v) -> ChannelParams& { return e.channel[static_cast<std::size_t>(v)]; };

    // Strips: cuts + EQ + compressor on the core of the kit.
    strip(BD).eqOn = true;
    strip(BD).lowCutHz = 30.0f;
    strip(BD).band[0] = EqBand{true, 60.0f, 1.0f, 3.0f};
    strip(BD).compOn = true;
    strip(BD).compThresholdDb = -12.0f;
    strip(BD).compRatio = 4.0f;
    strip(BD).compAttackMs = 10.0f;
    strip(BD).compReleaseMs = 120.0f;
    strip(BD).compMakeupDb = 2.0f;
    strip(SD).eqOn = true;
    strip(SD).lowCutHz = 100.0f;
    strip(SD).band[1] = EqBand{true, 5000.0f, 1.5f, 2.0f};
    strip(SD).compOn = true;
    strip(SD).compPreEq = true;
    strip(SD).compThresholdDb = -18.0f;
    strip(SD).compRatio = 3.0f;
    strip(CP).eqOn = true;
    strip(CP).lowCutHz = 200.0f;
    strip(CP).highCutHz = 9000.0f;
    strip(CP).resonance = 0.3f;
    for (VoiceId v : {CH, OH})
    {
        strip(v).eqOn = true;
        strip(v).lowCutHz = 300.0f;
        strip(v).highCutHz = 16000.0f;
    }

    // Mixer: a little pan, drive and spread; sends into bus 1, CP through bus 2.
    chan(BD).drive = 0.15f;
    chan(LT).pan = -0.4f;
    chan(MT).pan = 0.0f;
    chan(HT).pan = 0.4f;
    chan(CH).pan = 0.25f;
    chan(OH).pan = 0.25f;
    chan(CR).pan = -0.3f;
    chan(RD).pan = 0.3f;
    chan(RD).spread = 0.3f;
    chan(SD).sendBus = 1;
    chan(SD).send = 0.3f;
    chan(HT).sendBus = 1;
    chan(HT).send = 0.25f;
    chan(OH).sendBus = 1;
    chan(OH).send = 0.2f;
    chan(CR).sendBus = 1;
    chan(CR).send = 0.2f;
    chan(CP).route = ChannelRoute::FxBus2;
    chan(RS).sendBus = 2;
    chan(RS).send = 0.4f;
    chan(RS).sendPre = true;

    // FX bus 1: delay -> reverb. FX bus 2: distortion -> filter.
    auto place = [&](FxId id, std::uint8_t bus, std::uint8_t order) {
        e.fx.place[static_cast<std::size_t>(id)] = FxPlacement{true, bus, order};
    };
    place(FxId::Delay, 0, 0);
    place(FxId::Reverb, 0, 1);
    place(FxId::Distortion, 1, 0);
    place(FxId::Filter, 1, 1);
    e.fx.bus[0].levelDb = -6.0f;
    e.fx.bus[1].levelDb = -3.0f;
    e.fx.delay.sync = true;
    e.fx.delay.div = SyncDiv::D1_8;
    e.fx.delay.kind = SyncKind::Dotted;
    e.fx.delay.feedback = 0.35f;
    e.fx.delay.spread = 0.3f;
    e.fx.delay.loopFilter = FilterType::LowPass;
    e.fx.delay.cutoffHz = 4000.0f;
    e.fx.delay.mix = 0.5f;
    e.fx.reverb.decay = 0.4f;
    e.fx.reverb.mix = 0.6f;
    e.fx.distortion.preampDb = 12.0f;
    e.fx.distortion.thresholdDb = -12.0f;
    e.fx.distortion.mix = 0.6f;
    e.fx.filter.type = FilterType::LowPass;
    e.fx.filter.cutoffHz = 3000.0f;
    e.fx.filter.reso = 0.3f;
    e.fx.filter.lfoSync = true;
    e.fx.filter.lfoDiv = SyncDiv::D1_4;
    e.fx.filter.modAmount = 0.3f;

    // Trigger out: the rim-shot click, quietly.
    e.trig[0].on = true;
    e.trig[0].source = RS;
    e.trig[0].level = 0.25f;

    // Master: the hardware output stage, glue compression, limiter.
    e.master.stage = OutputStage::Original;
    e.master.drive = 0.2f;
    e.master.compOn = true;
    for (auto& b : e.master.band)
    {
        b.on = true;
        b.thresholdDb = -12.0f;
        b.ratio = 2.0f;
    }
    e.master.limiterOn = true;
    e.master.softClip = true;
    e.master.volumeDb = -3.0f;
}

const char* const kSetHelp = R"(  --set <key>=<value>, repeatable. Keys:
    <v>.<knob>          a voice knob by id, v = bd sd lt mt ht rs cp ch oh cr rd
                        (bd: tune attack decay pitch depth drive; sd: tune tone
                        snappy decay depth; toms: tune decay depth sweep noise;
                        rs: tune decay; cp: tune decay reverb; ch oh cr rd:
                        tune decay source color density, source = rom|synth|user)
    <v>.level|pan|spread|drive|send     channel (level 0.5 = calibration point)
    <v>.output <0..15>  bus;  <v>.route main|fx1|fx2;  <v>.sendbus 0|1|2;  <v>.sendpre 0|1
    <v>.strip.eq|comp|comppre 0|1, <v>.strip.lowcut|highcut <Hz>, .resonance,
        .threshold <dB>, .ratio, .attack <ms>, .release <ms>, .makeup <dB>
    master.stage off|modern|original, master.drive, master.comp 0|1,
        master.threshold <dB> (all bands), master.ratio, master.limiter 0|1,
        master.softclip 0|1, master.volume <dB>
    fx.<fx>.on 0|1, fx.<fx>.bus 1|2, fx.<fx>.order 0..9, fx.<fx>.mix, with
        <fx> = distortion eqa eqb delay reverb chorus bitcrusher dynamicsa dynamicsb filter;
        fx.delay.feedback, fx.reverb.decay, fx.distortion.preamp <dB>, fx.filter.cutoff <Hz>
    fx.bus1.level|fx.bus2.level <dB>, fx.bus1.output|fx.bus2.output <bus>
    hat.choke 0|1
)";

namespace {

int fxIndex(const std::string& name)
{
    static constexpr const char* kNames[kNumFxInstances] = {
        "distortion", "eqa", "eqb", "delay", "reverb", "chorus", "bitcrusher", "dynamicsa", "dynamicsb", "filter"};
    for (int i = 0; i < kNumFxInstances; ++i)
        if (name == kNames[i])
            return i;
    return -1;
}

float* fxMix(FxParams& fx, int index)
{
    switch (static_cast<FxId>(index))
    {
        case FxId::Distortion:
            return &fx.distortion.mix;
        case FxId::Delay:
            return &fx.delay.mix;
        case FxId::Reverb:
            return &fx.reverb.mix;
        case FxId::Chorus:
            return &fx.chorus.mix;
        case FxId::Bitcrusher:
            return &fx.bitcrusher.mix;
        case FxId::DynamicsA:
            return &fx.dynamics[0].mix;
        case FxId::DynamicsB:
            return &fx.dynamics[1].mix;
        case FxId::Filter:
            return &fx.filter.mix;
        default:
            return nullptr;
    }
}

void applyFxSetting(FxParams& fx, const std::string& field, const std::string& text, const std::string& opt)
{
    const std::size_t dot = field.find('.');
    if (dot == std::string::npos)
        throw UsageError(opt + ": expected fx.<name>.<field>");
    const std::string name = field.substr(0, dot);
    const std::string f = field.substr(dot + 1);
    if (name == "bus1" || name == "bus2")
    {
        auto& b = fx.bus[name == "bus1" ? 0U : 1U];
        if (f == "level")
            b.levelDb = static_cast<float>(parseDouble(opt, text, -60.0, 12.0));
        else if (f == "output")
            b.output = static_cast<std::uint8_t>(parseInt(opt, text, 0, kNumBuses - 1));
        else if (f == "tobus2" && name == "bus1")
            b.toBus2 = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
        else
            throw UsageError(opt + ": unknown bus field (level output tobus2)");
        return;
    }
    const int i = fxIndex(name);
    if (i < 0)
        throw UsageError(opt + ": unknown effect '" + name + "'");
    auto& pl = fx.place[static_cast<std::size_t>(i)];
    if (f == "on")
        pl.on = parseBool(opt, text);
    else if (f == "bus")
        pl.bus = static_cast<std::uint8_t>(parseInt(opt, text, 1, 2) - 1);
    else if (f == "order")
        pl.order = static_cast<std::uint8_t>(parseInt(opt, text, 0, 9));
    else if (f == "mix" && fxMix(fx, i) != nullptr)
        *fxMix(fx, i) = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (name == "delay" && f == "feedback")
        fx.delay.feedback = static_cast<float>(parseDouble(opt, text, 0.0, 0.98));
    else if (name == "reverb" && f == "decay")
        fx.reverb.decay = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (name == "distortion" && f == "preamp")
        fx.distortion.preampDb = static_cast<float>(parseDouble(opt, text, 0.0, 36.0));
    else if (name == "filter" && f == "cutoff")
        fx.filter.cutoffHz = static_cast<float>(parseDouble(opt, text, 20.0, 20000.0));
    else
        throw UsageError(opt + ": unknown field (see help pattern)");
}

void applyStripSetting(StripParams& s, const std::string& f, const std::string& text, const std::string& opt)
{
    if (f == "eq")
        s.eqOn = parseBool(opt, text);
    else if (f == "comp")
        s.compOn = parseBool(opt, text);
    else if (f == "comppre")
        s.compPreEq = parseBool(opt, text);
    else if (f == "lowcut")
        s.lowCutHz = static_cast<float>(parseDouble(opt, text, 20.0, 2000.0));
    else if (f == "highcut")
        s.highCutHz = static_cast<float>(parseDouble(opt, text, 500.0, 20000.0));
    else if (f == "resonance")
        s.resonance = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (f == "threshold")
        s.compThresholdDb = static_cast<float>(parseDouble(opt, text, -48.0, 0.0));
    else if (f == "ratio")
        s.compRatio = static_cast<float>(parseDouble(opt, text, 1.0, 20.0));
    else if (f == "attack")
        s.compAttackMs = static_cast<float>(parseDouble(opt, text, 1.0, 500.0));
    else if (f == "release")
        s.compReleaseMs = static_cast<float>(parseDouble(opt, text, 1.0, 500.0));
    else if (f == "makeup")
        s.compMakeupDb = static_cast<float>(parseDouble(opt, text, 0.0, 24.0));
    else
        throw UsageError(opt + ": unknown strip field");
}

} // namespace

void applySetting(MachineParams& params, const std::string& assignment)
{
    const std::size_t eq = assignment.find('=');
    const std::size_t dot = assignment.find('.');
    if (eq == std::string::npos || dot == std::string::npos || dot > eq)
        throw UsageError("--set: expected <target>.<field>=<value>, got '" + assignment + "'");
    const std::string target = lower(assignment.substr(0, dot));
    const std::string field = lower(assignment.substr(dot + 1, eq - dot - 1));
    const std::string text = assignment.substr(eq + 1);
    const std::string opt = "--set " + target + "." + field;
    EngineParams& e = params.engine;

    if (target == "master")
    {
        auto& m = e.master;
        if (field == "stage")
        {
            const std::string l = lower(text);
            if (l == "off")
                m.stage = OutputStage::Off;
            else if (l == "modern")
                m.stage = OutputStage::Modern;
            else if (l == "original")
                m.stage = OutputStage::Original;
            else
                throw UsageError(opt + ": use off, modern or original");
        }
        else if (field == "drive")
            m.drive = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
        else if (field == "comp")
            m.compOn = parseBool(opt, text);
        else if (field == "threshold")
            for (auto& b : m.band)
                b.thresholdDb = static_cast<float>(parseDouble(opt, text, -48.0, 0.0));
        else if (field == "ratio")
            for (auto& b : m.band)
                b.ratio = static_cast<float>(parseDouble(opt, text, 1.0, 20.0));
        else if (field == "limiter")
            m.limiterOn = parseBool(opt, text);
        else if (field == "softclip")
            m.softClip = parseBool(opt, text);
        else if (field == "volume")
            m.volumeDb = static_cast<float>(parseDouble(opt, text, -60.0, 12.0));
        else
            throw UsageError(opt + ": unknown master field (stage drive comp threshold ratio limiter softclip volume)");
        return;
    }
    if (target == "fx")
    {
        applyFxSetting(e.fx, field, text, opt);
        return;
    }
    if (target == "hat")
    {
        if (field != "choke")
            throw UsageError(opt + ": unknown field (choke)");
        e.hatChoke = parseBool(opt, text);
        return;
    }

    const int v = parseVoice(target);
    const auto vi = static_cast<std::size_t>(v);
    auto& ch = e.channel[vi];
    if (field.rfind("strip.", 0) == 0)
    {
        applyStripSetting(e.strip[vi], field.substr(6), text, opt);
        return;
    }
    const int k = knobIndex(v, field);
    if (k >= 0)
        e.voice[vi].k[static_cast<std::size_t>(k)] = parseKnobValue(v, k, opt, text);
    else if (field == "level")
        ch.level = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (field == "pan")
        ch.pan = static_cast<float>(parseDouble(opt, text, -1.0, 1.0));
    else if (field == "spread")
        ch.spread = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (field == "drive")
        ch.drive = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (field == "send")
        ch.send = static_cast<float>(parseDouble(opt, text, 0.0, 1.0));
    else if (field == "sendbus")
        ch.sendBus = static_cast<std::uint8_t>(parseInt(opt, text, 0, 2));
    else if (field == "sendpre")
        ch.sendPre = parseBool(opt, text);
    else if (field == "output")
        ch.output = static_cast<std::uint8_t>(parseInt(opt, text, 0, kNumBuses - 1));
    else if (field == "route")
    {
        const std::string l = lower(text);
        if (l == "main" || l == "output")
            ch.route = ChannelRoute::Output;
        else if (l == "fx1")
            ch.route = ChannelRoute::FxBus1;
        else if (l == "fx2")
            ch.route = ChannelRoute::FxBus2;
        else
            throw UsageError(opt + ": use main, fx1 or fx2");
    }
    else
        throw UsageError(opt + ": unknown field for " + target
                         + " (a knob id, level pan spread drive send sendbus sendpre output route, strip.*)");
}

// --- buses and output ----------------------------------------------------------------

BusStorage::BusStorage(int numSamples, bool allBuses)
{
    for (int b = 0; b < kNumBuses; ++b)
        for (auto& ch : data[static_cast<std::size_t>(b)])
            ch.assign(static_cast<std::size_t>(b == 0 || allBuses ? numSamples : 0), 0.0f);
}

BusBuffers BusStorage::view(int offset)
{
    BusBuffers v;
    for (std::size_t b = 0; b < kNumBuses; ++b)
        for (std::size_t c = 0; c < 2; ++c)
            v.ch[b][c] = data[b][c].empty() ? nullptr : data[b][c].data() + offset;
    return v;
}

void checkWavFits(long long numFrames, int channels, const std::string& formatText)
{
    SampleFormat format{};
    if (!parseSampleFormat(formatText, format))
        throw UsageError("--format: use 16, 24 or 32f");
    const long long bytesPerSample = format == SampleFormat::Pcm16 ? 2 : (format == SampleFormat::Pcm24 ? 3 : 4);
    // 128 bytes covers the RIFF/fmt/fact/data headers and the pad byte.
    const long long bytes = std::max(0LL, numFrames) * bytesPerSample * channels + 128;
    if (bytes > 0xFFFFFFFFLL)
        throw UsageError("the render would need " + std::to_string(bytes)
                         + " bytes, more than a WAV file holds (4 GiB): shorten it, lower --rate, use "
                           "--channels 1 or a smaller --format");
}

void writeStereoOrMono(const std::string& path, const std::vector<float>& left,
                       const std::vector<float>& right, int first, int numFrames, int sampleRate,
                       int channels, const std::string& formatText, bool quiet)
{
    SampleFormat format{};
    if (!parseSampleFormat(formatText, format))
        throw UsageError("--format: use 16, 24 or 32f");
    if (first < 0 || static_cast<std::size_t>(first + numFrames) > left.size())
        throw RunError("internal: write range outside the render");

    const float* l = left.data() + first;
    const float* r = right.data() + first;
    std::vector<float> mono;
    std::vector<const float*> planes;
    if (channels == 1)
    {
        mono.resize(static_cast<std::size_t>(numFrames));
        for (std::size_t i = 0; i < mono.size(); ++i)
            mono[i] = (l[i] + r[i]) * 0.5f;
        planes.push_back(mono.data());
    }
    else
    {
        planes.push_back(l);
        planes.push_back(r);
    }

    WriteStats stats;
    std::string error;
    if (!writeWav(path, planes, numFrames, sampleRate, format, &stats, error))
        throw RunError(error);
    if (!quiet)
    {
        char peak[48] = "silent";
        if (stats.peak > 0.0f)
            std::snprintf(peak, sizeof peak, "peak %.2f dBFS",
                          20.0 * std::log10(static_cast<double>(stats.peak)));
        std::fprintf(stderr, "wrote %s: %d frames, %d ch, %d Hz, %s, %s%s\n", path.c_str(),
                     numFrames, channels, sampleRate, sampleFormatName(format), peak,
                     stats.clipped > 0 ? " (CLIPPED)" : "");
    }
    if (stats.clipped > 0)
        std::fprintf(stderr, "warning: %s: %lld samples clipped\n", path.c_str(),
                     static_cast<long long>(stats.clipped));
}

RomSlot slotOf(RomRegion region)
{
    switch (region)
    {
        case RomRegion::Crash:
            return RomSlot::Crash;
        case RomRegion::Ride:
            return RomSlot::Ride;
        default:
            return RomSlot::Hat;
    }
}

RomRegion regionForVoice(int voice)
{
    switch (voice)
    {
        case CH:
            return RomRegion::HatClosed;
        case OH:
            return RomRegion::HatOpen;
        case CR:
            return RomRegion::Crash;
        case RD:
            return RomRegion::Ride;
        default:
            throw UsageError("--user-rom: only the sample voices (ch oh cr rd) take a user ROM");
    }
}

void loadUserRom(const std::string& path, RomRegion region, RomImage& image)
{
    WavData wav;
    std::string error;
    if (!readWav(path, wav, error))
        throw RunError("--user-rom: " + error);
    const int frames = wav.numFrames();
    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        float s = 0.0f;
        for (int c = 0; c < wav.channels; ++c)
            s += wav.samples[static_cast<std::size_t>(i * wav.channels + c)];
        mono[static_cast<std::size_t>(i)] = s / static_cast<float>(wav.channels);
    }
    if (!romFromAudio(mono.data(), frames, static_cast<double>(wav.sampleRate), region, image))
        throw RunError("--user-rom: '" + path + "' gave no usable ROM content");
}

void postUserRom(Machine& m, RomSlot slot, const RomImage& image)
{
    auto* h = new RomHandoff;
    h->slot = slot;
    h->clear = false;
    h->image = image;
    delete m.postUserRom(h); // a superseded, never-adopted handoff
}

// --- pattern sources -----------------------------------------------------------------

namespace {

template <typename NameOf>
int resolveByName(const std::string& text, int count, NameOf nameOf, const char* what)
{
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    if (!text.empty() && end != nullptr && *end == '\0')
    {
        if (n < 0 || n >= count)
            throw UsageError(std::string(what) + " " + text + " does not exist (0.."
                             + std::to_string(count - 1) + ")");
        return static_cast<int>(n);
    }
    const std::string l = lower(text);
    int partial = -1, partialCount = 0;
    for (int i = 0; i < count; ++i)
    {
        const std::string name = lower(std::string(nameOf(i)));
        if (name.empty())
            continue;
        if (name == l)
            return i;
        if (name.find(l) != std::string::npos)
        {
            partial = i;
            ++partialCount;
        }
    }
    if (partialCount == 1)
        return partial;
    if (partialCount > 1)
        throw UsageError(std::string(what) + " '" + text + "' is ambiguous (" + std::to_string(partialCount)
                         + " names contain it; see `mw909-render list`)");
    throw UsageError(std::string("no ") + what + " named '" + text + "' (see `mw909-render list`)");
}

} // namespace

int resolveMemory(const std::string& text)
{
    return resolveByName(text, factory::numBanks(), [](int i) { return factory::bankInfo(i).name; },
                         "factory memory");
}

int resolveKit(const std::string& text)
{
    return resolveByName(text, factory::numKits(), [](int i) { return factory::kit(i).name; }, "factory kit");
}

int resolvePreset(const std::string& text)
{
    return resolveByName(text, factory::numPresets(), [](int i) { return factory::preset(i).name; },
                         "factory preset");
}

void loadPatterns(const PatternSource& source, LoadedPatterns& out)
{
    out.bank = seq::PatternBank{};
    out.fromPreset = false;
    if (!source.textFile.empty())
    {
        auto text = std::make_unique<PatternText>();
        std::string error;
        if (!loadPatternText(source.textFile, *text, error))
            throw RunError(error);
        out.bank = text->bank;
        out.selected = text->first();
        if (!source.pattern.empty())
        {
            out.selected = parsePatternAddress(source.pattern);
            if (out.selected < 0)
                throw UsageError("--pattern: with --text, use an address (0..95 or I-1-1..II-3-16)");
        }
        out.name = text->names[static_cast<std::size_t>(out.selected)];
        out.tempo = text->tempo;
        out.sourceLabel = source.textFile + " " + patternLabel(out.selected);
        return;
    }

    int memory = resolveMemory(source.memory);
    int selected = 0;
    if (!source.preset.empty())
    {
        const factory::Preset& p = factory::preset(resolvePreset(source.preset));
        memory = p.bank;
        selected = p.pattern;
        out.fromPreset = true;
        out.presetKit = p.kit;
        out.presetSwing = p.swingScale;
        out.presetVariation = p.variation;
        out.presetPlay = p.play;
        out.presetSongTrack = p.songTrack;
        out.presetAutoFill = p.autoFill;
        out.tempo = static_cast<double>(p.tempo);
    }
    factory::buildBank(memory, out.bank);
    if (!source.pattern.empty())
    {
        selected = parsePatternAddress(source.pattern);
        if (selected < 0)
            selected = resolveByName(source.pattern, seq::kNumPatterns,
                                     [memory](int i) { return factory::patternName(memory, i); }, "pattern");
    }
    out.selected = selected;
    out.name = std::string(factory::patternName(memory, selected));
    if (!out.fromPreset)
        out.tempo = static_cast<double>(factory::bankInfo(memory).tempo);
    out.sourceLabel = "memory " + std::to_string(memory) + " (" + std::string(factory::bankInfo(memory).name)
                      + ") " + patternLabel(selected);
}

bool readPatternSourceOption(ArgReader& r, const std::string& opt, PatternSource& src)
{
    if (opt == "--memory" || opt == "--bank")
        src.memory = r.value();
    else if (opt == "--pattern")
        src.pattern = r.value();
    else if (opt == "--preset")
        src.preset = r.value();
    else if (opt == "--text")
        src.textFile = r.value();
    else
        return false;
    return true;
}

} // namespace mw909::tools
