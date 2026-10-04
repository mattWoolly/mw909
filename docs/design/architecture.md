<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# mw909 architecture

mw909 is a circuit-informed emulation of the classic 1983 hybrid drum machine (seven
analog voices and four 6-bit sample voices) with a built-in step sequencer, for Linux
(VST3, CLAP, LV2, standalone). This document is the contract between the modules.
Research that backs the numbers lives in `research/01..06`. The shell, state and UI
contract is `plugin.md`.

The design is mw808's, carried over where it was proven (three-layer JUCE-free core, one
realtime `Machine` entry point, lock-free queues, generation-stamped bank handoff) and
changed where mw808's contract was weak. The changes are marked **(new in mw909)**.

## 1. Rules that apply everywhere

- **No brand names** in anything shipped: code, comments, UI strings, presets, pattern
  names, file names, binary strings, README, design docs. `scripts/forbidden_tokens.txt`
  is the list and `scripts/branding-audit.sh` enforces it. Research documents are the only
  exception. Say "the original machine", "the hardware".
- **Licence header** on every source, CMake and shell file (two lines):
  `// SPDX-License-Identifier: AGPL-3.0-or-later` /
  `// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.`
- **C++20, g++ 13.** No `-ffast-math`, `-ffp-contract=off` on the core. Same input, same
  bytes out, on the same toolchain. Zero warnings in project code.
- **Realtime safety.** Anything reachable from `Machine::process` never allocates, locks,
  does I/O or throws. All state is fixed-size members or buffers allocated in `prepare`.
  `prepare(const VoiceSetup&)` of voices and DSP blocks is allocation-free, because the
  engine re-prepares voices on the audio thread when quality, seed, variance or revision
  changes.
- **Never reset voice state on retrigger.** A new hit lands on whatever is still ringing:
  envelope capacitors, filters, VCA state and noise keep their state. What the circuit
  itself resets on a trigger (the 909's VCO integrators are shorted by the trigger pulse;
  the sample counters are reset) is modelled as that circuit mechanism, not as a software
  `reset()`. `reset()` is only for silence (prepare, panic).
- **Sample-rate independence.** Pitch, decay to −20 dB and normal/accent Vpp within a few
  percent (tests: ±3 % pitch and level, ±5 % decay) from 44.1 to 192 kHz. Use the TPT
  filters in `dsp/Filters.h` (pre-warped) for anything tuned.
- **Determinism.** Output is a pure function of (parameters, hits, unit seed, sample rate,
  quality, block partition-independent). Noise and variance come from seeded generators.
- **NaN/Inf safety.** Every `setParams` sanitises: non-finite → the knob default, then
  clamp to range. A voice fed NaN parameters must keep producing finite output.
- **Click-free sleep and wake.** A voice goes to sleep only when its output and every state
  that could produce output have decayed below about −120 dBFS, and a sleeping voice wakes
  with exactly the state it would have had (zeros), so no discontinuity.
- `std::atomic<T>` only for T of 8 bytes or less (16-byte atomics are not lock-free here).

## 2. Layout and ownership

```
libs/mw909-core/include/mw909/
  Voices.h            roster, Hit + AccentDac, knobs, VoiceSetup, kVoiceInfo      (contract, frozen)
  MidiMap.h           note map, velocity -> Hit                                    (contract, frozen)
  dsp/Math.h Filters.h Halfband.h   shared primitives (carried from mw808)
  dsp/Noise.h         shared noise source, defined by spectral density             (contract, frozen)
  dsp/Rom.h           6-bit ROM images, factory images, romFromAudio               (contract, frozen)
  dsp/<other>.h       further primitives, added by the area that needs them
  voices/*.h          the voices, header-only                                       (§3)
  fx/FxParams.h       strip, FX bus, master and trigger-out parameters             (contract, frozen)
  fx/*.h              ChannelStrip, FxRack, MasterChain, TriggerClick               (§4.3)
  seq/Pattern.h       pattern POD model                                             (contract, frozen)
  seq/Sequencer.h     sequencer public API                                          (contract, frozen)
  seq/PatternOps.h    pattern editing helpers                                       (§5.6)
  seq/MidiFile.h      pattern <-> standard MIDI file                                (§5.7)
  Engine.h            voices + noise + ROMs + strips + FX + master                  (contract, frozen)
  Machine.h           Engine + Sequencer + MIDI + cross-thread queues               (contract, frozen)
  Factory.h           factory kits, pattern memories, presets                       (contract, frozen)
libs/mw909-core/src/  Engine.cpp Machine.cpp Factory*.cpp seq/*.cpp fx/*.cpp rom/*.cpp
plugin/               JUCE shell (src/) and editor (ui/)
tools/mw909-render/   CLI: render voices/patterns to WAV, MIDI export, bench
tools/rombuild/       builds the factory ROM images from assets/rom/ (CC0 sources)
assets/rom/           CC0 source recordings + PROVENANCE.md (shipped as derived 6-bit data only)
tests/unit/           Catch2, one executable per file
```

CMake targets: `mw909_dsp` (INTERFACE: dsp + voices + fx headers), `mw909_rom` (STATIC,
`src/rom/*.cpp`, linked by `mw909_dsp`, so a voice test that needs ROMs links it
automatically), `mw909_fx` (STATIC, `src/fx/*.cpp`, if FX code needs a .cpp), `mw909_seq`
(STATIC, `src/seq/*.cpp`), `mw909_core` (STATIC, `src/*.cpp`, links all). Sources and tests
are globbed. A test file `tests/unit/<prefix>_<name>.cpp` becomes executable
`mw909_test_<prefix>_<name>`; the prefix picks what it links: `dsp_` → `mw909_dsp`,
`fx_` → `mw909_dsp` + `mw909_fx`, `seq_` → `mw909_seq`, `core_` → `mw909_core`.

**Area ownership in the build phase** (disjoint files; an area may add files under its own
names; nobody edits another area's files without the orchestrator's say-so):

| Area | Owns |
|---|---|
| analog voices | `voices/{BassDrum,SnareDrum,Tom,RimShot,HandClap}.h`, new `dsp/` primitives they need (prefix the file with what it is, e.g. `dsp/TriangleVco.h`), `tests/unit/dsp_voice_{bd,sd,tom,rs,cp}.cpp`, `tests/unit/dsp_analog_*.cpp`, `tests/unit/AnalogTestUtil.h` |
| sample voices | `voices/{HiHat,Cymbal}.h`, `dsp/Pcm*.h`, `dsp/MetalSynth.h`, `src/rom/*`, `tools/rombuild/`, `assets/rom/`, `tests/unit/dsp_voice_{hats,cymbal}.cpp`, `tests/unit/dsp_pcm_*.cpp`, `tests/unit/PcmTestUtil.h` |
| sequencer | `seq/*`, `src/seq/*`, `tests/unit/seq_*.cpp`, `tests/unit/SeqTestUtil.h` |
| mixer and FX | `fx/*` except `FxParams.h`, `src/fx/*`, `tests/unit/fx_*.cpp` |
| engine and machine | `Engine.cpp`, `Machine.cpp`, `libs/mw909-core/CMakeLists.txt`, `tests/unit/core_{engine,machine}.cpp`, `tests/unit/CoreTestUtil.h` |
| factory library | `Factory.h` (data shape only with the orchestrator), `src/Factory*.cpp`, `tests/unit/core_factory.cpp` |
| plugin shell | `plugin/src/*`, `plugin/CMakeLists.txt`, `tests/plugin/*` |
| tools, goldens, scripts | `tools/*` (not rombuild), `tests/golden/*`, `scripts/*`, `CMakeLists.txt`, `CMakePresets.json`, `cmake/*` |
| editor UI | `plugin/ui/*`, `tests/ui/*` |

## 3. Voice contract

### 3.1 Roster and classes

Eleven voices in panel order (`VoiceId`): BD SD LT MT HT RS CP CH OH CR RD. BD, SD, the
three toms, RS and CP are analog; CH, OH, CR and RD are 6-bit PCM through an analog chain.

| File | Class | Voices | render signature |
|---|---|---|---|
| `voices/BassDrum.h` | `BassDrum` | BD | `(out, noise, n)` |
| `voices/SnareDrum.h` | `SnareDrum` | SD | `(out, noise, n)` |
| `voices/Tom.h` | `Tom`, `prepare(setup, VoiceId which)` | LT MT HT | `(out, noise, n)` |
| `voices/RimShot.h` | `RimShot` | RS | `(out, n)` |
| `voices/HandClap.h` | `HandClap` | CP | `(out, noise, n)` |
| `voices/HiHat.h` | `HiHat` | CH + OH | see §3.4 |
| `voices/Cymbal.h` | `Cymbal`, `prepare(setup, VoiceId which)` | CR RD | `(out, n)` |

A voice that does not use noise in the circuit still takes the signature above when the
table says so; a voice whose research shows no noise path may ignore the pointer.

### 3.2 Interface (every class)

```cpp
void prepare(const VoiceSetup&) noexcept;    // no allocation; may run on the audio thread
void reset() noexcept;                       // silence; clear all state
void setParams(const VoiceParams&) noexcept; // before render, at most once per sub-block; sanitises NaN/Inf
void trigger(const Hit&) noexcept;           // takes effect at the next rendered sample
void setAccLine(float volts) noexcept;       // the voice's accent DAC was rewritten by its partner (§3.3)
void render(float* out, [const float* noise,] int n) noexcept;  // OVERWRITES out[0..n), volts at VoiceSetup::sampleRate
bool isActive() const noexcept;              // false => render would produce exact zeros
int latencySamples() const noexcept;         // output delay in host-rate samples (0 for most voices)
static constexpr bool kFreeRunning;          // true if render() must run while asleep (§3.6)
```

Sample voices additionally take their ROMs:
`void setRoms(const RomImage* factory, const RomImage* user) noexcept;` (`user` may be
null; both pointers outlive the voice's use of them, see Machine §6).

### 3.3 Dynamics: the accent line (new in mw909)

The original fires a fixed 5 V / 2 ms trigger and delivers dynamics as a held control
voltage from a 6-bit accent DAC per voice (0..5 V), research/03 §3. So `Hit::acc` is the
accent-line voltage for this hit, and `AccentDac` in `Voices.h` builds it:
`code = clamp(base + voiceAccent·kVoiceAccentCodes + totalAccent·round(kTotalAccentCodes·knob), 0, 63)`.

- Feed `acc` into the circuit where the schematic does (BD: pitch envelope, click and VCA
  envelope; SD: tone VCA and snappy; toms: envelopes; RS, CP: envelope; hats and cymbals:
  the anti-log VCA control). Never as a plain post-gain.
- The hats and cymbals read the line **continuously** (their VCA control node), the analog
  voices sample it while the trigger gate is open. RS/CP share one DAC and CR/RD share
  another: when one fires, the engine calls `setAccLine(newVolts)` on its partner, which
  matters for the voices that read it continuously.
- The constants are FITTED. The analog-voice area owns them and fits them to the measured
  accent gains (research/05 §0.11: TOTAL ACCENT 0→100 % raises Vpp by +6.2 dB BD,
  +4.8 dB SD, +7.3–7.7 dB toms, +3.9 dB RS, +6.8 dB CH, +7.2 dB OH, +7.9 dB CR, +7.7 dB RD;
  research/03 §3.2: about 2× ENV amplitude for a voice accent), then records the fit in
  `Voices.h` and research/01.
- **Weak hits** (codes below normal: weak steps, low MIDI velocities) may behave in a
  voice-specific way near transistor thresholds (a circuit may go quiet abruptly where the
  real one would). Required: finite, click-free, and no louder than a normal hit.

### 3.4 HiHat (CH + OH)

The original has one hat circuit: one counter, one ROM (open hat at 0x0000–0x5FFF, closed
hat at 0x6000–0x7FFF), one VCA and envelope. Choke is a retrigger: a closed hit restarts
the counter in the closed region and recharges the envelope, which cuts the open hat
(research/02 §0.13).

```cpp
void prepare(const VoiceSetup&) noexcept;
void reset() noexcept;
void setParams(const VoiceParams& closed, const VoiceParams& open) noexcept;
void setChoke(bool on) noexcept;         // on (default) = one circuit, as the hardware
void triggerClosed(const Hit&) noexcept;
void triggerOpen(const Hit&) noexcept;
void setAccLine(float volts) noexcept;   // not shared on the hardware; kept for symmetry
void setRoms(const RomImage* factory, const RomImage* user) noexcept;
void render(float* outClosed, float* outOpen, int n) noexcept;
bool isActive() const noexcept;
int latencySamples() const noexcept;
```

- With choke on, the circuit's output goes to `outClosed` while the closed region plays and
  to `outOpen` while the open region plays; a trigger moves it. Closed and open triggered
  on the same sample play **open**, whatever the call order (the hardware's step code
  `11 = Open`).
- With choke off, CH and OH are two independent copies of the circuit (extension).
- CH and OH each have their own knobs (tune, decay, source, color, density); on the
  hardware both share the level and tune, and the two DECAY knobs are paralleled paths into
  one capacitor (research/02 §0.9). With choke on, model that coupling (CH decay changes
  slightly with the OH knob).

### 3.5 Sample voices: the chain

What must be modelled (research/02 §8, research/05 §0.8–0.10): ROM read by a counter
clocked by an RC oscillator (TUNE = clock rate: crash/ride over the pot range, the hats via
the extension knob, centre ≈ 32 kHz nominal per research/05's measurement; nominal value is
a fit parameter with variance), a 6-bit zero-order-hold DAC with per-unit bit-weight error,
the address-driven envelope DAC and anti-log VCA (crash/ride), the RC envelope and anti-log
VCA (hats), the accent line into the VCA control node, the two Sallen-Key low-passes
(25.9 kHz Q 0.55, 27.7 kHz Q 1.32), and the level stage.

- **Images are part of the sound.** The reconstruction filter does not remove the first
  image band, so render the ZOH signal band-limited (BLEP step insertion at exact edge
  times, research/05 §0.10) to the host rate, keeping image content below the host Nyquist
  and rejecting fold-back below −90 dB. Clock rates above the host rate must work.
- **Sources** (`knob::PcmSource`): `Rom` reads the factory image, `User` the imported image
  (falls back to `Rom` when the slot is empty), `Synth` runs the synthetic metal source at
  the ROM clock (deterministic, seeded; `Color` and `Density` shape it; it must produce
  envelope-flattened material like the ROM content, so the analog VCA restores the decay).
  `Color` and `Density` are inert for `Rom`/`User`.
- **Licensing rule:** no original ROM data, dumps, or recordings of the original machine
  anywhere in shipped code or data, and nothing fitted sample-by-sample to them. Reference
  recordings (`.deps/ref-samples/`) are for statistics in tests only.

### 3.6 Noise, levels, oversampling, latency, sleep (new in mw909 where marked)

- **Noise (new):** one `dsp::WhiteNoise` per engine, defined by spectral density: a sample
  at rate fs has variance `kDensity²·fs/2` (unit RMS at 48 kHz). Every noise voice gets the
  same host-rate buffer and needs no rate compensation: its own filters see the same
  in-band level at every rate. A voice that oversamples internally and needs noise at the
  higher rate interpolates or generates extra noise with the same density from a seed
  derived from the unit seed.
- **Output scale (new):** voices render **volts** at their individual output with their
  LEVEL pot at centre (1.0f = 1 V), the condition of the service manual's check figures.
  Calibrate normal and accented **Vpp** directly against research/01 and research/02 §7.
  The engine converts with `kDigitalPerVolt` (10 V peak = 0 dBFS).
- **Oversampling** is private to a voice: `VoiceSetup::oversample` (1, 2 or 4) is the factor
  it may use internally. The triangle VCOs need none (research/05 §0.6, use polyBLAMP at
  the corners and polyBLEP at the reset step); the PCM chain uses BLEP step insertion.
- **Latency (new):** `latencySamples()` reports a voice's output delay. The engine delays
  every voice to a common latency `L` that is constant for a prepared sample rate (the
  maximum over every quality), so voices stay phase-aligned, and the plugin reports `L`.
- **Free-running sources (new):** a voice with `kFreeRunning = true` has a source whose
  phase must keep moving while it sleeps (e.g. a free-running oscillator in the synthetic
  metal source). The engine calls `render()` on such voices every block even when inactive;
  an inactive free-running voice only advances that source and writes zeros. The analog
  VCOs are reset by the trigger on this machine, so they are not free-running.
- **Sleep:** `isActive()` false once output and every internal state are below about
  −120 dBFS; waking must be click-free.
- **Unit variance:** component values scale with `dsp::componentScale(unitSeed, componentId,
  tolerance, variance)`; `variance = 0` is the nominal schematic. `Revision::Early`
  applies the pre-change parts from the service notes (research/03 §7.1: BD C9 0.22 µF,
  RS R417 12 k, HH C134 10 µF, tom C54 0.0022 µF / R194 47 k; master output without the
  0.01 µF caps).

### 3.7 Voice header comment block and tests

Each voice header starts with a comment block stating the circuit it models, the component
values used with designators, which values come from the schematic and which are FITTED (and
to what), and what is not modelled.

Each voice has a unit test file that measures, at 44.1, 48, 96 and 192 kHz: pitch (all
pitched elements), decay to −20 dB, normal and accented Vpp, and checks them against the
targets; plus NaN/Inf parameter fuzzing, random trigger fuzzing (finite output, bounded
level), determinism (same seed, same bytes), no-reset-on-retrigger (a retrigger during a
ring differs from a fresh hit), click-free sleep/wake, block-partition independence, and,
where reference recordings exist (`.deps/ref-samples/`, skip with exit 77 when absent),
comparison of pitch, decay and spectral statistics. Each area writes a measured table
(model vs service manual vs reference recording) into its report for the README.

## 4. Engine, mixer and FX

`Engine` owns the 11 voices, the shared noise source, the ROM pointers, one `ChannelStrip`
per voice, the `FxRack` (two buses), three `TriggerClick`s and the `MasterChain`.
`Machine` owns an `Engine` and a `Sequencer`, merges sequencer hits with MIDI-played hits,
splits each block at event offsets (sample-accurate), and carries the lock-free queues
between the UI and the audio thread. The plugin shell and the CLI renderer both drive a
`Machine`; neither talks to voices, FX or the sequencer directly.

### 4.1 Signal flow

Per voice (mono until the pan stage):

```
voice (volts) × kDigitalPerVolt → drive → strip [Filter/EQ ⇄ Comp, order by compPreEq]
  → [latency alignment delay] → LEVEL → pan + spread → route:
       Output: bus `output` (0 = main; an individual out leaves the main mix, as on the hardware)
       FxBus1 / FxBus2: into that FX bus instead
  + send (sendBus, send, pre/post LEVEL) into an FX bus
```

FX bus b: sum of its inputs → its effect instances in ascending order key → return level →
bus `output`. Bus 1 can also feed bus 2 (`toBus2`, pre or post bus 1's effects).
Trigger outs: a click per hit of the source voice → route like a channel.
Main bus (bus 0) only: `MasterChain` = output stage (`Off` / `Modern` / `Original`) → drive
→ 3-band compressor → limiter (+ soft clip) → volume. Individual outs bypass all of it.

- LEVEL: `ChannelParams::level` 0..1 is the voice's LEVEL pot; 0.5 is the calibration point
  (gain 1), 1.0 is +7 dB (the hardware level stage's ×2.27 maximum), 0 is silence, audio
  taper in between.
- Pan law: −3 dB centre, constant power. Spread: one side delayed by up to 20 ms.
- Mute and solo gate TRIGGERS (in Machine), so a muted voice finishes its tail.
- Smoothing: level, pan, send and FX-mix changes are smoothed (no zipper noise, no clicks).

### 4.2 CPU rules

The budget (dense all-voices pattern, 48 kHz, Standard) is 8 % of one core with no strip
or FX enabled, and the strips/FX must not push a typical FX preset over 12 %. To get there:
inactive voices are skipped (except free-running ones); a strip section that is off costs
nothing; a strip whose voice is silent and whose own state has decayed is skipped; an FX bus
with no input and a decayed tail is skipped (each effect reports `isActive()`); the master
chain at rest is skipped on a silent main bus (the engine zero-fills and calls
`skipSilence(n)`); nothing is oversampled that does not need it. Worst block under 25 % of its deadline, no periodic
spikes (no work that happens every N blocks), zero allocation.

### 4.3 FX DSP classes (mixer-and-FX area)

```cpp
namespace mw909::fx {
class ChannelStrip {            // fx/ChannelStrip.h
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setParams(const StripParams&) noexcept;
    void process(float* mono, int n) noexcept;   // in place
    bool isActive() const noexcept;              // any internal state above the sleep floor
    bool isBypassed() const noexcept;            // both sections off
};
class FxRack {                  // fx/FxBus.h: both buses, all ten instances
    void prepare(double sampleRate, int maxBlockSize);           // allocates (delay lines, reverb)
    void reset() noexcept;
    void setParams(const FxParams&, double bpm) noexcept;
    void noteTrigger(VoiceId v) noexcept;        // VCA-mode dynamics and filter envelopes
    // in[b][c]: bus b's summed input (L, R), processed in place into its wet return
    // (before return level/routing, which the engine applies). key[v]: post-strip mono
    // signal of each voice for compressor sidechains (may be null).
    void process(float* const (&bus)[kNumFxBuses][2], const std::array<const float*, kNumVoices>& key, int n) noexcept;
    bool busActive(int b) const noexcept;
};
class MasterChain {             // fx/MasterChain.h
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParams(const MasterParams&, Revision) noexcept;
    void process(float* left, float* right, int n) noexcept;
    bool isActive() const noexcept;              // false at rest: every glide settled, every
                                                 // filter state exactly 0, no gain reduction,
                                                 // limiter gain exactly 1
    void skipSilence(int n) noexcept;            // = process() on n zeros while !isActive()
};
class TriggerClick {            // fx/TriggerOut.h
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void trigger(const Hit&) noexcept;
    void render(float* out, int n) noexcept;     // mono, overwrites; volts × kDigitalPerVolt not applied
    bool isActive() const noexcept;
};
}
```

Each effect is a separate class (`fx/Distortion.h`, `fx/ParamEq.h`, `fx/Delay.h`,
`fx/Reverb.h`, `fx/Chorus.h`, `fx/Bitcrusher.h`, `fx/Dynamics.h`, `fx/FilterFx.h`) with
`prepare/reset/setParams/process(L, R, n)/isActive/tailSeconds`. Tempo-synced times use the
`bpm` handed to `setParams`.

The master chain's rest is exact, not a sleep floor: at rest, processing zeros writes +0 and
changes no state, so skipping a silent block is byte-identical to running it, for any block
partition. `skipSilence(n)` only moves the control grid, which stays counted in samples
from `reset()`. Decaying states reach exact zero through the control grid's denormal flush
(1e-30), the band detectors' 1e-5 dB snap and the limiter's 1e-9 snap to unity. A silent
block in which only the limiter's gain is still releasing runs just that recursion. A band
compressor's detector keeps tracking while its band is switched out, so the band comes back
without a make-up overshoot. Look-ahead compression is not offered (it would change the
reported latency); the compressors are feed-forward with a fast peak detector.

### 4.4 MIDI (in Machine)

Notes play voices through `kNoteToVoice` (the original machine's receive map, research/04
§5.1), on any channel or on `receiveChannel`. Velocity → `Hit` per `velocityToHit`. Notes on
`bassChannel` play BD chromatically around `kBassRootNote`. HostNote mode: note 24 + i
selects and holds pattern i (24..119). MIDI out sends sequencer hits on `outChannel` with
`kVoiceNote` and velocity weak/normal/accent = 48/96/127. CCs are not interpreted by the core;
the plugin shell maps them (factory CC map + MIDI learn).

## 5. Sequencer semantics

Public API: `seq/Sequencer.h`. Data model: `seq/Pattern.h`. The original's semantics are
the default; the extensions are supersets that leave a hardware pattern unchanged.

### 5.1 Clock

- Position in quarter notes (PPQ). Step length from the pattern's `Scale`; a measure is one
  pass of the pattern (`length` steps).
- `HostSync`: plays while `Transport::playing`; step times are derived from host PPQ on every
  call (never accumulate samples while synced), converting PPQ to samples at the **host's
  own tempo, followed over 1..1000 BPM** (new in mw909's contract; the 20..400 clamp is for
  `tempo()` display and the internal clock only). `Internal`: plays while `Settings::run`;
  PPQ accumulated from `internalBpm` (clamped 20..400), starting at 0 on each start.
  `HostNote`: plays from the top of the held pattern on the internal clock. `Off`: silent.
  When the host supplies no transport, `HostSync` behaves as `Internal`.
- A step fires when its time falls in the half-open sample range `[start, end)`; use
  `floor`, so negative PPQ (pre-roll) works; a start exactly on a boundary fires at offset 0.
- Relocation: if host PPQ differs from the expected continuation by more than
  max(2 samples, 1 ms), taken at any tempo between the previous and current host tempo (a
  tempo ramp across the block is not a relocation), drop pending hits, re-anchor, and do not
  fire skipped steps. Smaller differences are host rounding: absorbed, never firing a hit twice.
- When playing stops, drop pending hits and set `currentStep()` to −1.

### 5.2 Measures, play modes, variations, fill

- In `HostSync`, playback is a pure function of host PPQ: measure index = floor(ppq /
  measureLength), starting mid-pattern when the transport starts mid-measure.
- `PlayMode::Pattern`: loop the selected pattern. A queued pattern (`selectPattern`) takes
  over at the end of the current measure; when stopped, at once; with `immediate`, at once
  keeping the step position.
- `PlayMode::Chain` walks `bank.chain`, `PlayMode::Track` walks `bank.tracks[songTrack]`
  (TRACK PLAY), one entry per measure, looping at the end. In `HostSync`, find the entry by
  walking from PPQ 0 (patterns may differ in length and scale). `songPosition()` reports the
  entry index. An empty chain/track behaves as `Pattern`.
- Variations (extension): `AB` plays A on even measures and B on odd ones, starting on A.
- Fill (extension): the fill pattern is slot `fillSlot` of the playing pattern's group;
  auto fill every N measures replaces measure N−1 of each group of N; `requestFill()` plays
  it for the next measure once. `currentPattern()` reports the fill while it sounds;
  `selectedPattern()` keeps the basic pattern.

### 5.3 Steps

- **Polymeter:** a voice track of length L reads step `(measure·length + stepInMeasure) mod L`.
  The total-accent track follows the pattern. **`PlayState::position` publishes the measure
  count and step together from day one** (Machine.h), so the UI's per-track playheads are
  right (mw808 could not show them).
- **Dynamics:** `Event::hit = Hit::fromCode(base + voiceAccent + totalAccent)` where base is
  `kNormalCode` (Normal, Accent) or `kWeakCode` (Weak), voice accent applies when the step is
  `Accent`, and total accent when the total-accent row is set on that step (strength from
  `Settings::totalAccent`). `Event::level` and `Event::totalAccent` carry the classes for MIDI out.
- **Shuffle** (the original's semantics): the delay of the off-beat steps (0-based odd
  steps) is `t = pattern.shuffleOf(track) × swingScale` ticks of 1/96 beat. 16th scale: delay
  t/96 beat. 32nd scale: `2·floor(t/4)`/96 beat (the hardware's level pairs 1=2, 3=4, 5=6, 7).
  Eighth (extension): 2t/96. Triplet scales: no shuffle. When the pattern length is odd, its
  last step is not delayed. Hardware levels: `shuffleTicksForKey(1..7)` = 0..12.
- **Flam:** a flam step fires two hits: the grace hit at the step time with the step's accent
  line × `flamGrace`/100, then the main hit `flamMs` later (fixed in milliseconds, tempo
  independent). A flam shorter than one sample fires once (the main hit only). With
  sub-steps, only the first sub-hit flams. Hardware: 20..48 ms (`flamMsForKey`), BD/SD/toms;
  the extension allows any voice and 1..100 ms.
- **Sub-steps ×N:** N hits spaced stepLength/N apart; the k-th hit's accent line is scaled by
  `lerp(subStart, subEnd, k/(N−1))/100`.
- **Probability:** a deterministic hash of (pattern, measure, step, track), so a render repeats.
- **Timing jitter (new, optional):** with `Settings::jitter`, every hit of a step is delayed by
  the same deterministic amount in [0, 4 ms) from a hash of (jitterSeed, measure, step), the
  original's interrupt-clock jitter (≈1.2 ms RMS, research/03 §0). Off by default.
- **Hat rule:** CH and OH on the same step play open (the engine's HiHat does it, §3.4).
- Hits after the end of the processed range wait in a fixed-size pending queue that keeps
  their musical time, so the result is the same for any block partition (1 to 8192 samples).

### 5.4 Tap write (recording)

`recordHit(voice, accent, sampleOffset)` quantises to the nearest step (ignoring shuffle) of
the playing pattern and variation and sets it to Normal or Accent (MIDI: velocity ≥
`kAccentVelocityThreshold`). If the nearest step is upcoming, the sequencer must not fire it
again on this pass; this holds for several recorded hits per block and per voice.

### 5.5 State for the UI

`currentStep()`, `currentPattern()`, `selectedPattern()`, `queuedPattern()`,
`currentVariation()`, `isPlaying()`, `measureCount()`, `songPosition()`, `tempo()`.

### 5.6 `seq/PatternOps.h` (message-thread helpers, pure functions)

mw808's set (`clearPattern`, `clearTrack`, `copyTrack`, `shiftTrack`, `shiftPattern`,
`invertTrack`, `copyVariation`, `randomizeTrack`, `randomizePattern` (deterministic for a
seed; hats never get CH and OH on one step), `expandPattern`, `setEuclidean`) plus:
`setScale(p, scale)` (a triplet scale sets the last step to 12, the 16th/32nd scales to 16,
as on the hardware, when the length was the previous scale's default), `insertMeasure` /
`deleteMeasure` / `copyMeasures` on a `SongTrack`, and `applyHardwareRules(p)` (accent only
on BD SD toms CH, flam only on BD SD toms and not with accent, length ≤ 16, hardware shuffle
and flam values) for the "authentic" editing mode.

### 5.7 `seq/MidiFile.h`

`writeMidiFile(pattern, variation, noteMap, options)` writes a format-0 file of one measure
(channel 10 by default, 480 PPQ, velocities 48/96/127, shuffle, flam and sub-steps rendered
as real note positions, optional tempo event). `readMidiFile(bytes, noteMap, pattern&)`
imports and quantises to the pattern scale (velocity ≥ 65 → Accent).

## 6. Machine and threading

- The **message thread** owns the authoritative `PatternBank` (the model the UI edits and
  state save/load reads). The **audio thread** owns the copy inside `Sequencer`.
- UI → audio: a lock-free SPSC queue of POD `Command`s (set pattern, chain or song track by
  value, select pattern, fill, audition, panic). Single edits are stamped with the bank
  generation; edits against an older generation are dropped once a newer whole bank is in
  place.
- Whole-bank handoff: `postBank` exchanges a heap `BankHandoff*` into an atomic; the next
  `process` adopts it (generation++) and pushes the old one to the retired queue, which the
  message thread drains and deletes.
- **User ROMs (new):** `postUserRom` uses the same protocol per ROM slot, so the audio thread
  swaps the pointer it hands to the sample voices without allocating or freeing.
- Audio → UI: a queue of small `Notice`s (step recorded, pattern selection changed) plus the
  `PlayState` atomics (position packed as measure<<8 | step+1, song position, tempo, peaks,
  per-voice hit counters and channel peaks).
- Block processing: drain commands, adopt pending bank/ROMs, run the sequencer and MIDI for
  the block, then render chunk by chunk between event offsets; MIDI out note-offs are kept
  per voice; the `MidiOutBuffer` is fixed-capacity.

## 7. Performance targets (bench tool, this Xeon E5-2699 v4)

- Dense all-voices pattern at 48 kHz Standard, strips and FX off: under 8 % of one core.
- Worst block under 25 % of its deadline; no periodic spikes.
- The block that applies a quality, seed, variance or revision change (the voices are
  re-prepared on the audio thread) is held to the same 25 % at 48 kHz, in 128- and 64-sample
  blocks (90th percentile over many changes, `core_bench`). Above 48 kHz in 64-sample blocks
  with variance above 0 it can exceed it: the three sample voices' step kernels are rebuilt
  within the one block (at variance 0 the cymbals share the hat's kernel and the cost halves).
- Zero allocations in `processBlock` (malloc/calloc/realloc/free and operator new interposed),
  block sizes 1..8192.
- Silent voices cost nothing beyond free-running sources.
