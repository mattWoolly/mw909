<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# mw909 plugin shell and UI

How the JUCE plugin wraps `mw909::Machine` (see `architecture.md` for the core). This is the
contract between the processor/state code (`plugin/src`), the UI (`plugin/ui`) and the tests
(`tests/plugin`, `tests/ui`). It is mw808's shell carried over, with the 909's parameters,
the mixer/FX additions and the lessons from mw808 written in as rules.

## 1. Identity and formats

| | |
|---|---|
| Product name | `mw909` (one CMake variable, `MW_PRODUCT_NAME`) |
| Company | `mwAudio`, manufacturer code `mwAu`, plugin code `mw99` (`mw90` is the name of the mw90 reverb; `mw99` is unused by every sibling project and installed plugin, research/06) |
| Bundle / CLAP id | `com.mwaudio.mw909` |
| LV2 URI | `https://github.com/mattWoolly/mw909` |
| Formats (Linux) | VST3, LV2, Standalone, plus CLAP through clap-juce-extensions |
| Category | `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`, `NEEDS_MIDI_OUTPUT TRUE`, `IS_MIDI_EFFECT FALSE`; VST3 `Instrument Drum`; CLAP `instrument drum-machine stereo` |
| Defines | `JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0` |
| Classes | `mw909Processor`, `mw909Editor`, `mw909LookAndFeel`, `mw909Colours` |

Buses: one stereo main output (enabled) and 15 stereo individual outputs `Out 2` … `Out 16`
(disabled by default; a layout is supported when Main is stereo and every other bus is stereo
or disabled). No audio inputs. Latency: `Machine::latencySamples()` for the prepared sample
rate, set in `prepareToPlay` only (it never changes while prepared). Tail 10 s (reverb).

## 2. Parameters (APVTS, `ParameterID{id, 1}`, ids are frozen once shipped)

`plugin/src/ParameterIDs.h` holds every id and builds the per-voice ids from
`kVoiceInfo[v].id` and the knob ids. Ranges never change at runtime. Bool parameters report
exactly 0 or 1 from `getValue()` (an `AudioParameterBool` subclass that snaps), which is what
clap-validator and pluginval's state test need.

### 2.1 Per voice (`<v>` = bd sd lt mt ht rs cp ch oh cr rd)

Sound: one parameter per `kVoiceInfo[v].knobs[i]`: id `<v>_<knob id>`, name
`<CODE> <knob name>`, range `min..max` (Semitones step 0.01; Choice as an
`AudioParameterChoice` with the `PcmSource` names `ROM`, `Synth`, `User`), default `def`.

Mixer and strip:

| id | name | range | default |
|---|---|---|---|
| `<v>_level` | `<CODE> Level` | 0..1 | 0.5 |
| `<v>_pan` | `<CODE> Pan` | −1..+1 | 0 |
| `<v>_spread` | `<CODE> Spread` | 0..1 | 0 |
| `<v>_ch_drive` | `<CODE> Channel Drive` | 0..1 | 0 (`_ch_` because `bd_drive` is the BD Shaper knob) |
| `<v>_mute` / `<v>_solo` | `<CODE> Mute` / `Solo` | bool | off |
| `<v>_out` | `<CODE> Output` | choice `Main`, `Out 2` … `Out 16` | Main |
| `<v>_route` | `<CODE> Route` | choice `Output`, `FX 1`, `FX 2` | Output |
| `<v>_send_bus` | `<CODE> Send Bus` | choice `Off`, `FX 1`, `FX 2` | Off |
| `<v>_send` | `<CODE> Send` | 0..1 | 0 |
| `<v>_send_pre` | `<CODE> Send Pre` | bool | off |
| `<v>_eq_on` | `<CODE> EQ On` | bool | off |
| `<v>_lowcut` / `<v>_highcut` | `<CODE> Low Cut` / `High Cut` | 20..2000 / 500..20000 Hz, log skew | 20 / 20000 |
| `<v>_reso` | `<CODE> Resonance` | 0..1 | 0 |
| `<v>_eq1_on`, `_eq1_freq`, `_eq1_width`, `_eq1_gain` (and `eq2`) | `<CODE> EQ1 On/Freq/Width/Gain` | bool, 20..20000 Hz log, 0.5..4 oct, −24..+24 dB | off, 200 (eq2: 3000), 1, 0 |
| `<v>_comp_on`, `_comp_pre` | `<CODE> Comp On`, `Comp Pre EQ` | bool | off |
| `<v>_comp_thresh`, `_comp_ratio`, `_comp_attack`, `_comp_release`, `_comp_makeup` | | −48..0 dB, 1..20, 1..500 ms log, 1..500 ms log, 0..24 dB | 0, 4, 10, 100, 0 |

### 2.2 FX

Placement, per instance `<fx>` = `dist`, `eqa`, `eqb`, `delay`, `reverb`, `chorus`,
`crush`, `dyna`, `dynb`, `filter`: `fx_<fx>_on` (bool, off), `fx_<fx>_bus` (choice `FX 1`,
`FX 2`; default: dist, eqa, delay, dyna, filter on FX 1, the rest on FX 2), `fx_<fx>_order`
(int 0..9, default the instance index).

Buses: `fx1_level`, `fx2_level` (−60..+12 dB, 0), `fx1_out`, `fx2_out` (choice Main … Out 16),
`fx1_to2` (0..1, 0), `fx1_to2_pre` (bool).

Effect parameters: one parameter per field of the `fx/FxParams.h` structs, id
`fx_<fx>_<field>` in snake_case (e.g. `fx_reverb_predelay`, `fx_delay_div`,
`fx_dyna_sidechain` as a choice `Off`, `BD` … `RD`), with the ranges and defaults written in
`FxParams.h`. The mapping table lives in `Parameters.cpp` next to the snapshot code, so
adding a field is one line in each.

### 2.3 Master, trigger outs, global

| id | name | range | default |
|---|---|---|---|
| `master_volume` | Master Volume | −60..+12 dB | 0 |
| `output_stage` | Output Stage | choice Off, Modern, Original | Modern |
| `master_drive` | Master Drive | 0..1 | 0 |
| `mcomp_on`, `mcomp_xlow`, `mcomp_xhigh` | Multiband Comp On / Low X / High X | bool, 40..1000 Hz, 1000..12000 Hz | off, 200, 3000 |
| `mcomp_<b>_on/_solo/_thresh/_ratio/_attack/_release/_makeup` (`b` = low, mid, high) | | as `CompBand` | as `CompBand` |
| `limiter`, `lim_thresh`, `lim_attack`, `lim_release`, `soft_clip` | | bool, −24..0 dB, 0.1..10 ms, 10..1000 ms, bool | on, 0, 1, 100, on |
| `trig<n>_on/_source/_route/_out/_pan/_level` (`n` = 1..3) | Trigger n … | as `TriggerOutParams` | off, RS, Output, Main, 0, 0.5 |
| `total_accent` | Total Accent | 0..1 | 0.7 |
| `hat_choke` | Hat Choke | bool | on |
| `quality` | Quality | choice Eco, Standard, High | Standard |
| `variance` | Analog Variance | 0..1 | 0 |
| `revision` | Unit Revision | choice Late, Early | Late |
| `seq_mode` | Sequencer Mode | choice Off, Host Sync, Internal, Host Note | Host Sync |
| `seq_run` | Run | bool | off |
| `tempo` | Tempo | 20..400 BPM, step 0.1 | 120 |
| `swing_scale` | Shuffle Depth | 0..1 | 1 |
| `variation` | Variation | choice A, AB, B | A |
| `auto_fill` | Auto Fill | choice Manual, 16, 12, 8, 4, 2 | Manual |
| `fill_slot` | Fill Pattern | choice 1 … 16 | 16 |
| `play_mode` | Play Mode | choice Pattern, Chain, Track | Pattern |
| `song_track` | Track | choice I-1 … I-4, II-1 … II-4 | I-1 |
| `record` | Tap Write | bool | off |
| `jitter` | Timing Jitter | bool | off |
| `velocity_sens` | Velocity Sensitive | bool | on |
| `midi_channel` | MIDI Channel | choice Omni, 1 … 16 | Omni |
| `bass_channel` | Bass Channel | choice Off, 1 … 16 | Off |
| `midi_out` | MIDI Out | bool | off |
| `midi_out_channel` | MIDI Out Channel | choice 1 … 16 | 10 |

Rules: a parameter listener never writes another parameter. `processBlock` reads the cached
`std::atomic<float>*` of every parameter into one `mw909::MachineParams` snapshot per block.

## 3. Non-parameter state

Owned on the message thread by `PatternModel` (`plugin/src/state/PatternModel.h`):

- the authoritative `seq::PatternBank` (96 patterns, chain, 8 song tracks),
- the selected pattern index,
- the unit seed ("serial number", `uint32`),
- the user ROM images (3 slots, each optional) and their source names,
- the MIDI-learn map (CC → parameter id; the factory CC map is the default, research/04 §5.3),
- UI state: scale factor, active view, selected track, step-edit mode, selected page,
  authentic editing mode.

`PatternModel` API (message thread; a mutex guards it against `getStateInformation` on
another thread; the audio thread never touches it): `pattern(i)`, `setPattern(i, p)`,
`edit(i, fn)`, `setChain`, `setSongTrack(i, t)`, `replaceBank` (via `Machine::postBank`,
generation++), `selectPattern(i, immediate)`, `setUserRom(slot, image, name)` /
`clearUserRom(slot)` (via `Machine::postUserRom`), `addListener`.

Delivery guarantees (carried from mw808):
- `replaceBank` and user ROMs go through the handoff, so the very next audio block uses
  them, even when no UI timer runs (offline bounce right after load).
- Single edits go through the command queue stamped with the bank generation. If the queue
  is full the item is marked dirty and re-posted from the processor's timer; edits are never lost.
- **Staged restores:** a state restore that arrives while an edit is in flight is adopted
  inside the same lock as the edit that would otherwise race it.
- The processor runs a 30 Hz message-thread timer whether or not the editor is open: drains
  `Machine::poll` notices, re-posts dirty items, deletes retired banks and ROM handoffs, and
  applies MIDI-learned CC values (`setValueNotifyingHost`).

### 3.1 Serialised state

`getStateInformation`: a ValueTree `mw909` with `stateVersion = 1`, holding the APVTS state
and a `sequencer` child with each non-empty pattern as a base64 blob of its raw bytes (with
`sizeof(Pattern)` and `patternFormat = seq::kPatternFormat`; blobs that do not match are
refused), the chain, the song tracks, selected pattern, unit seed, user ROMs (base64 of
the 6-bit data, 32 KiB each), MIDI-learn map and UI state.

- **Bit-exact:** `setStateInformation(getStateInformation())` restores exactly, and saving
  twice gives identical bytes (clap-validator 0.4.1 checks this). No timestamps, no
  unordered containers in the output.
- **Guards** (`StateCodec`): validate UTF-8 before `getXmlFromBinary`; cap total size at
  2 MiB and XML depth at 16; run the migration chain for older `stateVersion`s; on unknown
  future versions or corrupt data fall back to defaults; never crash.

## 4. Presets

Three kinds, one file format (ValueTree XML, extension `.mw909`), `kind` attribute:

- **kit**: per-voice sound, mixer and strip parameters, FX, trigger outs, total accent and
  master section (not the sequencer parameters, not the user ROMs unless a voice uses `User`),
- **patterns**: the pattern memory (96 patterns, chain, tracks),
- **full**: everything in the plugin state.

Factory content is compiled in (`mw909/Factory.h`). User presets live in
`~/.config/mwAudio/mw909/Presets/{Kits,Patterns,Full}/` (override with the
`MW909_PRESET_DIR` environment variable; the tests always set it to a scratch dir).
`PresetManager` lists factory + user presets per kind, loads, saves, deletes user presets,
and steps next/previous. Host programs expose the factory full presets. Loading a kit sets
parameters with `setValueNotifyingHost` inside begin/end gestures.

## 5. MIDI

- Notes → voices via `mw909::kNoteToVoice`; velocity → `velocityToHit`; `bass_channel` plays
  BD chromatically; Host Note mode: notes 24..119 select and hold patterns 1..96.
- **Factory CC map** (research/04 §5.3): CC 9 shuffle depth, 20–24 BD tune/attack/channel drive/
  decay/level, 25–29 SD tune/snappy/comp on/tone/level, 46–60 LT/MT/HT/RS/CP tune/decay/level,
  61–63 CH, 71 total accent, 80–82 OH, 83–85 CR, 86–88 RD. MIDI learn overrides per CC.
  The audio thread forwards CCs through a lock-free FIFO and never calls into APVTS.
- MIDI out (when `midi_out`): sequencer hits from `Machine`. The `MidiBuffer` used for
  output is reserved for the worst case once in `prepareToPlay` (the standalone wrapper
  reserves nothing).
- Drag-out: a pattern as a `.mid` file; drop-in: a `.mid` onto the grid imports it.

## 6. Processor API used by the editor

```cpp
juce::AudioProcessorValueTreeState& apvts();
PatternModel& patterns();
PresetManager& presets();
const mw909::PlayState& playState() const;
void audition(mw909::VoiceId, bool accent);
void requestFill();
void panic();
MidiLearn& midiLearn();
juce::File exportPatternToMidiFile(int patternIndex, int variation);
bool importMidiFile(const juce::File&, int patternIndex);
bool importUserRom(const juce::File& audioFile, mw909::RomRegion region);   // decodes, romFromAudio, posts
void clearUserRom(mw909::RomSlot slot);
```

## 7. Editor

Vector-drawn (JUCE `Graphics`; no images, OpenGL or WebView), static layers cached to an
image per scale factor, only changed regions repainted. Base size 1280 × 760, fixed aspect,
resizable 0.6×–2.0× with the scale stored in state. Follows the mwAudio style guide:
lowercase `mw` wordmark, `mwAudio` mark, a `mw909Colours` palette table.

**Family resemblance with mw808, its own identity.** Same layout grammar (header, view
switch, an always-visible sequencer strip), same interaction model, same wordmark style.
Different skin: a **light warm-grey panel with orange accents** (dark section headers with
orange legends, darker grey knob caps with orange pointers, orange step LEDs), where mw808 is
charcoal with a warm red/orange/yellow/white step run. It should read at a glance as "that
grey drum machine" without copying any maker's logo, lettering or panel artwork, and it must
not look like a reskin of mw808 or of the commercial emulations (research/04 §2.12).

Layout (top to bottom):

1. **Header**: wordmark, preset browser (kind, previous/next, name, save, menu), view switch
   **PANEL / GRID / MIXER / FX**, tempo readout, output meter, settings (quality, variance,
   serial number, unit revision, UI scale, MIDI options, about).
2. **PANEL view**: the 11 voices in hardware order, each with its hardware knobs (skirted
   caps) on top and its extended knobs (flat caps) below, LEVEL, a trigger LED from
   `playState().hits[v]`, mute and solo, and a name plate that auditions the voice and
   selects its track. CH and OH sit under one "HI HAT" block with the choke switch; the
   sample voices show their Source selector (ROM / Synth / User; User has an import button)
   and their Color/Density knobs. A global block holds TOTAL ACCENT, master volume, tempo and
   sequencer mode.
3. **Sequencer strip** (always visible): START/STOP, the 16 step keys with LEDs showing the
   selected track of the selected pattern (running step highlighted), page selector 1–4,
   track selector (11 voices + total accent), BANK I/II, GROUP 1–3 and pattern 1–16 keys
   (queued pattern blinks), last step, scale, shuffle (hardware keys 1–7 plus fine), flam
   interval, step-edit mode (LEVEL / FLAM / SUB / PROB), variation A / AB / B, fill, play
   mode (PATTERN / CHAIN / TRACK) with a chain/track editor, tap write, tools (copy, paste,
   clear, shift, random with seed prev/next, euclid, authentic), and a drag handle that drags
   the pattern out as MIDI. Step keys: click cycles Off → Normal → Accent → Off (shift-click:
   Weak), click-drag paints; in FLAM / SUB / PROB mode a click edits that property.
4. **GRID view**: all 12 lanes × the pattern's steps, playhead per lane computed from
   `PlayState::position` and the lane's own length (polymeter), same click semantics.
5. **MIXER view**: one channel per voice: output and route, send bus and amount, pan,
   spread, drive, the strip (EQ on, low/high cut, resonance, two bands, comp on/pre and its
   five controls, as a compact column that expands on click), mute, solo, level fader with a
   meter (`playState().channelPeak[v]`).
6. **FX view**: the two FX buses as slot rows showing each bus's effects in order (drag to
   reorder or move between buses), the selected effect's full controls, bus return level,
   output and bus-1-to-bus-2 send; the master chain (output stage, drive, 3-band compressor,
   limiter, volume); the three trigger outs.

Every knob: vertical drag, shift-drag fine, double-click resets, wheel adjusts, text entry
on double-click of the value readout, right-click menu with MIDI learn / forget. Every
control is attached through APVTS attachments; pattern editing goes through `PatternModel`.
The editor polls `playState()` on a 30 Hz timer and repaints only what changed.

### 7.1 Editor rules from mw808 (binding)

- Never use `GenericAudioProcessorEditor` or `AudioProcessor::addListener` in any editor:
  JUCE 8.0.13 reads that listener list without a lock (heap use-after-free when the editor
  opens or closes during automation). APVTS attachments only.
- When the editor closes: dismiss popup menus and dialogs, and end any knob gesture still in
  progress.
- Headless tools under Xvfb install their own X error handler.

## 8. Tests (shell)

`tests/plugin`: parameters (ids, ranges, bool 0/1), processing (all block sizes 1..8192,
buses on/off, sample rates, MIDI in/out), state (round trip bit-exact, save twice identical,
corrupt/huge/deep/non-UTF-8 input, old versions), patterns (model ↔ machine delivery,
staged restore), presets, MIDI learn, alloc spy (malloc/calloc/realloc/free and operator new
interposed, zero in `processBlock`), a 30 s multi-threaded stress test (automation, state
save/load, preset loads, editor-less UI commands, all at once), bench.
