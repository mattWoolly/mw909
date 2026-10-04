// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// The sound engine (architecture.md §4). Per voice, mono until the pan stage:
//
//   voice (volts) -> [Late: multi-out RC] -> x kDigitalPerVolt -> drive -> strip
//     -> latency alignment -> LEVEL -> pan + spread -> Output bus | FX bus 1 | FX bus 2
//                                    + send (pre/post LEVEL) into an FX bus
//
// FX bus returns, trigger clicks and channels land on the 16 stereo buses; a
// bus that is not connected falls back to the main mix, and only the main mix
// runs through the MasterChain.
//
// Work is done in sub-blocks of at most kSub samples. Every stage that is idle
// is skipped, and a skip is exact: a channel is skipped only when running it
// would write exact zeros and leave every state as it is (its voice asleep,
// its strip asleep or bypassed, its RC and delay lines holding only zeros),
// and the gain ramps are counted in samples, so skipping n samples moves them
// exactly as running them would. That keeps the output independent of how the
// caller splits blocks.

#include "mw909/Engine.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "mw909/dsp/Math.h"
#include "mw909/dsp/Noise.h"
#include "mw909/fx/ChannelStrip.h"
#include "mw909/fx/FxBus.h"
#include "mw909/fx/MasterChain.h"
#include "mw909/fx/TriggerOut.h"
#include "mw909/voices/BassDrum.h"
#include "mw909/voices/Cymbal.h"
#include "mw909/voices/HandClap.h"
#include "mw909/voices/HiHat.h"
#include "mw909/voices/RimShot.h"
#include "mw909/voices/SnareDrum.h"
#include "mw909/voices/Tom.h"

namespace mw909 {

namespace {

constexpr int kSub = 256;                     // internal sub-block (samples)
constexpr double kRampSeconds = 0.010;        // level / pan / send / drive / FX return glide
constexpr double kMaxSpreadSeconds = 0.020;   // Haas delay at spread = 1
constexpr double kSpreadFadeSeconds = 0.020;  // crossfade between Haas delay taps
constexpr long kQuietLong = 1L << 40;

// Two doubles in one SSE2 / NEON register (GCC and Clang vector extension).
typedef double V2 __attribute__((vector_size(16)));

// The multi-out RC of late units (research/03 §6): R500..R527 1k into
// C500..C510 0.01 uF. With the voice's jack plugged the pole is the bare RC
// (15.9 kHz); unplugged, the same capacitor also sees the voice's two mix
// resistors in parallel (18.6..18.9 kHz, the mix path).
constexpr double kMultiOutRcHz = 1.0 / (dsp::kTwoPi * 1.0e3 * 0.01e-6);   // 15.92 kHz
constexpr double kMixPathRcHz = 18750.0;

// LEVEL: 0.5 is the calibration point (gain 1), 1.0 is +7 dB, 0 is silence.
// Above the centre the gain rises linearly in dB; below it falls as the
// square of the travel (an audio taper: -12 dB at 0.25, -28 dB at 0.1).
constexpr float kLevelMaxDb = 7.0f;

float levelGain(float x) noexcept {
    if (!(x > 0.0f)) return 0.0f;
    if (x >= 0.5f) return std::pow(10.0f, kLevelMaxDb * (2.0f * std::min(x, 1.0f) - 1.0f) * 0.05f);
    const float t = 2.0f * x;
    return t * t;
}

// FX return: -60 dB (the bottom of the range) is off.
float returnGain(float db) noexcept { return db <= -60.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }

float finiteOr(float v, float def) noexcept { return std::isfinite(v) ? v : def; }
float sane(float v, float def, float lo, float hi) noexcept { return dsp::clamp(finiteOr(v, def), lo, hi); }

int nextPow2(int n) noexcept {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

int oversampleFor(Quality q, double sampleRate) noexcept {
    const bool low = sampleRate < 88200.0;
    switch (q) {
        case Quality::Eco: return 1;
        case Quality::High: return low ? 4 : 2;
        case Quality::Standard:
        default: return low ? 2 : 1;
    }
}

// Per-voice drive: input gain into the tanh (the output is divided by its square root).
float drivePre(float drive) noexcept { return 1.0f + 15.0f * drive * drive; }

// Every sample of both channels is a zero of either sign. An integer OR of the
// magnitude bits, so the loop vectorises (a float max reduction would not
// without -ffinite-math-only). A NaN counts as sound here; the master's own
// input scan turns it into 0 and still skips the block if it rests.
bool isSilent(const float* l, const float* r, int n) noexcept {
    std::uint32_t bits = 0;
    for (int i = 0; i < n; ++i) bits |= (std::bit_cast<std::uint32_t>(l[i]) | std::bit_cast<std::uint32_t>(r[i]));
    return (bits & 0x7fffffffu) == 0;
}

bool usesNoise(int v) noexcept { return v == BD || v == SD || v == LT || v == MT || v == HT || v == CP; }

// A linear ramp counted in samples: its value is a pure function of how many
// samples have passed since the target was set, so advancing it by n without
// running it lands exactly where n per-sample steps would.
struct Ramp {
    float start = 0.0f, target = 0.0f, inv = 1.0f;
    int pos = 0, len = 1;

    void setLength(int n) noexcept {
        len = std::max(1, n);
        inv = 1.0f / static_cast<float>(len);
        pos = len;
        start = target;
    }
    void snap(float v) noexcept {
        start = target = v;
        pos = len;
    }
    void set(float v) noexcept {
        if (v == target) return;
        start = value();
        target = v;
        pos = 0;
    }
    float value() const noexcept {
        return pos >= len ? target : start + (target - start) * (static_cast<float>(pos) * inv);
    }
    bool settled() const noexcept { return pos >= len; }
    float next() noexcept {
        const float v = value();
        if (pos < len) ++pos;
        return v;
    }
    void advance(int n) noexcept { pos = (len - pos <= n) ? len : pos + n; }
};

// A delay line of a fixed integer delay (latency alignment).
struct DelayLine {
    std::vector<float> buf;
    int mask = 0, w = 0, delay = 0;

    void allocate(int maxDelay) {
        buf.assign(static_cast<std::size_t>(nextPow2(maxDelay + 1)), 0.0f);
        mask = static_cast<int>(buf.size()) - 1;
        w = 0;
    }
    void clear() noexcept {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0;
    }
    // In place.
    void process(float* x, int n) noexcept {
        if (delay <= 0) return;
        float* b = buf.data();
        for (int i = 0; i < n; ++i) {
            b[w & mask] = x[i];
            x[i] = b[(w - delay) & mask];
            ++w;
        }
    }
};

}  // namespace

struct Engine::Impl {
    // --- voices ----------------------------------------------------------------
    voices::BassDrum bd;
    voices::SnareDrum sd;
    std::array<voices::Tom, 3> toms;
    voices::RimShot rs;
    voices::HandClap cp;
    voices::HiHat hat;
    std::array<voices::Cymbal, 2> cym;   // CR, RD
    dsp::WhiteNoise noise;
    bool noiseReseed = true;
    std::uint64_t clock = 0;             // samples rendered since prepare/reset

    std::array<const RomImage*, kNumRomSlots> userRom{};

    // --- configuration ---------------------------------------------------------
    double fs = 48000.0;
    bool prepared = false;
    Quality quality = Quality::Standard;
    std::uint32_t seed = 0;
    float variance = 0.0f;
    Revision revision = Revision::Late;
    int oversample = 2;
    int latency = 0;

    // --- parameters (sanitised) --------------------------------------------------
    std::array<VoiceParams, kNumVoices> voiceParams{};
    std::array<VoiceParams, kNumVoices> sentParams{};
    bool paramsSent = false;
    std::array<ChannelParams, kNumVoices> channel{};
    std::array<ChannelParams, kNumVoices> rawChannel{};      // as last received (change detection)
    std::array<TriggerOutParams, kNumTriggerOuts> rawTrig{};
    std::array<TriggerOutParams, kNumTriggerOuts> trig{};
    std::array<FxBusParams, kNumFxBuses> fxBus{};
    std::array<std::uint8_t, 2> sidechain{{kNoSource, kNoSource}};   // of the placed dynamics
    bool hatChoke = true;
    MasterParams masterParams{};
    bool primed = false;   // ramps snapped to the first parameters after prepare/reset

    // --- channels ----------------------------------------------------------------
    struct Channel {
        fx::ChannelStrip strip;
        Ramp gL, gR, sL, sR, drive;
        double rcS = 0.0;
        DelayLine align;
        // Haas spread: the right side read from a delay line; delay changes
        // crossfade between two taps.
        std::vector<float> haas;
        int haasMask = 0, haasW = 0;
        int haasCur = 0, haasNew = 0, haasPending = 0, fadePos = 0;
        bool fading = false;
        long quiet = kQuietLong;   // samples since the last non-zero sample into the alignment delay
    };
    std::array<Channel, kNumVoices> ch{};
    int spreadFadeLen = 960;
    float spreadFadeInv = 1.0f / 960.0f;
    double rcPlugG = 0.0, rcMixG = 0.0;   // TPT one-pole G for the two RC poles

    struct Click {
        fx::TriggerClick click;
        Ramp gL, gR;
        DelayLine align;
        long quiet = kQuietLong;
    };
    std::array<Click, kNumTriggerOuts> clicks{};

    fx::FxRack rack;
    fx::MasterChain master;
    std::array<Ramp, kNumFxBuses> retL{}, retR{};

    // --- scratch -----------------------------------------------------------------
    std::vector<float> noiseBuf, mainL, mainR, tmpR, clickBuf;
    std::array<std::vector<float>, kNumVoices> vbuf{};
    std::array<std::array<std::vector<float>, 2>, kNumFxBuses> fxIn{};
    std::array<float, kNumVoices> peak{};

    // ---------------------------------------------------------------------------

    Impl() {
        for (int v = 0; v < kNumVoices; ++v) voiceParams[static_cast<std::size_t>(v)] = defaultParams(v);
    }
    Impl(const Impl&) = delete;   // see prepareVoices()
    Impl& operator=(const Impl&) = delete;

    // The only place voices are prepared, always all together. The cymbals may
    // borrow a kernel (the crash the hat's, the ride the crash's, whichever it
    // resolves to), so the hat is prepared before them and is never re-prepared
    // alone: that would change the kernel under the cymbals without rebuilding
    // them (Cymbal::kernelCurrent(), asserted here and in debug renders). Nor is
    // Impl copied (deleted above): the sample voices hold pointers into their
    // own and each other's members.
    void prepareVoices(int os) noexcept {
        VoiceSetup s;
        s.sampleRate = fs;
        s.oversample = os;
        s.unitSeed = seed;
        s.variance = variance;
        s.revision = revision;
        bd.prepare(s);
        sd.prepare(s);
        toms[0].prepare(s, LT);
        toms[1].prepare(s, MT);
        toms[2].prepare(s, HT);
        rs.prepare(s);
        cp.prepare(s);
        hat.prepare(s);
        cym[0].prepare(s, CR, &hat.kernel());      // the hat's table when it is the same
        cym[1].prepare(s, RD, &cym[0].kernel());   // (variance 0): one to build and keep in cache
        assert(cym[0].kernelCurrent() && cym[1].kernelCurrent());
    }

    int voiceLatency(int v) const noexcept {
        switch (v) {
            case BD: return bd.latencySamples();
            case SD: return sd.latencySamples();
            case LT: case MT: case HT: return toms[static_cast<std::size_t>(v - LT)].latencySamples();
            case RS: return rs.latencySamples();
            case CP: return cp.latencySamples();
            case CH: case OH: return hat.latencySamples();
            case CR: return cym[0].latencySamples();
            case RD: return cym[1].latencySamples();
            default: return 0;
        }
    }

    void applyRoms() noexcept {
        hat.setRoms(&factoryRom(RomSlot::Hat), userRom[static_cast<std::size_t>(RomSlot::Hat)]);
        cym[0].setRoms(&factoryRom(RomSlot::Crash), userRom[static_cast<std::size_t>(RomSlot::Crash)]);
        cym[1].setRoms(&factoryRom(RomSlot::Ride), userRom[static_cast<std::size_t>(RomSlot::Ride)]);
    }

    void pushVoiceParams(bool force) noexcept {
        const auto changed = [&](int v) {
            const auto k = static_cast<std::size_t>(v);
            return force || !paramsSent ||
                   std::memcmp(&voiceParams[k], &sentParams[k], sizeof(VoiceParams)) != 0;
        };
        if (changed(BD)) bd.setParams(voiceParams[BD]);
        if (changed(SD)) sd.setParams(voiceParams[SD]);
        for (int i = 0; i < 3; ++i)
            if (changed(LT + i)) toms[static_cast<std::size_t>(i)].setParams(voiceParams[static_cast<std::size_t>(LT + i)]);
        if (changed(RS)) rs.setParams(voiceParams[RS]);
        if (changed(CP)) cp.setParams(voiceParams[CP]);
        if (changed(CH) || changed(OH)) hat.setParams(voiceParams[CH], voiceParams[OH]);
        if (changed(CR)) cym[0].setParams(voiceParams[CR]);
        if (changed(RD)) cym[1].setParams(voiceParams[RD]);
        sentParams = voiceParams;
        paramsSent = true;
    }

    // Re-prepare the voices for the current configuration (allocation-free).
    void configureVoices() noexcept {
        oversample = oversampleFor(quality, fs);
        prepareVoices(oversample);
        applyRoms();
        hat.setChoke(hatChoke);
        pushVoiceParams(true);
        for (int v = 0; v < kNumVoices; ++v) {
            Channel& c = ch[static_cast<std::size_t>(v)];
            c.align.delay = latency - voiceLatency(v);
            c.align.clear();
            // The voices were cut: drop everything still in flight after them
            // too (the multi-out RC and the Haas line), so a cut channel goes
            // back to sleep and never replays a stale tail later.
            c.rcS = 0.0;
            std::fill(c.haas.begin(), c.haas.end(), 0.0f);
        }
        for (auto& k : clicks) k.click.setRevision(revision);
        noiseReseed = true;
    }

    bool anyNoiseVoiceActive() const noexcept {
        return bd.isActive() || sd.isActive() || toms[0].isActive() || toms[1].isActive() || toms[2].isActive() ||
               cp.isActive();
    }

    bool voiceActive(int v) const noexcept {
        switch (v) {
            case BD: return bd.isActive() || voices::BassDrum::kFreeRunning;
            case SD: return sd.isActive() || voices::SnareDrum::kFreeRunning;
            case LT: case MT: case HT:
                return toms[static_cast<std::size_t>(v - LT)].isActive() || voices::Tom::kFreeRunning;
            case RS: return rs.isActive() || voices::RimShot::kFreeRunning;
            case CP: return cp.isActive() || voices::HandClap::kFreeRunning;
            case CH: case OH: return hat.isActive() || voices::HiHat::kFreeRunning;
            case CR: return cym[0].isActive() || voices::Cymbal::kFreeRunning;
            case RD: return cym[1].isActive() || voices::Cymbal::kFreeRunning;
            default: return false;
        }
    }

    void resetChannels() noexcept {
        for (auto& c : ch) {
            c.strip.reset();
            c.rcS = 0.0;
            c.align.clear();
            std::fill(c.haas.begin(), c.haas.end(), 0.0f);
            c.haasW = 0;
            c.haasCur = c.haasNew = c.haasPending;
            c.fading = false;
            c.fadePos = 0;
            c.quiet = kQuietLong;
            for (Ramp* r : {&c.gL, &c.gR, &c.sL, &c.sR, &c.drive}) r->snap(r->target);
        }
        for (auto& k : clicks) {
            k.click.reset();
            k.align.clear();
            k.quiet = kQuietLong;
            k.gL.snap(k.gL.target);
            k.gR.snap(k.gR.target);
        }
        for (int b = 0; b < kNumFxBuses; ++b) {
            retL[static_cast<std::size_t>(b)].snap(retL[static_cast<std::size_t>(b)].target);
            retR[static_cast<std::size_t>(b)].snap(retR[static_cast<std::size_t>(b)].target);
        }
        peak.fill(0.0f);
    }

    // --- parameters ------------------------------------------------------------

    void setParams(const EngineParams& p) noexcept {
        for (int v = 0; v < kNumVoices; ++v) {
            const auto k = static_cast<std::size_t>(v);
            const auto& info = kVoiceInfo[k];
            for (int i = 0; i < kMaxKnobs; ++i) {
                const auto j = static_cast<std::size_t>(i);
                voiceParams[k].k[j] = i < info.numKnobs
                                          ? sane(p.voice[k].k[j], info.knobs[j].def, info.knobs[j].min, info.knobs[j].max)
                                          : 0.0f;
            }
        }
        pushVoiceParams(false);
        hatChoke = p.hatChoke;
        hat.setChoke(hatChoke);

        const double bpm = std::isfinite(p.bpm) ? dsp::clamp(p.bpm, 1.0, 1000.0) : 120.0;

        for (int v = 0; v < kNumVoices; ++v) {
            const auto k = static_cast<std::size_t>(v);
            const ChannelParams& in = p.channel[k];
            ch[k].strip.setParams(p.strip[k]);   // the strip detects its own changes
            // Unchanged since the last call: the targets stand (the common case, every block).
            if (primed && std::memcmp(&in, &rawChannel[k], sizeof(ChannelParams)) == 0) continue;
            std::memcpy(&rawChannel[k], &in, sizeof(ChannelParams));
            ChannelParams& c = channel[k];
            c.level = sane(in.level, 0.5f, 0.0f, 1.0f);
            c.pan = sane(in.pan, 0.0f, -1.0f, 1.0f);
            c.spread = sane(in.spread, 0.0f, 0.0f, 1.0f);
            c.drive = sane(in.drive, 0.0f, 0.0f, 1.0f);
            c.output = in.output < kNumBuses ? in.output : 0;
            c.route = static_cast<int>(in.route) <= static_cast<int>(ChannelRoute::FxBus2) ? in.route : ChannelRoute::Output;
            c.sendBus = in.sendBus <= kNumFxBuses ? in.sendBus : 0;
            c.send = sane(in.send, 0.0f, 0.0f, 1.0f);
            c.sendPre = in.sendPre;

            Channel& s = ch[k];
            const float lvl = levelGain(c.level);
            const float angle = (c.pan + 1.0f) * 0.25f * static_cast<float>(dsp::kPi);
            const float pl = std::cos(angle), pr = std::sin(angle);
            const float send = c.sendBus == 0 ? 0.0f : c.send * (c.sendPre ? 1.0f : lvl);
            const int haas = static_cast<int>(std::lround(static_cast<double>(c.spread) * kMaxSpreadSeconds * fs));
            if (!primed) {
                s.gL.snap(lvl * pl);
                s.gR.snap(lvl * pr);
                s.sL.snap(send * pl);
                s.sR.snap(send * pr);
                s.drive.snap(c.drive);
                s.haasCur = s.haasNew = s.haasPending = haas;
                s.fading = false;
            } else {
                s.gL.set(lvl * pl);
                s.gR.set(lvl * pr);
                s.sL.set(send * pl);
                s.sR.set(send * pr);
                s.drive.set(c.drive);
                s.haasPending = haas;
                if (!s.fading && haas != s.haasCur) startSpreadFade(s);
            }
        }

        for (int t = 0; t < kNumTriggerOuts; ++t) {
            const auto k = static_cast<std::size_t>(t);
            const TriggerOutParams& in = p.trig[k];
            if (primed && std::memcmp(&in, &rawTrig[k], sizeof(TriggerOutParams)) == 0) continue;
            std::memcpy(&rawTrig[k], &in, sizeof(TriggerOutParams));
            TriggerOutParams& o = trig[k];
            o.on = in.on;
            o.source = in.source < kNumVoices ? in.source : static_cast<std::uint8_t>(RS);
            o.route = static_cast<int>(in.route) <= static_cast<int>(ChannelRoute::FxBus2) ? in.route : ChannelRoute::Output;
            o.output = in.output < kNumBuses ? in.output : 0;
            o.pan = sane(in.pan, 0.0f, -1.0f, 1.0f);
            o.level = sane(in.level, 0.5f, 0.0f, 1.0f);
            const float lvl = levelGain(o.level);
            const float angle = (o.pan + 1.0f) * 0.25f * static_cast<float>(dsp::kPi);
            Click& c = clicks[k];
            if (!primed) {
                c.gL.snap(lvl * std::cos(angle));
                c.gR.snap(lvl * std::sin(angle));
            } else {
                c.gL.set(lvl * std::cos(angle));
                c.gR.set(lvl * std::sin(angle));
            }
        }

        for (int b = 0; b < kNumFxBuses; ++b) {
            const auto k = static_cast<std::size_t>(b);
            fxBus[k].levelDb = sane(p.fx.bus[k].levelDb, 0.0f, -60.0f, 12.0f);
            fxBus[k].output = p.fx.bus[k].output < kNumBuses ? p.fx.bus[k].output : 0;
            const float g = returnGain(fxBus[k].levelDb);
            if (!primed) {
                retL[k].snap(g);
                retR[k].snap(g);
            } else {
                retL[k].set(g);
                retR[k].set(g);
            }
        }
        for (int i = 0; i < 2; ++i) {
            const auto& place = p.fx.place[static_cast<std::size_t>(static_cast<int>(FxId::DynamicsA) + i)];
            const auto& dyn = p.fx.dynamics[static_cast<std::size_t>(i)];
            sidechain[static_cast<std::size_t>(i)] =
                place.on && dyn.mode == DynamicsMode::Compressor && dyn.sidechain < kNumVoices ? dyn.sidechain : kNoSource;
        }

        rack.setParams(p.fx, bpm);
        masterParams = p.master;
        master.setParams(masterParams, revision);
        primed = true;
    }

    void startSpreadFade(Channel& s) noexcept {
        s.haasNew = s.haasPending;
        s.fadePos = 0;
        s.fading = true;
    }

    // Advance a channel's spread crossfade by n samples without running it.
    void advanceSpread(Channel& s, int n) noexcept {
        while (n > 0 && s.fading) {
            const int step = std::min(n, spreadFadeLen - s.fadePos);
            s.fadePos += step;
            n -= step;
            if (s.fadePos >= spreadFadeLen) {
                s.haasCur = s.haasNew;
                s.fading = false;
                if (s.haasPending != s.haasCur) startSpreadFade(s);
            }
        }
    }

    // --- triggering ------------------------------------------------------------

    void trigger(VoiceId v, const Hit& in) noexcept {
        Hit h = in;
        if (!std::isfinite(h.acc)) h.acc = AccentDac::volts(AccentDac::kNormalCode);
        h.acc = dsp::clamp(h.acc, 0.0f, AccentDac::kVolts);
        if (!std::isfinite(h.pitchSemis)) h.pitchSemis = 0.0f;

        if (usesNoise(v) && !anyNoiseVoiceActive()) noiseReseed = true;
        switch (v) {
            case BD: bd.trigger(h); break;
            case SD: sd.trigger(h); break;
            case LT: case MT: case HT: toms[static_cast<std::size_t>(v - LT)].trigger(h); break;
            case RS: rs.trigger(h); cp.setAccLine(h.acc); break;     // one accent DAC for RS and CP
            case CP: cp.trigger(h); rs.setAccLine(h.acc); break;
            case CH: hat.triggerClosed(h); break;
            case OH: hat.triggerOpen(h); break;
            case CR: cym[0].trigger(h); cym[1].setAccLine(h.acc); break;   // one accent DAC for CR and RD
            case RD: cym[1].trigger(h); cym[0].setAccLine(h.acc); break;
            default: return;
        }
        rack.noteTrigger(v);
        for (int t = 0; t < kNumTriggerOuts; ++t) {
            const auto& o = trig[static_cast<std::size_t>(t)];
            if (o.on && o.source == v) clicks[static_cast<std::size_t>(t)].click.trigger(h);
        }
    }

    // --- rendering -------------------------------------------------------------

    struct Dest {
        std::array<std::array<float*, 2>, kNumBuses> bus{};   // resolved: unconnected -> main scratch
        std::array<bool, kNumBuses> connected{};
    };

    float* fxPtr(int b, int c) noexcept {
        return fxIn[static_cast<std::size_t>(b)][static_cast<std::size_t>(c)].data();
    }
    // An FX bus input about to be summed into: cleared on its first use in a sub-block.
    void useFx(int b) noexcept {
        const auto k = static_cast<std::size_t>(b);
        if (fxUsed[k]) return;
        fxUsed[k] = true;
        std::fill_n(fxPtr(b, 0), subN, 0.0f);
        std::fill_n(fxPtr(b, 1), subN, 0.0f);
    }

    // Where a signal routed by (route, output) goes, as an L/R pair.
    std::array<float*, 2> destFor(const Dest& d, ChannelRoute route, int output) noexcept {
        if (route == ChannelRoute::FxBus1 || route == ChannelRoute::FxBus2) {
            const int b = route == ChannelRoute::FxBus1 ? 0 : 1;
            useFx(b);
            return {fxPtr(b, 0), fxPtr(b, 1)};
        }
        return d.bus[static_cast<std::size_t>(output)];
    }

    void renderVoices(int n, std::array<bool, kNumVoices>& active) noexcept {
        for (int v = 0; v < kNumVoices; ++v) active[static_cast<std::size_t>(v)] = voiceActive(v);
        const float* nz = noiseBuf.data();
        if (active[BD] || active[SD] || active[LT] || active[MT] || active[HT] || active[CP]) {
            if (noiseReseed) {
                noise.seed(dsp::hash32(dsp::hash32(seed, 0x300U + static_cast<std::uint32_t>(clock >> 32)),
                                       static_cast<std::uint32_t>(clock)));
                noiseReseed = false;
            }
            noise.fill(noiseBuf.data(), n);
        }
        auto buf = [this](int v) { return vbuf[static_cast<std::size_t>(v)].data(); };
        if (active[BD]) bd.render(buf(BD), nz, n);
        if (active[SD]) sd.render(buf(SD), nz, n);
        for (int i = 0; i < 3; ++i)
            if (active[static_cast<std::size_t>(LT + i)]) toms[static_cast<std::size_t>(i)].render(buf(LT + i), nz, n);
        if (active[RS]) rs.render(buf(RS), n);
        if (active[CP]) cp.render(buf(CP), nz, n);
        if (active[CH]) hat.render(buf(CH), buf(OH), n);
        if (active[CR]) cym[0].render(buf(CR), n);
        if (active[RD]) cym[1].render(buf(RD), n);
    }

    // A channel is skipped when running it would write exact zeros and
    // change no state (see skipChannel()).
    bool channelIdle(int v, bool voiceOn) const noexcept {
        const Channel& s = ch[static_cast<std::size_t>(v)];
        const int maxHaas = std::max(s.haasCur, std::max(s.haasNew, s.haasPending));
        return !voiceOn && !s.strip.isActive() && s.rcS == 0.0 &&
               s.quiet >= static_cast<long>(s.align.delay) + maxHaas + 1;
    }

    // Exactly what running an idle channel would do: zeros out, zeros into the
    // Haas line (a later spread increase reads that history), states otherwise
    // unchanged. Once the whole ring holds zeros, writing more changes
    // nothing, so a long-idle channel costs nothing.
    void skipChannel(int v, int n) noexcept {
        const auto k = static_cast<std::size_t>(v);
        Channel& s = ch[k];
        for (Ramp* r : {&s.gL, &s.gR, &s.sL, &s.sR, &s.drive}) r->advance(n);
        const int ring = s.haasMask + 1;
        if (s.quiet - static_cast<long>(s.align.delay) < static_cast<long>(ring)) {
            const int z = std::min(n, ring);
            const int first = std::min(z, ring - s.haasW);
            std::fill_n(s.haas.data() + s.haasW, first, 0.0f);
            std::fill_n(s.haas.data(), z - first, 0.0f);
        }
        s.haasW = (s.haasW + n) & s.haasMask;
        advanceSpread(s, n);
        s.quiet = std::min(s.quiet + n, kQuietLong);
        peak[k] = 0.0f;
    }

    // Voice volts -> (late units: the multi-out RC) -> digital, for every
    // channel that runs (run[v]), in place in vbuf. The RC is a recursion,
    // so one channel's loop waits on each sample's result; up to four
    // channels go through it side by side, as the lanes of two 2-wide vectors
    // (element-wise IEEE arithmetic: each lane is exactly the one-channel
    // loop, rcRun()).
    void voltsToDigital(const std::array<bool, kNumVoices>& run, const std::array<bool, kNumVoices>& voiceOn,
                        const Dest& d, int n) noexcept {
        std::array<int, kNumVoices> list{};
        int count = 0;
        for (int v = 0; v < kNumVoices; ++v) {
            if (!run[static_cast<std::size_t>(v)]) continue;
            if (!voiceOn[static_cast<std::size_t>(v)]) std::fill_n(vbuf[static_cast<std::size_t>(v)].data(), n, 0.0f);
            list[static_cast<std::size_t>(count++)] = v;
        }
        if (revision != Revision::Late) {
            for (int j = 0; j < count; ++j) {
                float* x = vbuf[static_cast<std::size_t>(list[static_cast<std::size_t>(j)])].data();
                for (int i = 0; i < n; ++i) x[i] *= kDigitalPerVolt;
            }
            return;
        }
        int j = 0;
        for (; j + 4 <= count; j += 4) rcRun<2>(list.data() + j, d, n);
        for (; j + 2 <= count; j += 2) rcRun<1>(list.data() + j, d, n);
        if (j < count) rcRunOne(list[static_cast<std::size_t>(j)], d, n);
    }

    double rcG(int v, const Dest& d) const noexcept {
        const ChannelParams& p = channel[static_cast<std::size_t>(v)];
        const bool plugged = p.route == ChannelRoute::Output && p.output > 0 && d.connected[p.output];
        return plugged ? rcPlugG : rcMixG;
    }

    // The multi-out RC of one channel.
    void rcRunOne(int v, const Dest& d, int n) noexcept {
        Channel& s = ch[static_cast<std::size_t>(v)];
        float* x = vbuf[static_cast<std::size_t>(v)].data();
        const double g = rcG(v, d);
        double st = s.rcS;
        for (int i = 0; i < n; ++i) {
            const double in = static_cast<double>(x[i]);
            const double vv = (in - st) * g;
            const double y = vv + st;
            st = y + vv;
            if (in == 0.0 && std::abs(st) < 1.0e-12) st = 0.0;   // exact rest, at a sample position
            x[i] = static_cast<float>(y) * kDigitalPerVolt;
        }
        s.rcS = st;
    }

    // rcRunOne() for the 2 x P channels v[0 .. 2P-1] at once, lane by lane
    // the same operations. The rest test sits off the recursion's path: it is
    // taken only on a sample where some lane's input is exactly 0.
    template <int P>
    void rcRun(const int* v, const Dest& d, int n) noexcept {
        constexpr int L = 2 * P;
        float* x[L];
        V2 g[P], st[P];
        for (int l = 0; l < L; ++l) x[l] = vbuf[static_cast<std::size_t>(v[l])].data();
        for (int q = 0; q < P; ++q) {
            g[q] = V2{rcG(v[2 * q], d), rcG(v[2 * q + 1], d)};
            st[q] = V2{ch[static_cast<std::size_t>(v[2 * q])].rcS, ch[static_cast<std::size_t>(v[2 * q + 1])].rcS};
        }
        for (int i = 0; i < n; ++i) {
            for (int q = 0; q < P; ++q) {
                const V2 in{static_cast<double>(x[2 * q][i]), static_cast<double>(x[2 * q + 1][i])};
                const V2 vv = (in - st[q]) * g[q];
                const V2 y = vv + st[q];
                st[q] = y + vv;
                for (int k = 0; k < 2; ++k) {
                    if (in[k] == 0.0 && std::abs(st[q][k]) < 1.0e-12) st[q][k] = 0.0;   // exact rest
                    x[2 * q + k][i] = static_cast<float>(y[k]) * kDigitalPerVolt;
                }
            }
        }
        for (int q = 0; q < P; ++q) {
            ch[static_cast<std::size_t>(v[2 * q])].rcS = st[q][0];
            ch[static_cast<std::size_t>(v[2 * q + 1])].rcS = st[q][1];
        }
    }

    // One running channel, after voltsToDigital(): returns the key pointer
    // (post-strip, aligned mono).
    const float* processChannel(int v, const Dest& d, int n) noexcept {
        const auto k = static_cast<std::size_t>(v);
        Channel& s = ch[k];
        const ChannelParams& p = channel[k];
        float* x = vbuf[k].data();

        // Per-voice overdrive (level-compensated tanh), before the strip. The
        // settled and gliding paths compute the same thing per sample (drive 0
        // is a bypass in both), so where a glide ends does not matter.
        if (!s.drive.settled() || s.drive.target > 0.0f) {
            if (s.drive.settled()) {
                const float pre = drivePre(s.drive.target);
                const float post = 1.0f / std::sqrt(pre);
                for (int i = 0; i < n; ++i) x[i] = std::tanh(x[i] * pre) * post;
            } else {
                for (int i = 0; i < n; ++i) {
                    const float dr = s.drive.next();
                    if (dr > 0.0f) {
                        const float pre = drivePre(dr);
                        x[i] = std::tanh(x[i] * pre) * (1.0f / std::sqrt(pre));
                    }
                }
            }
        }

        s.strip.process(x, n);

        // Track the silence going into the delay lines.
        int last = -1;
        for (int i = n - 1; i >= 0; --i)
            if (x[i] != 0.0f) {
                last = i;
                break;
            }
        s.quiet = last < 0 ? std::min(s.quiet + n, kQuietLong) : static_cast<long>(n - 1 - last);

        s.align.process(x, n);   // to the common latency

        // Spread: the right side through the Haas delay.
        const float* xr = x;
        {
            float* hb = s.haas.data();
            const int mask = s.haasMask;
            if (!s.fading && s.haasCur == 0) {
                for (int i = 0; i < n; ++i) hb[(s.haasW + i) & mask] = x[i];
                s.haasW = (s.haasW + n) & mask;
            } else {
                float* r = tmpR.data();
                int w = s.haasW;
                for (int i = 0; i < n; ++i) {
                    hb[w & mask] = x[i];
                    if (s.fading) {
                        const float a = hb[(w - s.haasCur) & mask];
                        const float b = hb[(w - s.haasNew) & mask];
                        r[i] = a + (b - a) * (static_cast<float>(s.fadePos) * spreadFadeInv);
                        if (++s.fadePos >= spreadFadeLen) {
                            s.haasCur = s.haasNew;
                            s.fading = false;
                            if (s.haasPending != s.haasCur) startSpreadFade(s);
                        }
                    } else {
                        r[i] = hb[(w - s.haasCur) & mask];
                    }
                    ++w;
                }
                s.haasW = w & mask;
                xr = r;
            }
        }

        // LEVEL + pan into the route, send into an FX bus.
        const auto dst = destFor(d, p.route, p.output);
        float* dl = dst[0];
        float* dr = dst[1];
        float pk = 0.0f;
        if (s.gL.settled() && s.gR.settled()) {
            const float gl = s.gL.target, gr = s.gR.target;
            if (gl != 0.0f || gr != 0.0f) {
                for (int i = 0; i < n; ++i) {
                    const float l = x[i] * gl, r = xr[i] * gr;
                    dl[i] += l;
                    dr[i] += r;
                    pk = std::max(pk, std::max(std::abs(l), std::abs(r)));
                }
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const float l = x[i] * s.gL.next(), r = xr[i] * s.gR.next();
                dl[i] += l;
                dr[i] += r;
                pk = std::max(pk, std::max(std::abs(l), std::abs(r)));
            }
        }
        peak[k] = pk;

        const bool sendLive = !s.sL.settled() || !s.sR.settled() || s.sL.target != 0.0f || s.sR.target != 0.0f;
        if (p.sendBus > 0 && sendLive) {
            const int b = p.sendBus - 1;
            useFx(b);
            float* sl = fxPtr(b, 0);
            float* sr = fxPtr(b, 1);
            if (s.sL.settled() && s.sR.settled()) {
                const float gl = s.sL.target, gr = s.sR.target;
                for (int i = 0; i < n; ++i) {
                    sl[i] += x[i] * gl;
                    sr[i] += xr[i] * gr;
                }
            } else {
                for (int i = 0; i < n; ++i) {
                    sl[i] += x[i] * s.sL.next();
                    sr[i] += xr[i] * s.sR.next();
                }
            }
        } else {
            s.sL.advance(n);
            s.sR.advance(n);
        }
        return x;
    }

    void processClick(int t, const Dest& d, int n) noexcept {
        Click& c = clicks[static_cast<std::size_t>(t)];
        const TriggerOutParams& o = trig[static_cast<std::size_t>(t)];
        const bool on = c.click.isActive();
        if (!on && c.quiet >= static_cast<long>(c.align.delay)) {
            c.gL.advance(n);
            c.gR.advance(n);
            c.quiet = std::min(c.quiet + n, kQuietLong);
            return;
        }
        float* x = clickBuf.data();
        c.click.render(x, n);   // writes zeros when idle
        int last = -1;
        for (int i = 0; i < n; ++i) {
            x[i] *= kDigitalPerVolt;
            if (x[i] != 0.0f) last = i;
        }
        c.quiet = last < 0 ? std::min(c.quiet + n, kQuietLong) : static_cast<long>(n - 1 - last);
        c.align.process(x, n);
        const auto dst = destFor(d, o.route, o.output);
        for (int i = 0; i < n; ++i) {
            dst[0][i] += x[i] * c.gL.next();
            dst[1][i] += x[i] * c.gR.next();
        }
    }

    std::array<bool, kNumFxBuses> fxUsed{};
    int subN = 0;

    void renderSub(const BusBuffers& out, int offset, int n) noexcept {
        Dest d;
        for (int b = 0; b < kNumBuses; ++b) {
            const auto& pair = out.ch[static_cast<std::size_t>(b)];
            const bool conn = b > 0 && pair[0] != nullptr && pair[1] != nullptr;
            d.connected[static_cast<std::size_t>(b)] = conn;
            if (conn) {
                d.bus[static_cast<std::size_t>(b)] = {pair[0] + offset, pair[1] + offset};
                std::fill_n(pair[0] + offset, n, 0.0f);
                std::fill_n(pair[1] + offset, n, 0.0f);
            } else {
                d.bus[static_cast<std::size_t>(b)] = {mainL.data(), mainR.data()};
            }
        }
        std::fill_n(mainL.data(), n, 0.0f);
        std::fill_n(mainR.data(), n, 0.0f);
        subN = n;
        fxUsed.fill(false);

        std::array<bool, kNumVoices> active{};
        renderVoices(n, active);

        std::array<bool, kNumVoices> run{};
        for (int v = 0; v < kNumVoices; ++v)
            run[static_cast<std::size_t>(v)] = !channelIdle(v, active[static_cast<std::size_t>(v)]);
        voltsToDigital(run, active, d, n);
        std::array<const float*, kNumVoices> key{};
        for (int v = 0; v < kNumVoices; ++v) {
            if (run[static_cast<std::size_t>(v)]) key[static_cast<std::size_t>(v)] = processChannel(v, d, n);
            else skipChannel(v, n);
        }

        for (int t = 0; t < kNumTriggerOuts; ++t) processClick(t, d, n);

        // FX buses: skipped when no input arrived, nothing rings and no
        // sidechain key could wake a compressor (exactly what running would do).
        bool runFx = fxUsed[0] || fxUsed[1] || rack.busActive(0) || rack.busActive(1);
        for (auto sc : sidechain)
            if (sc != kNoSource && key[sc] != nullptr) runFx = true;
        if (runFx) {
            for (int b = 0; b < kNumFxBuses; ++b) useFx(b);
            float* const bus[kNumFxBuses][2] = {{fxPtr(0, 0), fxPtr(0, 1)}, {fxPtr(1, 0), fxPtr(1, 1)}};
            rack.process(bus, key, n);
            for (int b = 0; b < kNumFxBuses; ++b) {
                const auto k = static_cast<std::size_t>(b);
                const auto& dst = d.bus[fxBus[k].output];
                const float* il = fxPtr(b, 0);
                const float* ir = fxPtr(b, 1);
                if (retL[k].settled() && retR[k].settled()) {
                    const float gl = retL[k].target, gr = retR[k].target;
                    if (gl != 0.0f || gr != 0.0f)
                        for (int i = 0; i < n; ++i) {
                            dst[0][i] += il[i] * gl;
                            dst[1][i] += ir[i] * gr;
                        }
                } else {
                    for (int i = 0; i < n; ++i) {
                        dst[0][i] += il[i] * retL[k].next();
                        dst[1][i] += ir[i] * retR[k].next();
                    }
                }
            }
        } else {
            for (int b = 0; b < kNumFxBuses; ++b) {
                retL[static_cast<std::size_t>(b)].advance(n);
                retR[static_cast<std::size_t>(b)].advance(n);
            }
        }

        // The master at rest turns silence into silence with no state change:
        // skip it (the zero-fill makes any -0 the +0 it would have written).
        if (!master.isActive() && isSilent(mainL.data(), mainR.data(), n)) {
            std::fill_n(mainL.data(), n, 0.0f);
            std::fill_n(mainR.data(), n, 0.0f);
            master.skipSilence(n);
        } else {
            master.process(mainL.data(), mainR.data(), n);
        }
        const auto& main = out.ch[0];
        if (main[0] != nullptr) std::copy_n(mainL.data(), n, main[0] + offset);
        if (main[1] != nullptr) std::copy_n(mainR.data(), n, main[1] + offset);
        clock += static_cast<std::uint64_t>(n);
    }
};

// ---------------------------------------------------------------------------

Engine::Engine() : impl_(new Impl()) {}

Engine::~Engine() { delete impl_; }

void Engine::prepare(double sampleRate, int maxBlockSize) {
    Impl& m = *impl_;
    sampleRate_ = std::isfinite(sampleRate) ? dsp::clamp(sampleRate, 8000.0, 768000.0) : 48000.0;
    maxBlock_ = std::max(1, maxBlockSize);
    m.fs = sampleRate_;

    const auto sub = static_cast<std::size_t>(kSub);
    m.noiseBuf.assign(sub, 0.0f);
    m.mainL.assign(sub, 0.0f);
    m.mainR.assign(sub, 0.0f);
    m.tmpR.assign(sub, 0.0f);
    m.clickBuf.assign(sub, 0.0f);
    for (auto& b : m.vbuf) b.assign(sub, 0.0f);
    for (auto& b : m.fxIn)
        for (auto& c : b) c.assign(sub, 0.0f);

    m.noise.prepare(sampleRate_);

    // The common latency: the largest any voice reports at any quality.
    int lat = 0;
    for (Quality q : {Quality::Eco, Quality::Standard, Quality::High}) {
        m.prepareVoices(oversampleFor(q, sampleRate_));
        for (int v = 0; v < kNumVoices; ++v) lat = std::max(lat, m.voiceLatency(v));
    }
    latency_ = lat;
    m.latency = lat;

    const int rampLen = static_cast<int>(std::lround(kRampSeconds * sampleRate_));
    const int haasMax = static_cast<int>(std::lround(kMaxSpreadSeconds * sampleRate_));
    m.spreadFadeLen = std::max(1, static_cast<int>(std::lround(kSpreadFadeSeconds * sampleRate_)));
    m.spreadFadeInv = 1.0f / static_cast<float>(m.spreadFadeLen);
    {
        const double g1 = dsp::prewarp(kMultiOutRcHz, sampleRate_);
        const double g2 = dsp::prewarp(kMixPathRcHz, sampleRate_);
        m.rcPlugG = g1 / (1.0 + g1);
        m.rcMixG = g2 / (1.0 + g2);
    }
    for (auto& c : m.ch) {
        c.strip.prepare(sampleRate_);
        for (Ramp* r : {&c.gL, &c.gR, &c.sL, &c.sR, &c.drive}) r->setLength(rampLen);
        c.align.allocate(lat);
        c.haas.assign(static_cast<std::size_t>(nextPow2(haasMax + 2)), 0.0f);
        c.haasMask = static_cast<int>(c.haas.size()) - 1;
    }
    for (auto& k : m.clicks) {
        k.click.prepare(sampleRate_);
        k.gL.setLength(rampLen);
        k.gR.setLength(rampLen);
        k.align.allocate(lat);
        k.align.delay = lat;
    }
    for (int b = 0; b < kNumFxBuses; ++b) {
        m.retL[static_cast<std::size_t>(b)].setLength(rampLen);
        m.retR[static_cast<std::size_t>(b)].setLength(rampLen);
    }
    m.rack.prepare(sampleRate_, kSub);
    m.master.prepare(sampleRate_, kSub);

    m.prepared = true;
    m.configureVoices();
    oversample_ = m.oversample;
    m.primed = false;
    m.resetChannels();
    m.rack.reset();
    m.master.reset();
    m.clock = 0;
    m.noiseReseed = true;
    // The caller's first setParams snaps every glide (here and in the strips,
    // FX and master); render() before any setParams uses the defaults.
    m.primed = false;
    channelPeak_.fill(0.0f);
}

void Engine::configure(Quality quality, std::uint32_t unitSeed, float variance, Revision revision) noexcept {
    Impl& m = *impl_;
    const float var = std::isfinite(variance) ? dsp::clamp(variance, 0.0f, 1.0f) : 0.0f;
    const Quality q = static_cast<int>(quality) <= static_cast<int>(Quality::High) ? quality : Quality::Standard;
    const Revision r = revision == Revision::Early ? Revision::Early : Revision::Late;
    if (q == m.quality && unitSeed == m.seed && var == m.variance && r == m.revision) return;
    const bool revChanged = r != m.revision;
    m.quality = q;
    m.seed = unitSeed;
    m.variance = var;
    m.revision = r;
    if (!m.prepared) return;
    m.configureVoices();
    oversample_ = m.oversample;
    if (revChanged) m.master.setParams(m.masterParams, r);
}

void Engine::reset() noexcept {
    Impl& m = *impl_;
    if (!m.prepared) return;
    m.bd.reset();
    m.sd.reset();
    for (auto& t : m.toms) t.reset();
    m.rs.reset();
    m.cp.reset();
    m.hat.reset();
    for (auto& c : m.cym) c.reset();
    m.resetChannels();
    m.rack.reset();
    m.master.reset();
    m.clock = 0;
    m.noiseReseed = true;
    channelPeak_.fill(0.0f);
}

void Engine::setParams(const EngineParams& params) noexcept {
    if (!impl_->prepared) return;
    impl_->setParams(params);
}

void Engine::trigger(VoiceId voice, const Hit& hit) noexcept {
    if (!impl_->prepared || static_cast<int>(voice) >= kNumVoices) return;
    impl_->trigger(voice, hit);
}

void Engine::setUserRom(RomSlot slot, const RomImage* image) noexcept {
    const auto s = static_cast<std::size_t>(slot);
    if (s >= static_cast<std::size_t>(kNumRomSlots)) return;
    impl_->userRom[s] = image;
    impl_->applyRoms();
}

void Engine::render(const BusBuffers& out, int offset, int numSamples) noexcept {
    Impl& m = *impl_;
    if (numSamples <= 0) return;
    if (!m.prepared) {
        for (const auto& pair : out.ch)
            for (float* c : pair)
                if (c != nullptr) std::fill_n(c + offset, numSamples, 0.0f);
        return;
    }
    if (!m.primed) {
        EngineParams p;
        p.voice = m.voiceParams;
        p.hatChoke = m.hatChoke;
        m.setParams(p);
    }
    std::array<float, kNumVoices> pk{};
    for (int pos = 0; pos < numSamples;) {
        const int n = std::min(kSub, numSamples - pos);
        m.renderSub(out, offset + pos, n);
        for (std::size_t v = 0; v < pk.size(); ++v) pk[v] = std::max(pk[v], m.peak[v]);
        pos += n;
    }
    channelPeak_ = pk;
}

bool Engine::anyVoiceActive() const noexcept {
    for (int v = 0; v < kNumVoices; ++v)
        if (impl_->voiceActive(v)) return true;
    return false;
}

}  // namespace mw909
