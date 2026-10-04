<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# mw909

mw909 is an emulation of the classic 1983 hybrid drum machine: seven analog voices modelled from the voicing-board schematic, four cymbal and hi-hat voices played from 6-bit sample ROMs through the original's analog chain, and the machine's 96-pattern sequencer with its shuffle, flam and track (song) modes. It builds as a Linux-native VST3, CLAP, LV2 and standalone instrument. The bass drum is a triangle oscillator whose integrator is shorted for the 2 ms trigger, swept from 302 Hz down to 49.5 Hz and shaped by an anti-parallel diode pair; the crash is a 32,768-word ROM read by a counter on an RC clock, a 6-bit DAC, and a 64-step envelope driven by the ROM's own address lines, so turning TUNE stretches the decay along with the pitch.

It installs to `~/.vst3`, `~/.clap` and `~/.lv2` (the standalone to `~/.local/bin/mw909`). pluginval 1.0.4 passes at strictness 10 (three separate randomised runs, and again on the installed bundle), clap-validator 0.4.1 reports 0 failures, and the LV2 bundle passes 11 structural checks.

## The voices

Eleven voices in panel order: bass drum, snare, low, mid and high tom, rim shot and hand clap are analog; closed hat, open hat, crash and ride are 6-bit samples. Each voice keeps the original's panel knobs (TUNE, DECAY, ATTACK, TONE, SNAPPY and LEVEL, wherever the hardware had them) and adds extended ones on flat caps underneath: Pitch, Tune Depth and Shaper on the bass drum, Tune Depth and Tune Decay on the toms, Reverb on the clap, Tune, Source, Color and Density on the sample voices, and so on. Every extended knob is centred on the stock circuit, so 0 semitones and 0.5 are the unmodified machine.

The analog voices are simulated in volts, part by part, and every value that is not read off the schematic is labelled FITTED in the comment block at the top of its header in `libs/mw909-core/include/mw909/voices/`, with what it was fitted to. Some of those fits are stand-ins: the rim shot's input kick is 0.5 V where the parts give 2.2 V, because 2.2 V keeps the diode clipper saturated about 10 ms too long against the recordings, and the header says so.

The sample voices model the whole chain after the ROM: counter clocked by a gated RC oscillator (TUNE is the clock rate), a 6-bit zero-order-hold DAC with per-unit bit-weight error, an anti-log VCA, and the two Sallen-Key reconstruction low-passes at 25.9 and 27.7 kHz. Those filters don't remove the first image band, and on the hardware that band is part of the sound, so the step edges are inserted band-limited (BLEP) at their exact times and the image content up to the host's Nyquist is kept, with fold-back held below −90 dB.

What the circuits do that matters for playing them:

- **Accent is a held voltage.** The original fires a fixed 5 V, 2 ms trigger and carries dynamics separately, as a 0–5 V level from a 6-bit accent DAC. mw909 does the same: a normal hit is code 24, a weak step 12, per-instrument accent adds 16, and TOTAL ACCENT adds up to 39 at full knob, clamped at 63. The analog voices sample the line while the trigger is open and feed it where the schematic does (the bass drum's click and VCA envelope, the snare's tone VCA and snappy, the tom envelopes), so an accented hit changes shape as well as level. The hats and cymbals read it continuously at their VCA control node. Rim shot and clap share one DAC and crash and ride share another, so a crash still ringing jumps in level when the ride fires with a different accent, as it does on the hardware.
- **Retriggering never resets a voice.** A new hit lands on whatever is still ringing: envelope capacitors, filters and noise keep their state. What the circuit itself resets on a trigger (the analog oscillators' integrators, the ROM counters) is modelled as that circuit, not as a software reset.
- **One hi-hat circuit.** Closed and open hat are one counter, one ROM (open hat in the first 24,576 words, closed in the last 8,192), one envelope capacitor and one VCA. A closed hit restarts the counter in the closed region and recharges the envelope, and that is the choke. The two DECAY knobs are parallel paths into the same capacitor, so the closed hat's decay moves slightly with the open hat's knob. Closed and open on the same step play open. Switching Hat Choke off gives two independent copies of the circuit.
- **Unit revision.** A switch between late and early units applies the parts the service notes list as changed: bass drum C9 0.22 µF (a shorter TUNE range), rim shot R417 12 k, hat C134 10 µF, tom-noise C54 2.2 nF and R194 47 k, the trigger-out jack without C600, and the individual outputs without their 1 k / 0.01 µF RC.
- **Analog Variance** spreads component values within their real tolerances (±20 % capacitors, ±5 % resistors, ±8 % on the ROM clocks, ±10 % on the hat's envelope capacitor, small DAC bit-weight errors) from a stored serial number, so a kit can sound like one particular unit and keep sounding like it. At 0 every part is nominal.
- Snare, toms, bass drum and clap draw on one shared noise source, defined by spectral density so each voice's own filters see the same in-band level at every sample rate.

Pitch, decay and level hold within ±3 % (decay ±5 %) from 44.1 to 192 kHz; the voice tests measure each one at 44.1, 48, 96 and 192 kHz.

### Measured against recordings

The service notes give one printed level for the analog voices: the clap's trimmer target, 2 Vpp at its individual output with LEVEL centred. Everything else on those pages is hand-drawn scope traces with no graticule and no common scale, and there is no decay table at all. So the analog levels are calibrated to that 2 Vpp clap and to each voice's measured level relative to the clap on a free sample set recorded from one original unit, used only in local tests (reference set A in the table). Pitch and decay were fitted to the same set. The sample voices do have printed check figures (closed hat 0.96, open hat 1.2, crash 2.3, ride 2.2 Vpp) and are calibrated to those, on the full-band output; at 44.1 kHz the image band above Nyquist is gone, so their raw Vpp reads 9–18 % lower while the level below 16 kHz is the same at every rate.

These numbers come from a separate measurement pass with its own analysis code, run on the same recordings and on renders from `mw909-render` (stock knobs, LEVEL 0.5, Standard quality, variance 0, late revision, 44.1 kHz; the median of 8 hits, 32 for snare and clap because their noise makes every hit different). The accented Vpp is the full accent, code 63. The level column was compared at 96 kHz on both sides so the bandwidth matches. "Unit 2" is a second, older set from another unit, used for pitch and decay only.

| Voice | Pitch, model | Pitch, set A | −20 dB, model (ms) | −20 dB, set A (ms) | Vpp normal / accent, model (V) | Level re clap, model / set A (dB) |
|---|---|---|---|---|---|---|
| Bass drum | 49.5 Hz, sweep from 302 Hz | 49.6 Hz, from 288 Hz | 171 | 157 | 1.03 / 2.27 | −5.4 / −5.4 |
| Snare | 174.0 + 261.8 Hz | 173.4 + 260.0 Hz | 95 | 98 | 1.52 / 2.78 | −1.9 / −1.8 |
| Low tom | 91.6 Hz settled | 89.6 Hz | 243 | 243 | 0.95 / 2.20 | −6.2 / −6.0 |
| Mid tom | 111.6 Hz settled | 111.9 Hz | 164 | 167 | 1.05 / 2.56 | −5.3 / −5.1 |
| High tom | 130.9 Hz settled | 131.5 Hz | 187 | 163 | 1.06 / 2.49 | −5.3 / −5.0 |
| Rim shot | 222 / 479 / 1017 Hz | 222 / 481 / 1019 Hz | 27 | 26 | 0.98 / 1.55 | −5.9 / −5.8 |
| Hand clap | band-pass 930 Hz; bursts at 0 / 10.0 / 21.9 / 31.3 ms | 918 Hz; 0 / 9.9 / 21.5 / 31.5 ms | 73 | 92 | 1.96 / 3.82 | 0 / 0 |
| Closed hat | ROM clock 31.90 kHz | 31.71 kHz | 69 | 78 | 0.84 / 1.84 | −7.1 / −3.8 |
| Open hat | ROM clock 31.90 kHz | 31.71 kHz | 420 (658 at DECAY 100) | 263 at DECAY 100 | 0.98 / 2.23 | −5.5 / −8.9 |
| Crash | ROM clock 37.72 kHz | 36.89 kHz | 644 | 633 | 1.71 / 4.24 | −0.1 / −12.9 |
| Ride | ROM clock 37.73 kHz | 37.70 kHz | 563 | 571 | 1.68 / 4.07 | −0.8 / −16.8 |

Snare, the mid and high tom and the rim shot are within 0.5 % of set A on pitch; the low tom is 2.2 % sharp (+38 cents). Every analog voice is within 0.3 dB of set A relative to the clap, and the clap's normal hit is 0.2 dB under its 2 Vpp target. The snare's second oscillator sits at 1.50× the first in both. TOTAL ACCENT from 0 to 100 % matches the measured span within 0.1 dB on the toms, rim shot, hats and cymbals; the bass drum is 0.7 dB over and the snare 0.4 dB. Pitch moves by at most 3 cents between 44.1 and 192 kHz on every voice except the clap, whose broad band-pass peak scatters by 50 cents even averaged over 32 hits.

The rows that don't agree are real disagreements, listed under [What it doesn't do yet](#what-it-doesnt-do-yet). The biggest is the cymbals: set A's crash and ride sit 13 and 17 dB below the clap, the service manual puts them about 1 dB above it, and the model can only follow one. It follows the manual.

## The sample ROMs

The original's ROM data belongs to its maker, so none of it is in mw909, and nothing was fitted to it sample by sample. There are three sources instead, chosen per voice with the Source selector:

- **ROM**, the factory images: three 32,768-word, 6-bit images (hats, crash, ride) built by `tools/rombuild` from four CC0 recordings of real cymbals in the Versilian Community Sample Library. Each recording is trimmed to mono with no gain change, given a voicing EQ that moves its band balance towards the published spectral statistics of the original's output (statistics only, never waveforms), envelope-flattened, normalised and quantised to 6 bits without dither, the way the original's sample data was prepared. The crash and ride are the same suspended cymbal struck mezzo-forte and mezzo-piano; the original also used one cymbal for both. `assets/rom/PROVENANCE.md` has the library commit, every file's checksum and the exact processing.
- **Synth**, a synthetic metal source computed at the ROM clock: inharmonic partials, filtered noise and their ring-modulated product, flattened like ROM data so the analog VCA puts the decay back. Color sets the spectral tilt and noise corner, Density the number and spacing of partials. It is seeded and restarts on every hit, so a hit is a pure function of its settings.
- **User**: import up to 10 seconds of any audio file into the open hat, closed hat, crash or ride region. It goes through the same flattening and 6-bit quantisation, is saved with the plugin state, and plays through the same chain.

## The sequencer

The original's memory layout: two banks (I and II) of three groups of 16 patterns, 96 in all, each pattern up to 16 steps on the hardware. Four step scales, in the order the SCALE key cycles them: 1/16, 1/32, 1/8 triplet and 1/16 triplet. A 1/8 scale is added as an extension. Switching to a triplet scale sets the last step to 12, as the hardware does.

Shuffle uses the hardware's semantics. Keys 1–7 delay the off-beat steps by 0, 2, 4 … 12 ticks of 1/96 beat in the 1/16 scale; in the 1/32 scale the levels pair up (1=2, 3=4, 5=6, 7); triplet scales don't shuffle; a pattern of odd length leaves its last step alone. Shuffle Depth scales all of it, for anything between the hardware's levels. Flam fires a grace hit at the step time and the main hit a fixed number of milliseconds later, independent of tempo: 20 to 48 ms in 4 ms steps on the hardware's eight keys, with the grace hit's level adjustable.

Accents come in the original's two kinds: the per-instrument accent (bass drum, snare, toms and closed hat on the hardware) and the TOTAL ACCENT row, which accents every voice on that step by the TOTAL ACCENT knob. Weak steps are added below normal. Tap write records hits played over MIDI into the running pattern, quantised to the nearest step (shuffle ignored), as accent when the velocity is 65 or more; a step it fills ahead of the playhead does not fire twice on the same pass.

Patterns play singly, as a live chain of up to 64, or as one of eight tracks (four per bank, up to 256 measures each). In Host Sync, a chain or track position is computed by walking from the start of the song, so starting the transport mid-song lands in the right pattern. Timing is computed from the host's position on every block rather than by counting samples, and a hit queued past the end of a block keeps its musical time, so the output is identical for any block size from 1 to 8192 samples.

Extensions, all of them supersets that leave a hardware pattern unchanged: 64 steps (four pages), A and B variations played A, AB or B, auto fill every 2, 4, 8, 12 or 16 measures from a chosen slot of the pattern's group (or a manual fill), per-track loop length for polymeter and per-track shuffle, ×2/×3/×4 sub-steps with a level ramp, probability in eighths, accent and flam on any voice with flam from 1 to 100 ms, and optional timing jitter (up to 4 ms, deterministic) after the original's interrupt clock. An Authentic editing mode keeps new edits to what the hardware could store. The tools are copy, paste, clear, shift, randomise (seeded) and Euclidean fill.

Sequencer modes: **Host Sync** follows the DAW transport (and runs on its own clock when the host has no transport, as in the standalone), **Internal** runs free at 20–400 BPM, **Host Note** selects and holds pattern N while MIDI note 23+N is down (notes 24–119 for patterns 1–96), and **Off** leaves the voices to MIDI.

## MIDI

Notes play voices on any channel (or one chosen channel) with the original's receive map plus General-MIDI aliases that never collide with it: BD 35/36, RS 37, SD 38/40, CP 39, LT 41/43, CH 42/44, MT 45/47, OH 46, HT 48/50, CR 49/52/55/57, RD 51/53/59.

Velocity 96 is a normal hit and 127 a full accent (per-instrument plus TOTAL ACCENT at the current knob). From 1 to 96 the accent code scales up to normal, so 48 is about a weak step; from 97 to 127 it moves to the full accent. With Velocity Sensitive off, three fixed bands give weak, normal and accent. Set a Bass Channel and notes on that channel play the bass drum chromatically, note 36 at the stock pitch.

The factory CC map: CC 9 shuffle depth; 20–24 bass drum tune, attack, channel drive, decay and level; 25–29 snare tune, snappy, compressor on, tone and level; then tune/decay/level triples at 46 (low tom), 49 (mid tom), 52 (high tom), 55 (rim shot), 58 (clap), 61 (closed hat), 80 (open hat), 83 (crash) and 86 (ride); CC 71 TOTAL ACCENT. Any parameter can be MIDI-learned from its right-click menu, which overrides the factory entry for that CC. The audio thread hands CCs to the message thread through a lock-free FIFO and never touches the parameter tree.

MIDI Out sends the sequencer's hits on channel 10 (configurable), at velocity 48, 96 or 127 for weak, normal and accent. A pattern drags out of the sequencer strip as a standard MIDI file, with shuffle, flam and sub-steps written as real note positions, and a `.mid` dropped onto it is imported and quantised to the pattern's scale.

## Mixer, effects and outputs

Every voice has a channel: level, pan, stereo spread (one side delayed up to 20 ms), channel drive, mute, solo, and a strip with low and high cut (with resonance), two parametric EQ bands and a compressor that can sit before or after the EQ. A channel goes to the main mix, to any of the 15 stereo individual outputs, or into one of two FX buses, and can also send pre- or post-fader to an FX bus. A voice on its own output leaves the main mix, as on the hardware. Mute and solo gate triggers, so a muted voice finishes its tail.

The two FX buses hold ten effects, each placed on either bus in any order: distortion, two parametric EQs, delay, reverb, chorus, bitcrusher, two dynamics processors (with a sidechain from any voice) and a filter. Bus 1 can feed bus 2, and each bus returns to the main mix or any output.

Three trigger outputs model the click of the original's TRIG OUT jack, fired by any voice's hits and routed like a channel.

The main mix alone runs through the master chain: output stage (Off; Modern, the summing op-amp's saturation; or Original, the hardware's main output with its ~130 Hz coupling high-pass and, on late units, a ~6.6 kHz low-pass), master drive, a 3-band compressor, a limiter with soft clip, and volume. Individual outputs bypass all of it. That makes 16 stereo outputs in total: main plus Out 2 to Out 16.

## Presets

25 kits, 9 pattern memories and 60 full presets, all compiled in. Eight of the memories hold 205 named patterns between them (Classic Memory, House Sessions, Techno Floor, Acid & Electro, Shuffle & Breaks, Hard & Fast, Trance & Disco, Rhythm Lab); the ninth is empty. Kits, pattern memories and full states save separately as `.mw909` files under `~/.config/mwAudio/mw909/Presets/{Kits,Patterns,Full}/` (the `MW909_PRESET_DIR` environment variable moves them). A new instance opens on the Init preset: the stock kit playing the first pattern of the Classic memory at 120 BPM.

## The interface

Four views over one always-visible sequencer strip. PANEL has the 11 voices in hardware order, hardware knobs on skirted caps above and extended knobs on flat caps below, with the hats under one HI HAT block with the choke switch and the sample voices' Source selector and import button. GRID shows all 12 lanes (11 voices plus TOTAL ACCENT) for the whole pattern, each with its own playhead when its loop length differs. MIXER has the channels, routing, strips and faders with meters. FX holds the two buses' slot rows (drag to reorder or move between buses), the master chain and the trigger outs. The panel is light warm grey with orange accents.

The editor is vector-drawn, opens at 1280 × 760, resizes from 0.6× to 2.0× at a fixed aspect and remembers its size. It polls the processor at 30 Hz and repaints only what changed; a UI test holds the idle editor under 5 % of a core.

## Performance

Release build on this machine (Xeon E5-2699 v4), 48 kHz, Standard quality, 128-sample blocks, pinned to one core. A dense pattern with every voice busy and no strips or effects takes 1.35 µs per sample, 6.48 % of one core, against a budget of 8 %. With a typical effects preset it is 10.31 % (budget 12 %). The slowest block took 17.91 % of its real-time deadline (limit 25 %), a silent engine costs 0.08 %, and the periodic-spike check found no work recurring every N blocks (ratios 1.01 and 1.04 against 1.5). In that same run the effects scenario's 99.9th percentile block read 34.3 % of the deadline, with a DAW running on the machine at the time.

Changing quality, serial number, variance or revision re-prepares the voices on the audio thread. That block's 90th percentile is at most 10.55 % of the deadline in 128-sample blocks and 19.50 % in 64-sample blocks at 48 kHz. The audio thread does no allocation, locking or file access; tests interpose `malloc`, `calloc`, `realloc`, `free` and `operator new` and require zero calls during processing at block sizes 1 to 8192.

The CLI prints the same report for any machine:

```
build/linux-release/tools/mw909-render/mw909-render bench
```

## Building

Requires g++ 13 or newer, CMake 3.25+, Ninja, and the usual JUCE Linux dev packages (ALSA, JACK, FreeType, X11, Xrandr, Xinerama, Xcursor). Dependencies (JUCE 8.0.13, clap-juce-extensions at a pinned commit, Catch2 3.7.1) come from `.deps/` when present and are fetched at those versions otherwise.

```
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release -j 16
tools/install-user.sh build/linux-release          # --dry-run to preview, --uninstall to remove
```

That build has 0 warnings in project code, and that test run is 547 tests: per-voice unit tests (pitch, decay and level at four sample rates, parameter and trigger fuzzing, determinism, retrigger, click-free sleep, and comparison with the reference recordings where present), sequencer timing oracles, the mixer and effects, 73 bit-exact golden renders, plugin state round trips, a 30-second multi-threaded stress test, 7 UI tests that drive the real editor under Xvfb, the licence-header and branding audits, and the three format validators. The validators download pinned, checksummed releases on first use and skip (exit 77) when offline; tests that need the reference recordings skip the same way when they are absent.

Other presets: `linux-default` (RelWithDebInfo), `asan` and `tsan` (core, tools and tests), `asan-plugin` and `tsan-plugin` (the plugin too). The test presets `golden`, `hygiene` and `validators` run one label each. `asan-plugin` and `tsan-plugin` run everything but the validators under the sanitizers and pass 543 of 543, the core suites under `asan` and `tsan` pass 461 of 461, all with no sanitizer reports; `asan-plugin-dod` and `tsan-plugin-dod` are the quicker 74-test subset (the plugin tests and the stress test).

Golden renders only change on purpose: `BLESS_REASON="why" tools/bless_goldens.sh build/linux-release`. The factory ROM images are regenerated from the CC0 sources with:

```
cmake --build build/linux-release --target mw909-rombuild
build/linux-release/tools/rombuild/mw909-rombuild assets/rom/sources libs/mw909-core/src/rom/RomData.cpp
```

`mw909-render` renders voices and patterns to WAV without a host, exports and imports patterns as MIDI files, compares WAVs and lists the factory content (`mw909-render help`).

## Layout

```
libs/mw909-core/   JUCE-free C++20 core: voices, sequencer, engine, effects, factory content
plugin/            JUCE shell (src/) and editor (ui/)
tools/             mw909-render, rombuild, golden blessing, validators, install
assets/rom/        the CC0 source recordings and PROVENANCE.md
tests/             unit, golden, plugin, ui
scripts/           branding audit and its token list
docs/design/       architecture.md (core contracts), plugin.md (shell and UI contracts)
research/          the circuit and product research behind the numbers above
```

mw909 started from its sibling, mw808. The build plumbing (presets, dependency fetching, licence and branding checks), the sequencer's timing core, the plugin shell with its state codec, preset manager and MIDI learn, the render tool, validator scripts, install script and golden blessing, and the test harnesses (allocation spy, stress test, UI tool) were copied from mw808 and adapted. There is no shared library: each project builds on its own, and a fix in one is not a fix in the other.

## What it doesn't do yet

From the measurement pass:

- **Cymbal level.** The crash and ride follow the service manual's 2.3 and 2.2 Vpp, which puts them 14–18 dB louder relative to the clap than on set A. The hats split the other way: the closed hat is 3.3 dB quieter and the open hat 3.4 dB louder than set A.
- **Open-hat decay** is about twice the recordings'. At stock DECAY the model reaches −20 dB at 420 ms where unit 2 measures 222–241 ms near centre; at DECAY 100 it is 658 ms against set A's 263. Only the manual's drawing (about 490 ms) is close.
- **Clap envelope.** Bursts 2–4 peak 4.5 to 7.2 dB above the first, against about 0 dB on set A, so the clap is back-loaded and reaches −20 dB at 73 ms instead of 92. Its TOTAL ACCENT span is +6.0 dB against +5.2 dB (10 ms RMS) on set A. The burst generator's comparator wiring is fitted, not derived.
- **Per-instrument accent is too strong.** On its own it adds 2.8 to 5.1 dB in the model, while set A's accent series starts at its no-accent level and climbs 6–8 dB on TOTAL ACCENT alone. The measured accent curves are flat over the first 10 % of the knob; the model rises from the first step, so at 50 % it leads by 0.8–1.6 dB on the bass drum, snare and toms.
- **Equal Vpp, unequal loudness.** Peak-to-peak levels match set A, but in 10 ms RMS relative to the bass drum the rim shot is 4.9 dB quieter than set A, the toms 2.2–3.2 dB and the snare 1.7 dB. The crest factors differ.
- **Bass drum** −20 dB at 171 ms against 157 (+9 %), sweep starting at 302 Hz against 288. **High tom** −20 dB at 187 ms against 163 (+14 %). **Closed hat** 69 ms against 78 (−11 %), and its decay doesn't change with accent where the manual's drawing shows it shortening. **Crash clock** 37.72 kHz against 36.89 (+39 cents).
- **High-rate tilt.** The sample voices' and the rim shot's 8–16 kHz band is 0.9–1.6 dB lower at 96 and 192 kHz than at 44.1 kHz, which moves the ride's −20 dB point from 563 to 594 ms.

From the voice models:

- The recordings' analog voices are negative-peak dominant (bass drum, snare, toms, rim shot); the models are near-symmetric, and the bass drum's sixth harmonic is 30 dB low. The mechanism isn't identified.
- The snare's first oscillator dips 2–3 % under its settled pitch at 15–40 ms on the reference unit; the model's bend relaxes monotonically.
- The rim shot's late low-frequency tail (a 59 Hz line about −39 dB down at 30–70 ms) isn't modelled.
- The hardware has one tom-noise circuit shared by the three toms; here each tom has its own, which only differs when two toms ring within about 25 ms.
- The late units' individual-output RC (a 15.9 kHz pole) is modelled once in the engine for every channel, not in the voices, so a voice measured on its own (the unit tests) is measured without it.

Elsewhere:

- Look-ahead compression isn't offered, because it would change the reported latency. The compressors are feed-forward with a fast peak detector.
- MIDI Out drops note-ons beyond 4,096 messages per block.
- The block that applies a quality, serial, variance or revision change can exceed 25 % of its deadline above 48 kHz in 64-sample blocks with variance above 0 (the three sample voices' step kernels are rebuilt in that one block).
- Quality (Eco, Standard, High) only changes the sample voices' anti-alias kernel, and Eco and Standard are identical at 88.2 kHz and above. The analog voices and the effects sound the same at every setting.
- LV2 is checked structurally only (bundle, Turtle, exported descriptor, a load); lv2lint and sord_validate aren't installed here.
- clap-validator's one warning, `state-invalid-random`: JUCE's state loader returns nothing, so random bytes "load". mw909 falls back to defaults on data it can't parse, but the host isn't told.
- No import of the original machine's SysEx pattern dumps, no per-step parameter locks, and patterns drag out as MIDI only, not audio.
- The sanitizer trees relax three checks that would otherwise measure the instrumentation: the multi-threaded state test may run past its 1.5 s (up to 30 s) to reach its 11 message-thread rounds, the editor open/close test does 40 cycles instead of 200, and the idle-editor CPU limit is 20 % of a core instead of 5 % (under TSan an idle view costs about 6 % on its own). TSan suppresses races inside the system's PulseAudio client library (`tools/sanitizers/tsan-suppressions.txt`), and the branding audit skips binary DWARF sections, which only the Debug trees carry. Release runs every check at full strength.

## Licence

AGPL-3.0-or-later (JUCE 8 is AGPLv3 or commercial). Copyright © 2026 mwAudio. The factory ROM images derive from CC0 1.0 recordings in the Versilian Community Sample Library; the source files, their checksums and the library commit are in `assets/rom/PROVENANCE.md`. The reference recordings the tests and the fidelity table compare against are free sample sets of original units, not CC0 and not redistributable; they live in `.deps/ref-samples/` for local tests only and are not part of the plugin or this source tree.
