<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# 04 — TR-909 Emulation Product Landscape

Purpose: define the feature **superset** mw909 must implement. The anchor is D16 Group Drumazon 2, the sibling of the owner's reference 808 plugin (Nepheton 2). It is cross-checked against Roland's own TR-909 plugin and hardware (TR-09, TR-8S), Behringer's RD-9, AudioRealism ADM and the rest of the 909 field, including open-source projects. Facts are cited inline by short tag; full URLs are in **Sources** at the bottom.

Tags: **CONFIRMED** = primary source (manual, service notes, MIDI chart, vendor page) or two independent good sources agree. **UNVERIFIED** = one secondary source, or inferred. **CONFLICTING** = sources (or one source with itself) disagree; both values given. **[DERIVED]** = my own arithmetic, formula shown. Local copies of primary PDFs are in `.deps/sources/` (local only, never shipped).

---

## 0. Design-critical summary

- **The original's sequencer data model is small and fully known** (CONFIRMED, [TR909OM p.25–26, p.39] + [AR-SYX]):
  - **Per step, per voice:** BD, SD, LT, MT, HT each have a 2-bit state {Off, Normal, Accent, Flam}. Flam and accent are therefore mutually exclusive on the hardware. HH has {Off, Closed normal, Closed accent, Open}. RS, CP, CR and RD are on/off only. Plus a 1-bit **Total Accent** row.
  - **Per pattern:** last step 1–16, scale 0–3, chain bit, **shuffle 1–7** and **flam interval 1–8**. Shuffle and flam are *pattern* properties on the 909, not global ones.
  - **Memory:** 2 banks × 3 pattern groups × 16 = **96 patterns**. Each bank also holds **4 tracks** (songs) sharing 896 measures.
  - **Per-instrument accent** ("Accent B") exists only for **BD, SD, LT, MT, HT and closed HH**. **Flam** exists only for **BD, SD, LT, MT, HT**. [TR909OM p.5, p.25–26]
- **Original panel knobs:** BD Tune/Level/Attack/Decay; SD Tune/Level/Tone/Snappy; LT/MT/HT Tune/Level/Decay; RS Level; HC Level; HH Level/CH Decay/OH Decay; Crash Level/Tune; Ride Level/Tune; Total Accent; Tempo 37–290 BPM; Volume. [TR909OM p.3, p.39] CONFIRMED.
- **The 909 MIDI receive map is the de facto standard, and GM inherited it.** Two notes per voice: **BD 35/36, RS 37, SD 38/40, HC 39, LT 41/43, CH 42/44, MT 45/47, OH 46, HT 48/50, CR 49, RD 51**. Receive defaults to ch 10 in Omni On mode; transmit to ch 11. [TR909SN p.14] CONFIRMED.
  - Drumazon 1/2 use exactly this map [D16KB42].
  - The Roland plugin uses it plus **24–31 = variation A–H, 32 = start, 33 = stop** [RC909M p.8].
  - TR-09 receives it but **transmits** 36/38/43/47/50/37/39/42/46/49/51 [TR09MIDI].
  - The **TR-8S** puts *alternate sounds* on 35/40/41/45/48/44/54/55/56/61/63 [TR8SMIDI].
  - The **RD-9** uses LT = 45, MT = 47, HT = 50 [RD9M] [ZB-RD]; its SD, CR and RD defaults are CONFLICTING between sources (§4.1).
- **Drumazon 2's voice set:** 11 synthesized voices, every one with Level after the strip. [DZ2M p.7–8] CONFIRMED from the manual plus its GUI screenshots. The knobs:
  - **BD:** Attack, Tune (= pitch-envelope release), Decay, **Pitch (base frequency)**, **Tune Depth (sweep range)**.
  - **SD:** Tune, Tune Depth, Tone, Decay, Snappy.
  - **RS:** Tune, Decay.
  - **CP:** Tune *or* Decay (CONFLICTING: the text says Decay, the screenshot says TUNE) plus Reverb.
  - **Toms:** Tune, Tune Depth, Tune Decay, Tone (noise), Decay.
  - **CH, OH, CR, RD:** Tune, Decay.
  - Two **CHOKE** toggles in the GUI (one under the toms, one under CH/OH), not documented in the text (UNVERIFIED semantics).
- **Drumazon 2 also matches Nepheton 2 almost feature for feature:**
  - the same per-voice **strip** (LC/HC + shared reso, 2-band para EQ, look-ahead comp);
  - the same **2 FX buses × 5 slots** (8 algorithms) and the same **master** (3-band comp + limiter with soft clip);
  - **3 audible trigger outs** and **16 stereo outs** with a "detach from master" checkbox;
  - **48 patterns (A–D × 12)** of **up to 64 steps**, scale 1/64–1/4 × {Full, Dot, Tri}, per-pattern shuffle;
  - Normal/Accent per step on **all** tracks plus a Global Accent row; Flam and Sub Step ×2/×3/×4 on **all** tracks;
  - Randomizer, Tap, Host Note, Host Link, MIDI out, drag-out MIDI, tag browser, >1000 presets and patterns. [DZ2M] [DZ2P]
  - **Gaps:** no per-track length [KVR-DZ2 forum], percent-style tuning rather than semitones [KVR-DZ2 forum], no Linux build.
- **Roland TR-909 plugin (ACB):**
  - 11 parts with variations A–H × 16 steps;
  - **per-instrument last step and per-instrument shuffle** (multiplied by the global knob);
  - WEAK steps, **9 flam types (0, 20–48 ms in 4 ms steps)**, sub steps 2/3/4;
  - GAIN (overdrive) and PAN on every part, and an extra **TUNE for the bass drum only** in the PANEL view;
  - a **fixed CC map** shared with the TR-09: BD 20–24, SD 25–29, toms 46–54, RS 55–57, CP 58–60, CH 61–63, accent 71, OH 80–82, CR 83–85, RD 86–88, shuffle 9;
  - drag-out as **MIDI or audio**, Main + 11 sub outs. [RC909M] [RC909P] CONFIRMED.
- **The TR-09's sequencer differs from the original in small, nameable ways** [TR09OM] [TR09ADD] CONFIRMED:
  - "backbeats": ENTER+key places a note **½ step late**, for 32 steps in one pattern;
  - shuffle and flam fine-adjustable with the tempo dial;
  - tempo 40.00–300.00 with decimals, against the original's 37–290;
  - step-key cycle order configurable (Off→Weak→Strong or Off→Strong→Weak);
  - a **separate trigger-out track** (the original's trigger follows the rim shot);
  - BD/SD **COMP**, GAIN and TUNE on every voice (rim, clap and hats included), PAN and mute/solo (v1.04);
  - no individual outs: USB MIX + 4 parallel USB outs.
- **Table stakes and differentiators** are in §6–§7. The recommended mw909 list is in §9. **mw808 already covers about 75 % of it** (sequencer superset, MIDI, outputs, main bus, variance). What it lacks:
  - the 909's own data semantics (group/bank/track memory, per-pattern flam interval, hardware accent and flam restrictions, the shuffle table);
  - the CH→OH choke as the 909 does it, and the BD **Pitch / Tune Depth** split;
  - a 909 SysEx bank import (only ADM has one);
  - a Roland-compatible CC map;
  - optionally, per-voice filter/EQ and send FX for Drumazon 2 parity, which mw808 never had.
- **Sample voices across the field:**
  - Drumazon 2 **synthesizes** hats and cymbals ("improved hats generator", "each sound is now fully synthesized") [KVR-DZ2a] [MT-DZ2].
  - VCV Drums **resynthesizes** cymbals from parameters extracted from public-domain recordings, about 16 KB of data and no ROM [VULT-DRUMS].
  - The RD-9 and ER-99 are sample-based for the cymbals [GN-RD9] [ER99].
  - Wave Alchemy and Pure909 are sample libraries [WA-TR] [PURE909].
  - This is input for the licensing decision in topic 02.
- **Platform gap persists:** no commercial 909 emulation ships for Linux (Drumazon 2: Win/Mac; Roland: VST3/AU/AAX; ADM: Win/Mac). The only Linux-native 909-ish plugins found are the **Niner** kick (GPL-3.0, Rust/nih-plug, VST3/CLAP) and the non-plugin research project **Drum9** (FAUST, GPL-3.0, explicitly "not suitable for use"). [NINER] [DRUM9]

---

## 1. The original TR-909 baseline (what "faithful" means for the feature set)

Primary sources: Owner's Manual (Dec 1983, scan in `.deps/sources/TR-909_OM.pdf`) [TR909OM]; Service Notes (Jun 1984) p.14 MIDI notes [TR909SN]; the pattern memory format as published by AudioRealism from the service manual [AR-SYX]. Topic 03 covers sequencer timing in depth; this section only fixes the feature inventory the products are compared against.

### 1.1 Voices and panel controls [TR909OM p.3, p.39] CONFIRMED
| Section | Knobs | Accent-capable (Accent B)? | Flam? |
|---|---|---|---|
| Total Accent | Level (knob) | — | — |
| Bass Drum | Tune, Level, Attack, Decay | Y | Y |
| Snare Drum | Tune, Level, Tone, Snappy | Y | Y |
| Low / Mid / Hi Tom | Tune, Level, Decay (each) | Y | Y |
| Rim Shot | Level | N | N |
| Hand Clap | Level | N | N |
| Hi-Hat | Level (shared), CH Decay, OH Decay | **CH only** | N |
| Cymbal | Crash Level, Crash Tune, Ride Level, Ride Tune | N | N |

- The spec page marks BD, SD, LT, MT, HT and CH with "*": "features the sound with and without accent". [TR909OM p.39]
- In step write, holding one of those voices makes the main key cycle **dark → dim (normal) → bright (accent)**. Other voices are on/off. "The intensity of this Accent cannot be changed"; only Total Accent has a knob. Setting both at once "may not be noticed" if Total Accent is high. [TR909OM p.25] CONFIRMED.
- Tap write: the **left key of a voice's key pair writes an accent and the right key a normal hit**. Total Accent cannot be tap-written. [TR909OM p.25]

### 1.2 Sequencer features [TR909OM p.24–31, p.39] CONFIRMED
- **Memory:** "48 Rhythm Patterns (16 × 3 Pattern Groups) × 2 (Bank I, II)" plus "4 Tracks (1 to 4: Continuous Maximum measures 896) × 2 (Banks I, II)". Steps 1–16 per measure.
- **Scale** cycles through 4 values: 1/16 → 1/32 → 1/8 triplet ("3") → 1/16 triplet ("6"). Changing scale auto-sets the last step to 16 or 12 according to the manual's figure (UNVERIFIED reading: 16 for the binary scales, 12 for the triplet ones).
- **Last Step:** hold LAST STEP and press a key 1–16.
- **Shuffle:** hold SHUFFLE/FLAM and press keys 1–7.
  - It only works in the 1/16 and 1/32 scales.
  - At 1/16: 1 = none, increasing towards 7.
  - At 1/32: only four distinct amounts, 1(=2), 3(=4), 5(=6) and 7, with 1 and 2 giving no shuffle.
  - "If the total number of measures are odd number, the last step will not take on the bounce."
- **Flam:** written as a voice state for BD, SD and the toms. Interval set with SHUFFLE/FLAM + keys **9–16**, so **8 options**; "Longer interval is obtained towards 16".
  - "The first sound of Flam is in time for a step, and the second one will follow."
  - "The position of the Tempo Knob does not affect the interval", so the interval is absolute time, not tempo-relative.
  - New data replaces old: a flam and a normal hit cannot coexist on one step.
- **Chain:** contiguous patterns within a group play as one long pattern ("even the entire Pattern Group (16) can be chained"). Chained patterns keep their own scale.
- **Track mode:** step-entering patterns per measure, with FWD/BACK, Insert, Delete (ranges, to the last measure), copy across banks, and a **per-track tempo memory**.
- **Edit:** pattern copy across groups and banks.
- **Tap write**, **step write**, **cycle play**, **available measures** display.
- **External Instrument mode:** the 16 main keys select **16 external MIDI "instruments" (Ext 1–16)** plus an accent row, written like internal voices and played out of MIDI OUT. Scale, last step and shuffle data are common with the internal pattern. "Flam is not available". [TR909OM p.37]

### 1.3 I/O [TR909OM p.32, p.34–35, p.38–39] CONFIRMED unless tagged
- **Outputs:** Master Out L and R/Mono (6 Vpp, 1 kΩ).
  - **10 multi-outs**: BD, SD, LT, MT, HT, RS, CP, Hi-Hat (one jack for CH + OH), Crash, Ride.
  - "Connecting any of these jacks removes that sound from the summed Master Out signal."
- **Trigger Out:** fires on every programmed **Rim Shot** step.
  - CONFLICTING pulse width: "+14V, 18ms" on p.38 against "+14V, 20 ms pulse" on the spec page p.39.
- **Other connectors:** Start/Stop footswitch (DP-2); DIN Sync In; tape Save/Load and Tape Sync; cartridge slot.
  - CONFLICTING cartridge name: M-64C on p.33 against M-46C on p.39, probably a typo.
- **Sync modes:** INT → MIDI → DIN, selected with SHIFT+TEMPO MODE.
- **MIDI:** IN plus **OUT ×2**.
  - Transmits notes 36–51 with velocity 64–96. Receives notes 35–51, plus SysEx (rhythm data), Song Position, Song Select 0–7, clock and start/stop. [TR909OM p.42 chart]
  - **Channel defaults:** receive ch 10 in MODE 1 (Omni On, Poly), displayed as "0" for Omni until a channel is selected; transmit ch 11. [TR909SN p.14]
  - CONFLICTING presentation: the OM p.38 text says receive is "automatically set to 0 … Omni", while the OM chart shows "10". The service notes reconcile them as "Defaults to MODE 1 (OMNI ON, POLY) 10 (1001)": basic channel 10, shown as "0" while Omni is on.

### 1.4 Pattern memory format (909 SysEx bank dump) [AR-SYX] CONFIRMED in outline, details tagged
- Request `F0 41 51 F7`. The data comes in **16 blocks** `F0 41 52 01 4n … ss F7` of 256 bytes sent as nibbles, each acknowledged with `F0 41 53 F7`. Bank size **16 × 256 = 4096 bytes** [DERIVED].
- Per step, 2 bytes:
  - Byte 1 `mmllssbb`: BD, SD, LT and MT, 2 bits each.
  - Byte 2 `zyxxcrhh`: HT 2 bits, RS 1 bit, CP 1 bit, HH 2 bits, CR 1 bit, RD 1 bit.
  - 2-bit codes: 01 = Accent, 10 = Normal, 00 = Off, **11 = Flam**. HH: 01 = Accent closed, 10 = Normal closed, **11 = Open**, 00 = Off.
- Pattern timing at 0xF80, 2 bytes per pattern: length (0–15 → 1–16), prescale (0–3), chain bit, shuffle (0–6 → 1–7) and flam (0–7 → 1–8).
- **Track data** sits at 0xC00–0xF7F, one byte per measure.
  - [DERIVED] 0xF7F − 0xC00 + 1 = 896 bytes, which matches the 896 measures in the spec. This cross-checks the layout.
  - **CONFLICTING (inside [AR-SYX]):** the page's Total Accent formula "byte3 = 3072 + step + pattern·16 + 256·group" lands inside that same track region (3072–3839 overlaps 3072–3967). One of the two descriptions is wrong. The service manual's SysEx section is in `.deps/sources/` and should be checked before an importer is written.
  - The track byte holds only a 4-bit pattern number ("hhhh = pattern 0–15"), so where the group and bank of each measure live is **UNVERIFIED**.
- **Why this matters:** (a) it is the authentic data model for an "authentic mode"; (b) a `.syx` importer is a real differentiator (only ADM has one, via its tool).

---

## 2. D16 Group Drumazon 2: full feature inventory

Primary source: *Drumazon 2 User Manual* (7 March 2024, 72 pp., `.deps/sources/Drumazon_2-manual-gb.pdf`) [DZ2M], including its GUI screenshots (pp. 5–9). The manual text is clearly derived from the Nepheton 2 manual: chapter order, wording and every FX range are identical to the mw808 Nepheton write-up. Where the text and the screenshots disagree, both are given.

**Product facts:**
- Released **9 September 2023** ("909 Day") at an intro price of €89; regular **€119**; upgrade from v1 **€29**. [MT-DZ2] [SA-DZ2] CONFIRMED.
- Current version **2.0.7** [DZ2P] [KVR-DZ2] CONFIRMED.
- Formats: Windows VST2/VST3/AAX; Mac VST2/VST3/AU/AAX (Universal); Windows 7+ and macOS 10.13+. **No Linux.** [DZ2P] [DZ2M p.2]

### 2.1 Instruments and per-voice synth controls [DZ2M p.7–8 + screenshot p.7]
"11 synthesized percussion instruments." Every voice has **Level**, placed after its strip. Screenshot knob layout by column (top-to-bottom, left-right), and the manual's description:

| Voice | Knobs in GUI (p.7 screenshot) | Manual description | Original 909 panel |
|---|---|---|---|
| Bass Drum | Attack, Level, Tune, Decay, Pitch, Tune Depth | Tune = release time of the frequency envelope; Attack = amplitude of the initial transient; Decay = amp decay; **Pitch = base frequency**; **Tune Depth = range of the frequency envelope** | Tune, Level, Attack, Decay. **Pitch and Tune Depth are additions.** D16's "Tune" keeps the 909 meaning (sweep time), not base pitch |
| Snare Drum | Tune Depth, Level, Tune, Decay, Tone, Snappy | Tune = freq-env release; Tone = **noise** amp release; Tune Depth; Decay = amp decay; Snappy = noise amplitude | Tune, Level, Tone, Snappy. **Decay and Tune Depth are additions** |
| Rim Shot | Level, Tune, Decay | Tune = base frequency; Decay | Level only. **Tune and Decay are additions** |
| Hand Clap | Level, **Tune**, Reverb | **Decay**, Reverb (reverb time) | Level only. **CONFLICTING:** the screenshot labels the middle knob TUNE, the text says Decay (and v1's manual also says Decay [DZ1M p.13]) |
| Low / Mid / High Tom | Tune Depth, Level, Tune, Decay, Tone, Tune Decay | Tune = base frequency; Tune Depth = freq-env range; Decay; Tone = noise amp release; Tune Decay = freq-env decay | Tune, Level, Decay. **Tune Depth, Tune Decay and Tone are additions** |
| Closed Hihat, Open Hihat | Level, Tune, Decay | Tune = base frequency; Decay | Shared Level, CH Decay, OH Decay. **Separate levels and Tune on both are additions** |
| Crash, Ride | Level, Tune, Decay | Tune = base frequency; Decay | Level, Tune. **Decay is an addition** |

Notes:
- **CHOKE** toggles: the p.5–7 screenshots show a "CHOKE ●" LED switch under the toms block and another under CH/OH (one lit, one dark). The text never mentions them. They probably switch the CH→OH choke and an inter-tom choke on and off, but the semantics are **UNVERIFIED**. Drumazon 1 had a fixed CH/OH choke according to a tutorial ("the Closed hat will cut the Open hat short … if both programmed at the same time, only the Open Hat will be heard") [MR-DZ1]. That simultaneous-hit rule matches the 909's HH data code "11 = Open" in §1.4.
- **Noise:** D16 staff say they "replaced digital noise generator with proper simulation of linear feedback shift register, which is used by claps, toms and snare", and "improved hats generator". [KVR-DZ2a] [KVR-DZ2b] UNVERIFIED (vendor forum post), but consistent with topic 01's circuit work.
- **Synthesis of the sample voices:** "each sound is now fully synthesized" [MT-DZ2]. This implies Drumazon 1 was not fully synthesized, presumably sample-based hats and cymbals (**UNVERIFIED**).
- Users report the pitch knobs are **percentage-scaled**, so tuning to a note needs an external tuner. v2.0.2 added "note pitch and frequency display in the BD Pitch tooltip". [KVR-DZ2a] [KVR-DZ2b]
- Product page claim: "Faithful 909 emulation with exact set of basic parameters the original drum machine" [KVR-DZ2].

### 2.2 Mute / Solo / Activity [DZ2M p.8]
- An M/S LED pair per instrument. Muting **stops new triggers but lets the envelope finish**. An activity LED per voice.

### 2.3 Per-instrument channel strip ("Strips" view) [DZ2M p.9–13]
The ranges are identical to Nepheton 2:
- The Instrument Selector groups voices as **Bass Drum | Snare/Clap (SD, RS, CP) | Toms | Hihats | Cymbals** (screenshot p.9).
- **Filter/EQ** (enable LED): Low Cut, Hi Cut, one shared **Resonance**; 2 parametric bands (enable, Freq, Width **4 to 0.5 oct**, Gain **±24 dB**).
- **Compressor** (enable LED): Look-ahead **Off/1/5 ms**; **Pre Eq** swaps the order; Attack and Release **1–500 ms**; Threshold **0 to −48 dB**; Ratio **1:1–1:20**.
- **Audio Out:** Route To {Master, Bus 1, Bus 2}, **Bus Slot 1–5**, Panning, **St. Spread** (L/R delay), Level.
- **Bus Send:** Route To {Bus 1, Bus 2}, Bus Slot, Panning, St. Spread, **Pre Lev**, Amount.
- **Instrument presets** (synth + strip); copy/paste of the Filter/EQ and Comp sections.
- User complaint: St. Spread "phases like crazy in mono" [KVR-DZ2a].

### 2.4 FX buses [DZ2M p.14–25]
**2 buses × 5 serial slots**, 10 instances from **8 algorithms**. EQ and Dynamics have two instances each.
- **Routing:** reorder by dragging tabs; swap effects between buses; a "More" button opens all parameters. **Bus 1 Setup** splits into Bus 2 (Amount, slot, Pre Level).
- **Distortion:** diode clipper; Dynamics pre-comp (Before: Clip/Split); 2-way crossover (Split Freq, Gap ±oct); Preamp, Threshold, Knee; Low Out, Clip Out, FX.
- **Parametric EQ:** 4 bands; low and high bands Cut or Shelf (Res/Width, Freq, Gain); 2 bells.
- **Delay:** 1–1000 ms or sync 1/32–1/4 × {Full, Dot, Tri}; Feedback; Spread; feedback-loop filter {Off, LP, BP, HP} with Cutoff and Reso; FX.
- **Reverb:** Pre-delay; Early (Size, Diffusion, Color, Modulate); Late (Size, Diffusion, Color, Feedback, Modulate); ER/LT; FX.
- **Chorus:** Offset, Depth, Feedback, Rate, Spatial, Lo/Hi Cut, Fatness, Gain, FX.
- **Bitcrusher:** Dither, Bits; Approximation filter (offset relative to Nyquist); Resampler **44 Hz–44.1 kHz** with Jitter; Images filter; FX.
- **Dynamics:**
  - **Comp** mode: look-ahead, Threshold, Ratio, **sidechain from another instrument**, Attack/Hold/Release 1–500 ms, Make Up, FX.
  - **VCA** mode: an AHR envelope fired by up to 3 instruments, with Modulation ± (gate or duck) and Gain.
- **Filter:** {Off, LP, BP, HP}, Cutoff, Reso. Mod source LFO (0.01–20 Hz or sync, Wave, St. Spread) or an envelope (A/H/R 1–500 ms, up to 3 trigger instruments). Mod Amt.

### 2.5 Master chain [DZ2M p.25–27]
- **3-band compressor:** per band Enable/Solo, Attack/Release 1–500 ms, Threshold 0 to −48 dB, Ratio 1:1–1:20, Make Up, GR meter. Two Cross Frq knobs; look-ahead Off/1/5 ms.
- **Limiter:** Attack, Release, Threshold, Output Volume (also on the main panel), **Soft Clip** (a diode-like clipper above 0 dB).

### 2.6 Trigger Outs (audible trigger) [DZ2M p.28]
- **Up to 3** trigger sounds, each with Enable, Source instrument, Route To {Master, Bus 1, Bus 2}, Bus Slot, Panning, Level. Off by default.
- Note: on the real 909 only the **rim shot** drives the trigger jack (§1.3). Drumazon lets any voice drive it.

### 2.7 Plugin audio outputs [DZ2M p.29–30]
- **16 stereo outputs.** By default Master → Out 1 only.
- Options → Audio Outputs → Edit Setup: any output can carry an instrument, trigger, bus or the master. Unticking the checkbox **takes the signal off the buses and master** ("detached"). This arrived in v2.0.2 [KVR-DZ2b].
- Load/Save of output setups.

### 2.8 Play modes and MIDI map [DZ2M p.30–36]
- **External** (Int Seq off): "MIDI Map follows the General MIDI standard, as it does in the original 909". Velocity is continuous ("127 levels").
- **Drum MIDI Map editor:** pick an instrument, click keys on an on-screen keyboard or MIDI-learn; several notes per instrument; "4-octave MIDI range only (starting with the 3rd octave)"; restore factory, load, save.
- The factory map is the 909 receive map (§5) [D16KB42]. That helpdesk article is for Drumazon 1; that Drumazon 2's factory map is identical is **UNVERIFIED** but very likely, since the manual says "as it does in the original 909".

### 2.9 Internal sequencer [DZ2M p.32–47]
**Storage and triggering**
- **48 patterns** = banks **A–D × 12** (screenshot: keys 1–12 plus A–D). Default A-1. Up to **64 steps**. [DZ2M p.34, p.41] [KVR-DZ2]
- **Live** mode: a selected pattern waits for the end of the current pattern's last step (blue LED = queued).
- **Host Note** mode: 48 MIDI notes, "starting from the C note of the second lowest octave" and spanning 4 octaves, each play one pattern while held. Note-off stops it. DAW Start is ignored.
- **Host Link Active**, off by default: off = sync to the host position once at start and then run on the internal clock; on = keep re-aligning (follows DAW loops).
- **Follow:** edit the playing pattern. With it off, blue = edited and red = playing.
- **MIDI Output:** in Live and Host Note modes, velocities come from Normal/Accent/Global Accent. External mode passes MIDI through. It can be disabled.

**Grid editing**
- 11 tracks plus a **GLOBAL ACC** row (on/off only).
- Track order in the screenshot: BD, SD, RS, CP, LT, MT, HT, CH, OH, CR, RD. v2.0.5 made the **instrument order configurable** [SEARCH-205] (UNVERIFIED, secondary).
- Step types: **Normal** (red) and **Accent** (brown) on **every** track; Ctrl-draw gives the other type. This is a superset of the 909, which restricts accent to BD, SD, toms and CH.
- **Velocity** per pattern: Normal, Accent and Gl. Acc. (an additive boost; accents stack). Screenshot example: 52 / 101 / +97.
- **Articulations:** None, **Flam**, **Sub Step ×2/×3/×4** on every track. Right-click cycles.
  - Flam: **Delay** as a fraction of the step (100 % = half a step) and grace-note **Volume** %, both per pattern.
  - Sub Step: **Start** and **End** volume %, linearly interpolated.
- **Pattern properties:** Scale **1/64 … 1/4 × {Full, Dotted, Triplet}**; Length up to 64; Shuffle (screenshot "43 %"). All per pattern. **No per-track length**, which users flagged as the main gap against Roland's plugin [KVR-DZ2b].
- **Edit:** select tracks (Ctrl = multi); copy/paste/clear tracks; shift selected tracks or the whole pattern left/right; copy/paste/clear pattern (yellow/orange/red buttons).
- **Browser and export:** load into slot (also drag-drop, multi); save; **Export** by dragging out as a **MIDI clip** (velocities from Normal/Accent/Global) or as a native file (also works between instances).
  - **Imports Drumazon 1** `.drptr`/`.drptrb` files [D16KB207].
  - v2.0.5 added "multi-pattern import by drag'n'drop" [SEARCH-205] (UNVERIFIED, secondary).
  - **No MIDI-file import** (user request) [KVR-DZ2b].
- **Randomizer** (Random tab): select tracks (none = all), set a per-track density "Amount", choose modifiers (accented, non-accented or both; articulations), **Next** for a new seed and **Prev** to go back. The grid stays editable.
- **Tap** tab: records only while running. The upper half of a pad = accent, the lower half = normal. From MIDI via the drum map: **velocity > 64 → Accent**.

### 2.10 MIDI, automation and options [DZ2M p.48–53]
- MIDI learn from the right-click menu (Learn/Clear); CC map files.
- **Automation is opt-in per parameter** ("thousands of sound parameters"); automation templates; MIDI/Automation Summary.
- Right-click shows the value, with no text entry. Ctrl/Cmd-drag gives fine control. Double-click resets.
- GUI: several **skin sizes**, a **System Scale** factor, HiDPI.
- **Quality:** separate **Real-time** and **Offline** settings. The levels are undocumented (UNVERIFIED).
- **Default State:** save as default, or restore factory.
- v2.0.5 fixed "not updating latency properly" [SEARCH-205]. The look-ahead compressors report latency.

### 2.11 Preset system [DZ2M p.54–71]
- **Scene** (all sound parameters + 48 patterns + Int Seq state) ⊃ **Drum Kit** (all voices, strips, buses, master) ⊃ **Instrument preset** (synth + strip) and **Master preset**. **Pattern** presets sit alongside.
- Browser: Factory (read-only) and User sources; cascaded tag **Groups** with an Any/All operator; search; **Favourites** (global); **Pins** (per project); Author and Description; rename, delete and multi-select; drag-drop import and export; user tags and groups.
- "Over 1000 presets and patterns" [DZ2P].

### 2.12 UI views (from the screenshots) [DZ2M p.5–9]
- **Header:** OPTIONS, SCENE preset, INT SEQ toggle (with a lock icon), DRUM KIT preset, logo.
- **Sound Control:** the 11 voice columns, with a VIEW selector **SYNTH / STRIPS / MASTER / TRIGGERS·MIDI** and an OUTPUT VOLUME knob with a meter. Under it, **BUS 1 | BUS 2**, each with 5 slot tabs, MORE, SETUP (Bus 1 only) and LEVEL.
- **Sequencer:** grid (Global Acc + 11 rows with M/S), a toolbar, pattern name and browser, a properties column (EDIT/RANDOM/TAP tabs, Scale, Length, Shuffle, Step Type, Articulation, NORMAL/FLAM/SUB S. tabs, Velocity), COPY/PASTE/CLEAR, LIVE/HOST NOTE tabs, START/STOP, cog (Host Link), and pattern keys 1–12 + A–D. The keys are styled after the 909 (cream keys, grey bank keys).
- **Palette:** light warm-grey panel, dark section headers with **orange** labels, dark red Level knobs, dark-brown steps. mw909's planned "lighter grey with orange accents" sits close to this. To avoid looking like a reskin of either Drumazon or mw808, prefer a different knob family and header treatment (design note, not a fact).

### 2.13 Drumazon 1 (for comparison; ideas worth keeping) [DZ1M]
- Same 11 voices and essentially the same knobs as Drumazon 2 (BD Tune/Attack/Decay/Pitch/Tune depth; SD Tune/Tone/Tune depth/Decay/"Snapy"; toms Tune/Tune depth/Decay/Tone/Tune decay; RS Tune/Decay; **HC Decay/Reverb**; CH/OH/CR/RD Tune + Decay). [DZ1M p.12–13]
- **12 outputs, stereo or mono**, assigned by dragging an LED; 1 trigger-out sound.
- **96 patterns = 8 banks × 12.** **Native mode** cycles a *range* of patterns (Ctrl-click the end pattern, or press two MIDI keys). **Host mode** plays one pattern per held note. A "Note Off Mode" option controls whether release stops it.
  - The manual's range is **CONFLICTING**: "C-1 to B-7" against "1 bank per octave… 8 octaves = 96".
  - [DERIVED] With D16's naming (C-2 = MIDI 36, so MIDI = 12·(oct+1) + pc), C-1 = 24, B-7 = 107 (84 notes) and B-8 = 119 (96 notes). Only 24–119 holds 96 patterns.
- **Accent per step on BD, SD, LT, MT, HT only**; "Rim Shot, Hand Clap, Crash and Ride have only two possible step values". Tap mode gives "first five instruments… two buttons each". This is close to the hardware rule, **but omits CH** (the 909 lets CH accent). [DZ1M p.16, p.18]
- **Flam** is a per-step state with one Flam knob, "always shorter than the duration of a whole single step". A flam overwrites a normal hit on the same step, as on the 909.
- Length 1–16 via LENGTH + key. **Tempo scale: 8, 4, 6, 3 steps per quarter** (= 1/32, 1/16, 1/16T, 1/8T). Shuffle per pattern. **Total accent** row + knob ("combined to give the loudest possible value").
- Int.Sync gives **per-pattern tempo**. "Host play detection" fallback: start on the lowest C note.
- Randomizer with a per-instrument intensity (the Level knobs double as intensity), Accept/Undo.
- External mode: "set the velocity to 127" for accent; continuous velocity.
- Fixed bank of 128 presets. XML files: `.drptr`, `.drptrb`, `.drprs`, `.drprb`, `.drdrmap`, `.drccmap`.

---

## 3. Roland's own 909 products

### 3.1 Roland TR-909 Software Rhythm Composer (Roland Cloud, ACB) [RC909M] [RC909P]
- **Engine and formats:** ACB circuit modelling. "Maximum Polyphony: 11 voices"; 11 parts. VST3/AU/AAX; Apple silicon. Win/Mac only. [RC909P] CONFIRMED.
- **Panel:** 909 knobs per part, plus a **[PANEL]** button that switches the edit section to **GAIN, PAN and TUNE (Bass Drum only)**. GAIN: "Individual gain lets you overdrive the internal circuitry". [RC909M p.3] [RC909P]
  - Reading (UNVERIFIED): the 909's own BD TUNE knob changes the pitch *sweep*, so Roland added a separate base-pitch TUNE for the BD, just as D16 added "Pitch". Two independent descriptions support this:
    - D16: BD "Tune - Release time of the frequency envelope" [DZ2M p.8];
    - Behringer: BD "TUNE controls the pitch envelope depth" [RD9M p.10].
    - They disagree on whether it sets the sweep's *time* or its *depth*. Topic 01 should settle it from the schematic.
  - The manual's knob descriptions are generic: TONE "for the snare drum, adjusts the tension of the head"; DECAY "for the hi-hat and cymbal… adjusted individually for each sound".
- **Fixed receive CC map** [RC909M p.8] CONFIRMED (the same as the TR-09's, §3.2; see §5.3):
  - 9 SHUFFLE;
  - 20 BD TUNE, 21 BD TONE, 22 BD COMP, 23 BD DECAY, 24 BD LEVEL;
  - 25 SD TONE, 26 SD SNAPPY, 27 SD COMP, 28 SD DECAY, 29 SD LEVEL;
  - toms 46–54 (TUNE, DECAY, LEVEL each);
  - RS 55–57 and CP 58–60 (TUNE, DECAY, LEVEL);
  - CH 61–63 (TUNE, DECAY, LEVEL);
  - 71 TOTAL ACCENT;
  - OH 80–82 (TUNE, DECAY, LEVEL);
  - **CC 83–85** (crash TONE, DECAY, LEVEL);
  - **RC 86–88** (ride TUNE, DECAY, LEVEL).
  - The labels for 21/25/28/83 differ from the TR-09 chart (BD ATTACK, SD TUNE, SD TONE, CC TUNE). The plugin manual looks copied from the TR-808 plugin manual (its Option table even says "TR-808 Software Rhythm Composer"). **CONFLICTING labels; prefer the TR-09 chart.**
- **Note map:** 24–31 = variation A–H; **32 = start, 33 = stop**; 35/36 BD; 37 RS; 38/40 SD; 39 CP; 42/44 CH; 41/43 LT; 46 OH; 45/47 MT; 49 CR; 48/50 HT; 51 RD. [RC909M p.8] CONFIRMED.
- **Memory:** a bank holds 128 patterns (manual: "16 patterns × 8 banks") + 128 kits; the number of banks is unlimited; banks are files.
- **Pattern:** **8 variations A–H** × 16 steps.
  - **LAST STEP per instrument** gives polymeter; the variation indicator shows each instrument's own variation when lengths differ.
  - **Per-instrument SHUFFLE slider**, multiplied by the main SHUFFLE knob (0 = none).
  - A TOTAL ACCENT row with the ACCENT LEVEL knob.
  - **WEAK** steps (Shift-click), **FLAM** (Type 1 = 0 ms, Types 2–9 = 20–48 ms in 4 ms steps), **SUB STEP** (duplets, triplets, quadruplets). Weak combines with flam or sub step. SCALE button (values UNVERIFIED). [RC909M p.5–6]
  - Unlike the 909, flam is shown as a selectable millisecond spacing rather than the 909's 8 key positions. The mapping of the 909's 8 flam keys to milliseconds is **UNVERIFIED**; topic 03 should measure it.
- **Edit:** variation copy/paste/clear; instrument copy, paste, clear, **INVERT**, **ALL**; mute/solo (solo wins).
- **Options:** drag a variation to the DAW **as MIDI or audio**; **Position Lock to DAW**; Layout Original/Compact; Zoom; "Optimize for Lower CPU Usage"; Initialize/Clear MIDI CC mapping; MIDI learn (one CC per control); **Send/Get pattern to/from TR-8S**.
- **Outputs:** Main + **Sub 1–11** (BD, SD, LT, MT, HT, RS, HC, CH, OH, CR, RD). [RC909M p.8]

### 3.2 Roland TR-09 (Boutique, ACB, 2016) [TR09OM] [TR09MIDI] [TR09ADD]
Behaviour that matches the original: 96 patterns (bank 2 × group 3 × 16), 8 tracks (bank 2 × 4); the 909 button grammar (SHIFT combos, Pattern/Track Play and Write, Tap/Step write, chain, track insert/delete/copy, tempo memory); 4 scales; total accent; shuffle on keys 1–7; flam on keys 9–16; flam only on BD, SD and toms; Off→Weak→Strong for BD, SD, toms and CH; other voices Off→Strong. CONFIRMED.

**Sequencer and behaviour differences from the original** (CONFIRMED from the TR-09 manual unless tagged):
1. **"Entering backbeats (32 steps)":** ENTER + main key enters a note "skewed 1/2 step later", giving "up to 32 steps in one pattern". The original needs the 1/32 scale or a chain. SOS: "the only way of making patterns of 32 steps". [TR09OM] [SOS-TR09]
2. **Finer shuffle and flam:** after choosing a key (shuffle 1–7, flam 9–16), "you can use the [TEMPO] dial to adjust the setting in detail". **SHUFFLE OFF = [SHUFFLE/FLAM]+[8]**.
3. **Tempo 40.00–300.00 BPM** with decimals, against the original's 37–290.
4. **Programming Mode** system setting: step-key cycle **Off→Weak→Strong** (default) or **Off→Strong→Weak**. A **SCALE button lock** option.
5. **Trigger out is its own track**, programmable per step (INSTRUMENT SELECT + TRIGGER OUT, then steps; also tap-writable). The original fires on rim-shot steps.
6. **Sound edits without knobs** (SHIFT+EDIT): **GAIN, TUNE, DECAY** per voice, so rim, clap and hats become tunable [ROL-TR09v8]. **COMP** level for **BD and SD**. **PAN** and **MUTE/SOLO** came in v1.04 [TR09ADD].
7. **Outputs:** one stereo mini OUTPUT plus a **MIX IN**. **USB audio = MIX + 4 parallel ports (USB1–4)**. A parallel mode splits the mini jack L/R between chosen voices. **No individual outputs.** [TR09OM] [SOS-TR09]
8. **MIDI:**
   - TX/RX ch 10 by default, against the original's TX 11. Soft Thru on/off. Sync source Auto/Int/MIDI/USB.
   - **Transmits** BD 36, SD 38, LT 43, MT 47, HT 50, RS 37, HC 39, CH 42, OH 46, CR 49, RD 51 with "v=80" in the chart (how accent is sent is UNVERIFIED). **Receives** the 909 map.
   - Full CC map (TX and RX): 9 SHUFFLE (RX only), 20 BD TUNE, 21 BD ATTACK, 22 BD COMP, 23 BD DECAY, 24 BD LEVEL, 25 SD TUNE, 26 SD SNAPPY, 27 SD COMP, 28 SD TONE, 29 SD LEVEL, toms 46–54, RS 55–57, HC 58–60, CH 61–63, 71 TOTAL ACCENT, OH 80–82, CC 83–85 (TUNE/DECAY/LEVEL), RC 86–88.
   - Song Position and Song Select 0–7. [TR09MIDI]
9. **No** External Instrument mode, tape sync or cartridge in the manual. Absence is UNVERIFIED; the manual is a 2-page sheet.
10. **Sound:** SOS: "even more like an example of a TR-909 you'd encounter today". It "accurately captures the gentle phasing whenever the clap and snare play at the same time — a product of a common noise source". The top end "begins to grate" over long sessions. [SOS-TR09] UNVERIFIED (one review).

### 3.3 Roland TR-8S (and TR-8) [TR8SREF] [TR8STONE] [TR8SMIDI]
- **909 ACB tones:**
  - BD: "909 Bass1", "909 Bass2", "909 Attack Bass", "909 Low Bass";
  - SD: "909 Snare1", "909 Snare2", "909 Attack Snare";
  - toms: "909 Low/Mid/High Tom", "909 Full Tom";
  - "909 Rim Shot", "909 Hand Clap", "909 Closed HH", "909 Open HH", "909 Crash Cymbal", "909 Ride Cymbal". [TR8STONE] CONFIRMED.
- **Per-instrument parameters:** Tune, Decay, Level, Pan, Reverb and Delay sends, INST FX, an LFO with a destination, and a **CTRL** knob that is voice-specific: BD **Attack**, SD **Snappy**, 909 toms **Color = "amount of resonance"**. [TR8SREF p.32–33] CONFIRMED.
- **Sequencer:**
  - 128 patterns × **8 variations A–H** + **2 fill-ins**; variation last step and **track last step** (the track's wins);
  - **sub steps** and **flam**; **weak beats**;
  - **per-step velocity** ("accent level"); **probability** (PROB, SUB PROB, master probability);
  - **ALT INST** per step; **Motion** recording of Tune/Decay/CTRL and FX knobs per step;
  - Nudge; **Scatter**; **Auto Fill In** every N measures; random pattern and random instrument;
  - **Step Loop**; a trigger-out track. [TR8SREF TOC + p.11–23] CONFIRMED.
- **I/O:** MIX OUT plus **ASSIGN OUT 1–6** (an assigned voice leaves the mix, or the jack works as a trigger output); MIDI notes configurable per track and per ALT; Program Change selects kits. [TR8SREF p.46] [TR8SMIDI]
- **The original TR-8 (2014):** Tune and Decay on every instrument; BD Attack + Comp; SD Snappy + Comp; scatter; A/B variations. [ATK-TR8] UNVERIFIED in detail (review summary).

### 3.4 Roland TR-1000 (hardware, 2025)
- "16 circuits inspired by the TR-808 and TR-909" in a **true analog** engine, plus ACB engines with "21 circuit-bent 8X and 9X options".
- Per-track shuffle and timing-shift controls; sampling. [DJM-TR1000] [SS-TR1000] UNVERIFIED in detail (news coverage only).
- Relevant only as a signal of where Roland's own feature expectations now sit: per-track shuffle/shift and "bent" 909 variants.

---

## 4. Survey of other 909 products

### 4.1 Behringer RD-9 (hardware analog clone, 2021)
Primary source: *RD-9 User Manual* (38 pp., local `.deps/sources/Behringer_RD-9_manual.pdf`) [RD9M]. Page numbers below are PDF pages.
- **Sound:**
  - "11 sounds in total with all the drum sounds being analog generated and the cymbals being sample-based"; "up to 10 simultaneous voices". [GN-RD9] UNVERIFIED (one review), consistent with [TH-RD9].
  - **Panel** [RD9M p.8, p.10–11] CONFIRMED:
    - BD: LEVEL, TUNE ("controls the pitch envelope depth applied to the bass drum"), ATTACK, DECAY, **PITCH DEPTH** ("how much the pitch envelope affects the bass drum oscillator… increase the slide") and **PITCH** ("sets the frequency of the bass drum oscillator").
    - SD: TUNE, TONE ("applies a filter… reducing the high frequencies"), SNAPPY, LEVEL. Toms: TUNE, LEVEL. RS and CP: LEVEL.
    - CH: LEVEL, **TUNE** ("controls the frequency of the hats with ENHANCED MODE on and affect both closed and open"), CH DECAY. OH DECAY. CRASH TUNE + LEVEL. RIDE TUNE + LEVEL.
  - **Enhanced Mode** (PREFS) "activates the extra controls for the Bass Drum (P.DEPTH and PITCH) and the Hi-Hat pitch"; off = the authentic 909 control set. [RD9M p.17–18] CONFIRMED. The same BD base-pitch/sweep-depth split as Drumazon and Roland's extra BD TUNE.
  - Choke: "when the closed hat is programmed on a step immediately after an OPEN HAT, the closed hat voice cuts the OPEN HAT voice". [RD9M p.10]
- **Accent model** [RD9M p.10, p.23–28]: per-voice **fixed accents** (press a step twice; "not affected by the accent control") plus **global accents** (TAP/HOLD + step) scaled by the ACCENT knob. An "Accent Mode" of Global or Independent with **per-voice accent values 0–255**, stored globally, per song or per pattern.
- **Flam:** one width **0–24** (global, per song or per pattern), enabled per step on **any voice**. [RD9M p.19]
- **Swing:** "25% negative swing, 50% straight to 75% full swing". Step sizes **1/8, 1/8T, 1/16, 1/16T, 1/32**. [RD9M p.8, p.19]
- **Features beyond the original** [TH-RD9] [MN-RD9 manual TOC] [GN-RD9]:
  - a **Wave Designer** (attack/sustain transient shaper) on the FX bus;
  - an **analog dual-mode LP/HP filter**, automatable per step;
  - a **64-step** sequencer with **polymeter**, **step repeat**, **note repeat**, live overdub, real-time triggering, track mute/solo;
  - "**Accent and Flam programmable per instrument**" (all instruments, unlike the 909);
  - **probability**, **random**, auto fill, auto scroll;
  - **256 patterns / 16 songs**;
  - 10 individual outputs (BD, SD, LT, MT, HT, RS, CP, **HH shared**, CR, RD; a used jack cuts the voice from the mix) + mono main + headphones; RETURN input;
  - **3 trigger outputs: RS, CP and "BD assignable", +5 V 2 ms** [RD9M p.9];
  - clock in/out; MIDI in/out/thru and USB MIDI; an editable MIDI **MAP** page; per-song "Preferences" deciding whether swing, flam, probability, accent, filter, mute and solo are global, per song or per pattern;
  - Settings pages: MIDI, USB, CLOCK, MAP, PREFS, FILTER, POLY, RAND, PROB, FLAM, RPT, step size, SYNC, DATA MODE, trigger output assign.
- **Default notes: CONFLICTING.**
  - The RD-9 manual's own MAP table [RD9M p.17] lists BD 36, **SD 40**, "Low Tom/Low Conga" 45, "Mid Tom/Mid Conga" 47, "Hi Tom/Hi Conga" 50, "Rim Shot/Claves" 37, "Clap/Maracas" 39, **Cowbell 56**, **Cymbal 51**, OH 46, CH 42. The 808-only voice names show this table was copied from the RD-8 manual, and it has no crash or ride entries.
  - The Zoë Blade transcription (citing "RD-9 manual, p.17") gives BD 36, RS 37, **SD 38**, CP 39, CH 42, LT 45, OH 46, MT 47, **CR 49**, HT 50, **RD 51** [ZB-RD].
  - Agreed by both: **LT 45, MT 47, HT 50**. The real firmware defaults for SD, CR and RD are UNVERIFIED.
- Price $349/€349 at launch [GN-RD9].

### 4.2 AudioRealism ADM (Drum Machine) [AR-ADM] [ADM-KVR]
- 606, 808 and 909 in one plugin: "25 drum generators… modeled after the classics' analog circuits and 4 PCM players, for a total of 34 sounds", plus 707/727 samples. 11-part polyphony.
- "The accents effect on the bridged-T oscillators found in the bass drum and tom-tom sounds have been faithfully replicated."
- "The hihats and cymbals in ADM closely resemble the originals". Whether these are the PCM players is UNVERIFIED. AudioRealism also sells a "TR-909 Sample Pack".
- **Sequencer:** 32 steps, **global accent per step**, **4 prescales per pattern**, **flam per step**, **shuffle per pattern**, 128 patterns; transforms (expand, shrink, swap, duplicate, randomize pattern/part, cut/copy/paste, undo).
- **PCF (Pattern Controlled Filter/FX):** a per-step value for any parameter (cutoff/Q, snappy, a voice switch such as maracas→clap "giving the illusion of more polyphony", "mangle" distortion, FX on/off).
- **Import:** **ReBirth pattern import** and **TR-909 SysEx import** (via the free web SysEx dumper, §1.4).
- Full MIDI CC learn; two MIDI modes (Pattern, Note); host tempo sync; **11 unprocessed + 2 master outputs** (VST only); user WAV/AIFF in parts; 80+ patterns.
- v1.6.2; €95; VST/VST3/AU/standalone; Win/Mac; key-file protection. [ADM-KVR] [AR-ADM] CONFIRMED.

### 4.3 Wave Alchemy (Revolution; Transistor Revolution)
- **Transistor Revolution** (2012, Kontakt): "22,000 TR-808 and 909 drum samples", "9,318 samples of the 909 bass drum alone". The knobs "scan smoothly through each and every sample". "Up to 7 random round robin variations per drum module". [WA-TR] CONFIRMED (two news sources).
- **Revolution** (2016, Kontakt Player, £149.95): 14 machines including the 909.
  - A "32-step multi-track sequencer with individual per step modulation routings" (step locks);
  - a "classic TR-style 16-button step-sequencer with full accent control, host sync, swing/shuffle and MIDI input";
  - a 14-track mixer with filters, EQs, **transient shapers** and dynamics per voice;
  - "160 … custom reverb impulses"; a **Character** tape-saturation knob; 1000+ patterns. [WA-REV] [SS-REV]
- No product called "Drumvolution" was found. The request presumably meant Revolution or Transistor Revolution (UNVERIFIED).
- Relevance: the reference for "sampled multi-velocity realism" and for the **per-step modulation** and **round-robin variance** expectations.

### 4.4 Softube Heartbeat [SB-HB-M]
- 8 instruments: BD1 (flexible analog-style), **BD2 "Kick", circuit-modelled on a "classic Japanese drum machine from the early 1980s"**, SD/RIM and SD/CLP (sampled snare waveforms blended with modelled synthesis), HH (open/closed with a choke), Perc 1 and 2, Cymbal.
- "Modeled analog synthesis… augmented with carefully selected waveforms". 6–8 parameters per drum plus a one-knob EQ.
- **No step sequencer.** An "Auto Layer Machine" chains up to 4 events.
- Reverb and echo sends, Dyna-mite dynamics, Filter Echo, TSAR-1D reverb, master saturation and EQ. **1 main + 8 separate stereo outs.** CONFIRMED (vendor manual).

### 4.5 Arturia Spark 2
- "Analog emulations of the TR-808, TR-909, TR-606, Simmons SDS-V…" (TAE + physical modelling, plus samples), 1900+ instruments, a **64-step** sequencer with shuffle/shift and loop Divide/Move; US$149, still on sale. [ART-SP2] CONFIRMED (vendor page).
- A multi-machine groovebox, not a dedicated 909, and Arturia has no dedicated 909 instrument (none found).

### 4.6 Others (brief)
- **Sonic Charge Microtonic:** 8 channels of "100% synthetic" drums with "a pattern based drum-machine engine"; $99; no Linux; no 909 claim [SC-MT]. A reference for CPU efficiency ("less than 5% on any low-end system"), not for 909 fidelity.
- **Reason Studios Jackbox 7R-909** (Rack Extension): 11 sounds, "Sample & Synthesis techniques"; per-voice multi outs; every parameter automatable with **CV inputs**; gate ins/outs. [RS-JB909] [INTEG] CONFIRMED.
- **ReBirth RB-338** added a 909 in v2.0 (Sep 1998). Desktop support ended 2005, after which it became a free download. The iOS app was pulled in 2017 after Roland claimed IP infringement. [WIKI-RB] CONFIRMED. ADM imports its patterns.
- **Synsonic BD-909** (free, VST/AU): "analysed the original circuit"; extras **Keyboard Pitch, Keyboard Accent, Tune Decay, Hold, Tune Depth, Noise Decay, Distort** [BONEDO-SYN] [RK-SYN]. The chromatic/"trap" BD expectation again.
- **Monade Pure909** ($12): sample-based, the 909 kit plus 19 resampled kits [PURE909].
- **MaxSynths DR-910** (free, 32-bit Windows VST2): a synthesized 909-style kit [INTEG]. UNVERIFIED.
- **Hardware clones and eurorack:** Hexinverter **NeinOhNein** kick, snare, rim and clap (DIY 909 replicas) [HEX]; Tiptop Audio BD909 and others. Jomox XBase 09 [JOMOX]. Roland T-8 (808/909/606 + 303 in ACB) [ITT].

### 4.7 Open-source and free 909 synths
| Project | Licence | Platform | What it is | Sample voices | Source |
|---|---|---|---|---|---|
| **Drum9** (Stanley J) | GPL-3.0 | FAUST (builds to plugins/apps) | "not based on sampled audio": SPICE black-box + transfer-function models from the schematics, with `bd/cp/rs/cb.sch` + `.sim` files and Python fits. The README itself says it is "not suitable for use as a softsynth": "Transistors were not modelled", instability "possible speaker damage" | `src/dsp/cymbals`, `hh` dirs exist; status unknown | [DRUM9] CONFIRMED (repo) |
| **Niner** (Hyperfocus DSP) | GPL-3.0 | Rust nih-plug, **VST3/CLAP/standalone, Linux x86_64** | 909-style kick (sub/mid/top layers) + a "909-modeled" clap, compressor/drive/limiter, 16-step seq, MIDI learn; v0.7.8 May 2026 | — | [NINER] UNVERIFIED (blog summary) |
| **ER-99** (Extralife / M. Cieplak) | GPL-3.0 | Web Audio | BD, SD, toms, RS, CP synthesized; **hats/crash/ride are WAVs** (`samples/hh.wav`, `crash.wav`, `ride.wav`) | samples (provenance unknown) | [ER99] CONFIRMED (repo tree) |
| **andremichelle/tr-909** | MIT | Web Audio | "about 90%" of the 909 UI/workflow; ships `resources/*.raw` for **every** voice (bassdrum-attack/cycle, snare-noise/tone, toms, rim, clap, hats, crash, ride), so sample-driven despite the "web-audio-api sound engine" wording | raw samples | [AM909] CONFIRMED (repo tree) |
| **sonicarchetype/TR909** | MIT | Web | 909-style UI, "Authentic Drum Sounds" (samples) | samples | [SA909] UNVERIFIED |
| **9W9** (athousanddetails) | open source (licence UNVERIFIED) | Ableton Move "Schwung" | circuit-modelled BD/SD/toms/RS/CP; "hi-hats, ride, and crash are sample-based"; derived from ER-99, Niner, AudioKit (Aug 2026) | samples | [SA-9W9] UNVERIFIED |
| **Vult Trummor / Trummor 2** | free (VultModulesFree) | VCV Rack | "909 style kicks", toms, simple snares/hats; Tune, Bend, Drive, AHD, noise | — | [TRUMMOR] |
| **VCV Drums** (Vult for VCV) | **proprietary, $30** | VCV Rack | Kick, Snare, Tom ×3, Rim, Clap modelled from **measured 909 PCBs** (Wolfram SystemModeler → code); hats/crash/ride as **synthesis**: "noise generation, analog filters and envelopes, and a sparkle of physical modeling", with frequencies "extracted… from public domain samples", ~16 KB of data, no ROM | resynthesis | [VCV-DRUMS] [VULT-DRUMS] CONFIRMED |
| **Hora Analog Drums** | (UNVERIFIED) | VCV Rack | BD/SD "using vult made filter model" | — | [HORA] |
| **Mutable Plaits** | MIT | eurorack/VCV ports | `synthetic_bass_drum.h`: "Naive bass drum model (modulated oscillator with FM + envelope). Inadvertently 909-ish." | — | [PLAITS-SRC] CONFIRMED |
| Geonkick, Hydrogen, drumkv1, avldrums, Drumrox | GPL | LV2/Linux | general drum synths or samplers; **no dedicated 909 circuit model** | — | [GEON] [AVL] |

**Conclusion:** there is still **no maintained, complete, open-source 909 plugin** (VST3/CLAP/LV2) with a sequencer, and nothing Linux-native at the Drumazon/Roland level. Drum9's schematic and SPICE assets (GPL-3.0) are a possible cross-check for topic 01. Do not copy them: mw909 is AGPL-3.0, so a GPL-3.0 combination is legally possible, but the house rule is clean-room.

### 4.8 How the field handles the sample voices (input for topic 02's licensing decision)
| Product | CH/OH/CR/RD approach | Source |
|---|---|---|
| Roland TR-909 plugin, TR-09, TR-8S | Roland's own ACB/ROM (they own the data) | [RC909P] [TR8STONE] |
| Drumazon 2 | synthesized ("improved hats generator"; "each sound is now fully synthesized") | [KVR-DZ2a] [MT-DZ2] |
| VCV Drums | resynthesis with parameters from public-domain recordings, ~16 KB | [VULT-DRUMS] |
| Behringer RD-9 | "sample-based" (provenance unknown) | [GN-RD9] |
| ADM | "closely resemble the originals" (method UNVERIFIED) | [AR-ADM] |
| ER-99, 9W9, andremichelle | WAV/raw samples bundled (provenance unknown) | [ER99] [SA-9W9] [AM909] |
| Wave Alchemy, Pure909 | recordings of real units (sample libraries) | [WA-TR] [PURE909] |

---

## 5. MIDI note maps and CC maps

### 5.1 Note maps
GM numbering below is the General MIDI percussion key map, which zoeblade's table notes "seems to be" based on the Roland TR maps [ZB-TR] [ZB-GM].

| MIDI | GM name | **TR-909 RX** [TR909SN] | Drumazon 1/2 [D16KB42] | Roland 909 plugin [RC909M] | TR-09 TX / RX [TR09MIDI] | TR-8S main / ALT [TR8SMIDI] | RD-9 [ZB-RD] / (manual table [RD9M]) | mw808 (current) |
|---:|---|---|---|---|---|---|---|---|
| 24–31 | — | — | (Host Note range, §2.9) | **Var A–H** | — | — | — | — |
| 32 / 33 | — | — | — | **Start / Stop** | — | — | — | — |
| 35 | Acoustic BD | BD | BD | BD | – / BD | BD alt | – | BD alias |
| 36 | BD 1 | **BD** | BD | BD | **BD** / BD | **BD** | BD | BD |
| 37 | Side stick | **RS** | RS | RS | **RS** / RS | **RS** | RS | RS |
| 38 | Acoustic snare | **SD** | SD | SD | **SD** / SD | **SD** | SD / (–) | SD |
| 39 | Hand clap | **HC** | HC | HC | **HC** / HC | **HC** | CP | CP |
| 40 | Electric snare | SD | SD | SD | – / SD | SD alt | – / (SD) | SD alias |
| 41 | Low floor tom | **LT** | LT | LT | – / LT | **LT alt** | – | LT |
| 42 | Closed HH | **CH** | CH | CH | **CH** / CH | **CH** | CH | CH |
| 43 | High floor tom | LT | LT | LT | **LT** / LT | **LT** | – | alias |
| 44 | Pedal HH | CH | CH | CH | – / CH | CH alt | – | CH alias |
| 45 | Low tom | **MT** | MT | MT | – / MT | **MT alt** | **LT** | MT |
| 46 | Open HH | **OH** | OH | OH | **OH** / OH | **OH** | OH | OH |
| 47 | Low-mid tom | MT | MT | MT | **MT** / MT | **MT** | **MT** | alias |
| 48 | Hi-mid tom | **HT** | HT | HT | – / HT | **HT alt** | – | HT |
| 49 | Crash 1 | **CR** | CR | CR | **CR** / CR | **CC** | CR / (–) | CY |
| 50 | High tom | HT | HT | HT | **HT** / HT | **HT** | **HT** | alias |
| 51 | Ride 1 | **RD** | RD | RD | **RD** / RD | **RC** | RD / ("Cymbal") | alias |
| 54/55/56/61/63 | (tamb./splash/cowbell/lo bongo/hi conga) | — | — | — | — | HC/OH/RS/CC/RC **alt** | — | — |

Observations:
- **The 909 receive map is universal among 909 products:** Drumazon, the Roland plugin and the TR-09 all receive exactly it. The TR-8S and RD-9 deviate, and in different ways.
- **What actually gets sent:** the TR-09 and TR-8S transmit the GM-preferred member of each pair (36, 38, 43, 47, 50), which still lands on the right voice in the 909 receive map. The **RD-9's LT = 45** lands on the **MT** under the 909 receive map, so an RD-9 clip played into a 909-map plugin swaps LT for MT. [DERIVED from the two tables]
- **The TR-8S trap:** its 41/45/48 trigger ALT sounds, not toms.
- **The 909 itself:** it transmits "36–51" [TR909OM chart], so BD is sent as 36. The exact transmitted member of each pair is **UNVERIFIED**.
- **Velocity:** the 909 transmits velocity "64–96" (chart); how normal, accent and total accent map into that range is **UNVERIFIED**. The TR-09 chart says "v=80". Drumazon receives continuous velocity and, in Tap, treats **> 64 = accent**. mw808 uses 96 = normal and 127 = full accent.
- **Channel:** the 909 receives on 10 and transmits on 11; the TR-09 uses 10/10; mw808 sends on ch 10.

### 5.2 Recommended mw909 map
- **Receive:** the full 909 map (35–51 as above), on all channels unless a channel is set. Optionally add GM aliases that never collide: 52/55/57 → CR, 53/59 → RD (the Nepheton map does similar).
  - [DERIVED] Do **not** remap 45 to LT for RD-9 compatibility: that breaks the 909, Drumazon and Roland behaviour, which all three agree on.
- **Transmit:** the TR-09/TR-8S choice: BD 36, SD 38, LT 43, MT 47, HT 50, RS 37, CP 39, CH 42, OH 46, CR 49, RD 51, on ch 10. These are GM-correct and round-trip through every 909-map receiver.
- **Control notes (Roland plugin compatible):** 24–31 = variation select, 32/33 = start/stop, so long as they do not collide with mw808-style Host Note (36 + N). In Host Note mode, notes ≥ 36 select patterns, so the voice map does not apply there anyway.
- **Velocity:** keep mw808's convention (96 = normal, 127 = full accent). Tap/record threshold: > 64 = accent (Drumazon).
- **Chromatic BD on a Bass Channel:** an mw808 feature, keep it.

### 5.3 CC map (Roland 909 family; adopt as the factory default)
Source: the TR-09 MIDI implementation chart (TX/RX) [TR09MIDI], cross-checked with the TR-909 plugin [RC909M p.8].
| CC | Param | CC | Param | CC | Param |
|---:|---|---:|---|---:|---|
| 9 | Shuffle | 46/47/48 | LT Tune/Decay/Level | 61/62/63 | CH Tune/Decay/Level |
| 20 | BD Tune | 49/50/51 | MT Tune/Decay/Level | 71 | Total Accent |
| 21 | BD Attack | 52/53/54 | HT Tune/Decay/Level | 80/81/82 | OH Tune/Decay/Level |
| 22 | BD Comp | 55/56/57 | RS Tune/Decay/Level | 83/84/85 | CR Tune/Decay/Level |
| 23 | BD Decay | 58/59/60 | HC Tune/Decay/Level | 86/87/88 | RD Tune/Decay/Level |
| 24 | BD Level | 25/26/27/28/29 | SD Tune/Snappy/Comp/Tone/Level | | |

This lets a TR-09, a TR-8S (in its 909 subset; the TR-8S's own map has extra CTRL CCs, e.g. 109/110 for CC/RC CTRL [TR8SMIDI]) or any controller templated for the Roland plugin drive mw909 with no learning. MIDI learn overrides it.

---

## 6. Feature matrices

Legend: **Y** yes; **P** partial or limited; **N** no; **?** not confirmed; "–" not applicable.

Columns:
- **909** = original hardware;
- **DZ2** / **DZ1** = Drumazon 2 / 1;
- **R909** = Roland plugin;
- **09** = TR-09; **8S** = TR-8S; **RD9** = Behringer RD-9;
- **ADM** = AudioRealism; **REV** = Wave Alchemy Revolution; **HB** = Softube Heartbeat; **VCV** = VCV Drums;
- **mw808** = what mw808 already ships (the superset baseline).

### 6.1 Sound engine, voices and mixing
| Feature | 909 | DZ2 | DZ1 | R909 | 09 | 8S | RD9 | ADM | REV | HB | VCV | mw808 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| All 11 909 voices | Y | Y | Y | Y | Y | Y | Y | Y | Y (samples) | N (inspired) | Y | – (808) |
| Modelled analog voices | analog | Y | Y | ACB | ACB | ACB | analog | Y | N | Y | Y | Y |
| Hats/cymbals method | 6-bit ROM | synth | ? | ACB | ACB | ACB | samples | ? | samples | synth+wave | resynth | (808: synth) |
| Tune on every voice | N | Y (CP: CONFLICTING) | P (CP no) | P (BD extra tune; CC tune on all) | Y (EDIT) | Y | P (no RS/CP) | ? | Y | Y | Y | Y |
| Decay on every voice | N | Y | Y (CP decay) | P | Y (EDIT) | Y | P (BD, CH, OH) | ? | Y | Y | Y | Y |
| BD base pitch separate from 909 "Tune" (sweep) | N | **Y (Pitch)** | Y | Y (PANEL TUNE) | ? | ? | **Y (Pitch)** | ? | ? | – | Y (Tune/Sweep) | – |
| BD sweep depth control | N | Y (Tune Depth) | Y | N | N | N | Y (Pitch Depth) | ? | ? | – | Y (Sweep) | Y (808 sweep) |
| Snare/tom noise and sweep extras | N | Y (Tune Depth, Tune Decay, Tone) | Y | N | N | Y (tom Color) | ? | ? | ? | – | P | – |
| Clap reverb/tail control | N | Y | Y | N | N | ? | ? | ? | ? | – | Y (Space) | Y (808 CP) |
| CH→OH choke | Y (circuit) | Y (toggle?) | Y | Y? | Y | Y | Y | ? | ? | Y | Y (Mute) | Y (808 CH/OH) |
| Per-voice drive/gain | N | via bus | N | **Y (GAIN)** | Y (GAIN) | INST FX | N | Y (mangle) | Y | P | N | Y |
| Per-voice compressor | N | Y (strip) | N | BD/SD (CC) | **BD/SD** | INST FX | N | ? | Y | N | N | N |
| Per-voice filter/EQ | N | Y | N | N | N | INST FX | master filter | Y (PCF filter) | Y | 1-knob EQ | N | N |
| Per-voice pan | N | Y | N | Y | Y (1.04) | Y | N | ? | Y | Y | Y | Y |
| Transient shaper | N | VCA mode | N | N | N | INST FX | **Y (Wave Designer)** | N | Y | N | N | N |
| Send FX (reverb/delay) | N | Y (2 buses × 5) | N | N | N | Y | N | Y | Y | Y | N | N |
| Master dynamics/limiter | N | Y (3-band + limiter) | N | N | N | Y | N | ? | Y | Y | N | Y (comp + limiter + analog stage) |
| Audible trigger-click | – | Y (3) | Y (1) | N | N | N | N (3 trig jacks) | N | N | N | N | N |
| Individual outputs | 10 (HH shared) | 16 st assignable | 12 st/mono | 11 + main | USB 4 + mix | 6 assign | 10 | 11 + 2 | Kontakt | 8 + main | per module | 11 st |
| Unit variance / tolerance | (real) | ? | ? | ? | ? | ? | (real) | ? | round-robin | ? | ? | Y (serial) |
| Quality / oversampling | – | RT + Offline | N | "Lower CPU" | – | – | – | ? | – | ? | ? | Eco/Std/High |
| Linux native | – | N | N | N | – | – | – | N | N | N | Y (VCV) | **Y** |

### 6.2 Sequencer, MIDI and workflow
| Feature | 909 | DZ2 | DZ1 | R909 | 09 | 8S | RD9 | ADM | REV | mw808 |
|---|---|---|---|---|---|---|---|---|---|---|
| Max steps / pattern | 16 (chain for more) | 64 | 16 | 16 × 8 var | 16 (+ backbeats = 32) | 16 × 8 var | 64 | 32 | 32 | 64 |
| Pattern memory | 96 (2 × 3 × 16) | 48 (4 × 12) | 96 (8 × 12) | 128/bank, ∞ banks | 96 | 128 | 256 | 128 | ? | 64 (4 × 16) + A/B |
| Song / track mode | **4 tracks/bank, 896 meas, insert/delete/copy, tempo memory** | N | N | N | Y (8) | N | 16 songs | ? | ? | chain only |
| Scale | 4 (1/16, 1/32, 1/8T, 1/16T) | 1/64–1/4 × F/D/T | 4 | Y (values ?) | 4 | Y | Y (step size) | 4 prescales | ? | 5 |
| Last step | per pattern | per pattern | per pattern | **per instrument** | per pattern | per var + **per track** | **polymeter** | ? | ? | per pattern + per track |
| Shuffle | per pattern, 7 steps | per pattern % | per pattern | global × **per instrument** | per pattern + fine | Y | continuous | per pattern | Y | per pattern + depth |
| Per-instrument accent | BD, SD, toms, CH | all tracks | BD, SD, toms | weak/normal | BD, SD, toms, CH | velocity | all | N (global only) | Y | all (weak/normal/accent) |
| Total/global accent row | Y + knob | Y + boost | Y + knob | Y + knob | Y + knob | ? | Y | Y | ? | Y |
| Flam | BD, SD, toms; 8 intervals per pattern | all; % of step + vol | knob | 9 types (ms) | 8 + fine | Y | **all instruments** | per step | ? | per step, global ms |
| Sub-steps ×2/×3/×4 | N | Y (+ vol ramp) | N | Y | N | Y | step/note repeat | N | ? | Y |
| Weak/ghost steps | N | N (velocity values) | N | Y | Y (weak = normal) | Y | ? | N | ? | Y |
| Per-step probability | N | N | N | N | N | **Y** | **Y** | N | ? | Y |
| Per-step parameter lock / motion | N | N | N | N | N | Motion | filter auto | **PCF** | **step locks** | N |
| Randomizer | N | Y (density, seeds) | Y (intensity) | N | N | Y | Y | Y | ? | Y (+ Euclid) |
| Tap write | Y (L key = accent) | Y (upper half = accent; MIDI > 64) | Y | ? | Y | INST REC | real-time | ? | MIDI in | MIDI record |
| Pattern chain | Y (adjacent) | N (queue) | range cycling | multi-variation | Y | Y | chaining mode | ? | ? | Y |
| Queue switch at pattern end | Y | Y | Y | ? | Y | Y | Y | ? | ? | Y |
| Pattern select by MIDI note | Song Select 0–7 | Host Note 48 | Host 96 | var 24–31 | Song Sel | PC | ? | Pattern mode | ? | Host Note |
| MIDI out of sequence | Y + **Ext Inst mode** | Y | Y | ? | Y | Y | Y | ? | ? | Y |
| Drag-out | – | MIDI | N | **MIDI or audio** | – | – | – | ? | ? | MIDI |
| MIDI file drop-in / import | – | DZ1 files only | N | N | – | – | – | ReBirth + **909 SysEx** | ? | **Y (.mid)** |
| Editable note map | N | Y (GUI + learn) | file | N (fixed) | N | Y | Y (MAP) | ? | ? | N (fixed GM) |
| Fixed documented CC map | – | N | N | **Y** | **Y** | Y | ? | learn | ? | N |
| MIDI learn | – | Y | Y | Y | – | – | – | Y | Y | Y |
| Host sync modes | INT/MIDI/DIN/tape | once vs continuous | host vs int tempo | Position Lock | Auto/Int/MIDI/USB | – | – | host | host | Host/Internal/Note/Off |
| Hierarchical presets + tags | – | Y | P | patterns + kits | – | – | – | ? | ? | kit/pattern/full |

---

## 7. Table stakes (every serious 909 plugin has these)

1. **All 11 voices with the original panel controls** and their meanings: BD Tune (sweep)/Attack/Decay; SD Tune/Tone/Snappy; toms Tune/Decay; RS and CP Level; hats Level + CH Decay + OH Decay; Crash and Ride Tune. Plus **accent that changes the circuit drive** (per-instrument + total), not only level. DZ1 says accent "also causes subtle changes to the sound character" [DZ1M p.16]; ADM replicates "the accents effect on the bridged-T oscillators".
2. **Tune and Decay on every voice**, including rim, clap and hats: Drumazon, the TR-09, TR-8/8S, VCV and RD-9 (partly). Plus a **separate BD base pitch** alongside the 909 sweep-style Tune (Drumazon "Pitch", Roland's extra BD TUNE, RD-9 "Pitch").
3. **CH chokes OH**, and hats played together resolve to open (909 data code `11 = Open`).
4. **Per-voice level, pan, mute/solo**, where mute lets tails ring (Drumazon, Roland).
5. **Multi-out:** at least 10–11 individual outputs plus main (909: 10; Roland: 11; ADM: 11; Drumazon: 16).
6. **The 909 MIDI receive map** (§5) as the default, with continuous velocity.
7. **Internal step sequencer** synced to the host: total accent row, per-step accent, **flam**, **shuffle**, **last step**, scale (at least the 909's 4), pattern switching at pattern end, MIDI out.
8. **Drag-and-drop MIDI export** (Drumazon, Roland).
9. **MIDI learn** plus host automation.
10. **Presets** for kits and patterns with a factory pattern library (Drumazon >1000; ADM 80+; Roland banks).
11. **Resizable / HiDPI GUI.**

## 8. Differentiators (what the best ones add)

**From Drumazon 2 (parity targets for "every feature the popular 909 plugins have")**
- Per-voice **strip** (LC/HC + reso, 2-band para EQ, look-ahead comp).
- **2 FX buses**: reverb, delay, distortion, bitcrusher, chorus, filter with LFO/env, dynamics with **instrument sidechain/VCA ducking**.
- 3-band master comp + limiter with soft clip.
- 16 stereo outs with "detach from master".
- 3 audible trigger clicks.
- Extra synthesis knobs: BD Pitch/Tune Depth; SD and tom Tune Depth/Tune Decay/Tone; CP Reverb.
- Choke toggles.
- Sub Step start→end volume ramp; flam grace volume.
- Randomizer with seed history.
- Host Note mode; Host Link once vs continuous.
- Scene/kit/instrument/pattern preset hierarchy with a tag browser.

**From Roland (plugin, TR-09, TR-8S)**
- **Per-instrument last step and per-instrument shuffle** (× global): the one feature Drumazon users explicitly miss [KVR-DZ2b].
- 8 variations A–H; WEAK steps; **flam spacing in ms** (0, 20–48).
- **Drag out as audio.**
- GAIN (circuit overdrive) and BD/SD **COMP** per voice.
- **Fixed documented CC map** (§5.3); note-controlled variation select and start/stop (24–33).
- TR-09: **½-step "backbeat"** entry (32 steps without changing scale), a **programmable trigger track**, fine shuffle/flam.
- TR-8S: probability, motion recording, ALT instrument per step, scatter, auto fill, step loop, nudge, per-step velocity.

**From AudioRealism ADM / RD-9 / Wave Alchemy**
- **Per-step parameter locks** (ADM PCF, Revolution step locks, TR-8S motion, RD-9 per-step filter).
- **909 SysEx bank import** and ReBirth import (ADM).
- Pattern expand/shrink/swap and undo (ADM).
- **Wave Designer** transient shaper and an **analog LP/HP performance filter** (RD-9).
- Accent and flam on **all** instruments, an **"authentic" mode switch** that turns the extensions off, probability, note/step repeat, polymeter (RD-9).
- Round-robin micro-variation (Wave Alchemy).

**Unique to the mwAudio line**
- **Linux-native VST3 + CLAP + LV2 + standalone.** No commercial 909 competitor offers this; only the Niner kick does.
- Analog variance from a stored serial number.
- Zero-allocation real-time path.
- Bit-exact state.
- An oracle-tested sequencer.

---

## 9. Recommended feature list for mw909

Key: **[808]** = already in mw808's superset (carry over and adapt); **[808~]** = exists in mw808 but needs 909-specific semantics; **[NEW]** = not in mw808. Priority: **P1** must, **P2** should, **P3** nice-to-have.

**Voices and sound**
1. **P1 [NEW] 11 independent 909 voices** (BD, SD, LT, MT, HT, RS, CP, CH, OH, CR, RD) with the original knobs and their original meanings. BD "Tune" is the sweep, per topic 01.
2. **P1 [808] Tune + Decay on every voice.** 0 = stock circuit, as in mw808. Sample voices tune by **clock rate** (brief).
3. **P1 [NEW] BD Pitch (base frequency) and Tune Depth (sweep amount)** as extended knobs: the Drumazon/RD-9/Roland consensus. **P2:** SD/tom Tune Depth and noise Tone (Drumazon).
4. **P1 [808~] CH→OH choke** via the circuit's discharge path. Simultaneous CH+OH on a step plays open (909 HH code). **P3:** a choke on/off toggle (Drumazon).
5. **P1 [808~] Accent model:** a per-instrument accent (Accent B) **plus** Total Accent, both driving the circuits. Accent B's level is fixed on hardware; only Total Accent has a knob.
6. **P2 [808] Per-voice drive/gain** (Roland GAIN parity). **P2 [NEW] BD/SD compressor** (TR-09/Roland COMP parity; CC 22/27).
7. **P2 [NEW] Clap tail control** (Drumazon "Reverb"), if topic 01 finds a circuit parameter to expose.
8. **P3 [NEW] Per-voice LC/HC filter + 2-band EQ**, and **2 send FX** (reverb, delay) with sidechain ducking: Drumazon parity. mw808 has neither, so this is a **scope decision for the user** (CPU and UI cost; the brief asks for "every feature the popular 909 plugins have").
9. **P3 [NEW] Transient shaper** (RD-9 Wave Designer) and a master LP/HP performance filter.
10. **P3 [NEW] Audible trigger-out click** tied to the **rim shot** (909) or any voice (Drumazon).

**Sequencer**
11. **P1 [808~] 909 memory layout:** 2 banks × 3 groups × 16 patterns (96), **4 tracks per bank** with measure insert/delete/copy and a **per-track tempo memory**: the 909's track/song mode. mw808's 4 × 16 + chain does not express this.
12. **P1 [808] Scale:** the 909's 4 (1/16, 1/32, 1/8T, 1/16T); mw808 has these plus 1/8.
13. **P1 [808~] Shuffle as stored per pattern on the 909** (7 levels at 1/16, 4 effective at 1/32; inactive in triplet scales), with exact timing from topic 03. **P2 [NEW]** per-track shuffle × global (Roland).
14. **P1 [808~] Flam interval stored per pattern** (8 values, absolute time, not tempo-relative), with a millisecond readout and the Roland 0/20–48 ms set as an extended range. mw808's flam is global, so it needs moving into the pattern.
15. **P1 [808] Last step 1–16** (909) extended to 64 + **per-track length** (polymeter; Roland). Already in mw808.
16. **P1 [NEW] "Authentic" mode switch:** accent only on BD/SD/toms/CH; flam only on BD/SD/toms; flam ⊕ accent exclusive; 16 steps; scale, shuffle and flam limited to hardware values. "Extended" (default) lifts every restriction (RD-9 and Drumazon precedent).
17. **P1 [808]** Weak steps, sub-steps ×2/×3/×4 (P2: add the Drumazon start→end volume ramp), probability, randomizer, Euclid, copy/paste/shift/clear.
18. **P1 [808~] Tap write:** the 909 key-pair semantics (left key = accent) in the GUI; MIDI record with velocity > 64 → accent.
19. **P2 [NEW] ½-step "backbeat" entry** (TR-09): a per-step micro-offset of +½ step, or more generally per-step nudge (TR-8S). This overlaps the timing model in topic 03.
20. **P1 [808]** Pattern chain, queue at pattern end, Host Note mode, host-sync modes. **P2 [NEW]** Host Link "sync once vs continuous" (Drumazon).
21. **P2 [NEW] TR-909 SysEx bank import** (`.syx`, 4096-byte bank, §1.4): only ADM has it. Needs the service-manual SysEx page checked first (§1.4 conflict).
22. **P3 [NEW]** Per-step parameter locks (ADM PCF / TR-8S motion); 8 variations A–H (mw808 has A/B).

**MIDI and I/O**
23. **P1 [808~] Note map:** the 909 receive map (§5.2) and TR-09-style transmit on ch 10. mw808's GM map needs re-pointing (LT 41/43, MT 45/47, HT 48/50, RD 51, CR 49).
24. **P1 [NEW] Factory CC map = the Roland 909 family map** (§5.3); MIDI learn [808] overrides it.
25. **P2 [NEW]** Note 24–31 variation select and 32/33 start/stop (Roland plugin), if variations exist.
26. **P1 [808]** MIDI out, MIDI learn, drag-out MIDI, drop-in MIDI, the chromatic BD Bass Channel.
27. **P1 [808~] Outputs:** main + **11 stereo individual**, with the hardware rule that a voice sent out leaves the mix [808]. **P3:** an "HH shared output" option to mirror the 909's 10 jacks.
28. **P3 [NEW]** Drag out as audio (Roland).
29. **P1 [808]** Main bus analog stage + compressor + limiter, analog variance from a serial, quality modes, presets (kits/patterns/full), resizable UI with PANEL/GRID/MIXER.

---

## 10. UNVERIFIED / open items
- **Drumazon 2's CHOKE toggles** (what each chokes) and **clap middle knob** (Tune or Decay): needs the demo or a newer manual.
- Drumazon 2 version history beyond 2.0.2/2.0.5 (2.0.6, 2.0.7 changelogs not found); default Normal/Accent velocities; quality-setting levels.
- **How the 909 transmits accent** in MIDI velocity (chart: 64–96) and which member of each note pair it sends; the TR-09's "v=80".
- **The 909's 8 flam intervals and 7 shuffle levels in milliseconds/ticks**: hand off to topic 03 (measure, or derive from the CPU clock).
- [AR-SYX] internal conflict on the Total Accent byte location and on the track-to-group encoding: verify against the service manual's SysEx pages in `.deps/sources/` before writing an importer.
- Roland plugin SCALE values; whether its per-instrument accent is "normal vs weak" or a true Accent B; whether its CC 21/25/28/83 labels or the TR-09's are right (assumed TR-09's).
- The RD-9's real firmware default notes for SD, CR and RD: its manual's MAP table is an RD-8 copy (§4.1). Also its ACCENT per-voice value scale (0–255) as heard.
- Whether Drumazon 1's hats and cymbals were samples (implied by "now fully synthesized").
- Licences of 9W9, Hora and sonicarchetype sound sources; provenance of the samples in ER-99, andremichelle and 9W9.
- No product named "Drumvolution" was found (the brief probably meant Wave Alchemy Revolution or Transistor Revolution).

## Sources
- [TR909OM] Roland TR-909 Owner's Manual (Dec 1983, scan) — https://static.roland.com/assets/media/pdf/TR-909_OM.pdf (local: `.deps/sources/TR-909_OM.pdf`; pages cited are the manual's printed page numbers, PDF page = printed − 1)
- [TR909SN] Roland TR-909 Service Notes (Jun 1984), p.14 "MIDI NOTES / MIDI KEY ASSIGNMENT" — local `.deps/sources/TR-909_Service_Manual.pdf` (also cited by https://notebook.zoeblade.com/Roland_TR_MIDI_map.html)
- [AR-SYX] AudioRealism TR-909 SysEx Tool / format description — https://www.audiorealism.se/products/samples-tools/tr-909-sysex
- [DZ2M] D16 Drumazon 2 User Manual (7 Mar 2024) — https://m.d16.pl/pub/manuals/Drumazon%202-manual-gb.pdf (local: `.deps/sources/Drumazon_2-manual-gb.pdf`)
- [DZ1M] D16 Drumazon (1) User Manual — https://m.d16.pl/pub/manuals/Drumazon-manual-gb.pdf (local: `.deps/sources/Drumazon-manual-gb.pdf`)
- [DZ2P] D16 Drumazon 2 product page — https://d16.pl/drumazon2
- [KVR-DZ2] KVR product page, Drumazon 2 — https://www.kvraudio.com/product/drumazon-2-by-d16-group
- [KVR-DZ2a] KVR forum "D16 Drumazon 2 is coming" (D16 staff m00gie on the LFSR noise and hats generator; user reports) — https://www.kvraudio.com/forum/viewtopic.php?p=8744462
- [KVR-DZ2b] same thread (2.0.2 changelog, per-track length request, MIDI import request) — https://www.kvraudio.com/forum/viewtopic.php?p=8760599
- [SEARCH-205] Drumazon 2 v2.0.5 notes as quoted in search results (KVR marketplace / dubspot) — https://www.kvraudio.com/marketplace/drumazon-2-by-d16-group (secondary)
- [D16KB42] D16 Helpdesk "External Mode – Drum Map" (Drumazon) — https://helpdesk.d16.pl/knowledge_base/article/42
- [D16KB207] D16 Helpdesk "Can I import patterns from Drumazon 1?" — https://helpdesk.d16.pl/knowledge_base/article/207
- [MT-DZ2] MusicTech news, Drumazon 2 — https://musictech.com/news/gear/d16-group-drumazon-2/
- [SA-DZ2] Synth Anatomy, Drumazon 2 — https://synthanatomy.com/2023/09/d16-group-drumazon-2-major-update-for-the-popular-tr-909-emulation.html
- [MR-DZ1] MusicRadar "How to edit sounds in D16 Drumazon" (CH/OH choke, via search summary) — https://www.musicradar.com/tuition/tech/how-to-edit-sounds-in-d16-drumazon-633366
- [RC909M] Roland TR-909 Software Rhythm Composer Owner's Manual (2018) — https://r2.gear4music.com/media/68/683872/download_683872.pdf (Roland index: https://www.roland.com/global/support/by_product/rc_tr-909/owners_manuals/)
- [RC909P] Roland TR-909 Software Rhythm Composer product page — https://www.roland.com/global/products/rc_tr-909/
- [TR09OM] Roland TR-09 Owner's Manual (eng03) — https://static.roland.com/assets/media/pdf/TR-09_eng03_W.pdf
- [TR09ADD] TR-09 "Functionality Added (Ver.1.04)" — https://static.roland.com/assets/media/pdf/TR-09_AddFunc_Errata_e01_W.pdf
- [TR09MIDI] TR-09 MIDI Implementation Chart (2016) — https://static.roland.com/assets/media/pdf/TR-09_MIDI_Imple_e01_W.pdf
- [ROL-TR09v8] Roland UK blog "TR-09 vs TR-8" — https://www.roland.com/uk/blog/tr-09-vs-tr-8/
- [SOS-TR09] Sound On Sound, Roland Boutique TR-09 & TB-03 review — https://www.soundonsound.com/reviews/roland-boutique-tr-09-tb-03
- [TR8SREF] Roland TR-8S Reference Manual v3.00 — https://static.roland.com/assets/media/pdf/TR-8S_Reference_eng05_W.pdf
- [TR8STONE] TR-8S Preset INST Tone List v3.00 — https://static.roland.com/assets/media/pdf/TR-8S_PresetToneList_eng04_W.pdf
- [TR8SMIDI] TR-8S MIDI Implementation Chart — https://static.roland.com/assets/media/pdf/TR-8S_MIDIImpleChart_eng03_W.pdf
- [ATK-TR8] Attack Magazine, Roland AIRA TR-8 review — https://www.attackmagazine.com/reviews/gear-software/roland-aira-tr-8-rhythm-performer/
- [DJM-TR1000] DJ Mag, TR-1000 announcement — https://djmag.com/tech/roland-announces-new-true-analogue-drum-machine-tr-1000-rhythm-creator
- [SS-TR1000] Sonic State, TR-1000 presentation — https://www.sonicstate.com/news/2025/10/01/presentation-roland-introduces-tr-1000-rhythm-creator
- [GN-RD9] gearnews, Behringer RD-9 — https://gearnews.com/?p=98589
- [TH-RD9] Thomann, Behringer RD-9 spec — https://thomannmusic.com/behringer_rd_9.htm
- [MN-RD9] Behringer RD-9 User's Manual (table of contents via manualsnet) — https://manualsnet.com/behringer/rd-9
- [RD9M] Behringer RD-9 User Manual (38 pp., PDF; local `.deps/sources/Behringer_RD-9_manual.pdf`, fetched by a sibling researcher). Online copy: https://manualsnet.com/behringer/rd-9
- [ZB-RD] Zoë Blade, Behringer RD MIDI map (cites the RD-9 manual p.17) — https://notebook.zoeblade.com/Behringer_RD_MIDI_map.html
- [ZB-TR] Zoë Blade, Roland TR MIDI map — https://notebook.zoeblade.com/Roland_TR_MIDI_map.html
- [ZB-GM] Zoë Blade, General MIDI Percussion Map — https://notebook.zoeblade.com/General_MIDI_Percussion_Map.html
- [AR-ADM] AudioRealism ADM product page — https://www.audiorealism.se/products/audiorealism-drum-machine
- [ADM-KVR] AudioRealism ADM on KVR — https://www.kvraudio.com/product/2553
- [WA-REV] Wave Alchemy Revolution announcement — https://synthanatomy.com/2016/12/wave-alchemy-released-revolution-drum.html
- [SS-REV] Sonic State, "14 Classic Drum Machines For Kontakt" — https://www.sonicstate.com/news/2017/01/10/14-classic-drum-machines/
- [WA-TR] Wave Alchemy Transistor Revolution — https://bedroomproducersblog.com/2012/12/19/transistor-revolution-by-wave-alchemy/ and https://sonicstate.com/news/2012/12/18/transistor-revolution-released/
- [SB-HB-M] Softube Heartbeat manual — https://www.softube.com/manuals/heartbeat
- [ART-SP2] Arturia Spark 2 — https://www.arturia.com/store/drums/spark2
- [SC-MT] Sonic Charge Microtonic — https://soniccharge.com/microtonic
- [RS-JB909] Reason Studios Jackbox 7R-909 — https://www.reasonstudios.com/shop/rack-extension/jackbox-7r-909-analog-drum-module/
- [INTEG] Integraudio "6 Best 909 Plugins" — https://integraudio.com/6-best-909-plugins/
- [ITT] Internet Tattoo, best TR-909 VSTs and hardware (2026) — https://www.internettattoo.com/blog/the-best-tr-909-vsts-and-hardware-for-house-and-techno-producers
- [WIKI-RB] Wikipedia, ReBirth RB-338 — https://en.wikipedia.org/wiki/ReBirth_RB-338
- [BONEDO-SYN] Bonedo, Synsonic BD-808 und BD-909 — https://www.bonedo.de/artikel/synsonic-bd-808-und-bd-909/
- [RK-SYN] rekkerd.org, Synsonic BD-909 — https://rekkerd.org/?p=111680
- [PURE909] Sonic State, Pure909 — https://sonicstate.com/news/2016/06/28/tr-909-emulated
- [HEX] Hexinverter NeinOhNein (search summary; ModularGrid) — https://modulargrid.net/e/modules/view/2061
- [JOMOX] Jomox XBase 09 manual — https://www.jomox.de/upload/manuals/XBase09_209Engl.pdf
- [DRUM9] Drum9 (FAUST, GPL-3.0) — https://gitlab.com/stanleyj/drum9
- [NINER] Niner by Hyperfocus DSP — https://github.com/hyperfocusdsp/niner ; summary https://blog.imseankim.com/niner-hyperfocus-dsp-free-open-source-909-kick-drum-synth-vst3-clap-2026/
- [ER99] ER-99 (GPL-3.0) — https://github.com/matthewcieplak/er-99 ; https://www.matrixsynth.com/2022/09/er-99-online-web-based-roland-tr-909.html
- [AM909] andremichelle/tr-909 (MIT) — https://github.com/andremichelle/tr-909
- [SA909] sonicarchetype/TR909 (MIT) — https://github.com/sonicarchetype/TR909
- [SA-9W9] Synth Anatomy, 9W9 for Ableton Move Schwung (Aug 2026) — https://synthanatomy.com/2026/08/athousanddetails-9w9-ableton-move-schwung-now-has-a-tr-909-drum-machine.html
- [TRUMMOR] Vult Trummor — https://modlfo.github.io/VultModules/trummor/
- [VCV-DRUMS] VCV Drums — https://vcvrack.com/Drums
- [VULT-DRUMS] Vult, "Making of the VCV Drums" — https://www.vult-dsp.com/post/making-of-the-vcv-drums
- [HORA] VCV Library, Hora Analog Drums — https://library.vcvrack.com/Hora-AnalogDrums
- [PLAITS-SRC] Mutable Instruments eurorack source, plaits/dsp/drums/synthetic_bass_drum.h (MIT) — https://github.com/pichenettes/eurorack/blob/master/plaits/dsp/drums/synthetic_bass_drum.h
- [GEON] Geonkick — https://github.com/Geonkick-Synthesizer/geonkick
- [AVL] avldrums.lv2 — https://www.archlinux.org/packages/extra/x86_64/avldrums.lv2
