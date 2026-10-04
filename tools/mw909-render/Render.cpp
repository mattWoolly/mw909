// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The `voice`, `pattern`, `midi`, `list` and `compare` subcommands. Every
// render goes through a full mw909::Machine, exactly as the plugin drives it:
// voice hits are UI auditions landing on exact samples, patterns play from the
// sequencer in HostSync against a rolling host transport. The engine's
// constant latency (Machine::latencySamples) is rendered and then dropped, so
// a hit or step 1 sounds at sample 0 of the file (--keep-latency keeps it).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>

#include "Common.h"
#include "PatternText.h"
#include "Wav.h"
#include "mw909/Factory.h"
#include "mw909/MidiMap.h"
#include "mw909/seq/MidiFile.h"

namespace mw909::tools {

namespace {

constexpr double kMinRate = 8000.0;
constexpr double kMaxRate = 384000.0;
constexpr int kMaxBlock = 8192;
constexpr double kMaxSeconds = 3600.0;

bool patternEmpty(const seq::Pattern& p)
{
    for (const auto& var : p.steps)
        for (const auto& track : var)
            for (const auto& s : track)
                if (s.on())
                    return false;
    return true;
}

std::string requireOut(const std::string& out, const char* sub)
{
    if (out.empty())
        throw UsageError(std::string(sub) + ": --out <file> is required");
    return out;
}

// Stems: every voice on its own individual out (bus v+1), dry (no route into
// an FX bus, no send), so the stems are pre-FX and pre-master like the
// hardware's separate outputs.
void routeToStems(MachineParams& p)
{
    static_assert(kNumVoices + 1 <= kNumBuses, "one individual out per voice");
    for (std::size_t v = 0; v < kNumVoices; ++v)
    {
        p.engine.channel[v].output = static_cast<std::uint8_t>(v + 1);
        p.engine.channel[v].route = ChannelRoute::Output;
        p.engine.channel[v].sendBus = 0;
        p.engine.channel[v].send = 0.0f;
    }
}

void writeStems(const std::string& dir, BusStorage& s, int first, int frames, int rate, int channels,
                const std::string& format, bool quiet)
{
    for (std::size_t v = 0; v < kNumVoices; ++v)
    {
        const int bus = static_cast<int>(v) + 1;
        const std::string path = dir + "/" + std::string(kVoiceInfo[v].id) + ".wav";
        writeStereoOrMono(path, s.left(bus), s.right(bus), first, frames, rate, channels, format, quiet);
    }
}

std::string knobSummary(int v, const VoiceParams& p)
{
    const auto& info = kVoiceInfo[static_cast<std::size_t>(v)];
    std::string s;
    for (int k = 0; k < info.numKnobs; ++k)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%s %.3g", k == 0 ? "" : ", ",
                      std::string(info.knobs[static_cast<std::size_t>(k)].id).c_str(),
                      static_cast<double>(p.k[static_cast<std::size_t>(k)]));
        s += buf;
    }
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------
// voice
// ---------------------------------------------------------------------------------

int cmdVoice(const std::vector<std::string>& args)
{
    int voiceIndex = -1;
    std::vector<std::string> hitTokens{"normal"};
    double spacing = 0.5, length = -1.0, rate = 48000.0, totalKnob = 0.7, level = 0.5, pan = 0.0;
    int block = 256, channels = 2;
    Quality quality = Quality::Standard;
    Revision revision = Revision::Late;
    std::uint32_t seed = 0;
    float variance = 0.0f;
    std::string masterMode = "clean", format = "24", out, stemsDir, userRom, userRegion;
    bool quiet = false, keepLatency = false;
    std::vector<std::pair<std::string, std::string>> knobArgs;
    std::vector<std::string> sets;

    ArgReader r(args);
    while (r.more())
    {
        const std::string opt = r.option();
        if (opt == "--voice")
            voiceIndex = parseVoice(r.value());
        else if (opt == "--hits")
            hitTokens = splitList(r.value());
        else if (opt == "--spacing")
            spacing = parseDouble(opt, r.value(), 0.001, 60.0);
        else if (opt == "--length")
            length = parseDouble(opt, r.value(), 0.001, kMaxSeconds);
        else if (opt == "--total-accent")
            totalKnob = parseDouble(opt, r.value(), 0.0, 1.0);
        else if (opt == "--knob")
        {
            const std::string kv = r.value();
            const std::size_t eq = kv.find('=');
            if (eq == std::string::npos)
                throw UsageError("--knob: expected <id>=<value>");
            knobArgs.emplace_back("--" + kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (opt == "--level")
            level = parseDouble(opt, r.value(), 0.0, 1.0);
        else if (opt == "--pan")
            pan = parseDouble(opt, r.value(), -1.0, 1.0);
        else if (opt == "--master")
            masterMode = r.value();
        else if (opt == "--set")
            sets.push_back(r.value());
        else if (opt == "--rate")
            rate = parseDouble(opt, r.value(), kMinRate, kMaxRate);
        else if (opt == "--block")
            block = parseInt(opt, r.value(), 1, kMaxBlock);
        else if (opt == "--quality")
            quality = parseQuality(r.value());
        else if (opt == "--revision")
            revision = parseRevision(r.value());
        else if (opt == "--seed")
            seed = parseU32(opt, r.value());
        else if (opt == "--variance")
            variance = static_cast<float>(parseDouble(opt, r.value(), 0.0, 1.0));
        else if (opt == "--user-rom")
            userRom = r.value();
        else if (opt == "--user-region")
            userRegion = lower(r.value());
        else if (opt == "--stems")
            stemsDir = r.value();
        else if (opt == "--channels")
            channels = parseInt(opt, r.value(), 1, 2);
        else if (opt == "--format")
            format = r.value();
        else if (opt == "--out" || opt == "-o")
            out = r.value();
        else if (opt == "--keep-latency")
        {
            r.flag();
            keepLatency = true;
        }
        else if (opt == "--quiet" || opt == "-q")
        {
            r.flag();
            quiet = true;
        }
        else if (opt.rfind("--", 0) == 0 && opt.size() > 2)
            knobArgs.emplace_back(opt, r.value()); // a knob of the voice, resolved below
        else
            r.unknown("voice");
    }
    if (voiceIndex < 0)
        throw UsageError("voice: --voice <code> is required");
    requireOut(out, "voice");
    if (masterMode != "clean" && masterMode != "stock")
        throw UsageError("--master: use clean or stock");

    const auto v = static_cast<std::size_t>(voiceIndex);
    std::vector<float> volts;
    for (const auto& t : hitTokens)
    {
        const auto k = static_cast<float>(totalKnob);
        if (t == "weak")
            volts.push_back(Hit::weak().acc);
        else if (t == "normal")
            volts.push_back(Hit::normal().acc);
        else if (t == "accent")
            volts.push_back(Hit::accented(true, false, k).acc);
        else if (t == "total")
            volts.push_back(Hit::accented(false, true, k).acc);
        else if (t == "full")
            volts.push_back(Hit::accented(true, true, k).acc);
        else if (t.size() > 1 && (t[0] == 'c' || t[0] == 'C'))
            volts.push_back(AccentDac::volts(parseInt("--hits", t.substr(1), 0, AccentDac::kMaxCode)));
        else
            volts.push_back(static_cast<float>(parseDouble("--hits", t, 0.0, static_cast<double>(AccentDac::kVolts))));
    }
    if (volts.empty())
        throw UsageError("--hits: need at least one hit");

    MachineParams params;
    params.engine = stockEngine();
    params.seq.mode = seq::Mode::Off;
    params.seq.totalAccent = static_cast<float>(totalKnob);
    params.quality = quality;
    params.revision = revision;
    params.unitSeed = seed;
    params.variance = variance;
    auto& vp = params.engine.voice[v];
    bool sourceGiven = false;
    for (const auto& [opt, value] : knobArgs)
    {
        const int k = knobIndex(voiceIndex, opt.substr(2));
        if (k < 0)
            throw UsageError("unknown option '" + opt + "' for 'voice' (" + std::string(kVoiceInfo[v].id)
                             + " knobs: " + knobSummary(voiceIndex, defaultParams(voiceIndex)) + ")");
        vp.k[static_cast<std::size_t>(k)] = parseKnobValue(voiceIndex, k, opt, value);
        sourceGiven = sourceGiven || opt == "--source";
    }
    params.engine.channel[v].level = static_cast<float>(level);
    params.engine.channel[v].pan = static_cast<float>(pan);
    if (masterMode == "clean")
        setCleanMaster(params.engine.master);
    for (const auto& s : sets)
        applySetting(params, s);

    std::unique_ptr<RomImage> userImage;
    RomSlot userSlot = RomSlot::Hat;
    if (!userRom.empty())
    {
        RomRegion region = regionForVoice(voiceIndex);
        if (userRegion == "open")
            region = RomRegion::HatOpen;
        else if (userRegion == "closed")
            region = RomRegion::HatClosed;
        else if (userRegion == "crash")
            region = RomRegion::Crash;
        else if (userRegion == "ride")
            region = RomRegion::Ride;
        else if (!userRegion.empty())
            throw UsageError("--user-region: use open, closed, crash or ride");
        userImage = std::make_unique<RomImage>();
        userImage->data.fill(kRomMidScale);
        loadUserRom(userRom, region, *userImage);
        userSlot = slotOf(region);
        if (!sourceGiven && isSampleVoice(voiceIndex))
            vp.k[knob::PcmSource] = static_cast<float>(PcmSource::User);
    }

    if (spacing * rate < 1.0)
        throw UsageError("--spacing is shorter than one sample");
    if (length < 0.0)
        length = spacing * static_cast<double>(volts.size());
    checkWavFits(std::llround(length * rate) + (keepLatency ? 64 : 0), channels, format);
    const auto frames = static_cast<int>(std::llround(length * rate));
    std::vector<int> hitAt;
    for (std::size_t i = 0; i < volts.size(); ++i)
        hitAt.push_back(static_cast<int>(std::llround(static_cast<double>(i) * spacing * rate)));

    const auto renderPass = [&](bool stems) {
        auto machine = std::make_unique<Machine>();
        machine->prepare(rate, block);
        if (userImage)
            postUserRom(*machine, userSlot, *userImage);
        const int latency = machine->latencySamples();
        const int total = frames + latency;
        auto storage = std::make_unique<BusStorage>(total, stems);
        MachineParams p = params;
        if (stems)
            routeToStems(p);
        std::size_t next = 0;
        for (int pos = 0; pos < total;)
        {
            while (next < hitAt.size() && hitAt[next] <= pos)
            {
                Command c;
                c.type = Command::Type::Audition;
                c.index = voiceIndex;
                c.value = volts[next];
                if (!machine->post(c))
                    throw RunError("command queue full");
                ++next;
            }
            int n = std::min(block, total - pos);
            if (next < hitAt.size())
                n = std::min(n, hitAt[next] - pos);
            Machine::ProcessArgs a;
            a.out = storage->view(pos);
            a.numSamples = n;
            machine->process(a, p);
            pos += n;
        }
        return std::make_pair(std::move(storage), latency);
    };

    int fired = 0;
    for (int at : hitAt)
        fired += at < frames ? 1 : 0;
    const auto [main, latency] = renderPass(false);
    const int first = keepLatency ? 0 : latency;
    if (!quiet)
    {
        std::fprintf(stderr, "voice %s (%s): %d hit(s) at", std::string(kVoiceInfo[v].code).c_str(),
                     std::string(kVoiceInfo[v].name).c_str(), fired);
        for (int i = 0; i < fired; ++i)
            std::fprintf(stderr, " %.2fV", static_cast<double>(volts[static_cast<std::size_t>(i)]));
        std::fprintf(stderr, "; %s; %s, %s revision, %.0f Hz, seed %u, variance %.2f, latency %d%s\n",
                     knobSummary(voiceIndex, vp).c_str(), qualityName(quality), revisionName(revision),
                     rate, seed, static_cast<double>(variance), latency,
                     keepLatency ? " (kept)" : " (removed)");
    }
    if (static_cast<std::size_t>(fired) < volts.size())
        std::fprintf(stderr, "warning: %zu hit(s) fall after --length and were not rendered\n",
                     volts.size() - static_cast<std::size_t>(fired));

    const int outFrames = keepLatency ? frames + latency : frames;
    writeStereoOrMono(out, main->left(0), main->right(0), first, outFrames, static_cast<int>(rate),
                      channels, format, quiet);
    if (!stemsDir.empty())
    {
        auto stems = renderPass(true);
        writeStems(stemsDir, *stems.first, first, outFrames, static_cast<int>(rate), channels, format, quiet);
    }
    return 0;
}

// ---------------------------------------------------------------------------------
// pattern
// ---------------------------------------------------------------------------------

int cmdPattern(const std::vector<std::string>& args)
{
    PatternSource src;
    int measures = 2, block = 512, channels = 2, songTrack = -1, fillSlot = -1;
    double tail = 1.0, tempo = 0.0, rate = 48000.0, swing = -1.0, totalAccent = -1.0, seconds = -1.0;
    int shuffleTicks = -1, flamMs = -1;
    Quality quality = Quality::Standard;
    Revision revision = Revision::Late;
    bool revisionGiven = false;
    std::uint32_t seed = 0, jitterSeed = 0;
    float variance = 0.0f;
    seq::VariationMode variation = seq::VariationMode::A;
    seq::AutoFill autoFill = seq::AutoFill::Manual;
    seq::PlayMode play = seq::PlayMode::Pattern;
    bool variationGiven = false, autoFillGiven = false, playGiven = false;
    bool printText = false, quiet = false, jitter = false, keepLatency = false, songTrackGiven = false;
    std::string format = "24", out, stemsDir, kitText, mix = "kit";
    std::vector<std::string> sets;

    ArgReader r(args);
    while (r.more())
    {
        const std::string opt = r.option();
        if (readPatternSourceOption(r, opt, src))
            continue;
        if (opt == "--measures")
            measures = parseInt(opt, r.value(), 1, 10000);
        else if (opt == "--seconds")
            seconds = parseDouble(opt, r.value(), 0.01, kMaxSeconds);
        else if (opt == "--tail")
            tail = parseDouble(opt, r.value(), 0.0, 60.0);
        else if (opt == "--tempo")
            tempo = parseDouble(opt, r.value(), 20.0, 400.0); // the text format's and sequencer's range
        else if (opt == "--rate")
            rate = parseDouble(opt, r.value(), kMinRate, kMaxRate);
        else if (opt == "--block")
            block = parseInt(opt, r.value(), 1, kMaxBlock);
        else if (opt == "--quality")
            quality = parseQuality(r.value());
        else if (opt == "--revision")
        {
            revision = parseRevision(r.value());
            revisionGiven = true;
        }
        else if (opt == "--seed")
            seed = parseU32(opt, r.value());
        else if (opt == "--variance")
            variance = static_cast<float>(parseDouble(opt, r.value(), 0.0, 1.0));
        else if (opt == "--kit")
            kitText = r.value();
        else if (opt == "--mix")
            mix = lower(r.value());
        else if (opt == "--total-accent")
            totalAccent = parseDouble(opt, r.value(), 0.0, 1.0);
        else if (opt == "--variation")
        {
            variation = parseVariationMode(r.value());
            variationGiven = true;
        }
        else if (opt == "--swing-scale")
            swing = parseDouble(opt, r.value(), 0.0, 1.0);
        else if (opt == "--shuffle-ticks")
            shuffleTicks = parseInt(opt, r.value(), 0, seq::kMaxShuffleTicks);
        else if (opt == "--shuffle-key")
            shuffleTicks = seq::shuffleTicksForKey(parseInt(opt, r.value(), 1, 7));
        else if (opt == "--flam-ms")
            flamMs = parseInt(opt, r.value(), seq::kMinFlamMs, seq::kMaxFlamMs);
        else if (opt == "--flam-key")
            flamMs = seq::flamMsForKey(parseInt(opt, r.value(), 1, 8));
        else if (opt == "--auto-fill")
        {
            autoFill = parseAutoFill(r.value());
            autoFillGiven = true;
        }
        else if (opt == "--fill-slot")
            fillSlot = parseInt(opt, r.value(), 1, seq::kPatternsPerGroup) - 1;
        else if (opt == "--play")
        {
            play = parsePlayMode(r.value());
            playGiven = true;
        }
        else if (opt == "--chain")
        {
            r.flag();
            play = seq::PlayMode::Chain;
            playGiven = true;
        }
        else if (opt == "--song-track")
        {
            songTrack = parseInt(opt, r.value(), 1, seq::kNumSongTracks) - 1;
            songTrackGiven = true;
        }
        else if (opt == "--jitter")
        {
            r.flag();
            jitter = true;
        }
        else if (opt == "--jitter-seed")
        {
            jitterSeed = parseU32(opt, r.value());
            jitter = true;
        }
        else if (opt == "--set")
            sets.push_back(r.value());
        else if (opt == "--channels")
            channels = parseInt(opt, r.value(), 1, 2);
        else if (opt == "--format")
            format = r.value();
        else if (opt == "--stems")
            stemsDir = r.value();
        else if (opt == "--out" || opt == "-o")
            out = r.value();
        else if (opt == "--print-text")
        {
            r.flag();
            printText = true;
        }
        else if (opt == "--keep-latency")
        {
            r.flag();
            keepLatency = true;
        }
        else if (opt == "--quiet" || opt == "-q")
        {
            r.flag();
            quiet = true;
        }
        else
            r.unknown("pattern");
    }
    if (mix != "kit" && mix != "typical" && mix != "dry")
        throw UsageError("--mix: use kit, typical or dry");

    auto loaded = std::make_unique<LoadedPatterns>();
    loadPatterns(src, *loaded);
    seq::Pattern& selected = loaded->bank.patterns[static_cast<std::size_t>(loaded->selected)];
    if (shuffleTicks >= 0)
        selected.shuffle = static_cast<std::uint8_t>(shuffleTicks);
    if (flamMs >= 0)
        selected.flamMs = static_cast<std::uint8_t>(flamMs);
    if (tempo <= 0.0)
        tempo = loaded->tempo > 0.0 ? loaded->tempo : 120.0;
    if (loaded->fromPreset)
    {
        if (!variationGiven)
            variation = loaded->presetVariation;
        if (!autoFillGiven)
            autoFill = loaded->presetAutoFill;
        if (!playGiven)
            play = loaded->presetPlay;
        if (songTrack < 0)
            songTrack = loaded->presetSongTrack;
        if (swing < 0.0)
            swing = static_cast<double>(loaded->presetSwing);
        if (kitText.empty())
            kitText = std::to_string(loaded->presetKit);
    }
    if (songTrack < 0)
        songTrack = 0;
    if (swing < 0.0)
        swing = 1.0;
    if (songTrackGiven && !playGiven)
        play = seq::PlayMode::Track;

    if (printText)
    {
        std::cout << "# mw909 pattern " << loaded->sourceLabel << "\n"
                  << formatPatternText(selected, loaded->selected, loaded->name, tempo);
        if (out.empty())
            return 0;
    }
    requireOut(out, "pattern");
    if (patternEmpty(selected) && play == seq::PlayMode::Pattern)
        std::fprintf(stderr, "warning: pattern %s is empty\n", patternLabel(loaded->selected).c_str());

    // Sound: "stock" (default) is the tool's own stock machine, independent of
    // the factory library; a number or name picks a factory kit.
    MachineParams params;
    std::string kitName = "stock";
    float kitAccent = 0.7f;
    if (kitText.empty() || lower(kitText) == "stock")
        params.engine = stockEngine();
    else
    {
        const factory::Kit& kit = factory::kit(resolveKit(kitText));
        params.engine = stockEngine();
        params.engine.voice = kit.voice;
        params.engine.channel = kit.channel;
        params.engine.strip = kit.strip;
        params.engine.fx = kit.fx;
        params.engine.trig = kit.trig;
        params.engine.master = kit.master;
        kitAccent = kit.totalAccent;
        kitName = std::string(kit.name);
        if (!revisionGiven)
            revision = kit.revision;
    }
    if (mix == "typical")
        applyTypicalMix(params.engine);
    else if (mix == "dry")
    {
        for (auto& s : params.engine.strip)
            s = StripParams{};
        params.engine.fx = FxParams{};
        for (auto& c : params.engine.channel)
        {
            c.route = ChannelRoute::Output;
            c.sendBus = 0;
        }
        setCleanMaster(params.engine.master);
    }
    params.engine.bpm = tempo;
    params.seq.mode = seq::Mode::HostSync;
    params.seq.totalAccent = totalAccent >= 0.0 ? static_cast<float>(totalAccent) : kitAccent;
    params.seq.swingScale = static_cast<float>(swing);
    params.seq.variation = variation;
    params.seq.autoFill = autoFill;
    if (fillSlot >= 0)
        params.seq.fillSlot = static_cast<std::uint8_t>(fillSlot);
    params.seq.play = play;
    params.seq.songTrack = static_cast<std::uint8_t>(songTrack);
    params.seq.internalBpm = std::clamp(tempo, 20.0, 400.0);
    params.seq.jitter = jitter;
    params.seq.jitterSeed = jitterSeed;
    params.quality = quality;
    params.revision = revision;
    params.unitSeed = seed;
    params.variance = variance;
    for (const auto& s : sets)
        applySetting(params, s);

    // Length: the measures that will play (chain/track entries in order, else
    // the selected pattern repeated), at the pattern's own scale and length.
    const auto measureQn = [&](int index) {
        const seq::Pattern& p = loaded->bank.patterns[static_cast<std::size_t>(index)];
        return static_cast<double>(p.length) * seq::stepLengthQn(p.scale);
    };
    double qn = 0.0;
    int listLen = 0;
    const std::uint8_t* list = nullptr;
    if (play == seq::PlayMode::Chain && loaded->bank.chain.length > 0)
    {
        listLen = loaded->bank.chain.length;
        list = loaded->bank.chain.entries.data();
    }
    else if (play == seq::PlayMode::Track && loaded->bank.tracks[static_cast<std::size_t>(songTrack)].length > 0)
    {
        listLen = loaded->bank.tracks[static_cast<std::size_t>(songTrack)].length;
        list = loaded->bank.tracks[static_cast<std::size_t>(songTrack)].entries.data();
    }
    else if (play != seq::PlayMode::Pattern)
        std::fprintf(stderr, "warning: the %s is empty; playing the selected pattern\n",
                     play == seq::PlayMode::Chain ? "chain" : "song track");
    for (int m = 0; m < measures; ++m)
        qn += measureQn(list != nullptr ? list[m % listLen] : loaded->selected);
    double renderSeconds = qn * 60.0 / tempo + tail;
    if (seconds > 0.0)
        renderSeconds = seconds;
    if (renderSeconds > kMaxSeconds)
        throw UsageError("render would be longer than an hour");
    checkWavFits(std::llround(renderSeconds * rate) + (keepLatency ? 64 : 0), channels, format);
    const auto frames = static_cast<int>(std::llround(renderSeconds * rate));

    const auto renderPass = [&](bool stems) {
        auto machine = std::make_unique<Machine>();
        machine->prepare(rate, block);
        machine->sequencer().setBank(loaded->bank);
        machine->sequencer().selectPattern(loaded->selected);
        const int latency = machine->latencySamples();
        const int total = frames + latency;
        auto storage = std::make_unique<BusStorage>(total, stems);
        MachineParams p = params;
        if (stems)
            routeToStems(p);
        for (int pos = 0; pos < total;)
        {
            const int n = std::min(block, total - pos);
            Machine::ProcessArgs a;
            a.transport.hostValid = true;
            a.transport.playing = true;
            a.transport.bpm = tempo;
            a.transport.ppq = static_cast<double>(pos) * tempo / (60.0 * rate);
            a.out = storage->view(pos);
            a.numSamples = n;
            machine->process(a, p);
            pos += n;
        }
        return std::make_pair(std::move(storage), latency);
    };

    if (!quiet)
        std::fprintf(stderr,
                     "pattern %s%s%s: %d step(s), scale %s, shuffle %d ticks, flam %d ms; play %s; "
                     "%d measure(s) at %.2f BPM -> %.3f s; kit %s, mix %s, %s, %s revision, %.0f Hz, block %d\n",
                     loaded->sourceLabel.c_str(), loaded->name.empty() ? "" : " ", loaded->name.c_str(),
                     static_cast<int>(selected.length), scaleText(selected.scale),
                     static_cast<int>(selected.shuffle), static_cast<int>(selected.flamMs),
                     play == seq::PlayMode::Pattern ? "pattern"
                                                    : (play == seq::PlayMode::Chain ? "chain" : "track"),
                     measures, tempo, renderSeconds, kitName.c_str(), mix.c_str(), qualityName(quality),
                     revisionName(revision), rate, block);

    const auto [main, latency] = renderPass(false);
    const int first = keepLatency ? 0 : latency;
    const int outFrames = keepLatency ? frames + latency : frames;
    writeStereoOrMono(out, main->left(0), main->right(0), first, outFrames, static_cast<int>(rate), channels,
                      format, quiet);
    if (!stemsDir.empty())
    {
        auto stems = renderPass(true);
        writeStems(stemsDir, *stems.first, first, outFrames, static_cast<int>(rate), channels, format, quiet);
    }
    return 0;
}

// ---------------------------------------------------------------------------------
// midi
// ---------------------------------------------------------------------------------

int cmdMidi(const std::vector<std::string>& args)
{
    PatternSource src;
    seq::MidiFileOptions options;
    int variation = 0, flamMs = -1;
    double tempo = 0.0;
    std::string out, readPath;
    bool quiet = false, printText = false;

    ArgReader r(args);
    while (r.more())
    {
        const std::string opt = r.option();
        if (readPatternSourceOption(r, opt, src))
            continue;
        if (opt == "--variation")
        {
            const std::string v = lower(r.value());
            if (v == "a" || v == "0")
                variation = 0;
            else if (v == "b" || v == "1")
                variation = 1;
            else
                throw UsageError("--variation: use A or B");
        }
        else if (opt == "--tempo")
            tempo = parseDouble(opt, r.value(), 1.0, 1000.0);
        else if (opt == "--flam-ms")
            flamMs = parseInt(opt, r.value(), seq::kMinFlamMs, seq::kMaxFlamMs);
        else if (opt == "--swing-scale")
            options.swingScale = static_cast<float>(parseDouble(opt, r.value(), 0.0, 1.0));
        else if (opt == "--one-step-notes")
        {
            r.flag();
            options.oneStepNotes = true;
        }
        else if (opt == "--no-tempo")
        {
            r.flag();
            options.includeTempo = false;
        }
        else if (opt == "--no-embed")
        {
            r.flag();
            options.embedPattern = false;
        }
        else if (opt == "--accent-note")
            options.accentNote = parseInt(opt, r.value(), 0, 127);
        else if (opt == "--read")
            readPath = r.value();
        else if (opt == "--print-text")
        {
            r.flag();
            printText = true;
        }
        else if (opt == "--out" || opt == "-o")
            out = r.value();
        else if (opt == "--quiet" || opt == "-q")
        {
            r.flag();
            quiet = true;
        }
        else
            r.unknown("midi");
    }

    if (!readPath.empty())
    {
        // Import: a .mid file -> pattern text on stdout.
        std::ifstream f(readPath, std::ios::binary);
        if (!f)
            throw RunError("cannot open '" + readPath + "'");
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        seq::Pattern p;
        if (!seq::readMidiFile(bytes, kVoiceNote, p, variation))
            throw RunError("'" + readPath + "' is not a well-formed metrical MIDI file");
        std::cout << "# imported from " << readPath << "\n" << formatPatternText(p, 0, "", 0.0);
        return 0;
    }

    requireOut(out, "midi");
    auto loaded = std::make_unique<LoadedPatterns>();
    loadPatterns(src, *loaded);
    options.bpm = tempo > 0.0 ? tempo : (loaded->tempo > 0.0 ? loaded->tempo : 120.0);
    seq::Pattern pattern = loaded->bank.patterns[static_cast<std::size_t>(loaded->selected)];
    if (flamMs >= 0)
        pattern.flamMs = static_cast<std::uint8_t>(flamMs);
    if (printText)
        std::cout << formatPatternText(pattern, loaded->selected, loaded->name, options.bpm);

    const std::vector<std::uint8_t> bytes = seq::writeMidiFile(pattern, variation, kVoiceNote, options);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f)
        throw RunError("cannot open '" + out + "' for writing");
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f)
        throw RunError("write failed for '" + out + "'");

    // Check: read the file back. With the embedded pattern it must restore the
    // pattern exactly; from notes alone it is lossy by design, so report the
    // cells that differ in the exported variation.
    seq::Pattern back = pattern;
    if (!seq::readMidiFile(bytes, kVoiceNote, back, variation))
        throw RunError("check failed: the written file does not read back as a MIDI file");
    int differing = 0;
    for (int t = 0; t < kNumVoices; ++t)
        for (int s = 0; s < seq::kMaxSteps; ++s)
            differing += back.at(variation, t, s) == pattern.at(variation, t, s) ? 0 : 1;
    if (options.embedPattern && !(back == pattern))
        throw RunError("check failed: the embedded pattern does not restore exactly");
    if (!quiet)
        std::fprintf(stderr, "wrote %s: %zu bytes, %s variation %c, %.2f BPM; read-back: %s\n", out.c_str(),
                     bytes.size(), loaded->sourceLabel.c_str(), variation == 0 ? 'A' : 'B', options.bpm,
                     options.embedPattern ? "exact (embedded pattern)"
                                          : (std::to_string(differing) + " voice cell(s) differ (notes only)").c_str());
    return 0;
}

// ---------------------------------------------------------------------------------
// list
// ---------------------------------------------------------------------------------

int cmdList(const std::vector<std::string>& args)
{
    const std::string what = args.empty() ? "all" : lower(args[0]);
    const auto memories = [] {
        std::printf("factory memories (--memory <n|name>):\n");
        for (int i = 0; i < factory::numBanks(); ++i)
            std::printf("  %2d  %-22s %-12s %.0f BPM\n", i, std::string(factory::bankInfo(i).name).c_str(),
                        std::string(factory::bankInfo(i).category).c_str(),
                        static_cast<double>(factory::bankInfo(i).tempo));
    };
    const auto kits = [] {
        std::printf("factory kits (--kit <n|name>; \"stock\" = the tool's own stock machine):\n");
        for (int i = 0; i < factory::numKits(); ++i)
            std::printf("  %2d  %-24s %s\n", i, std::string(factory::kit(i).name).c_str(),
                        std::string(factory::kit(i).category).c_str());
    };
    const auto presets = [] {
        std::printf("factory presets (--preset <n|name>): kit, memory, pattern, tempo\n");
        for (int i = 0; i < factory::numPresets(); ++i)
        {
            const auto& p = factory::preset(i);
            std::printf("  %2d  %-24s kit %2d  memory %d  %-8s %.0f BPM\n", i, std::string(p.name).c_str(), p.kit,
                        p.bank, patternLabel(p.pattern).c_str(), static_cast<double>(p.tempo));
        }
    };
    const auto voices = [] {
        std::printf("voices (--voice <id>) and knobs (--<knob> <value> or --set <id>.<knob>=<value>):\n");
        for (std::size_t v = 0; v < kNumVoices; ++v)
        {
            const auto& info = kVoiceInfo[v];
            std::printf("  %-2s %-14s", std::string(info.id).c_str(), std::string(info.name).c_str());
            for (int k = 0; k < info.numKnobs; ++k)
            {
                const auto& ki = info.knobs[static_cast<std::size_t>(k)];
                std::printf(" %s[%g..%g=%g]", std::string(ki.id).c_str(), static_cast<double>(ki.min),
                            static_cast<double>(ki.max), static_cast<double>(ki.def));
            }
            std::printf("\n");
        }
    };
    if (what == "memories")
        memories();
    else if (what == "kits")
        kits();
    else if (what == "presets")
        presets();
    else if (what == "voices")
        voices();
    else if (what == "patterns")
    {
        const int m = resolveMemory(args.size() > 1 ? args[1] : "0");
        auto bank = std::make_unique<seq::PatternBank>();
        factory::buildBank(m, *bank);
        std::printf("memory %d (%s): patterns (--pattern <address|name>)\n", m,
                    std::string(factory::bankInfo(m).name).c_str());
        for (int i = 0; i < seq::kNumPatterns; ++i)
        {
            const std::string_view name = factory::patternName(m, i);
            const auto& p = bank->patterns[static_cast<std::size_t>(i)];
            if (name.empty() && patternEmpty(p))
                continue;
            bool poly = false;
            for (auto l : p.trackLength)
                poly = poly || l != 0;
            std::printf("  %2d %-8s %-28s len %2d scale %-3s shuffle %2d flam %2d%s\n", i, patternLabel(i).c_str(),
                        std::string(name).c_str(), static_cast<int>(p.length), scaleText(p.scale),
                        static_cast<int>(p.shuffle), static_cast<int>(p.flamMs), poly ? " polymeter" : "");
        }
        if (bank->chain.length > 0)
            std::printf("  chain: %d entries\n", static_cast<int>(bank->chain.length));
        for (int t = 0; t < seq::kNumSongTracks; ++t)
            if (bank->tracks[static_cast<std::size_t>(t)].length > 0)
                std::printf("  song track %d: %d measures\n", t + 1,
                            static_cast<int>(bank->tracks[static_cast<std::size_t>(t)].length));
    }
    else if (what == "all")
    {
        memories();
        kits();
        presets();
        voices();
    }
    else
        throw UsageError("list: use memories, patterns [memory], kits, presets or voices");
    return 0;
}

// ---------------------------------------------------------------------------------
// compare
// ---------------------------------------------------------------------------------

int cmdCompare(const std::vector<std::string>& args)
{
    std::string candidate, blessed, tol;
    bool quiet = false;
    ArgReader r(args);
    while (r.more())
    {
        const std::string opt = r.option();
        if (opt == "--candidate")
            candidate = r.value();
        else if (opt == "--blessed" || opt == "--reference")
            blessed = r.value();
        else if (opt == "--tol")
            tol = r.value();
        else if (opt == "--quiet" || opt == "-q")
        {
            r.flag();
            quiet = true;
        }
        else
            r.unknown("compare");
    }
    if (candidate.empty() || blessed.empty())
        throw UsageError("compare: --candidate <file> and --blessed <file> are required");
    const int rc = compareWavFiles(candidate, blessed, tol, quiet);
    if (rc == 2)
        throw RunError("compare: unreadable file or bad --tol");
    return rc;
}

} // namespace mw909::tools
