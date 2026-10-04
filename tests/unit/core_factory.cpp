// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The factory library: every pattern memory builds into legal, named
// patterns (the hardware memory into patterns the original could store),
// every kit value is finite and inside its documented range, kit 0 is the
// stock machine, presets point at real content, names are free of brand
// tokens, and building is deterministic. Beyond validity, it checks the
// library as music: song tracks and chains are phrased in 4-bar phrases with
// the fills on the last bar, no pattern repeats another's groove, each fill
// swings like its group, and preset tempos suit their genre. Pure data
// checks: nothing here needs the engine.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mw909/Factory.h"

using namespace mw909;
namespace fx = mw909::factory;

namespace {

constexpr int kHardwareMemory = 0;  // "Classic Memory": written to the original's rules

std::unique_ptr<seq::PatternBank> build(int bank) {
    auto b = std::make_unique<seq::PatternBank>();
    fx::buildBank(bank, *b);
    return b;
}

bool variationEmpty(const seq::Pattern& p, int var) {
    for (int t = 0; t < seq::kNumTracks; ++t)
        for (int s = 0; s < seq::kMaxSteps; ++s)
            if (p.at(var, t, s).bits != 0) return false;
    return true;
}
bool patternEmpty(const seq::Pattern& p) { return variationEmpty(p, 0) && variationEmpty(p, 1); }

bool memoryEmpty(const seq::PatternBank& b) {
    for (const auto& p : b.patterns)
        if (!patternEmpty(p)) return false;
    return true;
}

// OH and CH sounding on the same step, following each track's own loop.
bool hatsCollide(const seq::Pattern& p, int var) {
    const int lo = p.lengthOf(OH), lc = p.lengthOf(CH);
    const int period = lo / std::gcd(lo, lc) * lc;
    for (int t = 0; t < period; ++t)
        if (p.at(var, OH, t % lo).on() && p.at(var, CH, t % lc).on()) return true;
    return false;
}

// What the original machine could store (research/03 §0, §2; the rules of
// seq::applyHardwareRules), checked independently of the sequencer library.
std::string hardwareViolation(const seq::Pattern& p) {
    if (p.length < 1 || p.length > 16) return "length over 16";
    if (static_cast<int>(p.scale) >= seq::kNumHardwareScales) return "non-hardware scale";
    if (p.shuffle > 12 || p.shuffle % 2 != 0) return "shuffle not a hardware key";
    if ((p.scale == seq::Scale::EighthTriplet || p.scale == seq::Scale::SixteenthTriplet) && p.shuffle != 0)
        return "shuffle in a triplet scale";
    if (p.flamMs < 20 || p.flamMs > 48 || (p.flamMs - 20) % 4 != 0) return "flam not a hardware key";
    for (int v = 0; v < kNumVoices; ++v)
        if (p.trackLength[static_cast<std::size_t>(v)] != 0 || p.trackShuffle[static_cast<std::size_t>(v)] != 0)
            return "per-track length or shuffle";
    if (p.steps[0] != p.steps[1]) return "a variation B";
    for (int t = 0; t < seq::kNumTracks; ++t) {
        for (int s = 0; s < seq::kMaxSteps; ++s) {
            const seq::Step c = p.at(0, t, s);
            if (!c.on()) {
                if (c.bits != 0) return "modifier on an off step";
                continue;
            }
            if (c.level() == seq::StepLevel::Weak) return "weak step";
            if (c.subSteps() != 1) return "sub-steps";
            if (c.probabilityCode() != 0) return "probability";
            if (t == seq::kAccentTrack) {
                if (c.level() != seq::StepLevel::Normal || c.flam()) return "total-accent cell not plain";
                continue;
            }
            const VoiceInfo& info = kVoiceInfo[static_cast<std::size_t>(t)];
            const bool accent = c.level() == seq::StepLevel::Accent;
            if (accent && !info.hardwareAccent) return std::string("accent on ") + std::string(info.code);
            if (c.flam() && !info.hardwareFlam) return std::string("flam on ") + std::string(info.code);
            if (c.flam() && accent) return "flam with accent";
        }
    }
    if (hatsCollide(p, 0)) return "CH and OH on one step";
    return {};
}

// ---- phrasing -----------------------------------------------------------------------------------------
// One measure of a pattern in 96ths of a 4/4 bar: whole bars and whole
// fractions of a bar as they are, an odd meter (3/4, 7/8, 5/4) as one bar.
constexpr int kBar = 96;
constexpr int kPhrase = 4 * kBar;

int measureUnits(const seq::Pattern& p) {
    int perStep = 6;
    switch (p.scale) {
        case seq::Scale::Sixteenth: perStep = 6; break;
        case seq::Scale::ThirtySecond: perStep = 3; break;
        case seq::Scale::EighthTriplet: perStep = 8; break;
        case seq::Scale::SixteenthTriplet: perStep = 4; break;
        case seq::Scale::Eighth: perStep = 12; break;
        default: break;
    }
    const int u = p.length * perStep;
    return (u % kBar == 0 || kBar % u == 0) ? u : kBar;
}

bool isFill(int index) { return index % seq::kPatternsPerGroup == seq::kPatternsPerGroup - 1; }

// Empty when every fill ends a 4-bar phrase and the list is whole phrases;
// otherwise what is wrong.
std::string phraseProblem(const seq::PatternBank& bank, const std::uint8_t* entries, int count) {
    int units = 0;
    for (int i = 0; i < count; ++i) {
        units += measureUnits(bank.patterns[entries[i]]);
        if (isFill(entries[i]) && units % kPhrase != 0)
            return "fill at entry " + std::to_string(i + 1) + " ends at bar " + std::to_string(units / kBar) +
                   " + " + std::to_string(units % kBar) + "/96";
    }
    if (units % kPhrase != 0) return "length " + std::to_string(units) + "/96 bars is not whole 4-bar phrases";
    return {};
}

// ---- grooves --------------------------------------------------------------------------------------------
// How many voice cells (on/off, variation A) two patterns of the same length
// and scale differ in; levels, modifiers, the accent row and shuffle are not
// counted, so a copy with only its accents moved still reads as a copy.
// Patterns of a different length, scale or track loop never collide.
int grooveDistance(const seq::Pattern& a, const seq::Pattern& b) {
    if (a.length != b.length || a.scale != b.scale) return 1000;
    int d = 0;
    for (int t = 0; t < kNumVoices; ++t) {
        if (a.lengthOf(t) != b.lengthOf(t)) return 1000;
        for (int s = 0; s < a.lengthOf(t); ++s)
            if (a.at(0, t, s).on() != b.at(0, t, s).on()) ++d;
    }
    return d;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Forbidden brand tokens, read from scripts/forbidden_tokens.txt at test
// time and matched the way the audit matches them (grep -wiE).
std::vector<std::pair<std::string, std::regex>> forbiddenTokens() {
    std::vector<std::pair<std::string, std::regex>> out;
#ifdef MW909_SOURCE_DIR
    std::ifstream in(std::string(MW909_SOURCE_DIR) + "/scripts/forbidden_tokens.txt");
    std::string line;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        line = line.substr(first);
        if (line.rfind("text:", 0) == 0) line = line.substr(5);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        out.emplace_back(line, std::regex("(^|[^[:alnum:]_])(" + line + ")($|[^[:alnum:]_])",
                                          std::regex::extended | std::regex::icase));
    }
#endif
    return out;
}

std::string brandHit(std::string_view text, const std::vector<std::pair<std::string, std::regex>>& tokens) {
    const std::string s(text);
    for (const auto& [src, re] : tokens)
        if (std::regex_search(s, re)) return src;
    return {};
}

bool hasDigitRun(std::string_view s, int n) {
    int run = 0;
    for (const char c : s) {
        run = (c >= '0' && c <= '9') ? run + 1 : 0;
        if (run >= n) return true;
    }
    return false;
}

// Every display name in the library, with a label for failure messages.
std::vector<std::pair<std::string, std::string>> allNames() {
    std::vector<std::pair<std::string, std::string>> names;
    for (int b = 0; b < fx::numBanks(); ++b) {
        names.emplace_back("bank " + std::to_string(b), std::string(fx::bankInfo(b).name));
        names.emplace_back("bank category " + std::to_string(b), std::string(fx::bankInfo(b).category));
        for (int i = 0; i < seq::kNumPatterns; ++i)
            if (!fx::patternName(b, i).empty())
                names.emplace_back("pattern " + std::to_string(b) + "/" + std::to_string(i),
                                   std::string(fx::patternName(b, i)));
    }
    for (int k = 0; k < fx::numKits(); ++k) {
        names.emplace_back("kit " + std::to_string(k), std::string(fx::kit(k).name));
        names.emplace_back("kit category " + std::to_string(k), std::string(fx::kit(k).category));
    }
    for (int p = 0; p < fx::numPresets(); ++p) {
        names.emplace_back("preset " + std::to_string(p), std::string(fx::preset(p).name));
        names.emplace_back("preset category " + std::to_string(p), std::string(fx::preset(p).category));
    }
    return names;
}

// ---- kit comparisons and ranges --------------------------------------------------------------------
bool eq(const EqBand& a, const EqBand& b) {
    return a.on == b.on && a.freqHz == b.freqHz && a.widthOct == b.widthOct && a.gainDb == b.gainDb;
}
bool eq(const StripParams& a, const StripParams& b) {
    return a.eqOn == b.eqOn && a.lowCutHz == b.lowCutHz && a.highCutHz == b.highCutHz &&
           a.resonance == b.resonance && eq(a.band[0], b.band[0]) && eq(a.band[1], b.band[1]) &&
           a.compOn == b.compOn && a.compPreEq == b.compPreEq && a.compThresholdDb == b.compThresholdDb &&
           a.compRatio == b.compRatio && a.compAttackMs == b.compAttackMs && a.compReleaseMs == b.compReleaseMs &&
           a.compMakeupDb == b.compMakeupDb;
}
bool eq(const ChannelParams& a, const ChannelParams& b) {
    return a.level == b.level && a.pan == b.pan && a.spread == b.spread && a.drive == b.drive &&
           a.output == b.output && a.route == b.route && a.sendBus == b.sendBus && a.send == b.send &&
           a.sendPre == b.sendPre;
}
bool eq(const TriggerOutParams& a, const TriggerOutParams& b) {
    return a.on == b.on && a.source == b.source && a.route == b.route && a.output == b.output && a.pan == b.pan &&
           a.level == b.level;
}
bool eq(const CompBand& a, const CompBand& b) {
    return a.on == b.on && a.solo == b.solo && a.thresholdDb == b.thresholdDb && a.ratio == b.ratio &&
           a.attackMs == b.attackMs && a.releaseMs == b.releaseMs && a.makeupDb == b.makeupDb;
}
bool eq(const MasterParams& a, const MasterParams& b) {
    for (std::size_t i = 0; i < a.band.size(); ++i)
        if (!eq(a.band[i], b.band[i])) return false;
    return a.stage == b.stage && a.drive == b.drive && a.compOn == b.compOn && a.xoverLowHz == b.xoverLowHz &&
           a.xoverHighHz == b.xoverHighHz && a.limiterOn == b.limiterOn && a.limThresholdDb == b.limThresholdDb &&
           a.limAttackMs == b.limAttackMs && a.limReleaseMs == b.limReleaseMs && a.softClip == b.softClip &&
           a.volumeDb == b.volumeDb;
}
bool eq(const ParamEqParams& a, const ParamEqParams& b) {
    return a.lowShelf == b.lowShelf && a.lowHz == b.lowHz && a.lowGainDb == b.lowGainDb && a.lowRes == b.lowRes &&
           eq(a.mid[0], b.mid[0]) && eq(a.mid[1], b.mid[1]) && a.highShelf == b.highShelf && a.highHz == b.highHz &&
           a.highGainDb == b.highGainDb && a.highRes == b.highRes;
}
bool eq(const DynamicsParams& a, const DynamicsParams& b) {
    return a.mode == b.mode && a.thresholdDb == b.thresholdDb && a.ratio == b.ratio && a.attackMs == b.attackMs &&
           a.holdMs == b.holdMs && a.releaseMs == b.releaseMs && a.makeupDb == b.makeupDb &&
           a.sidechain == b.sidechain && a.triggers == b.triggers && a.vcaDepth == b.vcaDepth && a.mix == b.mix;
}
bool eq(const FxParams& a, const FxParams& b) {
    for (std::size_t i = 0; i < a.place.size(); ++i)
        if (a.place[i].on != b.place[i].on || a.place[i].bus != b.place[i].bus || a.place[i].order != b.place[i].order)
            return false;
    for (std::size_t i = 0; i < a.bus.size(); ++i)
        if (a.bus[i].levelDb != b.bus[i].levelDb || a.bus[i].output != b.bus[i].output ||
            a.bus[i].toBus2 != b.bus[i].toBus2 || a.bus[i].toBus2Pre != b.bus[i].toBus2Pre)
            return false;
    const auto& da = a.distortion;
    const auto& db = b.distortion;
    if (da.preampDb != db.preampDb || da.thresholdDb != db.thresholdDb || da.knee != db.knee ||
        da.splitHz != db.splitHz || da.lowOutDb != db.lowOutDb || da.clipOutDb != db.clipOutDb || da.mix != db.mix)
        return false;
    if (!eq(a.eq[0], b.eq[0]) || !eq(a.eq[1], b.eq[1])) return false;
    const auto& la = a.delay;
    const auto& lb = b.delay;
    if (la.sync != lb.sync || la.timeMs != lb.timeMs || la.div != lb.div || la.kind != lb.kind ||
        la.feedback != lb.feedback || la.spread != lb.spread || la.loopFilter != lb.loopFilter ||
        la.cutoffHz != lb.cutoffHz || la.reso != lb.reso || la.mix != lb.mix)
        return false;
    const auto& ra = a.reverb;
    const auto& rb = b.reverb;
    if (ra.predelayMs != rb.predelayMs || ra.earlySize != rb.earlySize || ra.earlyDiffusion != rb.earlyDiffusion ||
        ra.earlyColor != rb.earlyColor || ra.lateSize != rb.lateSize || ra.lateDiffusion != rb.lateDiffusion ||
        ra.lateColor != rb.lateColor || ra.decay != rb.decay || ra.modulation != rb.modulation ||
        ra.earlyLate != rb.earlyLate || ra.mix != rb.mix)
        return false;
    const auto& ca = a.chorus;
    const auto& cb = b.chorus;
    if (ca.offsetMs != cb.offsetMs || ca.depth != cb.depth || ca.rateHz != cb.rateHz || ca.feedback != cb.feedback ||
        ca.spatial != cb.spatial || ca.lowCutHz != cb.lowCutHz || ca.highCutHz != cb.highCutHz ||
        ca.fatness != cb.fatness || ca.gainDb != cb.gainDb || ca.mix != cb.mix)
        return false;
    const auto& ba = a.bitcrusher;
    const auto& bb = b.bitcrusher;
    if (ba.bits != bb.bits || ba.dither != bb.dither || ba.rateHz != bb.rateHz || ba.jitter != bb.jitter ||
        ba.approxFilter != bb.approxFilter || ba.imagesFilter != bb.imagesFilter || ba.mix != bb.mix)
        return false;
    if (!eq(a.dynamics[0], b.dynamics[0]) || !eq(a.dynamics[1], b.dynamics[1])) return false;
    const auto& fa = a.filter;
    const auto& fb = b.filter;
    return fa.type == fb.type && fa.cutoffHz == fb.cutoffHz && fa.reso == fb.reso && fa.source == fb.source &&
           fa.lfoSync == fb.lfoSync && fa.lfoRateHz == fb.lfoRateHz && fa.lfoDiv == fb.lfoDiv &&
           fa.lfoWave == fb.lfoWave && fa.lfoSpread == fb.lfoSpread && fa.envAttackMs == fb.envAttackMs &&
           fa.envHoldMs == fb.envHoldMs && fa.envReleaseMs == fb.envReleaseMs && fa.envTriggers == fb.envTriggers &&
           fa.modAmount == fb.modAmount && fa.mix == fb.mix;
}

// Finite and inside [lo, hi]; collects failures with a label.
struct RangeCheck {
    std::vector<std::string> failures;
    void operator()(const std::string& what, float x, float lo, float hi) {
        if (!std::isfinite(x) || x < lo || x > hi)
            failures.push_back(what + " = " + std::to_string(x) + " not in [" + std::to_string(lo) + ", " +
                               std::to_string(hi) + "]");
    }
    void index(const std::string& what, int x, int lo, int hi) {
        if (x < lo || x > hi) failures.push_back(what + " = " + std::to_string(x));
    }
};

void checkBand(RangeCheck& c, const std::string& w, const EqBand& b) {
    c(w + ".freqHz", b.freqHz, 20.0f, 20000.0f);
    c(w + ".widthOct", b.widthOct, 0.5f, 4.0f);
    c(w + ".gainDb", b.gainDb, -24.0f, 24.0f);
}

void checkKit(RangeCheck& c, const fx::Kit& k) {
    const std::string kn(k.name);
    c(kn + " totalAccent", k.totalAccent, 0.0f, 1.0f);
    for (int v = 0; v < kNumVoices; ++v) {
        const auto vi = static_cast<std::size_t>(v);
        const VoiceInfo& info = kVoiceInfo[vi];
        const std::string w = kn + " " + std::string(info.code);
        for (int i = 0; i < info.numKnobs; ++i) {
            const KnobInfo& ki = info.knobs[static_cast<std::size_t>(i)];
            const float x = k.voice[vi].k[static_cast<std::size_t>(i)];
            c(w + " " + std::string(ki.id), x, ki.min, ki.max);
            if (ki.unit == KnobUnit::Choice) {
                if (x != std::floor(x)) c.failures.push_back(w + " choice knob not whole");
                if (x == static_cast<float>(PcmSource::User)) c.failures.push_back(w + " uses the user ROM");
            }
        }
        const ChannelParams& ch = k.channel[vi];
        c(w + " level", ch.level, 0.0f, 1.0f);
        c(w + " pan", ch.pan, -1.0f, 1.0f);
        c(w + " spread", ch.spread, 0.0f, 1.0f);
        c(w + " drive", ch.drive, 0.0f, 1.0f);
        c(w + " send", ch.send, 0.0f, 1.0f);
        c.index(w + " output", ch.output, 0, kNumBuses - 1);
        c.index(w + " sendBus", ch.sendBus, 0, kNumFxBuses);
        c.index(w + " route", static_cast<int>(ch.route), 0, 2);
        const StripParams& s = k.strip[vi];
        c(w + " lowCut", s.lowCutHz, 20.0f, 2000.0f);
        c(w + " highCut", s.highCutHz, 500.0f, 20000.0f);
        c(w + " resonance", s.resonance, 0.0f, 1.0f);
        checkBand(c, w + " band0", s.band[0]);
        checkBand(c, w + " band1", s.band[1]);
        c(w + " compThreshold", s.compThresholdDb, -48.0f, 0.0f);
        c(w + " compRatio", s.compRatio, 1.0f, 20.0f);
        c(w + " compAttack", s.compAttackMs, 1.0f, 500.0f);
        c(w + " compRelease", s.compReleaseMs, 1.0f, 500.0f);
        c(w + " compMakeup", s.compMakeupDb, 0.0f, 24.0f);
    }
    const FxParams& f = k.fx;
    for (int i = 0; i < kNumFxInstances; ++i) {
        c.index(kn + " fx bus", f.place[static_cast<std::size_t>(i)].bus, 0, 1);
        c.index(kn + " fx order", f.place[static_cast<std::size_t>(i)].order, 0, 9);
    }
    for (const FxBusParams& b : f.bus) {
        c(kn + " bus level", b.levelDb, -60.0f, 12.0f);
        c(kn + " bus toBus2", b.toBus2, 0.0f, 1.0f);
        c.index(kn + " bus output", b.output, 0, kNumBuses - 1);
    }
    c(kn + " dist preamp", f.distortion.preampDb, 0.0f, 36.0f);
    c(kn + " dist threshold", f.distortion.thresholdDb, -36.0f, 0.0f);
    c(kn + " dist knee", f.distortion.knee, 0.0f, 1.0f);
    c(kn + " dist split", f.distortion.splitHz, 20.0f, 2000.0f);
    c(kn + " dist lowOut", f.distortion.lowOutDb, -60.0f, 12.0f);
    c(kn + " dist clipOut", f.distortion.clipOutDb, -60.0f, 12.0f);
    c(kn + " dist mix", f.distortion.mix, 0.0f, 1.0f);
    for (const ParamEqParams& e : f.eq) {
        c(kn + " eq lowHz", e.lowHz, 20.0f, 2000.0f);
        c(kn + " eq lowGain", e.lowGainDb, -24.0f, 24.0f);
        c(kn + " eq lowRes", e.lowRes, 0.0f, 1.0f);
        checkBand(c, kn + " eq mid0", e.mid[0]);
        checkBand(c, kn + " eq mid1", e.mid[1]);
        c(kn + " eq highHz", e.highHz, 500.0f, 20000.0f);
        c(kn + " eq highGain", e.highGainDb, -24.0f, 24.0f);
        c(kn + " eq highRes", e.highRes, 0.0f, 1.0f);
    }
    c(kn + " delay time", f.delay.timeMs, 1.0f, 1000.0f);
    c(kn + " delay feedback", f.delay.feedback, 0.0f, 0.98f);
    c(kn + " delay spread", f.delay.spread, -1.0f, 1.0f);
    c(kn + " delay cutoff", f.delay.cutoffHz, 20.0f, 20000.0f);
    c(kn + " delay reso", f.delay.reso, 0.0f, 1.0f);
    c(kn + " delay mix", f.delay.mix, 0.0f, 1.0f);
    c.index(kn + " delay div", static_cast<int>(f.delay.div), 0, static_cast<int>(SyncDiv::kCount) - 1);
    c.index(kn + " delay kind", static_cast<int>(f.delay.kind), 0, 2);
    c.index(kn + " delay filter", static_cast<int>(f.delay.loopFilter), 0, 3);
    const ReverbParams& r = f.reverb;
    c(kn + " reverb predelay", r.predelayMs, 0.0f, 250.0f);
    for (const float x : {r.earlySize, r.earlyDiffusion, r.earlyColor, r.lateSize, r.lateDiffusion, r.lateColor,
                          r.decay, r.modulation, r.earlyLate, r.mix})
        c(kn + " reverb 0..1", x, 0.0f, 1.0f);
    const ChorusParams& ch = f.chorus;
    c(kn + " chorus offset", ch.offsetMs, 1.0f, 30.0f);
    c(kn + " chorus rate", ch.rateHz, 0.02f, 10.0f);
    c(kn + " chorus feedback", ch.feedback, -0.95f, 0.95f);
    c(kn + " chorus lowCut", ch.lowCutHz, 20.0f, 20000.0f);
    c(kn + " chorus highCut", ch.highCutHz, 20.0f, 20000.0f);
    c(kn + " chorus gain", ch.gainDb, -24.0f, 24.0f);
    for (const float x : {ch.depth, ch.spatial, ch.fatness, ch.mix}) c(kn + " chorus 0..1", x, 0.0f, 1.0f);
    const BitcrusherParams& bc = f.bitcrusher;
    c(kn + " crusher bits", bc.bits, 1.0f, 24.0f);
    c(kn + " crusher rate", bc.rateHz, 44.0f, 44100.0f);
    for (const float x : {bc.jitter, bc.approxFilter, bc.imagesFilter, bc.mix}) c(kn + " crusher 0..1", x, 0.0f, 1.0f);
    for (const DynamicsParams& d : f.dynamics) {
        c(kn + " dyn threshold", d.thresholdDb, -48.0f, 0.0f);
        c(kn + " dyn ratio", d.ratio, 1.0f, 20.0f);
        c(kn + " dyn attack", d.attackMs, 1.0f, 500.0f);
        c(kn + " dyn hold", d.holdMs, 0.0f, 500.0f);
        c(kn + " dyn release", d.releaseMs, 1.0f, 500.0f);
        c(kn + " dyn makeup", d.makeupDb, 0.0f, 24.0f);
        c(kn + " dyn vcaDepth", d.vcaDepth, -1.0f, 1.0f);
        c(kn + " dyn mix", d.mix, 0.0f, 1.0f);
        if (d.sidechain != kNoSource) c.index(kn + " dyn sidechain", d.sidechain, 0, kNumVoices - 1);
        for (const std::uint8_t t : d.triggers)
            if (t != kNoSource) c.index(kn + " dyn trigger", t, 0, kNumVoices - 1);
    }
    const FilterFxParams& fl = f.filter;
    c(kn + " filter cutoff", fl.cutoffHz, 20.0f, 20000.0f);
    c(kn + " filter reso", fl.reso, 0.0f, 1.0f);
    c(kn + " filter lfo rate", fl.lfoRateHz, 0.01f, 20.0f);
    c(kn + " filter lfo spread", fl.lfoSpread, 0.0f, 1.0f);
    c(kn + " filter env attack", fl.envAttackMs, 1.0f, 500.0f);
    c(kn + " filter env hold", fl.envHoldMs, 0.0f, 500.0f);
    c(kn + " filter env release", fl.envReleaseMs, 1.0f, 500.0f);
    c(kn + " filter mod", fl.modAmount, -1.0f, 1.0f);
    c(kn + " filter mix", fl.mix, 0.0f, 1.0f);
    for (const std::uint8_t t : fl.envTriggers)
        if (t != kNoSource) c.index(kn + " filter trigger", t, 0, kNumVoices - 1);
    for (const TriggerOutParams& t : k.trig) {
        c.index(kn + " trig source", t.source, 0, kNumVoices - 1);
        c.index(kn + " trig output", t.output, 0, kNumBuses - 1);
        c(kn + " trig pan", t.pan, -1.0f, 1.0f);
        c(kn + " trig level", t.level, 0.0f, 1.0f);
    }
    const MasterParams& m = k.master;
    c(kn + " master drive", m.drive, 0.0f, 1.0f);
    c(kn + " master xoverLow", m.xoverLowHz, 40.0f, 1000.0f);
    c(kn + " master xoverHigh", m.xoverHighHz, 1000.0f, 12000.0f);
    if (m.xoverHighHz <= m.xoverLowHz) c.failures.push_back(kn + " master crossovers out of order");
    c(kn + " master limThreshold", m.limThresholdDb, -24.0f, 0.0f);
    c(kn + " master limAttack", m.limAttackMs, 0.1f, 10.0f);
    c(kn + " master limRelease", m.limReleaseMs, 10.0f, 1000.0f);
    c(kn + " master volume", m.volumeDb, -60.0f, 12.0f);
    c.index(kn + " master stage", static_cast<int>(m.stage), 0, 2);
    for (const CompBand& b : m.band) {
        c(kn + " band threshold", b.thresholdDb, -48.0f, 0.0f);
        c(kn + " band ratio", b.ratio, 1.0f, 20.0f);
        c(kn + " band attack", b.attackMs, 1.0f, 500.0f);
        c(kn + " band release", b.releaseMs, 1.0f, 500.0f);
        c(kn + " band makeup", b.makeupDb, 0.0f, 24.0f);
    }
    if (!m.limiterOn) c.failures.push_back(kn + " limiter off");
}

}  // namespace

TEST_CASE("The library has the promised size", "[core][factory]") {
    int contentMemories = 0;
    int named = 0;
    for (int b = 0; b < fx::numBanks(); ++b) {
        const auto bank = build(b);
        if (!memoryEmpty(*bank)) ++contentMemories;
        for (int i = 0; i < seq::kNumPatterns; ++i)
            if (!fx::patternName(b, i).empty()) ++named;
    }
    CHECK(contentMemories >= 6);
    CHECK(named >= 150);
    CHECK(fx::numKits() >= 20);
    CHECK(fx::numPresets() >= 40);
}

TEST_CASE("Every memory builds into legal, named patterns", "[core][factory]") {
    for (int b = 0; b < fx::numBanks(); ++b) {
        INFO("memory " << b << " " << fx::bankInfo(b).name);
        const auto bank = build(b);
        CHECK(!fx::bankInfo(b).name.empty());
        CHECK(!fx::bankInfo(b).category.empty());
        CHECK(fx::bankInfo(b).tempo >= 20.0f);
        CHECK(fx::bankInfo(b).tempo <= 400.0f);
        std::set<std::string_view> names;
        for (int i = 0; i < seq::kNumPatterns; ++i) {
            const seq::Pattern& p = bank->patterns[static_cast<std::size_t>(i)];
            const std::string_view name = fx::patternName(b, i);
            INFO("pattern " << i << " '" << name << "'");
            if (patternEmpty(p)) {
                CHECK(name.empty());
                continue;
            }
            CHECK(!name.empty());
            CHECK(names.insert(name).second);  // unique within a memory
            CHECK(p.length >= 1);
            CHECK(p.length <= seq::kMaxSteps);
            CHECK(static_cast<int>(p.scale) < static_cast<int>(seq::Scale::kNumScales));
            CHECK(p.shuffle <= seq::kMaxShuffleTicks);
            CHECK(p.flamMs >= seq::kMinFlamMs);
            CHECK(p.flamMs <= seq::kMaxFlamMs);
            CHECK(p.flamGrace <= 100);
            CHECK(p.subStart <= 100);
            CHECK(p.subEnd <= 100);
            CHECK(p.reserved == 0);
            for (int v = 0; v < kNumVoices; ++v) {
                CHECK(p.trackLength[static_cast<std::size_t>(v)] <= seq::kMaxSteps);
                CHECK(p.trackShuffle[static_cast<std::size_t>(v)] <= seq::kMaxShuffleTicks + 1);
            }
            for (int var = 0; var < seq::kNumVariations; ++var) {
                bool anyVoice = false;
                for (int t = 0; t < seq::kNumTracks; ++t) {
                    for (int s = 0; s < seq::kMaxSteps; ++s) {
                        const seq::Step c = p.at(var, t, s);
                        if (s >= p.lengthOf(t)) {
                            CHECK(c.bits == 0);  // nothing hides beyond a track's loop
                            continue;
                        }
                        if (!c.on()) {
                            CHECK(c.bits == 0);
                            continue;
                        }
                        if (t == seq::kAccentTrack) {
                            CHECK(c.bits == static_cast<std::uint8_t>(seq::StepLevel::Normal));
                        } else {
                            anyVoice = true;
                        }
                    }
                }
                CHECK(anyVoice);
                CHECK_FALSE(hatsCollide(p, var));
            }
        }
    }
}

TEST_CASE("The classic memory obeys the original machine's rules", "[core][factory]") {
    REQUIRE(fx::bankInfo(kHardwareMemory).category == "Classic");
    const auto bank = build(kHardwareMemory);
    int count = 0;
    for (int i = 0; i < seq::kNumPatterns; ++i) {
        const seq::Pattern& p = bank->patterns[static_cast<std::size_t>(i)];
        if (patternEmpty(p)) continue;
        ++count;
        INFO("pattern " << i << " '" << fx::patternName(kHardwareMemory, i) << "'");
        CHECK(hardwareViolation(p).empty());
        CAPTURE(hardwareViolation(p));
    }
    CHECK(count >= 40);
    // Tracks 1..4 play bank I, tracks 5..8 bank II, as on the hardware.
    for (int t = 0; t < seq::kNumSongTracks; ++t) {
        const seq::SongTrack& song = bank->tracks[static_cast<std::size_t>(t)];
        for (int m = 0; m < song.length; ++m)
            CHECK(seq::bankOf(song.entries[static_cast<std::size_t>(m)]) == t / seq::kSongTracksPerBank);
    }
    // The chain is entered as on the original (research/03 §2.8): hold the
    // first and last key, so it is one run of adjoining patterns of a group,
    // with no repeats and no jumps.
    for (int m = 1; m < bank->chain.length; ++m) {
        const int prev = bank->chain.entries[static_cast<std::size_t>(m - 1)];
        const int cur = bank->chain.entries[static_cast<std::size_t>(m)];
        INFO("chain entry " << m);
        CHECK(cur == prev + 1);
        CHECK(cur / seq::kPatternsPerGroup == prev / seq::kPatternsPerGroup);
    }
    // The extended memories really are extended: at least one breaks a rule.
    for (int b = 0; b < fx::numBanks(); ++b) {
        if (b == kHardwareMemory) continue;
        const auto other = build(b);
        if (memoryEmpty(*other)) continue;
        bool extended = false;
        for (const auto& p : other->patterns)
            if (!patternEmpty(p) && !hardwareViolation(p).empty()) extended = true;
        CHECK(extended);
    }
}

TEST_CASE("Every group with patterns has a fill in slot 16", "[core][factory]") {
    for (int b = 0; b < fx::numBanks(); ++b) {
        const auto bank = build(b);
        for (int g = 0; g < seq::kNumBanks * seq::kGroupsPerBank; ++g) {
            bool any = false;
            for (int s = 0; s < seq::kPatternsPerGroup; ++s)
                any = any || !patternEmpty(bank->patterns[static_cast<std::size_t>(g * seq::kPatternsPerGroup + s)]);
            if (!any) continue;
            INFO("memory " << b << " group " << g);
            CHECK_FALSE(patternEmpty(bank->patterns[static_cast<std::size_t>(g * seq::kPatternsPerGroup + 15)]));
        }
    }
}

TEST_CASE("Every memory with content has a song track over its own patterns", "[core][factory]") {
    for (int b = 0; b < fx::numBanks(); ++b) {
        const auto bank = build(b);
        INFO("memory " << b);
        int songs = 0;
        for (const seq::SongTrack& song : bank->tracks) {
            CHECK(song.length <= seq::kMaxSongLength);
            if (song.length > 0) ++songs;
            for (int m = 0; m < seq::kMaxSongLength; ++m) {
                const std::uint8_t e = song.entries[static_cast<std::size_t>(m)];
                if (m >= song.length) {
                    CHECK(e == 0);
                    continue;
                }
                REQUIRE(e < seq::kNumPatterns);
                CHECK_FALSE(patternEmpty(bank->patterns[e]));
            }
        }
        CHECK(bank->chain.length <= seq::kMaxChainLength);
        for (int m = 0; m < seq::kMaxChainLength; ++m) {
            const std::uint8_t e = bank->chain.entries[static_cast<std::size_t>(m)];
            if (m >= bank->chain.length) {
                CHECK(e == 0);
                continue;
            }
            REQUIRE(e < seq::kNumPatterns);
            CHECK_FALSE(patternEmpty(bank->patterns[e]));
        }
        if (memoryEmpty(*bank)) {
            CHECK(songs == 0);
            CHECK(bank->chain.length == 0);
        } else {
            CHECK(songs >= 1);
        }
    }
}

TEST_CASE("Song tracks and chains are phrased in 4 bars with fills on the last bar", "[core][factory]") {
    for (int b = 0; b < fx::numBanks(); ++b) {
        const auto bank = build(b);
        for (int t = 0; t < seq::kNumSongTracks; ++t) {
            const seq::SongTrack& song = bank->tracks[static_cast<std::size_t>(t)];
            if (song.length == 0) continue;
            INFO("memory " << b << " TRACK " << t + 1);
            const std::string problem = phraseProblem(*bank, song.entries.data(), song.length);
            CAPTURE(problem);
            CHECK(problem.empty());
        }
        if (bank->chain.length > 0) {
            INFO("memory " << b << " chain");
            const std::string problem = phraseProblem(*bank, bank->chain.entries.data(), bank->chain.length);
            CAPTURE(problem);
            CHECK(problem.empty());
        }
    }
    // The measure arithmetic itself: a 32-step 16th pattern is two bars, a
    // 16-step 32nd pattern half a bar, 14 steps of 16ths (7/8) one bar of its own.
    seq::Pattern p{};
    p.length = 32;
    CHECK(measureUnits(p) == 2 * kBar);
    p.length = 16;
    p.scale = seq::Scale::ThirtySecond;
    CHECK(measureUnits(p) == kBar / 2);
    p.length = 14;
    p.scale = seq::Scale::Sixteenth;
    CHECK(measureUnits(p) == kBar);
}

TEST_CASE("No pattern repeats another's groove", "[core][factory]") {
    // Across the whole library, two patterns of the same length and scale
    // differ in at least two voice cells of variation A (moving accents,
    // ghost levels, ratchets or the shuffle does not make a new groove).
    // Two cells is a deliberately low bar: four-to-the-floor house beats
    // that differ only in where the clap or a rim sits are real variants.
    constexpr int kMinDistance = 2;
    struct Ref {
        int bank;
        int index;
    };
    std::vector<std::unique_ptr<seq::PatternBank>> banks;
    std::vector<Ref> refs;
    for (int b = 0; b < fx::numBanks(); ++b) {
        banks.push_back(build(b));
        for (int i = 0; i < seq::kNumPatterns; ++i)
            if (!patternEmpty(banks.back()->patterns[static_cast<std::size_t>(i)])) refs.push_back({b, i});
    }
    for (std::size_t x = 0; x < refs.size(); ++x) {
        const seq::Pattern& a = banks[static_cast<std::size_t>(refs[x].bank)]->patterns[static_cast<std::size_t>(refs[x].index)];
        for (std::size_t y = x + 1; y < refs.size(); ++y) {
            const seq::Pattern& c =
                banks[static_cast<std::size_t>(refs[y].bank)]->patterns[static_cast<std::size_t>(refs[y].index)];
            const int d = grooveDistance(a, c);
            if (d >= kMinDistance) continue;
            FAIL_CHECK("'" << fx::patternName(refs[x].bank, refs[x].index) << "' (" << refs[x].bank << "#"
                           << refs[x].index << ") and '" << fx::patternName(refs[y].bank, refs[y].index) << "' ("
                           << refs[y].bank << "#" << refs[y].index << ") differ in only " << d << " cells");
        }
    }
}

TEST_CASE("Pattern names are unique across the library", "[core][factory]") {
    std::map<std::string, std::string> seen;
    for (int b = 0; b < fx::numBanks(); ++b)
        for (int i = 0; i < seq::kNumPatterns; ++i) {
            const std::string_view name = fx::patternName(b, i);
            if (name.empty()) continue;
            const std::string where = std::to_string(b) + "#" + std::to_string(i);
            const auto [it, fresh] = seen.emplace(lower(name), where);
            if (!fresh) FAIL_CHECK("'" << name << "' at " << where << " and " << it->second);
        }
}

TEST_CASE("Each group's fill swings like its grooves", "[core][factory]") {
    // Auto fill and the song tracks drop the fill into the group's grooves,
    // so its shuffle stays within 4 ticks of the median of theirs.
    for (int b = 0; b < fx::numBanks(); ++b) {
        const auto bank = build(b);
        for (int g = 0; g < seq::kNumBanks * seq::kGroupsPerBank; ++g) {
            std::vector<int> shuffles;
            for (int s = 0; s < seq::kPatternsPerGroup - 1; ++s) {
                const seq::Pattern& p = bank->patterns[static_cast<std::size_t>(g * seq::kPatternsPerGroup + s)];
                if (!patternEmpty(p)) shuffles.push_back(2 * p.shuffle);  // in half ticks: the median may fall between
            }
            if (shuffles.empty()) continue;
            std::sort(shuffles.begin(), shuffles.end());
            const std::size_t n = shuffles.size();
            const int median2 = (shuffles[(n - 1) / 2] + shuffles[n / 2]) / 2;
            const seq::Pattern& fill = bank->patterns[static_cast<std::size_t>(g * seq::kPatternsPerGroup + 15)];
            INFO("memory " << b << " group " << g + 1 << " fill '" << fx::patternName(b, g * 16 + 15) << "' shuffle "
                           << int(fill.shuffle) << ", group median " << median2 / 2.0);
            CHECK(std::abs(2 * fill.shuffle - median2) <= 8);
        }
    }
}

TEST_CASE("Variation B differs from A wherever the memory writes one", "[core][factory]") {
    // In the extended memories most patterns carry a B; count them.
    int withB = 0;
    for (int b = 0; b < fx::numBanks(); ++b) {
        if (b == kHardwareMemory) continue;
        const auto bank = build(b);
        for (const auto& p : bank->patterns)
            if (!patternEmpty(p) && p.steps[0] != p.steps[1]) ++withB;
    }
    CHECK(withB >= 100);
}

TEST_CASE("The library exercises the extended sequencer", "[core][factory]") {
    bool weak = false, flam = false, sub = false, prob = false, ownLength = false, ownShuffle = false;
    bool steps32 = false, steps64 = false, accentRow = false, oddLength = false;
    std::set<int> scales;
    std::set<int> shuffles;
    for (int b = 0; b < fx::numBanks(); ++b) {
        const auto bank = build(b);
        for (const auto& p : bank->patterns) {
            if (patternEmpty(p)) continue;
            scales.insert(static_cast<int>(p.scale));
            shuffles.insert(p.shuffle);
            steps32 = steps32 || p.length == 32;
            steps64 = steps64 || p.length == 64;
            oddLength = oddLength || (p.length != 16 && p.length != 12 && p.length != 32 && p.length != 64 &&
                                      p.length != 24);
            for (int v = 0; v < kNumVoices; ++v) {
                ownLength = ownLength || p.trackLength[static_cast<std::size_t>(v)] != 0;
                ownShuffle = ownShuffle || p.trackShuffle[static_cast<std::size_t>(v)] != 0;
            }
            for (int var = 0; var < seq::kNumVariations; ++var)
                for (int t = 0; t < seq::kNumTracks; ++t)
                    for (int s = 0; s < p.lengthOf(t); ++s) {
                        const seq::Step c = p.at(var, t, s);
                        if (!c.on()) continue;
                        if (t == seq::kAccentTrack) {
                            accentRow = true;
                            continue;
                        }
                        weak = weak || c.level() == seq::StepLevel::Weak;
                        flam = flam || c.flam();
                        sub = sub || c.subSteps() > 1;
                        prob = prob || c.probabilityCode() != 0;
                    }
        }
    }
    CHECK(weak);
    CHECK(flam);
    CHECK(sub);
    CHECK(prob);
    CHECK(ownLength);
    CHECK(ownShuffle);
    CHECK(steps32);
    CHECK(steps64);
    CHECK(accentRow);
    CHECK(oddLength);
    CHECK(scales.size() == static_cast<std::size_t>(seq::Scale::kNumScales));
    CHECK(shuffles.count(12) == 1);  // the heaviest hardware shuffle
    CHECK(shuffles.size() >= 5);
}

TEST_CASE("Names are present and free of brand tokens and model numbers", "[core][factory]") {
    const auto tokens = forbiddenTokens();
#ifndef MW909_SOURCE_DIR
    FAIL("MW909_SOURCE_DIR is not defined, so the brand-token list cannot be read (tests/CMakeLists.txt defines it)");
#else
    REQUIRE(tokens.size() >= 10);
    // The matcher works: a plain-word token from the file matches itself in a
    // sentence, and not as part of a longer word.
    for (const auto& [src, re] : tokens) {
        if (src.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != std::string::npos)
            continue;
        CHECK(std::regex_search("a " + src + " b", re));
        CHECK_FALSE(std::regex_search("a x" + src + "x b", re));
        break;
    }
#endif
    for (const auto& [label, name] : allNames()) {
        INFO(label << " '" << name << "'");
        CHECK(!name.empty());
        CHECK(brandHit(name, tokens).empty());
        CHECK_FALSE(hasDigitRun(name, 3));
    }
}

TEST_CASE("Names stay clear of known titles the token list cannot catch", "[core][factory]") {
    // Common phrases that are also the names of releases, bands, labels or
    // worse, retired from the library after review. The brand-token audit
    // covers makers and products only; this keeps these from coming back.
    const std::set<std::string> retired{"iron march", "steel mill", "machine shop", "golden era",
                                        "night drive", "mirror ball", "body music"};
    for (const auto& [label, name] : allNames()) {
        INFO(label << " '" << name << "'");
        CHECK(retired.count(lower(name)) == 0);
    }
}

TEST_CASE("Every kit value is finite and in range, names unique", "[core][factory]") {
    std::set<std::string_view> names;
    for (int i = 0; i < fx::numKits(); ++i) {
        const fx::Kit& k = fx::kit(i);
        INFO("kit " << i << " " << k.name);
        CHECK(!k.name.empty());
        CHECK(!k.category.empty());
        CHECK(names.insert(k.name).second);
        RangeCheck c;
        checkKit(c, k);
        for (const auto& f : c.failures) FAIL_CHECK(f);
        // Knob slots a voice does not have stay at the (unused) default.
        for (int v = 0; v < kNumVoices; ++v)
            for (int n = kVoiceInfo[static_cast<std::size_t>(v)].numKnobs; n < kMaxKnobs; ++n)
                CHECK(k.voice[static_cast<std::size_t>(v)].k[static_cast<std::size_t>(n)] ==
                      defaultParams(v).k[static_cast<std::size_t>(n)]);
    }
}

TEST_CASE("Kit 0 is the stock machine", "[core][factory]") {
    const fx::Kit& k = fx::kit(0);
    for (int v = 0; v < kNumVoices; ++v) {
        INFO("voice " << kVoiceInfo[static_cast<std::size_t>(v)].code);
        const auto vi = static_cast<std::size_t>(v);
        CHECK(k.voice[vi].k == defaultParams(v).k);
        CHECK(eq(k.channel[vi], ChannelParams{}));
        CHECK(eq(k.strip[vi], StripParams{}));
    }
    CHECK(eq(k.fx, FxParams{}));
    for (const auto& t : k.trig) CHECK(eq(t, TriggerOutParams{}));
    CHECK(eq(k.master, MasterParams{}));
    CHECK(k.master.stage == OutputStage::Modern);
    CHECK(k.totalAccent == 0.7f);
}

TEST_CASE("The kits use the strips, FX buses and trigger outs", "[core][factory]") {
    bool strip = false, comp = false, send = false, route = false, trig = false, masterDrive = false;
    bool original = false, synthSource = false, sidechain = false, vca = false;
    std::set<int> effects;
    for (int i = 1; i < fx::numKits(); ++i) {
        const fx::Kit& k = fx::kit(i);
        for (int v = 0; v < kNumVoices; ++v) {
            const auto vi = static_cast<std::size_t>(v);
            strip = strip || k.strip[vi].eqOn;
            comp = comp || k.strip[vi].compOn;
            send = send || (k.channel[vi].sendBus != 0 && k.channel[vi].send > 0.0f);
            route = route || k.channel[vi].route != ChannelRoute::Output;
            if (isSampleVoice(v)) synthSource = synthSource || k.voice[vi].k[knob::PcmSource] == 1.0f;
        }
        for (int e = 0; e < kNumFxInstances; ++e)
            if (k.fx.place[static_cast<std::size_t>(e)].on) effects.insert(e);
        for (const auto& t : k.trig) trig = trig || t.on;
        for (const auto& d : k.fx.dynamics) {
            sidechain = sidechain || d.sidechain != kNoSource;
            vca = vca || d.mode == DynamicsMode::Vca;
        }
        masterDrive = masterDrive || k.master.drive > 0.0f;
        original = original || k.master.stage == OutputStage::Original;
    }
    CHECK(strip);
    CHECK(comp);
    CHECK(send);
    CHECK(route);
    CHECK(trig);
    CHECK(masterDrive);
    CHECK(original);
    CHECK(synthSource);
    CHECK(sidechain);
    CHECK(vca);
    CHECK(effects.size() >= 8);
    // The early-unit kit: the early master has no output low-pass, and the
    // Original stage carries the (default) Late revision's, so it must not
    // use it.
    for (int i = 0; i < fx::numKits(); ++i)
        if (fx::kit(i).name == "First Run") CHECK(fx::kit(i).master.stage != OutputStage::Original);
}

TEST_CASE("Presets reference valid kits, memories and patterns", "[core][factory]") {
    const fx::Preset& init = fx::preset(0);
    CHECK(init.name == "Init");
    CHECK(init.kit == 0);
    CHECK(init.bank == 0);
    CHECK(init.pattern == 0);
    CHECK(init.tempo == 120.0f);
    CHECK(init.swingScale == 1.0f);

    std::set<std::string_view> names;
    std::set<std::string_view> categories;
    std::vector<std::unique_ptr<seq::PatternBank>> banks;
    for (int b = 0; b < fx::numBanks(); ++b) banks.push_back(build(b));
    for (int i = 0; i < fx::numPresets(); ++i) {
        const fx::Preset& p = fx::preset(i);
        INFO("preset " << i << " " << p.name);
        CHECK(!p.name.empty());
        CHECK(!p.category.empty());
        CHECK(names.insert(p.name).second);
        categories.insert(p.category);
        REQUIRE(p.kit >= 0);
        REQUIRE(p.kit < fx::numKits());
        REQUIRE(p.bank >= 0);
        REQUIRE(p.bank < fx::numBanks());
        REQUIRE(p.pattern >= 0);
        REQUIRE(p.pattern < seq::kNumPatterns);
        const auto& bank = *banks[static_cast<std::size_t>(p.bank)];
        if (!memoryEmpty(bank)) CHECK_FALSE(patternEmpty(bank.patterns[static_cast<std::size_t>(p.pattern)]));
        CHECK(p.tempo >= 20.0f);
        CHECK(p.tempo <= 400.0f);
        CHECK(p.swingScale >= 0.0f);
        CHECK(p.swingScale <= 1.0f);
    }
    // Genre coverage of the presets.
    for (const char* c : {"House", "Techno", "Acid", "Garage", "Hardcore", "Electro", "Breaks", "Trance", "Hip-Hop",
                          "EBM", "Industrial", "Disco", "Experimental"}) {
        INFO(c);
        CHECK(categories.count(c) == 1);
    }
    // Tempos that suit each genre category (BPM).
    const std::map<std::string_view, std::pair<float, float>> genreTempo{
        {"House", {115.0f, 130.0f}},   {"Techno", {120.0f, 150.0f}},     {"Acid", {115.0f, 140.0f}},
        {"Garage", {125.0f, 140.0f}},  {"Hardcore", {150.0f, 220.0f}},   {"Electro", {100.0f, 135.0f}},
        {"Breaks", {95.0f, 140.0f}},   {"Trance", {125.0f, 150.0f}},     {"Hip-Hop", {70.0f, 100.0f}},
        {"EBM", {110.0f, 135.0f}},     {"Industrial", {115.0f, 145.0f}}, {"Disco", {100.0f, 135.0f}},
        {"Dub", {90.0f, 130.0f}},
    };
    for (int i = 0; i < fx::numPresets(); ++i) {
        const fx::Preset& p = fx::preset(i);
        const auto it = genreTempo.find(p.category);
        if (it == genreTempo.end()) continue;
        INFO("preset " << i << " " << p.name << " (" << p.category << ") at " << p.tempo);
        CHECK(p.tempo >= it->second.first);
        CHECK(p.tempo <= it->second.second);
    }
    // Gabber and hardcore at 160 BPM and over get a driven or distorted kick.
    for (int i = 0; i < fx::numPresets(); ++i) {
        const fx::Preset& p = fx::preset(i);
        if (p.category != "Hardcore" || p.tempo < 160.0f) continue;
        const fx::Kit& k = fx::kit(p.kit);
        INFO("preset " << i << " " << p.name << " kit " << k.name);
        const bool bdDrive = k.voice[BD].k[knob::BdDrive] > 0.6f || k.channel[BD].drive > 0.2f;
        const bool distortion = k.fx.place[static_cast<std::size_t>(FxId::Distortion)].on;
        CHECK((bdDrive || distortion || k.master.drive > 0.2f));
    }
    // Every memory and every kit is reachable from some preset.
    std::set<int> kitsUsed, banksUsed;
    for (int i = 0; i < fx::numPresets(); ++i) {
        kitsUsed.insert(fx::preset(i).kit);
        banksUsed.insert(fx::preset(i).bank);
    }
    CHECK(banksUsed.size() == static_cast<std::size_t>(fx::numBanks()));
    CHECK(kitsUsed.size() == static_cast<std::size_t>(fx::numKits()));
}

TEST_CASE("Out-of-range indices clamp", "[core][factory]") {
    CHECK(fx::kit(-5).name == fx::kit(0).name);
    CHECK(fx::kit(1000).name == fx::kit(fx::numKits() - 1).name);
    CHECK(fx::preset(-1).name == fx::preset(0).name);
    CHECK(fx::preset(1000).name == fx::preset(fx::numPresets() - 1).name);
    CHECK(fx::bankInfo(-1).name == fx::bankInfo(0).name);
    CHECK(fx::bankInfo(1000).name == fx::bankInfo(fx::numBanks() - 1).name);
    CHECK(fx::patternName(0, -1).empty());
    CHECK(fx::patternName(0, seq::kNumPatterns).empty());
    CHECK(fx::patternName(-3, 0) == fx::patternName(0, 0));
}

TEST_CASE("buildBank is deterministic and overwrites everything", "[core][factory]") {
    for (int b = 0; b < fx::numBanks(); ++b) {
        INFO("memory " << b);
        auto x = std::make_unique<seq::PatternBank>();
        auto y = std::make_unique<seq::PatternBank>();
        // Start y from garbage: everything must be overwritten.
        std::memset(static_cast<void*>(y.get()), 0xA5, sizeof(seq::PatternBank));
        fx::buildBank(b, *x);
        fx::buildBank(b, *y);
        CHECK(x->patterns == y->patterns);
        CHECK(x->chain == y->chain);
        CHECK(x->tracks == y->tracks);
        // And a rebuild over another memory gives the same again.
        fx::buildBank((b + 1) % fx::numBanks(), *y);
        fx::buildBank(b, *y);
        CHECK(x->patterns == y->patterns);
        CHECK(x->chain == y->chain);
        CHECK(x->tracks == y->tracks);
    }
}
