// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Factory content: kits and presets, and the public entry points of
// mw909/Factory.h. The pattern memories (and the step notation they are
// written in) live in FactoryPatterns*.cpp and FactoryNotation.h.
//
// Kits are built at compile time from the stock machine (every knob at its
// kVoiceInfo default, LEVEL at the calibration point, centred, no strip, no
// FX, master stage Modern) and change only what they need to. Knob notes:
//   * BD: TUNE (0..1) is the pitch-sweep time, PITCH (semitones) the base
//     frequency, DEPTH the sweep amount, SHAPER the waveshaper (0.5 = stock).
//   * SD: TONE and SNAPPY are the panel pots; DECAY and DEPTH are extensions.
//   * Toms: TUNE and DECAY are the panel pots; DEPTH/SWEEP/NOISE extensions.
//   * Sample voices: CH/OH TUNE is in semitones (extension), CR/RD TUNE is the
//     panel pot (0..1). SOURCE 0 = ROM, 1 = the synthetic metal source.
//   * Kits that drive voices or the master pull the master volume down so a
//     preset change does not jump in loudness; the limiter stays on.
// Every value is checked against the documented range at compile time
// (kitsValid below) and again by tests/unit/core_factory.cpp.

#include "mw909/Factory.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "FactoryNotation.h"

namespace mw909::factory {

namespace {

using knob::Bd;
using knob::Cp;
using knob::Pcm;
using knob::Rs;
using knob::Sd;
using knob::Tom;

constexpr std::uint8_t kBus1 = 0;  // FxPlacement::bus value for FX bus 1
constexpr std::uint8_t kBus2 = 1;  // ... and FX bus 2

constexpr Kit stockKit(std::string_view name, std::string_view category) noexcept {
    Kit k{};
    k.name = name;
    k.category = category;
    for (std::size_t v = 0; v < kNumVoices; ++v) {
        k.voice[v] = defaultParams(static_cast<int>(v));
        k.channel[v] = ChannelParams{};
        k.strip[v] = StripParams{};
    }
    k.fx = FxParams{};
    for (auto& t : k.trig) t = TriggerOutParams{};
    k.totalAccent = 0.7f;
    k.master = MasterParams{};
    return k;
}

// ---- small builders ---------------------------------------------------------------
constexpr void set(Kit& k, VoiceId v, int knobIndex, float value) noexcept {
    k.voice[v].k[static_cast<std::size_t>(knobIndex)] = value;
}
// Mixer: level, pan, drive.
constexpr void mix(Kit& k, VoiceId v, float level, float pan = 0.0f, float drive = 0.0f) noexcept {
    k.channel[v].level = level;
    k.channel[v].pan = pan;
    k.channel[v].drive = drive;
}
constexpr void send(Kit& k, VoiceId v, int bus, float amount, bool pre = false) noexcept {
    k.channel[v].sendBus = static_cast<std::uint8_t>(bus);
    k.channel[v].send = amount;
    k.channel[v].sendPre = pre;
}
constexpr void cuts(Kit& k, VoiceId v, float lowCutHz, float highCutHz, float resonance = 0.0f) noexcept {
    k.strip[v].eqOn = true;
    k.strip[v].lowCutHz = lowCutHz;
    k.strip[v].highCutHz = highCutHz;
    k.strip[v].resonance = resonance;
}
constexpr void band(Kit& k, VoiceId v, int b, float hz, float widthOct, float gainDb) noexcept {
    k.strip[v].eqOn = true;
    k.strip[v].band[static_cast<std::size_t>(b)] = EqBand{true, hz, widthOct, gainDb};
}
constexpr void comp(Kit& k, VoiceId v, float thresholdDb, float ratio, float attackMs, float releaseMs,
                    float makeupDb) noexcept {
    k.strip[v].compOn = true;
    k.strip[v].compThresholdDb = thresholdDb;
    k.strip[v].compRatio = ratio;
    k.strip[v].compAttackMs = attackMs;
    k.strip[v].compReleaseMs = releaseMs;
    k.strip[v].compMakeupDb = makeupDb;
}
constexpr void place(Kit& k, FxId id, std::uint8_t bus, std::uint8_t order) noexcept {
    k.fx.place[static_cast<std::size_t>(id)] = FxPlacement{true, bus, order};
}
constexpr void trigger(Kit& k, int slot, VoiceId source, float level, float pan) noexcept {
    TriggerOutParams& t = k.trig[static_cast<std::size_t>(slot)];
    t.on = true;
    t.source = source;
    t.route = ChannelRoute::Output;
    t.output = 0;
    t.level = level;
    t.pan = pan;
}
constexpr void masterOut(Kit& k, float volumeDb, float drive = 0.0f, OutputStage stage = OutputStage::Modern) noexcept {
    k.master.volumeDb = volumeDb;
    k.master.drive = drive;
    k.master.stage = stage;
}
// The 3-band master compressor: one threshold/ratio for all bands, gentle.
constexpr void glue(Kit& k, float thresholdDb, float ratio, float makeupDb) noexcept {
    k.master.compOn = true;
    for (auto& b : k.master.band) {
        b.on = true;
        b.thresholdDb = thresholdDb;
        b.ratio = ratio;
        b.makeupDb = makeupDb;
    }
}
// The three toms at once: tune (panel 0..1), decay.
constexpr void toms(Kit& k, float tuneLow, float tuneMid, float tuneHigh, float decay) noexcept {
    set(k, LT, Tom::TomTune, tuneLow);
    set(k, MT, Tom::TomTune, tuneMid);
    set(k, HT, Tom::TomTune, tuneHigh);
    set(k, LT, Tom::TomDecay, decay);
    set(k, MT, Tom::TomDecay, decay);
    set(k, HT, Tom::TomDecay, decay);
}
// The hardware's fixed stereo placement (research/03 §0 "Master mix"): LT and
// RIDE 5.25 dB louder on one side, HT and the hats 5.25 dB louder on the
// other, CRASH 1.58 dB towards the HT side, the rest centred. Which side is
// CONFLICTING in the sources; this follows the traced master-amp wiring (LT
// and RIDE left). With the mixer's constant-power pan (L = cos, R = sin of
// (pan + 1) pi/4), an L/R ratio of r dB is pan = atan(10^(r/20)) / (pi/4) - 1:
// 5.25 dB -> 0.3633, 1.58 dB -> 0.1152. `width` scales the image (1 = the
// hardware).
constexpr float kHwPan = 0.3633f;
constexpr float kHwCrashPan = 0.1152f;
constexpr void hardwarePan(Kit& k, float width = 1.0f) noexcept {
    k.channel[LT].pan = -kHwPan * width;
    k.channel[RD].pan = -kHwPan * width;
    k.channel[HT].pan = kHwPan * width;
    k.channel[CH].pan = kHwPan * width;
    k.channel[OH].pan = kHwPan * width;
    k.channel[CR].pan = kHwCrashPan * width;
}
constexpr void reverb(Kit& k, std::uint8_t bus, float predelayMs, float size, float decay, float color,
                      float levelDb) noexcept {
    place(k, FxId::Reverb, bus, 5);
    k.fx.reverb.predelayMs = predelayMs;
    k.fx.reverb.earlySize = size;
    k.fx.reverb.lateSize = size;
    k.fx.reverb.decay = decay;
    k.fx.reverb.earlyColor = color;
    k.fx.reverb.lateColor = color;
    k.fx.reverb.mix = 1.0f;
    k.fx.bus[bus].levelDb = levelDb;
}

// ---- the kits ------------------------------------------------------------------------

// 0 — the reference machine: every knob at its default, flat mixer, nothing added.
constexpr Kit kitStock() noexcept { return stockKit("Stock", "Classic"); }

// 1 — kick and toms tuned down a few semitones, a little longer.
constexpr Kit kitTunedDown() noexcept {
    Kit k = stockKit("Tuned Down", "Classic");
    set(k, BD, Bd::BdPitch, -3.0f);
    set(k, BD, Bd::BdDecay, 0.6f);
    set(k, BD, Bd::BdDepth, 0.55f);
    set(k, SD, Sd::SdTune, 0.35f);
    toms(k, 0.25f, 0.3f, 0.35f, 0.6f);
    return k;
}

// 2 — the long boom: decay near the end of the pot, low pitch, soft click.
constexpr Kit kitLongBoom() noexcept {
    Kit k = stockKit("Long Boom", "Bass");
    set(k, BD, Bd::BdPitch, -2.0f);
    set(k, BD, Bd::BdDecay, 0.95f);
    set(k, BD, Bd::BdAttack, 0.3f);
    set(k, BD, Bd::BdDepth, 0.6f);
    set(k, BD, Bd::BdTune, 0.6f);
    set(k, SD, Sd::SdTone, 0.4f);
    set(k, CP, Cp::CpDecay, 0.6f);
    mix(k, BD, 0.6f);
    cuts(k, BD, 25.0f, 20000.0f);
    band(k, BD, 1, 350.0f, 1.5f, -3.0f);
    comp(k, BD, -14.0f, 3.0f, 20.0f, 200.0f, 2.0f);
    cuts(k, CH, 250.0f, 20000.0f);
    cuts(k, OH, 250.0f, 20000.0f);
    masterOut(k, -1.0f);
    return k;
}

// 3 — tight techno: short punchy kick, clipped hats, compressed kick and snare.
constexpr Kit kitTightTechno() noexcept {
    Kit k = stockKit("Tight Techno", "Techno");
    set(k, BD, Bd::BdDecay, 0.4f);
    set(k, BD, Bd::BdAttack, 0.7f);
    set(k, BD, Bd::BdTune, 0.45f);
    set(k, SD, Sd::SdSnappy, 0.6f);
    set(k, SD, Sd::SdDecay, 0.4f);
    set(k, CH, Pcm::PcmDecay, 0.25f);
    set(k, OH, Pcm::PcmDecay, 0.35f);
    set(k, CP, Cp::CpReverb, 0.35f);
    mix(k, BD, 0.6f);
    mix(k, CH, 0.45f, 0.15f);
    mix(k, OH, 0.45f, 0.15f);
    mix(k, RD, 0.4f, -0.2f);
    comp(k, BD, -12.0f, 4.0f, 10.0f, 80.0f, 3.0f);
    comp(k, SD, -15.0f, 3.0f, 5.0f, 100.0f, 3.0f);
    band(k, BD, 1, 300.0f, 1.0f, -4.0f);
    cuts(k, CH, 400.0f, 20000.0f);
    cuts(k, OH, 400.0f, 20000.0f);
    cuts(k, CP, 180.0f, 20000.0f);
    k.totalAccent = 0.8f;
    return k;
}

// 4 — warehouse: a big concrete room on claps, snare and rims (FX bus 2).
constexpr Kit kitWarehouse() noexcept {
    Kit k = stockKit("Warehouse", "Techno");
    set(k, BD, Bd::BdDecay, 0.6f);
    set(k, BD, Bd::BdAttack, 0.6f);
    set(k, CP, Cp::CpReverb, 0.6f);
    mix(k, BD, 0.55f, 0.0f, 0.15f);
    send(k, CP, 2, 0.45f);
    send(k, SD, 2, 0.35f);
    send(k, RS, 2, 0.4f);
    send(k, OH, 2, 0.2f);
    reverb(k, kBus2, 20.0f, 0.8f, 0.55f, 0.4f, -4.0f);
    cuts(k, OH, 300.0f, 20000.0f);
    cuts(k, CH, 300.0f, 20000.0f);
    comp(k, BD, -10.0f, 3.0f, 15.0f, 100.0f, 2.0f);
    masterOut(k, -1.0f);
    return k;
}

// 5 — garage: short kick, crisp snappy snare, forward rims, a little plate on
// claps and rims, bright closed hats.
constexpr Kit kitGarage() noexcept {
    Kit k = stockKit("Garage Shuffle", "Garage");
    set(k, BD, Bd::BdDecay, 0.45f);
    set(k, BD, Bd::BdPitch, 1.0f);
    set(k, SD, Sd::SdTone, 0.65f);
    set(k, SD, Sd::SdSnappy, 0.7f);
    set(k, SD, Sd::SdTune, 0.6f);
    set(k, RS, Rs::RsTune, 1.0f);
    set(k, CH, Pcm::PcmTune, 1.0f);
    set(k, CH, Pcm::PcmDecay, 0.35f);
    set(k, OH, Pcm::PcmTune, 1.0f);
    set(k, OH, Pcm::PcmDecay, 0.4f);
    mix(k, RS, 0.6f, 0.2f);
    mix(k, CH, 0.5f, 0.25f);
    mix(k, OH, 0.45f, -0.25f);
    send(k, CP, 2, 0.3f);
    send(k, RS, 2, 0.25f);
    send(k, SD, 2, 0.2f);
    reverb(k, kBus2, 5.0f, 0.35f, 0.3f, 0.65f, -6.0f);
    cuts(k, CH, 500.0f, 20000.0f);
    comp(k, SD, -18.0f, 3.0f, 3.0f, 80.0f, 4.0f);
    return k;
}

// 6 — gabber: the kick through its own shaper and channel drive, then routed
// into FX bus 1 (distortion, then an EQ that scoops the mud and lifts the
// knock), master drive on top. Hats and clap stay clean.
constexpr Kit kitGabber() noexcept {
    Kit k = stockKit("Gabber Drive", "Hardcore");
    set(k, BD, Bd::BdPitch, -1.0f);
    set(k, BD, Bd::BdDecay, 0.7f);
    set(k, BD, Bd::BdAttack, 0.8f);
    set(k, BD, Bd::BdDrive, 0.9f);
    set(k, BD, Bd::BdDepth, 0.7f);
    set(k, BD, Bd::BdTune, 0.35f);
    mix(k, BD, 0.55f, 0.0f, 0.6f);
    k.channel[BD].route = ChannelRoute::FxBus1;
    place(k, FxId::Distortion, kBus1, 0);
    k.fx.distortion.preampDb = 18.0f;
    k.fx.distortion.thresholdDb = -12.0f;
    k.fx.distortion.knee = 0.3f;
    k.fx.distortion.splitHz = 20.0f;
    k.fx.distortion.lowOutDb = 0.0f;
    k.fx.distortion.clipOutDb = -2.0f;
    k.fx.distortion.mix = 1.0f;
    place(k, FxId::EqA, kBus1, 1);
    k.fx.eq[0].lowHz = 35.0f;
    k.fx.eq[0].mid[0] = EqBand{true, 450.0f, 1.2f, -5.0f};
    k.fx.eq[0].mid[1] = EqBand{true, 2500.0f, 1.0f, 3.0f};
    k.fx.bus[0].levelDb = -3.0f;
    set(k, CP, Cp::CpDecay, 0.4f);
    set(k, OH, Pcm::PcmDecay, 0.35f);
    mix(k, CP, 0.45f);
    mix(k, OH, 0.45f, 0.1f);
    cuts(k, OH, 500.0f, 20000.0f);
    k.totalAccent = 0.85f;
    masterOut(k, -5.0f, 0.6f);
    return k;
}

// 7 — acid: a punchy kick, tight snappy hats so the accents bite, a dotted
// echo on the clap.
constexpr Kit kitAcid() noexcept {
    Kit k = stockKit("Acid Box", "Acid");
    set(k, BD, Bd::BdDecay, 0.55f);
    set(k, BD, Bd::BdAttack, 0.65f);
    set(k, CH, Pcm::PcmDecay, 0.3f);
    set(k, OH, Pcm::PcmDecay, 0.45f);
    set(k, CP, Cp::CpReverb, 0.4f);
    mix(k, CH, 0.55f, 0.1f);
    send(k, CP, 1, 0.25f);
    place(k, FxId::Delay, kBus1, 0);
    k.fx.delay.sync = true;
    k.fx.delay.div = SyncDiv::D1_8;
    k.fx.delay.kind = SyncKind::Dotted;
    k.fx.delay.feedback = 0.3f;
    k.fx.delay.spread = 0.5f;
    k.fx.delay.loopFilter = FilterType::HighPass;
    k.fx.delay.cutoffHz = 600.0f;
    k.fx.bus[0].levelDb = -8.0f;
    comp(k, BD, -12.0f, 4.0f, 8.0f, 90.0f, 3.0f);
    k.totalAccent = 0.8f;
    return k;
}

// 8 — lo-fi: everything but the kick sent into a 9-bit, 14 kHz bitcrusher,
// dull hats, the original output stage.
constexpr Kit kitLoFi() noexcept {
    Kit k = stockKit("Lo-Fi Crunch", "Lo-Fi");
    set(k, BD, Bd::BdDecay, 0.65f);
    set(k, BD, Bd::BdPitch, -2.0f);
    set(k, SD, Sd::SdTone, 0.35f);
    set(k, CH, Pcm::PcmTune, -1.0f);
    set(k, OH, Pcm::PcmTune, -1.0f);
    for (VoiceId v : {SD, LT, MT, HT, RS, CP, CH, OH, CR, RD}) send(k, v, 1, 0.6f);
    place(k, FxId::Bitcrusher, kBus1, 0);
    k.fx.bitcrusher.bits = 9.0f;
    k.fx.bitcrusher.rateHz = 14000.0f;
    k.fx.bitcrusher.jitter = 0.1f;
    k.fx.bitcrusher.approxFilter = 0.9f;
    k.fx.bitcrusher.imagesFilter = 0.8f;
    place(k, FxId::EqB, kBus1, 1);
    k.fx.eq[1].lowHz = 150.0f;
    k.fx.eq[1].highHz = 7000.0f;
    k.fx.bus[0].levelDb = -3.0f;
    cuts(k, CH, 20.0f, 8000.0f);
    cuts(k, OH, 20.0f, 8000.0f);
    cuts(k, CR, 20.0f, 7000.0f);
    cuts(k, RD, 20.0f, 7000.0f);
    masterOut(k, -1.0f, 0.2f, OutputStage::Original);
    return k;
}

// 9 — live room: reverb sends on clap, snare and both hats.
constexpr Kit kitLiveRoom() noexcept {
    Kit k = stockKit("Live Room", "Ambience");
    send(k, CP, 2, 0.5f);
    send(k, SD, 2, 0.4f);
    send(k, CH, 2, 0.2f);
    send(k, OH, 2, 0.3f);
    send(k, LT, 2, 0.25f);
    send(k, MT, 2, 0.25f);
    send(k, HT, 2, 0.25f);
    reverb(k, kBus2, 15.0f, 0.6f, 0.45f, 0.55f, -3.0f);
    k.fx.reverb.earlyLate = 0.4f;
    hardwarePan(k);
    cuts(k, BD, 20.0f, 20000.0f);
    band(k, BD, 1, 250.0f, 1.0f, -2.0f);
    return k;
}

// 10 — dub: a ping-pong dotted-8th echo through a band-pass loop filter on
// rims, clap, snare and open hat, feeding a dark plate.
constexpr Kit kitDub() noexcept {
    Kit k = stockKit("Dub Echo", "Dub");
    set(k, BD, Bd::BdDecay, 0.7f);
    set(k, BD, Bd::BdAttack, 0.35f);
    set(k, RS, Rs::RsDecay, 0.6f);
    set(k, CP, Cp::CpReverb, 0.3f);
    send(k, RS, 1, 0.55f);
    send(k, CP, 1, 0.4f);
    send(k, SD, 1, 0.35f);
    send(k, OH, 1, 0.25f);
    send(k, MT, 1, 0.3f);
    place(k, FxId::Delay, kBus1, 0);
    k.fx.delay.sync = true;
    k.fx.delay.div = SyncDiv::D1_8;
    k.fx.delay.kind = SyncKind::Dotted;
    k.fx.delay.feedback = 0.58f;
    k.fx.delay.spread = 0.8f;
    k.fx.delay.loopFilter = FilterType::BandPass;
    k.fx.delay.cutoffHz = 1400.0f;
    k.fx.delay.reso = 0.3f;
    k.fx.bus[0].levelDb = -4.0f;
    k.fx.bus[0].toBus2 = 0.4f;
    reverb(k, kBus2, 30.0f, 0.7f, 0.6f, 0.3f, -8.0f);
    cuts(k, CH, 350.0f, 12000.0f);
    return k;
}

// 11 — first run: the early unit (before S/N 415300, research/03 §7.1) on a
// machine left at the default Late revision. A kit cannot set the revision
// (a machine setting); on a machine switched to Revision::Early the voices
// already carry these changes, so use the stock kit there instead.
//   * No output caps (C500..C512): the early master has no ~6.6 kHz output
//     low-pass, so the output stage stays Modern. (The Original stage adds
//     that low-pass on a Late machine: the later units' darker sound.)
//   * HH C134 10 uF instead of 0.01 uF: no ~720 Hz high-pass on the hat
//     output, so the early hats keep their low end. A wide +6 dB bell at
//     350 Hz on CH and OH puts most of what the late high-pass removes back
//     (it takes 7 dB at 350 Hz, 3 dB at 720 Hz).
//   * BD C9 0.22 uF instead of 0.33 uF: the pitch sweep is 2/3 as long at
//     every TUNE setting. The early unit's centre TUNE (tau = 0.22 uF x
//     34.8 kOhm = 7.7 ms) is TUNE 0.07 on the late one (BassDrum.h taper).
//   * Tom C54 / R194: the later units "emphasize attack of TOM TOMs", so the
//     tom noise comes down a little.
//   * The hardware's own stereo placement.
// (The early rim shot, R417 12k, is a circuit change no rim-shot knob reaches.)
constexpr Kit kitFirstRun() noexcept {
    Kit k = stockKit("First Run", "Classic");
    set(k, BD, Bd::BdTune, 0.07f);
    band(k, CH, 0, 350.0f, 2.0f, 6.0f);
    band(k, OH, 0, 350.0f, 2.0f, 6.0f);
    set(k, LT, Tom::TomNoise, 0.4f);
    set(k, MT, Tom::TomNoise, 0.4f);
    set(k, HT, Tom::TomNoise, 0.4f);
    hardwarePan(k);
    masterOut(k, 0.0f, 0.1f);
    return k;
}

// 12 — Detroit soul: melodic toms with a long tail through a chorus, a bright
// ride, a softer clap with more of its own reverb.
constexpr Kit kitDetroit() noexcept {
    Kit k = stockKit("Detroit Soul", "Techno");
    toms(k, 0.45f, 0.55f, 0.65f, 0.7f);
    set(k, LT, Tom::TomSweep, 0.6f);
    set(k, MT, Tom::TomSweep, 0.6f);
    set(k, HT, Tom::TomSweep, 0.6f);
    set(k, CP, Cp::CpReverb, 0.65f);
    set(k, RD, Pcm::PcmTune, 0.6f);
    mix(k, LT, 0.45f, -0.4f);
    mix(k, MT, 0.45f, 0.0f);
    mix(k, HT, 0.45f, 0.4f);
    mix(k, RD, 0.45f, -0.3f);
    send(k, LT, 1, 0.5f);
    send(k, MT, 1, 0.5f);
    send(k, HT, 1, 0.5f);
    place(k, FxId::Chorus, kBus1, 0);
    k.fx.chorus.depth = 0.6f;
    k.fx.chorus.rateHz = 0.4f;
    k.fx.chorus.spatial = 0.8f;
    k.fx.chorus.mix = 1.0f;
    k.fx.bus[0].levelDb = -5.0f;
    send(k, CP, 2, 0.3f);
    reverb(k, kBus2, 10.0f, 0.5f, 0.4f, 0.5f, -6.0f);
    cuts(k, CH, 300.0f, 20000.0f);
    cuts(k, OH, 300.0f, 20000.0f);
    return k;
}

// 13 — deep velvet: soft-attack round kick, warm low snare, rounded hats,
// rim forward with a short room.
constexpr Kit kitDeepVelvet() noexcept {
    Kit k = stockKit("Deep Velvet", "House");
    set(k, BD, Bd::BdDecay, 0.65f);
    set(k, BD, Bd::BdAttack, 0.3f);
    set(k, BD, Bd::BdPitch, -1.0f);
    set(k, SD, Sd::SdTone, 0.3f);
    set(k, SD, Sd::SdSnappy, 0.4f);
    set(k, CH, Pcm::PcmDecay, 0.4f);
    set(k, CH, Pcm::PcmTune, -1.0f);
    set(k, OH, Pcm::PcmTune, -1.0f);
    mix(k, RS, 0.6f, 0.15f);
    mix(k, CH, 0.4f, 0.2f);
    mix(k, OH, 0.4f, 0.2f);
    mix(k, RD, 0.4f, -0.3f);
    cuts(k, CH, 300.0f, 9000.0f);
    cuts(k, OH, 300.0f, 9000.0f);
    cuts(k, RD, 200.0f, 10000.0f);
    send(k, RS, 2, 0.35f);
    send(k, CP, 2, 0.3f);
    reverb(k, kBus2, 12.0f, 0.4f, 0.4f, 0.35f, -6.0f);
    comp(k, BD, -16.0f, 2.5f, 25.0f, 150.0f, 2.0f);
    glue(k, -10.0f, 1.5f, 1.0f);
    return k;
}

// 14 — piano house: snappy bright snare and clap, longer open hat, a lift on
// the claps' presence, a hall on the clap.
constexpr Kit kitPianoHouse() noexcept {
    Kit k = stockKit("Piano House", "House");
    set(k, SD, Sd::SdSnappy, 0.7f);
    set(k, SD, Sd::SdTone, 0.6f);
    set(k, OH, Pcm::PcmDecay, 0.6f);
    set(k, CP, Cp::CpReverb, 0.55f);
    set(k, BD, Bd::BdAttack, 0.55f);
    mix(k, CP, 0.6f);
    band(k, CP, 1, 2500.0f, 1.5f, 3.0f);
    cuts(k, CP, 200.0f, 20000.0f);
    send(k, CP, 2, 0.35f);
    send(k, SD, 2, 0.2f);
    reverb(k, kBus2, 20.0f, 0.65f, 0.5f, 0.6f, -6.0f);
    comp(k, BD, -12.0f, 3.0f, 12.0f, 100.0f, 2.0f);
    glue(k, -12.0f, 2.0f, 2.0f);
    return k;
}

// 15 — electro punch: higher, short kick with a hard click, very snappy
// snare, short swept toms, tight hats, and the closed hat's trigger-out click
// layered in for extra tick.
constexpr Kit kitElectro() noexcept {
    Kit k = stockKit("Electro Punch", "Electro");
    set(k, BD, Bd::BdPitch, 2.0f);
    set(k, BD, Bd::BdDecay, 0.4f);
    set(k, BD, Bd::BdAttack, 0.85f);
    set(k, BD, Bd::BdDepth, 0.75f);
    set(k, SD, Sd::SdSnappy, 0.8f);
    set(k, SD, Sd::SdTone, 0.65f);
    toms(k, 0.5f, 0.6f, 0.7f, 0.3f);
    set(k, LT, Tom::TomDepth, 0.8f);
    set(k, MT, Tom::TomDepth, 0.8f);
    set(k, HT, Tom::TomDepth, 0.8f);
    set(k, CH, Pcm::PcmDecay, 0.2f);
    mix(k, LT, 0.5f, -0.4f);
    mix(k, HT, 0.5f, 0.4f);
    comp(k, SD, -14.0f, 4.0f, 2.0f, 60.0f, 3.0f);
    trigger(k, 0, CH, 0.15f, 0.3f);
    return k;
}

// 16 — trance pump: tight kick, long open hat, a plate on clap and hats that
// ducks under every kick (dynamics after the reverb, keyed from BD).
constexpr Kit kitTrancePump() noexcept {
    Kit k = stockKit("Trance Pump", "Trance");
    set(k, BD, Bd::BdDecay, 0.6f);
    set(k, BD, Bd::BdAttack, 0.7f);
    set(k, BD, Bd::BdPitch, -1.0f);
    set(k, OH, Pcm::PcmDecay, 0.55f);
    send(k, CP, 2, 0.5f);
    send(k, OH, 2, 0.3f);
    send(k, CH, 2, 0.2f);
    send(k, SD, 2, 0.4f);
    reverb(k, kBus2, 25.0f, 0.75f, 0.6f, 0.6f, -3.0f);
    place(k, FxId::DynamicsA, kBus2, 6);
    k.fx.dynamics[0].mode = DynamicsMode::Compressor;
    k.fx.dynamics[0].thresholdDb = -30.0f;
    k.fx.dynamics[0].ratio = 8.0f;
    k.fx.dynamics[0].attackMs = 2.0f;
    k.fx.dynamics[0].releaseMs = 180.0f;
    k.fx.dynamics[0].sidechain = BD;
    comp(k, BD, -10.0f, 4.0f, 10.0f, 80.0f, 2.0f);
    cuts(k, OH, 400.0f, 20000.0f);
    glue(k, -12.0f, 2.0f, 2.0f);
    return k;
}

// 17 — disco room: natural snare, swept toms in the syn-drum direction, long
// open hat, a medium room on snare, toms and hats.
constexpr Kit kitDisco() noexcept {
    Kit k = stockKit("Disco Room", "Disco");
    set(k, SD, Sd::SdSnappy, 0.55f);
    set(k, SD, Sd::SdDecay, 0.6f);
    toms(k, 0.5f, 0.55f, 0.6f, 0.6f);
    set(k, LT, Tom::TomDepth, 0.85f);
    set(k, MT, Tom::TomDepth, 0.85f);
    set(k, HT, Tom::TomDepth, 0.85f);
    set(k, LT, Tom::TomSweep, 0.75f);
    set(k, MT, Tom::TomSweep, 0.75f);
    set(k, HT, Tom::TomSweep, 0.75f);
    set(k, OH, Pcm::PcmDecay, 0.65f);
    hardwarePan(k);
    send(k, SD, 2, 0.3f);
    send(k, LT, 2, 0.3f);
    send(k, MT, 2, 0.3f);
    send(k, HT, 2, 0.3f);
    send(k, OH, 2, 0.2f);
    reverb(k, kBus2, 15.0f, 0.5f, 0.4f, 0.55f, -5.0f);
    glue(k, -12.0f, 1.8f, 1.5f);
    return k;
}

// 18 — industrial metal: hats from the synthetic metal source, driven kick and
// snare, a distortion bus on snare, rims and toms, a swept filter on the hats,
// and a harsh trigger-out click on the snare.
constexpr Kit kitIndustrial() noexcept {
    Kit k = stockKit("Industrial Metal", "Industrial");
    set(k, CH, Pcm::PcmSource, 1.0f);
    set(k, CH, Pcm::PcmColor, 0.8f);
    set(k, CH, Pcm::PcmDensity, 0.7f);
    set(k, OH, Pcm::PcmSource, 1.0f);
    set(k, OH, Pcm::PcmColor, 0.8f);
    set(k, OH, Pcm::PcmDensity, 0.7f);
    set(k, BD, Bd::BdDrive, 0.75f);
    set(k, BD, Bd::BdDecay, 0.55f);
    set(k, SD, Sd::SdSnappy, 0.75f);
    mix(k, BD, 0.5f, 0.0f, 0.4f);
    mix(k, SD, 0.45f, 0.0f, 0.3f);
    send(k, SD, 1, 0.5f);
    send(k, RS, 1, 0.6f);
    send(k, LT, 1, 0.4f);
    send(k, MT, 1, 0.4f);
    send(k, HT, 1, 0.4f);
    place(k, FxId::Distortion, kBus1, 0);
    k.fx.distortion.preampDb = 24.0f;
    k.fx.distortion.thresholdDb = -18.0f;
    k.fx.distortion.knee = 0.2f;
    k.fx.distortion.splitHz = 150.0f;
    k.fx.distortion.lowOutDb = -6.0f;
    k.fx.distortion.clipOutDb = -4.0f;
    k.fx.distortion.mix = 0.8f;
    k.fx.bus[0].levelDb = -6.0f;
    send(k, CH, 2, 0.5f);
    send(k, OH, 2, 0.5f);
    place(k, FxId::Filter, kBus2, 0);
    k.fx.filter.type = FilterType::BandPass;
    k.fx.filter.cutoffHz = 3000.0f;
    k.fx.filter.reso = 0.5f;
    k.fx.filter.source = ModSource::Lfo;
    k.fx.filter.lfoSync = true;
    k.fx.filter.lfoDiv = SyncDiv::D1_1;
    k.fx.filter.lfoWave = LfoWave::Triangle;
    k.fx.filter.lfoSpread = 0.5f;
    k.fx.filter.modAmount = 0.5f;
    k.fx.bus[1].levelDb = -6.0f;
    trigger(k, 0, SD, 0.25f, -0.2f);
    masterOut(k, -4.0f, 0.4f);
    return k;
}

// 19 — hip-hop dust: deep long kick, fat low snare, dull hats, a light
// 12-bit grit on snare and hats, master glue.
constexpr Kit kitHipHop() noexcept {
    Kit k = stockKit("Hip-Hop Dust", "Hip-Hop");
    set(k, BD, Bd::BdDecay, 0.75f);
    set(k, BD, Bd::BdPitch, -2.0f);
    set(k, BD, Bd::BdAttack, 0.45f);
    set(k, SD, Sd::SdTune, 0.35f);
    set(k, SD, Sd::SdTone, 0.35f);
    set(k, SD, Sd::SdSnappy, 0.45f);
    set(k, SD, Sd::SdDecay, 0.6f);
    set(k, CH, Pcm::PcmTune, -1.5f);
    mix(k, SD, 0.55f);
    cuts(k, CH, 250.0f, 7000.0f);
    cuts(k, OH, 250.0f, 7000.0f);
    send(k, SD, 1, 0.4f);
    send(k, CH, 1, 0.5f);
    send(k, CP, 1, 0.4f);
    place(k, FxId::Bitcrusher, kBus1, 0);
    k.fx.bitcrusher.bits = 12.0f;
    k.fx.bitcrusher.rateHz = 22050.0f;
    k.fx.bitcrusher.mix = 1.0f;
    k.fx.bus[0].levelDb = -4.0f;
    comp(k, SD, -16.0f, 4.0f, 8.0f, 120.0f, 4.0f);
    comp(k, BD, -14.0f, 3.0f, 15.0f, 150.0f, 2.0f);
    glue(k, -14.0f, 2.0f, 3.0f);
    masterOut(k, -1.0f);
    return k;
}

// 20 — minimal click: short tight kick, high short rim with a ping-pong 16th
// echo, very short closed hat, and the rim's trigger-out click in the mix.
constexpr Kit kitMinimal() noexcept {
    Kit k = stockKit("Minimal Click", "Minimal");
    set(k, BD, Bd::BdDecay, 0.32f);
    set(k, BD, Bd::BdAttack, 0.8f);
    set(k, RS, Rs::RsTune, 3.0f);
    set(k, RS, Rs::RsDecay, 0.35f);
    set(k, CH, Pcm::PcmDecay, 0.15f);
    set(k, CP, Cp::CpDecay, 0.35f);
    mix(k, RS, 0.5f, 0.3f);
    mix(k, CH, 0.45f, -0.2f);
    send(k, RS, 1, 0.4f);
    place(k, FxId::Delay, kBus1, 0);
    k.fx.delay.sync = true;
    k.fx.delay.div = SyncDiv::D1_16;
    k.fx.delay.kind = SyncKind::Straight;
    k.fx.delay.feedback = 0.45f;
    k.fx.delay.spread = 1.0f;
    k.fx.delay.loopFilter = FilterType::HighPass;
    k.fx.delay.cutoffHz = 1200.0f;
    k.fx.bus[0].levelDb = -6.0f;
    cuts(k, CH, 600.0f, 20000.0f);
    trigger(k, 0, RS, 0.2f, -0.3f);
    return k;
}

// 21 — breakbeat crunch: compressed hard, kick and snare into a parallel
// distortion, a driven snare.
constexpr Kit kitBreakbeat() noexcept {
    Kit k = stockKit("Breakbeat Crunch", "Breaks");
    set(k, BD, Bd::BdDecay, 0.55f);
    set(k, BD, Bd::BdAttack, 0.75f);
    set(k, SD, Sd::SdSnappy, 0.65f);
    set(k, SD, Sd::SdDecay, 0.55f);
    mix(k, SD, 0.5f, 0.0f, 0.3f);
    comp(k, BD, -18.0f, 6.0f, 5.0f, 60.0f, 5.0f);
    comp(k, SD, -18.0f, 6.0f, 3.0f, 60.0f, 5.0f);
    send(k, BD, 1, 0.35f);
    send(k, SD, 1, 0.45f);
    send(k, CH, 1, 0.3f);
    place(k, FxId::Distortion, kBus1, 0);
    k.fx.distortion.preampDb = 20.0f;
    k.fx.distortion.thresholdDb = -15.0f;
    k.fx.distortion.splitHz = 120.0f;
    k.fx.distortion.lowOutDb = -60.0f;
    k.fx.distortion.mix = 1.0f;
    k.fx.bus[0].levelDb = -8.0f;
    glue(k, -14.0f, 2.5f, 3.0f);
    masterOut(k, -2.0f, 0.25f);
    return k;
}

// 22 — body gate: EBM's gated snare. The snare and clap go into a big reverb
// that a VCA on the bus opens on each snare hit and shuts ~120 ms later.
constexpr Kit kitBodyGate() noexcept {
    Kit k = stockKit("Body Gate", "EBM");
    set(k, BD, Bd::BdDecay, 0.45f);
    set(k, BD, Bd::BdAttack, 0.7f);
    set(k, SD, Sd::SdSnappy, 0.6f);
    set(k, SD, Sd::SdTone, 0.55f);
    send(k, SD, 2, 0.6f);
    send(k, CP, 2, 0.5f);
    reverb(k, kBus2, 0.0f, 0.9f, 0.7f, 0.6f, -2.0f);
    place(k, FxId::DynamicsB, kBus2, 6);
    k.fx.dynamics[1].mode = DynamicsMode::Vca;
    k.fx.dynamics[1].attackMs = 1.0f;
    k.fx.dynamics[1].holdMs = 90.0f;
    k.fx.dynamics[1].releaseMs = 30.0f;
    k.fx.dynamics[1].triggers = {SD, CP, kNoSource};
    k.fx.dynamics[1].vcaDepth = 1.0f;
    comp(k, BD, -12.0f, 4.0f, 10.0f, 80.0f, 3.0f);
    comp(k, SD, -14.0f, 3.0f, 5.0f, 100.0f, 2.0f);
    return k;
}

// 23 — wide stereo: the hardware's own stereo placement made wider, Haas
// spread on the cymbals and open hat.
constexpr Kit kitWide() noexcept {
    Kit k = stockKit("Wide Stereo", "Utility");
    hardwarePan(k, 1.65f);  // pan 0.6 for LT/RD/HT/hats
    k.channel[MT].pan = 0.0f;
    k.channel[RS].pan = -0.2f;
    k.channel[CP].pan = 0.1f;
    k.channel[OH].spread = 0.4f;
    k.channel[CR].spread = 0.6f;
    k.channel[RD].spread = 0.5f;
    return k;
}

// 24 — peak hour: a driven kick with a lifted knock, bright rides, the
// multiband master compressor and a touch of master drive.
constexpr Kit kitPeakHour() noexcept {
    Kit k = stockKit("Peak Hour", "Techno");
    set(k, BD, Bd::BdDecay, 0.5f);
    set(k, BD, Bd::BdAttack, 0.75f);
    set(k, BD, Bd::BdDrive, 0.65f);
    set(k, RD, Pcm::PcmTune, 0.6f);
    set(k, CR, Pcm::PcmTune, 0.55f);
    set(k, OH, Pcm::PcmDecay, 0.45f);
    mix(k, BD, 0.55f, 0.0f, 0.25f);
    band(k, BD, 0, 60.0f, 1.0f, 2.0f);
    band(k, BD, 1, 3000.0f, 1.0f, 2.0f);
    comp(k, BD, -10.0f, 4.0f, 10.0f, 70.0f, 2.0f);
    cuts(k, RD, 400.0f, 20000.0f);
    cuts(k, OH, 400.0f, 20000.0f);
    send(k, CP, 2, 0.3f);
    reverb(k, kBus2, 10.0f, 0.55f, 0.35f, 0.6f, -8.0f);
    glue(k, -16.0f, 2.5f, 3.0f);
    k.master.xoverLowHz = 150.0f;
    k.master.xoverHighHz = 4000.0f;
    masterOut(k, -3.0f, 0.3f);
    k.totalAccent = 0.8f;
    return k;
}

constexpr std::array<Kit, 25> kKits{{
    kitStock(),       //  0
    kitTunedDown(),   //  1
    kitLongBoom(),    //  2
    kitTightTechno(), //  3
    kitWarehouse(),   //  4
    kitGarage(),      //  5
    kitGabber(),      //  6
    kitAcid(),        //  7
    kitLoFi(),        //  8
    kitLiveRoom(),    //  9
    kitDub(),         // 10
    kitFirstRun(),    // 11
    kitDetroit(),     // 12
    kitDeepVelvet(),  // 13
    kitPianoHouse(),  // 14
    kitElectro(),     // 15
    kitTrancePump(),  // 16
    kitDisco(),       // 17
    kitIndustrial(),  // 18
    kitHipHop(),      // 19
    kitMinimal(),     // 20
    kitBreakbeat(),   // 21
    kitBodyGate(),    // 22
    kitWide(),        // 23
    kitPeakHour(),    // 24
}};

// ---- compile-time range check (the same ranges the tests check) --------------------------------
constexpr bool in(float x, float lo, float hi) noexcept { return x >= lo && x <= hi; }  // false for NaN

constexpr bool bandOk(const EqBand& b) noexcept {
    return in(b.freqHz, 20.0f, 20000.0f) && in(b.widthOct, 0.5f, 4.0f) && in(b.gainDb, -24.0f, 24.0f);
}

constexpr bool kitValid(const Kit& k) noexcept {
    if (k.name.empty() || k.category.empty() || !in(k.totalAccent, 0.0f, 1.0f)) return false;
    for (std::size_t v = 0; v < kNumVoices; ++v) {
        const VoiceInfo& info = kVoiceInfo[v];
        for (std::size_t i = 0; i < static_cast<std::size_t>(kMaxKnobs); ++i) {
            const float x = k.voice[v].k[i];
            if (static_cast<int>(i) >= info.numKnobs) {
                if (x != info.knobs[i].def) return false;
                continue;
            }
            if (!in(x, info.knobs[i].min, info.knobs[i].max)) return false;
            if (info.knobs[i].unit == KnobUnit::Choice && (x != static_cast<float>(static_cast<int>(x)) || x >= 2.0f))
                return false;  // whole numbers; never the (empty) user image
        }
        const ChannelParams& c = k.channel[v];
        if (!in(c.level, 0.0f, 1.0f) || !in(c.pan, -1.0f, 1.0f) || !in(c.spread, 0.0f, 1.0f) ||
            !in(c.drive, 0.0f, 1.0f) || c.output >= kNumBuses || c.sendBus > kNumFxBuses || !in(c.send, 0.0f, 1.0f))
            return false;
        const StripParams& s = k.strip[v];
        if (!in(s.lowCutHz, 20.0f, 2000.0f) || !in(s.highCutHz, 500.0f, 20000.0f) || !in(s.resonance, 0.0f, 1.0f) ||
            !bandOk(s.band[0]) || !bandOk(s.band[1]) || !in(s.compThresholdDb, -48.0f, 0.0f) ||
            !in(s.compRatio, 1.0f, 20.0f) || !in(s.compAttackMs, 1.0f, 500.0f) || !in(s.compReleaseMs, 1.0f, 500.0f) ||
            !in(s.compMakeupDb, 0.0f, 24.0f))
            return false;
    }
    const FxParams& f = k.fx;
    for (const FxPlacement& p : f.place)
        if (p.bus > 1 || p.order > 9) return false;
    for (const FxBusParams& b : f.bus)
        if (!in(b.levelDb, -60.0f, 12.0f) || b.output >= kNumBuses || !in(b.toBus2, 0.0f, 1.0f)) return false;
    const DistortionParams& d = f.distortion;
    if (!in(d.preampDb, 0.0f, 36.0f) || !in(d.thresholdDb, -36.0f, 0.0f) || !in(d.knee, 0.0f, 1.0f) ||
        !in(d.splitHz, 20.0f, 2000.0f) || !in(d.lowOutDb, -60.0f, 12.0f) || !in(d.clipOutDb, -60.0f, 12.0f) ||
        !in(d.mix, 0.0f, 1.0f))
        return false;
    for (const ParamEqParams& e : f.eq)
        if (!in(e.lowHz, 20.0f, 2000.0f) || !in(e.lowGainDb, -24.0f, 24.0f) || !in(e.lowRes, 0.0f, 1.0f) ||
            !bandOk(e.mid[0]) || !bandOk(e.mid[1]) || !in(e.highHz, 500.0f, 20000.0f) ||
            !in(e.highGainDb, -24.0f, 24.0f) || !in(e.highRes, 0.0f, 1.0f))
            return false;
    const DelayParams& dl = f.delay;
    if (!in(dl.timeMs, 1.0f, 1000.0f) || static_cast<int>(dl.div) >= static_cast<int>(SyncDiv::kCount) ||
        !in(dl.feedback, 0.0f, 0.98f) || !in(dl.spread, -1.0f, 1.0f) || !in(dl.cutoffHz, 20.0f, 20000.0f) ||
        !in(dl.reso, 0.0f, 1.0f) || !in(dl.mix, 0.0f, 1.0f))
        return false;
    const ReverbParams& r = f.reverb;
    if (!in(r.predelayMs, 0.0f, 250.0f) || !in(r.earlySize, 0.0f, 1.0f) || !in(r.earlyDiffusion, 0.0f, 1.0f) ||
        !in(r.earlyColor, 0.0f, 1.0f) || !in(r.lateSize, 0.0f, 1.0f) || !in(r.lateDiffusion, 0.0f, 1.0f) ||
        !in(r.lateColor, 0.0f, 1.0f) || !in(r.decay, 0.0f, 1.0f) || !in(r.modulation, 0.0f, 1.0f) ||
        !in(r.earlyLate, 0.0f, 1.0f) || !in(r.mix, 0.0f, 1.0f))
        return false;
    const ChorusParams& ch = f.chorus;
    if (!in(ch.offsetMs, 1.0f, 30.0f) || !in(ch.depth, 0.0f, 1.0f) || !in(ch.rateHz, 0.02f, 10.0f) ||
        !in(ch.feedback, -0.95f, 0.95f) || !in(ch.spatial, 0.0f, 1.0f) || !in(ch.lowCutHz, 20.0f, 20000.0f) ||
        !in(ch.highCutHz, 20.0f, 20000.0f) || !in(ch.fatness, 0.0f, 1.0f) || !in(ch.gainDb, -24.0f, 24.0f) ||
        !in(ch.mix, 0.0f, 1.0f))
        return false;
    const BitcrusherParams& bc = f.bitcrusher;
    if (!in(bc.bits, 1.0f, 24.0f) || !in(bc.rateHz, 44.0f, 44100.0f) || !in(bc.jitter, 0.0f, 1.0f) ||
        !in(bc.approxFilter, 0.0f, 1.0f) || !in(bc.imagesFilter, 0.0f, 1.0f) || !in(bc.mix, 0.0f, 1.0f))
        return false;
    for (const DynamicsParams& dy : f.dynamics) {
        if (!in(dy.thresholdDb, -48.0f, 0.0f) || !in(dy.ratio, 1.0f, 20.0f) || !in(dy.attackMs, 1.0f, 500.0f) ||
            !in(dy.holdMs, 0.0f, 500.0f) || !in(dy.releaseMs, 1.0f, 500.0f) || !in(dy.makeupDb, 0.0f, 24.0f) ||
            !in(dy.vcaDepth, -1.0f, 1.0f) || !in(dy.mix, 0.0f, 1.0f))
            return false;
        if (dy.sidechain != kNoSource && dy.sidechain >= kNumVoices) return false;
        for (const std::uint8_t t : dy.triggers)
            if (t != kNoSource && t >= kNumVoices) return false;
    }
    const FilterFxParams& fl = f.filter;
    if (!in(fl.cutoffHz, 20.0f, 20000.0f) || !in(fl.reso, 0.0f, 1.0f) || !in(fl.lfoRateHz, 0.01f, 20.0f) ||
        !in(fl.lfoSpread, 0.0f, 1.0f) || !in(fl.envAttackMs, 1.0f, 500.0f) || !in(fl.envHoldMs, 0.0f, 500.0f) ||
        !in(fl.envReleaseMs, 1.0f, 500.0f) || !in(fl.modAmount, -1.0f, 1.0f) || !in(fl.mix, 0.0f, 1.0f))
        return false;
    for (const std::uint8_t t : fl.envTriggers)
        if (t != kNoSource && t >= kNumVoices) return false;
    for (const TriggerOutParams& t : k.trig)
        if (t.source >= kNumVoices || t.output >= kNumBuses || !in(t.pan, -1.0f, 1.0f) || !in(t.level, 0.0f, 1.0f))
            return false;
    const MasterParams& m = k.master;
    if (!in(m.drive, 0.0f, 1.0f) || !in(m.xoverLowHz, 40.0f, 1000.0f) || !in(m.xoverHighHz, 1000.0f, 12000.0f) ||
        m.xoverHighHz <= m.xoverLowHz || !in(m.limThresholdDb, -24.0f, 0.0f) || !in(m.limAttackMs, 0.1f, 10.0f) ||
        !in(m.limReleaseMs, 10.0f, 1000.0f) || !in(m.volumeDb, -60.0f, 12.0f) || !m.limiterOn)
        return false;
    for (const CompBand& b : m.band)
        if (!in(b.thresholdDb, -48.0f, 0.0f) || !in(b.ratio, 1.0f, 20.0f) || !in(b.attackMs, 1.0f, 500.0f) ||
            !in(b.releaseMs, 1.0f, 500.0f) || !in(b.makeupDb, 0.0f, 24.0f))
            return false;
    return true;
}

constexpr int firstInvalidKit() noexcept {
    for (std::size_t i = 0; i < kKits.size(); ++i)
        if (!kitValid(kKits[i])) return static_cast<int>(i);
    return -1;
}
static_assert(firstInvalidKit() == -1, "factory kit parameter out of range (value = kit index)");

// ---- presets -----------------------------------------------------------------------------------
// Pattern indices are global within the memory: bank I groups 1..3 are
// 0..15, 16..31, 32..47; bank II groups 1..3 are 48..63, 64..79, 80..95.
// Memories: 0 Classic, 1 House Sessions, 2 Techno Floor, 3 Acid & Electro,
// 4 Shuffle & Breaks, 5 Hard & Fast, 6 Trance & Disco, 7 Rhythm Lab, 8 Empty.
constexpr std::array<Preset, 60> kPresets{{
    // name                    category        kit mem pat  tempo   swing
    {"Init",                   "Basic",          0, 0,  0, 120.0f, 1.0f},
    {"Classic Four",           "Classic",        0, 0,  0, 125.0f, 1.0f},
    {"Chicago Jack",           "House",         11, 0,  1, 124.0f, 1.0f},
    {"Warehouse Jack",         "House",          4, 0,  2, 126.0f, 1.0f},
    {"Deep Night",             "House",         13, 0,  3, 120.0f, 1.0f},
    {"Piano Era",              "House",         14, 0,  4, 122.0f, 1.0f},
    {"Detroit Drive",          "Techno",        12, 0, 16, 130.0f, 1.0f},
    {"Minimal Pulse",          "Techno",        20, 0, 18, 126.0f, 1.0f},
    {"Industrial Stomp",       "Industrial",    18, 0, 21, 135.0f, 1.0f},
    {"Acid Line Classic",      "Acid",           7, 0, 32, 128.0f, 1.0f},
    {"Electro Funk",           "Electro",       15, 0, 36, 118.0f, 1.0f},
    {"Disco Strut",            "Disco",         17, 0, 48, 118.0f, 1.0f},
    {"Boom Bap Classic",       "Hip-Hop",        1, 0, 64,  90.0f, 1.0f},
    {"Full Shuffle",           "Classic",        0, 0, 85, 110.0f, 1.0f},
    {"Cellar Jack",          "House",          4, 1,  0, 124.0f, 1.0f},
    {"Late Hours",             "House",         13, 1, 17, 120.0f, 1.0f},
    {"Garage Organ",           "House",          5, 1, 34, 124.0f, 1.0f},
    {"Piano Anthem",           "House",         14, 1, 32, 124.0f, 1.0f},
    {"Tribal Drums",           "House",          9, 1, 48, 126.0f, 1.0f},
    {"Assembly Line",          "Techno",        12, 2,  0, 130.0f, 1.0f},
    {"Minimal Tick",           "Techno",        20, 2, 16, 125.0f, 1.0f},
    {"Rubber Loop",            "Techno",        10, 2, 19, 124.0f, 1.0f},
    {"Peak Time",              "Techno",        24, 2, 32, 134.0f, 1.0f},
    {"Rolling Rumble",        "Techno",         3, 2, 34, 136.0f, 1.0f},
    {"Hard Groove",            "Techno",         3, 2, 48, 140.0f, 1.0f},
    {"Distorted Stomp",        "Industrial",    18, 2, 50, 138.0f, 1.0f},
    {"Squelch Shuffle",        "Acid",           7, 3,  3, 126.0f, 1.0f},
    {"Acid Journey",           "Acid",           7, 3, 32, 130.0f, 1.0f},
    {"Robot Electro",          "Electro",       15, 3, 17, 120.0f, 1.0f},
    {"Miami Drive",            "Electro",        2, 3, 20, 130.0f, 1.0f},
    {"Two-Step Shuffle",       "Garage",         5, 4,  0, 134.0f, 1.0f},
    {"Garage Skip",            "Garage",         5, 4,  1, 132.0f, 1.0f},
    {"Speed Garage",           "Garage",         2, 4, 16, 132.0f, 1.0f},
    {"Big Beat Smash",         "Breaks",        21, 4, 32, 128.0f, 1.0f},
    {"Breakbeat Funk",         "Breaks",        21, 4, 33, 110.0f, 1.0f},
    {"Basement Bap",           "Hip-Hop",       19, 4, 48,  92.0f, 1.0f},
    {"Lo-Fi Lean",             "Hip-Hop",        8, 4, 51,  80.0f, 1.0f},
    {"Gabber Stomp",           "Hardcore",       6, 5,  0, 185.0f, 1.0f},
    {"Terror Pulse",           "Hardcore",       6, 5,  3, 200.0f, 1.0f},
    {"Happy Hardcore",         "Hardcore",      24, 5, 16, 170.0f, 1.0f},
    {"Rave Stab Beat",         "Hardcore",      21, 5, 17, 150.0f, 1.0f},
    {"Body Drill",             "EBM",           22, 2, 64, 122.0f, 1.0f},
    {"Hammer Step",            "EBM",           22, 2, 65, 118.0f, 1.0f},
    {"Pistons",                "Industrial",    18, 2, 81, 130.0f, 1.0f},
    {"Trance Gate",            "Trance",        16, 6,  0, 138.0f, 1.0f},
    {"Psy Gallop",             "Trance",        16, 6,  3, 145.0f, 1.0f},
    {"Hard Trance",            "Trance",        24, 6, 16, 145.0f, 1.0f},
    {"Breakdown Riser",        "Trance",         9, 6,  2, 138.0f, 1.0f},
    {"Neon Italo",             "Disco",         17, 6, 48, 120.0f, 1.0f},
    {"Hi-NRG Rush",            "Disco",         15, 6, 52, 130.0f, 1.0f},
    {"Glitter Floor",          "Disco",         17, 6, 66, 118.0f, 1.0f},
    {"Three Over Four",        "Experimental",  23, 7,  0, 120.0f, 1.0f},
    {"Glitch Thirty-Two",      "Experimental",   8, 7, 18,  96.0f, 1.0f},
    {"Half-Time Triplets",     "Experimental",  10, 7, 32, 140.0f, 1.0f},
    {"Dice Groove",            "Experimental",  20, 7, 48, 124.0f, 1.0f},
    {"Dub Chord Pulse",        "Dub",           10, 1, 19, 118.0f, 1.0f},
    {"Detroit Soul",           "Techno",        12, 2,  1, 128.0f, 1.0f},
    {"Tamed Rims",             "Garage",         5, 4,  3, 130.0f, 0.6f},
    {"Wide Voice Check",       "Utility",       23, 0, 87, 120.0f, 1.0f},
    {"Empty Canvas",           "Basic",          0, 8,  0, 120.0f, 1.0f},
}};

template <class T, std::size_t N>
constexpr int clampIndex(const std::array<T, N>&, int index) noexcept {
    return index < 0 ? 0 : (index >= static_cast<int>(N) ? static_cast<int>(N) - 1 : index);
}

}  // namespace

int numKits() noexcept { return static_cast<int>(kKits.size()); }
const Kit& kit(int index) noexcept { return kKits[static_cast<std::size_t>(clampIndex(kKits, index))]; }

int numBanks() noexcept { return detail::bankCount(); }
const BankInfo& bankInfo(int index) noexcept { return detail::bankInfoAt(index); }
void buildBank(int index, seq::PatternBank& out) noexcept { detail::buildBankAt(index, out); }
std::string_view patternName(int bankIndex, int patternIndex) noexcept {
    return detail::patternNameAt(bankIndex, patternIndex);
}

int numPresets() noexcept { return static_cast<int>(kPresets.size()); }
const Preset& preset(int index) noexcept { return kPresets[static_cast<std::size_t>(clampIndex(kPresets, index))]; }

}  // namespace mw909::factory
