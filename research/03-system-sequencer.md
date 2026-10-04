<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# 03 — TR-909 as a System: Panel, Sequencer, Accent, Outputs, Timing, Quirks

Scope: everything outside the individual voice circuits. That covers the front and rear panel, the CPU sequencer and its memory layout, scale, shuffle and flam, the two-level accent system and how its voltages reach each voice, MIDI / DIN / tape sync, the trigger output, the mix bus and outputs, timing behaviour, and system-level quirks and mods. Voice DSP is covered in 01 (analog voices) and 02 (sample voices).

## Sources (primary first)

| Tag | Source | URL / location |
|---|---|---|
| **[OM]** | Roland TR-909 Owner's Manual (scan, 43 pdf pages). Page numbers below are the **printed** page numbers, which equal pdf page + 1 (for example printed p.4 "Panel Description" is pdf page 3, printed p.26 is pdf page 25) (corrected by fact-check: was "pdf page + 2 (printed p.4 is pdf page 2)"; the +2 offset only holds for 0-based image indices of the scan). | https://static.roland.com/assets/media/pdf/TR-909_OM.pdf (local: `.deps/sources/TR-909_OM.pdf`) |
| **[SN]** | Roland TR-909 Service Notes, First Edition, 15 Jun 1984, 16 pp. Page numbers are the printed ones (p.1 spec, p.4 block diagram, p.5 to p.6 circuit descriptions, p.7 change information, p.8 switch board, p.9 scope traces, p.11 voicing board, p.12 multi-jack/DIN/PSU boards, p.13 M-64C, p.14 to p.16 MIDI). Read from the 300 dpi scan and the 8110 × 5732 px JPEG2000 set. | https://archive.org/details/synth_Roland_TR-909_service_notes ; https://archive.org/details/roland_TR-909_SERVICE_NOTES (local: `.deps/sources/TR-909_Service_Manual.pdf`, `TR-909_service_notes_archive_jp2.zip`, `TR-909_SERVICE_NOTES_text.pdf`) |
| **[RC909]** | Roland TR-909 Software Rhythm Composer owner's manual (modern plugin; used only for the flam and shuffle cross-check) | https://static.roland.com/assets/media/pdf/TR-909_Software_Rhythm_Composer_eng04_W.pdf (local: `RC_TR-909_OM_gear4music.pdf`) |
| **[TR09]** | Roland Boutique TR-09 owner's manual (modern re-implementation; cross-check of scale, shuffle and flam UI) | https://www.roland.com/global/products/tr-09/ (local: `TR-09_eng03_W.pdf`) |
| **[KVR]** | neoxtrope, KVR forum, 31 Aug 2009: measured shuffle offsets from 96 kHz recordings | https://www.kvraudio.com/forum/viewtopic.php?t=261858 |
| **[EL-FLAM]** | EchoBoy, e-licktronic forum "Flam timings", 22 Jul 2017: flam trigger-pulse measurements | http://www.e-licktronic.com/forum/viewtopic.php?f=25&t=1430 |
| **[EL-ACC]** | EchoBoy et al., e-licktronic forum "Accent level in 1.023", Jul 2017: original 909 accent latch/DAC behaviour versus the Nava clone | https://www.e-licktronic.com/forum/viewtopic.php?t=1425 |
| **[NAVA]** | Nava 909-clone firmware v1.028 (community fork); `define.h`, `Clock.ino` | https://github.com/junk16/Nava_909_Custom_Firmware_V1.0 |
| **[JV]** | J. Vosmaer, "Nava DMUX" blog post: TR-909 accent DAC structure | https://blog.jacobvosmaer.nl/0043-nava-dmux/ |
| **[MW]** | Muff Wiggler thread "TR-909 OS Rom File or how is the shuffle exactly Implemented" (archived 2019) | http://web.archive.org/web/20190517022721/https://www.muffwiggler.com/forum/viewtopic.php?t=135002 |
| **[GS-JIT]** | Gearspace, "Roland TR909 jitter test results (internal sequencer, Din sync and MIDI clock slaved)". **Could not be opened (HTTP 403)**; the figures come from search-engine excerpts. | https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/1111455-roland-tr909-jitter-test-results-internal-sequencer-din-sync-midi-clock-slaved.html |
| **[LIT]** | Innerclock Systems "Litmus" timing tests, as quoted on MPC-Forums and in search excerpts. **The Litmus page itself did not show a TR-909 row when fetched.** | https://www.innerclocksystems.com/litmus ; https://www.mpc-forums.com/viewtopic.php?p=1510794 |
| **[ICS]** | Innerclock Systems commentary on TR-909 timing, quoted by MATRIXSYNTH | https://www.matrixsynth.com/2014/07/roland-tr-909-timing-issues.html |
| **[CF]** | Colin Fraser, "909 Sound Mods" (18 May 2004) | http://www.colinfraser.com/tr909/909mods/909mods.htm |
| **[RW]** | Robin Whittle, "TR-909 sound mods" (10 Oct 2018) and the firmware/RAM/battery notes on his site | https://www.firstpr.com.au/rwi/tr-909/ (local: `TR-909-Sound-Mods_RobinWhittle.pdf`) |
| **[REV]** | Addictive Instruments "Revolution 909" v5 firmware page | https://addictiveinstruments.fr/en/kits/1-revolution909.html ; https://www.synthtopia.com/content/2024/05/19/the-roland-tr-909-gets-an-upgrade-after-40-years-with-revolution-909-mod/ |
| **[EM-ACC]** | electro-music forum, "TR-909 accent offset modification" | https://electro-music.com/forum/topic-56592.html |
| **[DIN]** | DIN sync summary | https://en.wikipedia.org/wiki/DIN_sync |

**Tags.** **CONFIRMED**: read in a primary source ([OM] or [SN]), or two independent good sources agree. **UNVERIFIED**: one secondary source only, or my inference. **CONFLICTING**: sources disagree; both values are given. **[DERIVED]**: my own arithmetic from component values or from manual statements; the formula is shown. **[MEASURED]**: numbers I measured myself (here, only pixel measurements of the [SN] p.9 scope drawings). Scratch scripts are in `/tmp/claude-1000/mw909-research-sequencer/` (`mix.py`), and are not shipped.

---

## 0. Design-critical facts (executive summary)

| Item | Value | Status |
|---|---|---|
| Voices / trigger lines | 11 sounds on 10 trigger lines: BD, SD, LT, MT, HT, RS, HC (hand clap), HH (CH/OH, one trigger plus a CLOSED/OPEN level), CRASH, RIDE | CONFIRMED [SN p.4 block diagram, TRIG latches IC1/IC10] |
| Pattern memory | **2 banks (I, II) × 3 pattern groups × 16 patterns = 96 patterns** (48 per bank). There are no A/B variations; longer patterns use **Chain**. | CONFIRMED [OM p.12, p.39; SN p.1] |
| Steps | 1 to 16 per pattern (LAST STEP). The scale change auto-sets 16 (16th, 32nd) or 12 (both triplet scales). | CONFIRMED [OM p.24] |
| Scale (4 values) | **16th** (4/beat), **32nd** (8/beat), **8th triplet** (3/beat), **16th triplet** (6/beat); the SCALE button cycles 16th → 32nd → 8th-trip → 16th-trip | CONFIRMED [OM p.24; TR09] |
| Tracks (songs) | 4 tracks per bank, **896 measures per bank shared by all 4 tracks**; a track stores pattern numbers and cannot cross banks | CONFIRMED [OM p.15, p.17; SN p.1] |
| Tempo | ♩ = **37 to 290** BPM, from a CPU-read pot (VR601 50 kΩ B) and a 12 MHz crystal CPU (µPD7811G); this is not an analog RC clock as on the 808 | CONFIRMED [SN p.1, p.8] |
| Shuffle | keys 1 to 7 (1 = off). **16th scale: even-numbered steps delayed by (n − 1)/48 of a beat**, which is 0, 2, 4, 6, 8, 10, 12 /96. **32nd scale: only 4 distinct levels {1=2, 3=4, 5=6, 7}.** Not available in either triplet scale. With an odd last step, the last step is not delayed. | Level count, key 1 = off, 32nd pairing, no triplet shuffle: CONFIRMED [OM p.26]. 16th delay magnitudes: **UNVERIFIED** (one forum measurement [KVR]; consistent with the 32nd pairing) (corrected by fact-check: was "CONFIRMED"). 32nd values: [DERIVED] |
| Shuffle as swing % | 50, 54.2, 58.3, 62.5, 66.7, 70.8, **75 %** for n = 1 to 7 | [DERIVED] |
| Flam | **BD, SD, LT, MT, HT only**. 8 intervals (keys 9 to 16), stored per pattern, **fixed in ms and independent of tempo**. The first hit falls on the step, the second follows. | CONFIRMED [OM p.26] |
| Flam spacing | **CONFLICTING:** 10 to 38 ms in 4 ms steps (hardware trigger measurement [EL-FLAM]) versus 20 to 48 ms in 4 ms steps ([RC909] Roland software; [NAVA] source comment attributing it to the same measurer). Linear 4 ms steps either way. | CONFLICTING |
| Accent | Two kinds: **Total Accent** (one per-step row for all voices; strength from the TOTAL ACCENT knob) plus **per-voice Accent** (BD, SD, LT, MT, HT, CH only; fixed strength). The CPU adds them digitally, then writes a **6-bit code per voice** to a 4174 latch plus an RM0621 binary-weighted resistor DAC (0 to 5 V). | CONFIRMED [OM p.5, p.25; SN p.4, p.5, p.11] |
| Accent DACs | 8 DACs for 10 trigger lines: BD (IC2), SD (IC3), LT (IC4), MT (IC5), HT (IC6), HH (IC7 per block diagram), **RS+HC shared (IC8 per block diagram)**, **CRASH+RIDE shared (IC9)**. The p.11 circuit diagram wires **IC7/RA6 to the RS and HC accent buffers** (so IC8 = HH), as does [JV]. | Sharing: CONFIRMED [SN p.4, p.11]. IC7/IC8 designators: **CONFLICTING** within [SN] (p.4 vs p.11) (added by fact-check) |
| Accent hold | Each latched accent voltage holds until that DAC is rewritten at the voice's next trigger | CONFIRMED [SN p.5]; [EL-ACC] |
| TRIG pulse | 5 V, **2 ms**, from latches IC1/IC10; accent is latched "almost concurrent" with it | CONFIRMED [SN p.4, p.5] |
| MIDI | Receives on ch. 10 (Omni on) at power-up; transmits on ch. 11. Notes BD 35/36, RS 37, SD 38/40, HC 39, LT 41/43, CH 42/44, MT 45/47, OH 46, HT 48/50, CRASH 49, RIDE 51. **Rx velocity 1 to 127**; **Tx velocity 64 to 96 = "accent min–max"**. Clock, Start/Stop/Continue, SPP and Song Select (track) both ways; 2 × MIDI OUT; OUT also merges IN (soft thru). | CONFIRMED [SN p.14, p.15] |
| Sync | **DIN SYNC IN only** (no DIN out on the hardware): pin 1 Run/Stop, 2 GND, 3 Clock (24 ppqn, positive edge), 5 Continue. TEMPO MODE cycles INT → MIDI → DIN. Tape sync is FSK at 1200/2400 Hz, toggling every 1/48 beat. | CONFIRMED [SN p.1, p.6, p.12, p.15; OM p.34–36] |
| Trigger out | **RIM SHOT only**: +14 V pulse, 20 ms per [SN p.1]/[OM p.39], 18 ms per [OM p.38]. The width is set by an RC pulse stretcher (C114 0.1 µF, R409 47 kΩ) behind the 2 ms RS TRIG, which gives roughly 15 to 17 ms nominal; with capacitor tolerance, neither printed figure is ruled out (added by fact-check). | CONFLICTING (18 vs 20 ms) |
| Individual outs | 10 jacks (BD, SD, LT, MT, HT, RS, HC, HH, CRASH, RIDE). Each is a **switched jack: plugging in removes that voice from both L and R mix**. Output 1 kΩ series plus 0.01 µF shunt (units from S/N 415300) = 15.9 kHz pole. | CONFIRMED (schematic) [SN p.12, p.7] |
| Master mix | **Stereo** with fixed panning by unequal mix resistors: LT and RIDE on one side, HT and HH on the other, each **5.25 dB** louder on its side; CRASH 1.58 dB to the HT/HH side; BD, SD, MT, RS, HC centre (corrected by fact-check: was 4.8 dB and 1.45 dB; the shared 1 kΩ series resistor had been double-counted). **Which side is CONFLICTING:** the jack-board bus labels put LT/RIDE right and HT/HH left; the voicing-board connector labels and the traced master-amp wiring put LT/RIDE **left** and HT/HH/CRASH **right**. | [DERIVED] from [SN p.11, p.12]; side CONFLICTING |
| Master tone | **HPF ≈ 130 Hz** (C153/C154 1 µF into the summing virtual ground, source ≈ 1.2 kΩ) and **LPF ≈ 6.6 kHz** (R473 1 k + R530 2.2 k, C511/C512 0.01 µF, R531/R533 10 k; units from S/N 415300) | [DERIVED]; LPF existence also reported by users (UNVERIFIED) |
| Timing jitter | Internal clock: ≈ 1.2 ms RMS, ≈ 2.2 to 2.3 ms peak between consecutive 16ths [GS-JIT]; Litmus max 4.46 ms internal, 6.40 ms DIN, 7.79 ms MIDI [LIT] | UNVERIFIED (pages not openable) |
| Internal resolution | The shuffle grid (1/48 beat) and the tape-sync FSK (a toggle every 1/48 beat) both point to a **48 ppqn** internal time base. That is the coarsest grid that fits the evidence; a finer grid (96 ppqn or more) is not ruled out (fact-check note). **No primary source says 96 ppqn.** | [DERIVED]; 96 ppqn UNVERIFIED |

---

## 1. Front-panel inventory ([OM p.4 "Panel Description", p.8, p.39]; [SN p.1, p.8, p.11])

### 1.1 Sound section (top row, left to right)

| Section | Knobs (as printed) | Pot part / value [SN p.1] | Notes |
|---|---|---|---|
| **TOTAL ACCENT** | one knob | EVJFDAF30B15 **100 kΩ B (linear)**, VR1 on the voicing board | Its wiper goes to the **CPU's analog input** ("Analog Voltage Input from TOTAL Control" [SN p.5]; switch-board connector F1 "ACC LEVEL", "ACC +5V", "GND" [SN p.8]). It is not in the audio or trigger path. **CONFIRMED** |
| **BASS DRUM** | TUNE, LEVEL / ATTACK, DECAY | LEVEL 100 kΩ B; DECAY 1 MΩ A; ATTACK 500 Ω B; TUNE 100 kΩ A (the callout lines are slightly ambiguous; leave this to file 01) | [OM p.8] "Tune … Whatever the position this knob is set, the Decay knob works at its set time." |
| **SNARE DRUM** | TUNE, LEVEL / TONE, SNAPPY | LEVEL 50 kΩ B | SNAPPY fully CCW removes the snare-wire noise [OM p.8] |
| **LOW TOM / MID TOM / HI TOM** | TUNE, LEVEL / DECAY (each) | TUNE 10 kΩ B; LEVEL 50 kΩ B; DECAY 500 kΩ B | "you can use them as Melody Tom" [OM p.8] |
| **RIM SHOT / HAND CLAP** | LEVEL, LEVEL | RS 100 kΩ B; HC 50 kΩ B | Level only |
| **HI HAT** | LEVEL / CH DECAY, OH DECAY | LEVEL 50 kΩ B; CH DECAY 100 kΩ B; OH DECAY 1 MΩ A (VR21/VR23 [SN p.5]) | One circuit for CH and OH |
| **CYMBAL** | CRASH LEVEL, RIDE LEVEL / CRASH TUNE, RIDE TUNE | LEVEL 50 kΩ B; TUNE 10 kΩ B | [OM p.8] calls TUNE "Tone … adjust the pitch of the Cymbal sound". The spec page [OM p.39] says "Crash Cymbal (Level, Tune)". |

Voices marked "*" in [OM p.39] "feature the sound with and without accent": BD, SD, LT, MT, HT, CH. **CONFIRMED.**

### 1.2 Control section

- **START** and **STOP/CONT** keys. STOP/CONT pressed again resumes from the stop point ("Continue Play Function") [OM p.14, p.18]. **CONFIRMED.**
- **MEAS/TEMPO** display: 3-digit 7-segment (LB-603VK, D636). It shows tempo in BPM (TEMPO button), measure number, file number and available measures [OM p.18, p.22; SN p.8]. **CONFIRMED.**
- **TEMPO** knob, large, SLOW to FAST. VR601 is 50 kΩ B (part EWHLNAF20B54) on a CPU analog input; range 37 to 290 [SN p.1, p.8; OM p.39]. **CONFIRMED.**
- **VOLUME** (master) knob, MIN to MAX. Part EWK77A027B54 50 kΩ B, dual; it is the feedback resistor VR28a/b of the IC66 master amp [SN p.1, p.11] (§6). **CONFIRMED.**
- **TRACK PLAY 1 to 4** (Shift = TRACK WRITE). **PATTERN PLAY 1 to 3**, the pattern-group buttons (Shift = PATTERN WRITE). **EXT INST**. Each has an LED [OM p.4; SN p.8 LEDs D625 to D631, D624]. **CONFIRMED.**
- Six function buttons with LEDs. The name below each is its Shift function:
  - **TEMPO**: shows tempo (Shift: **STEP** = Step Write mode).
  - **BACK** (Shift: **TAP** = Tap Write mode).
  - **FWD** (Shift: **BANK I**).
  - **AVAILABLE MEAS** (Shift: **BANK II**).
  - **CYCLE/GUIDE** (Shift: **LAST MEAS**).
  - **TAPE SYNC** (Shift: **TEMPO MODE**).

  The PATTERN WRITE MODE, BANK, LAST MEAS and TEMPO MODE legends sit under these keys [OM p.4]. **CONFIRMED.**
- **CARTRIDGE/ENTER** key with LED. Its secondary legend is **TOTAL ACCENT**, used with INSTRUMENT SELECT to edit the total-accent row [OM p.4, p.25]. **CONFIRMED.**
- Left key block, six buttons without LEDs: **LAST STEP**, **SCALE** (with **4 scale LEDs** D632 to D635 printed beside the step strips), **SHUFFLE/FLAM**, **CLEAR**, **INSTRUMENT SELECT**, **SHIFT** [OM p.4; SN p.8]. **CONFIRMED.**
- **16 MAIN KEYS** with LEDs (D601 to D616). The LEDs show **two brightnesses: dim = normal, bright = accent** [OM p.10, p.11, p.25]. Above the keys are 4 printed timing strips, one per scale, showing which keys fall on beats. Legends:
  - Number entry: 1 to 9, 0 on keys 1 to 10.
  - Shift edits: **COPY** (11), **INS** (12), **DEL** (13), **SAVE** (14), **VERIFY** (15), **LOAD** (16).
  - Instrument labels under the keys: BD (1, 2), SD (3, 4), LT (5, 6), MT (7, 8), HT (9, 10), RS (11), HC (12), HH CLOSED / OPEN / CLOSED (13, 14), CRASH (15), RIDE (16) [OM p.4, p.10].

  **CONFIRMED.**
- **Instrument key semantics** [OM p.7, p.10]:
  - Two-key instruments: the **left key = accented, right key = normal** (BD 1/2, SD 3/4, LT 5/6, MT 7/8, HT 9/10, CH 13/14).
  - **OH = keys 13 + 14 pressed together**, which plays at "normal volume".
  - Pressing both keys of BD/SD/toms in INSTRUMENT SELECT enters **Flam Write** for that voice.

  **CONFIRMED.**

### 1.3 Rear panel ([OM p.6, p.39]; [SN p.1, p.12])

Left to right:
- **CARTRIDGE** slot (M-64C).
- **MASTER OUT R(MONO), L**: 6 Vp-p, 1 kΩ. With nothing in L, the L jack's switch contact ties the L signal onto the R(MONO) node, so a single plug in R carries a mono sum (§6).
- **MULTI OUT** ×10: RIDE, CRASH, HI HAT, CLAP, RIM SHOT, HI TOM, MID TOM, LOW TOM, SD, BD (the OM drawing on p.6 is cropped after LOW TOM; [SN p.12] lists all ten).
- **SYNC IN** (5-pin DIN).
- **EXT CONTROL**: **START/STOP** (footswitch DP-2) and **TRIG OUT (RIM SHOT)**.
- **TAPE MEMORY/SYNC**: **SAVE/OUT** and **LOAD/IN**.
- **MIDI IN**, **MIDI OUT ×2**.
- Power switch and AC inlet.

**CONFIRMED.**

**Panel-drawing conflict:** the small rear-panel drawing in [OM p.38] labels the DIN jack "SYNC OUT". The schematic [SN p.12] shows only a "SYNC IN" DIN socket, wired to CPU INT1 and to PC5/PC6 inputs. [SN p.1], [OM p.35] ("Here, the TR-909 serves only as a slave unit") and [OM p.39] also say "Sync In". **Resolved: input only. The p.38 label is a drawing error. CONFIRMED.**

---

## 2. Sequencer architecture

### 2.1 Hardware ([SN p.4, p.5, p.8])
- CPU **NEC µPD7811G-033/036 (IC604)**, **12.0 MHz crystal** (C608/C609 10 pF). EPROM IC609 is an M5M2764 (8 KB), 250 ns; a 2364 mask ROM was used in later units. RAM IC608 is a TC5565 (8 KB static), battery-backed by two 1.5 V cells [OM p.5]. **CONFIRMED.**
- Port use [SN p.5]:
  - **INT1** = clock input from DIN.
  - **PC0/PC1** = MIDI TX/RX serial.
  - **PC2** = footswitch.
  - **PC3** = tape data input.
  - **PC4** = tape-sync output.
  - **PC5/PC6** = DIN Start/Stop and Continue inputs.
  - **AN** inputs = TEMPO pot and TOTAL ACCENT pot.

  **CONFIRMED.**
- The CPU sends TRIG and ACCENT data to the voicing board over the data bus. The address decoders IC612/IC613 (LS138) strobe them into ten 4174 hex D-latches (IC1 to IC10 on the voicing board) [SN p.4, p.5, p.8]. **CONFIRMED.**
- The analog TEMPO and TOTAL ACCENT values are therefore **quantised by the CPU's A/D converter**. The µPD7811's converter is 8-bit (my recollection of the part; no datasheet was retrieved), so there are at most 256 tempo steps and 256 accent steps before the firmware's own mapping. **UNVERIFIED.** (Fact-check: the datasheet of the CMOS successor µPD78C11A describes an "8-bit A/D converter" with 8 analog inputs, https://www.cpcwiki.eu/imgs/4/4b/UPD78C11A_Datasheet.pdf. That supports 8 bits but is not the NMOS µPD7811 datasheet, so the tag stays UNVERIFIED.)

### 2.2 Pattern memory ([OM p.12, p.13, p.17, p.39]; [SN p.1, p.16])

```
2 banks (I, II)                                  -- selected with SHIFT+BANK I/II (in Track mode)
  × 3 pattern groups (PATTERN 1..3)
     × 16 patterns (main keys 1..16)             = 48 patterns per bank, 96 total
        each pattern: LAST STEP 1..16, SCALE, SHUFFLE 1..7, FLAM interval 9..16,
                      rows: TOTAL ACCENT, BD, SD, LT, MT, HT (off/normal/accent/flam),
                            RS, HC, CRASH, RIDE (off/on), HH (off/CH/CH-accent/OH)
        + optional EXT pattern (16 external rows + accent) sharing scale/last step/shuffle
  4 tracks per bank; 896 measures per bank shared across the 4 tracks
```
- "Each Bank consists of 3 Pattern Groups, and 16 Rhythm Patterns can be written in each Group. Therefore, 48 (16 × 3) kinds of Rhythm Patterns are available within a Bank" [OM p.12]. **CONFIRMED.** So the common assumptions of "4 banks" and "pattern A/B" do not apply: **the original has no A/B variations, intro or fill slots, or auto-fill** (those are TR-808 features). Pattern groups 1 to 3 are the nearest equivalent of banks.
- **Hi-hat is one pattern row.** The rhythm-chart sheet [OM p.40] and the external-instrument figure [OM p.37] show one HI-HAT row whose cells are small dot = CH normal, large dot = CH accent, ringed dot = Open. CH and OH therefore **cannot sound on the same step**. They also share one TRIG line plus a CLOSED/OPEN level line [SN p.4: IC10 "Hi-HAT Trig", "Close/Open"]. **CONFIRMED.**
- **RS and HC** are separate rows and triggers but share accent DAC IC8. **CRASH and RIDE** are separate rows and triggers but share accent DAC IC9 [SN p.4]. **CONFIRMED.**
- Per-voice step states [OM p.11, p.25]:
  - **BD/SD/LT/MT/HT/CH**: each key press cycles **off → normal (LED dim) → accent (LED bright)**.
  - **RS, HC, OH, CRASH, RIDE**: cycles **off → "Written with accent" (LED bright)**. As printed in [OM p.11] these voices have no separate normal level: they always use their single on-level, which still responds to Total Accent.

  **CONFIRMED as printed**. How the single on-level relates to the two-key voices' "accent" DAC code is **UNVERIFIED**.
- **Flam** is a per-voice, per-step state: "It is not possible to dub a voice with Flam on the one without Flam … new data always has priority" [OM p.26]. **CONFIRMED.**
- SysEx bank dump: **16 blocks × 512 data bytes**, each byte carrying one nibble ("0000 xxxx / 0000 yyyy → yyyyxxxx"), so **4096 bytes of real data per bank** [SN p.16] [DERIVED: 16 × 512 / 2]. The M-64C cartridge holds 4 × TC5517 (2 KB) = 8 KB [SN p.13] = **2 banks** [DERIVED]. That matches [OM p.33], where Bank I of the unit is saved to Bank II of the cartridge. **CONFIRMED / DERIVED.**
- Factory content: demonstration data in **Bank I, Track 1, Patterns 1 to 8** [OM p.7; SN p.7]. Reload by holding TRACK 1 + PATTERN 1 at power-on. That function is missing in the third-party v4 ROM [RW]. **CONFIRMED.**

### 2.3 Scale (step resolution) ([OM p.13, p.24]; [TR09])

The SCALE button cycles **16th → 32nd → 8th-note triplet → 16th-note triplet** ("1 Step = 1 Key"; the icon sequence is printed in [OM p.24]). The four printed strips above the main keys show the beat grid for each. [TR09] confirms the four values: "16th note triplet: six steps will be one beat; 8th note triplet: three steps; 32nd note: eight steps; 16th note: four steps". The default is 16th. **CONFIRMED.**

| Scale | Steps per beat | Step length (beat) | Ticks @24 ppqn | Ticks @48 ppqn | 16 steps = | Auto LAST STEP on change |
|---|---|---|---|---|---|---|
| 16th | 4 | 1/4 | 6 | 12 | 1 bar of 4/4 | 16 |
| 32nd | 8 | 1/8 | 3 | 6 | ½ bar (chain 2 for a bar) | 16 |
| 8th triplet | 3 | 1/3 | 8 | 16 | (12 steps = 1 bar) | **12** |
| 16th triplet | 6 | 1/6 | 4 | 8 | (12 steps = ½ bar) | **12** |

- Tick columns: [DERIVED] (24/steps-per-beat and 48/steps-per-beat). The auto-12/16 rule: [OM p.24] "the last step is automatically set 16th, 32nd ⇒ 16; 8th-trip, 16th-trip ⇒ 12". **CONFIRMED.**
- Scale must be set while running in Step/Tap Write [OM p.24]. Scale, last step, shuffle and flam are **per pattern, not per voice** [OM p.37 "Scale, Last Step and Shuffle data are common" to the internal and external rows; REV]. **CONFIRMED.**
- "All the Rhythms are in tune with each other" [OM p.24]: tempo is always quarter-note based, and the scale changes the step length only. **CONFIRMED.**
- Chained patterns "play in the tempo as they would usually do. You need to change the Scale before writing … so that they will play in the same tempo throughout" [OM p.28]. Each pattern in a chain keeps its own scale. **CONFIRMED.**

### 2.4 Last step ([OM p.24])
"While holding the Last Step Button, set any number you like by using the Main Keys" (1 to 16), while running in write mode. Pattern length 1 to 16 steps. **Last step is per pattern; there is no per-instrument last step (polymeter) on the original.** **CONFIRMED.**

### 2.5 Shuffle ([OM p.26]; [KVR]; [MW])

Primary text [OM p.26]:
- "This function is available only when [16th] or [32nd] Scale is selected."
- "seven options for [16th] and four for [32nd]".
- "Scale [16th]: 1 gives no effect, and faster bounce will be obtained towards 7."
- "Scale [32nd]: There is no difference between 1 and 2, 3 and 4 or 5 and 6 … four options of bounce speeds; 1(=2), 3(=4), 5(=6) and 7."
- "If the total number of measures [steps] are odd number, the last step will not take on the bounce."
- Set with SHUFFLE/FLAM + main key 1 to 7, while running in step write. **CONFIRMED.**

Measurement [KVR] (16th scale, rimshots recorded at 96 kHz, slowest tempo): "delays each even-numbered 1/16th by 2/96 of a beat for shuffle setting 1, 4/96 for 2 … 12/96 for 6". The six "settings" are the six non-zero panel levels 2 to 7. **UNVERIFIED** for the magnitudes (corrected by fact-check: was CONFIRMED). The level count and "1 = off" are primary [OM p.26], but the delay values rest on this one forum measurement. [MW] only restates the level count and measures nothing. The magnitudes are internally consistent with the [OM p.26] 32nd-scale pairing on a 1/48-beat grid (see below), and the quote was re-read at the URL during the fact-check.

| Panel key n | Delay of even steps (beat) | @48 ppqn ticks | @96 ppqn | ms @120 BPM | Swing % (16th) |
|---|---|---|---|---|---|
| 1 | 0 | 0 | 0 | 0 | 50.0 |
| 2 | 1/48 | 1 | 2 | 10.4 | 54.2 |
| 3 | 2/48 | 2 | 4 | 20.8 | 58.3 |
| 4 | 3/48 | 3 | 6 | 31.3 | 62.5 |
| 5 | 4/48 | 4 | 8 | 41.7 | 66.7 |
| 6 | 5/48 | 5 | 10 | 52.1 | 70.8 |
| 7 | 6/48 | 6 | 12 | 62.5 | 75.0 |

[DERIVED]: delay = (n − 1)/48 beat; swing % = 50 + 50·delay/(1/4 beat).

**32nd scale [DERIVED, consistent with OM p.26]:** delay = floor((n − 1)·S/12) ticks @48 ppqn, where S is the step length in 48-ppqn ticks (12 for 16th, 6 for 32nd). For the 32nd scale this gives 0, 0, 1, 1, 2, 2, 3 ticks for n = 1 to 7, which reproduces the manual's pairings 1 = 2, 3 = 4, 5 = 6, 7 exactly. Maximum = 3/48 beat = 50 % of a 32nd step = 75 % swing, the same proportion as the 16th maximum. The integer-division rule is my reconstruction (**UNVERIFIED** against the ROM), but it is the simplest rule that fits both the pairing statement and the 48-ppqn grid.

**Which steps:** "even-numbered" 16ths (2, 4, 6 …, counting from 1) [KVR; MW corrected post]. They are counted per pattern from step 1. With an odd LAST STEP the final (odd) step stays undelayed [OM p.26], so the alternation restarts each pattern repeat. **CONFIRMED.**

Not on the original: per-voice shuffle, negative swing, shuffle in triplet scales. The Revolution 909 v5 firmware adds "shuffle and flam … on all 4 scales" [REV], which implies the original lacks this.

### 2.6 Flam ([OM p.10, p.26]; [EL-FLAM]; [RC909]; [NAVA])

Primary [OM p.26]:
- "You can write Flam into the Bass Drum, Snare Drum, Low Tom, Mid Tom, or Hi Tom. The eight options of Flam interval … can be selected by using the Main Keys" (SHUFFLE/FLAM + keys **9 to 16**).
- "Longer interval is obtained towards 16."
- "the first sound of Flam is in time for a step, and the second one will follow. The position of the Tempo Knob does not affect the interval."
- Entry: INSTRUMENT SELECT + both keys of the voice, then tap steps.
- Flam is not available for external instruments [OM p.37].

**CONFIRMED.** So the flam's **grace note is on the grid and the main note is late**, by a fixed number of milliseconds.

| Key | [EL-FLAM] measured pulse spacing (min) | [RC909] / [NAVA]-comment spacing |
|---|---|---|
| 9 | 10 ms (6 min) | 20 ms |
| 10 | 14 (10) | 24 |
| 11 | 18 (14) | 28 |
| 12 | 22 (18) | 32 |
| 13 | 26 (22) | 36 |
| 14 | 30 (26) | 40 |
| 15 | 34 (30) | 44 |
| 16 | 38 (34) | 48 |

**CONFLICTING.**
- [EL-FLAM] (EchoBoy, 2017, measured on a TR-909: "the first pulse is the initial trigger – a few ms later the TR generates a second trigger") gives 10 to 38 ms. The "(x ms min)" figures are not a continuous spread. EchoBoy writes that the second trigger shows up at **two discrete positions** because of sequencer jitter, and that the later one (the listed value) "is played way more often". So the second hit lands either on the listed value or **4 ms early**, which points to a ≈ 4 ms flam timer tick (the same as the 4 ms step between settings) (corrected by fact-check: was "the ≈ 4 ms spread fits the ≈ 2 ms CPU timing granularity").
- [RC909] specifies "TYPE2–9: 20–48 msec in 4 msec units" (TYPE1 = 0 ms).
- The [NAVA] firmware carries the comment "flam timings from EchoBoy (in ms: 20, 24, 28, 32, 36, 40, 44, 48)", but its timer table (`OCR3A` = 4999, 5999, 7999, 10999, 14999, 19999, 25999, 29999 at 4 µs per count = 20, 24, 32, 44, 60, 80, 104, 120 ms) does not match its own comment. So [NAVA] is not usable as evidence.
- The two credible series both use **4 ms steps** and differ by a constant 10 ms. **Recommendation:** implement `flamMs = base + 4·(k − 9)` with base selectable, defaulting to 20 ms (Roland's own modern spec), and note 10 ms as the measured alternative. A ROM disassembly or a fresh scope capture would settle it.
- **Levels of the two flam hits:** no source states whether the first (on-grid) hit is weaker or whether accent applies to both hits. **UNVERIFIED.**
- **Trigger mechanics [DERIVED from SN p.4 to p.5]:** each flam hit is a separate 2 ms TRIG latch write. The voice's accent DAC is written "almost concurrent" with each TRIG and holds between writes. Both hits therefore excite the analog circuit from its running state, with no reset.

### 2.7 Pattern write: step write vs tap write ([OM p.9 to p.14, p.25])
- **Step write** (SHIFT + TEMPO = STEP): with the pattern running, hold INSTRUMENT SELECT and press the voice key; then each main key toggles that step through the voice's states. The LED chase shows the playhead (left to right) [OM p.9, p.10]. **CONFIRMED.**
- **Tap write** (SHIFT + BACK = TAP): while running, tap the instrument keys in real time. The left key writes an accented hit and the right key a normal one. CYCLE/GUIDE turns on a **rim-shot metronome on quarter notes**. Entries are **quantised to the current scale step** ("It is not possible to write shorter timing than the one set with the Scale") [OM p.14, p.25]. **Total Accent cannot be written in tap mode** [OM p.25]. Erase while running: CLEAR + voice key [OM p.14]. **CONFIRMED.** The rounding rule (nearest step versus floor) is **UNVERIFIED**; [NAVA] counts a tap into the next step after half a step, i.e. rounds to nearest.
- **Total Accent row:** INSTRUMENT SELECT + TOTAL ACCENT (ENTER) key, then main keys [OM p.25]. **CONFIRMED.**
- **Clear a pattern:** CLEAR + main key (pattern number) in write mode, while stopped [OM p.9, p.14]. **CONFIRMED.**
- **Copy pattern:** select the destination, then SHIFT + COPY + BANK + PATTERN group + key + ENTER. This works across groups and banks [OM p.27]. **CONFIRMED.**
- **Chain** (pattern write, adjoining patterns only): hold the first and last keys, press ENTER; release with key + ENTER. Even a whole group of 16 can be chained. Chains then count as one pattern in pattern play and track program [OM p.28]. **CONFIRMED.**

### 2.8 Pattern play ([OM p.14])
- Select group + key, START. "The new Rhythm will be played from the beginning of the next measure" when changed during play. Unlike the 808, there is no first-beat exception in the 909 manual. **CONFIRMED.**
- **Block pattern play:** press two main keys together after the group button, and all patterns between them play in sequence [OM p.14]. **CONFIRMED.**
- Patterns from Bank I and Bank II cannot be played continuously, because changing bank needs Track mode [OM p.13]. **CONFIRMED.**

### 2.9 Track (song) mode ([OM p.15 to p.18, p.27 to p.31])
- 4 tracks per bank. Up to **896 measures per bank, shared across the 4 tracks** ("this does not mean 224 for each Track") [OM p.17]. AVAILABLE MEAS shows the remaining capacity [OM p.17]. **CONFIRMED.**
- **Track program:** SHIFT + TRACK n; clear with SHIFT + DEL + LAST MEAS + ENTER. Then START, and for each measure pick a pattern (group + key) and press ENTER, which advances the measure counter. STOP ends [OM p.15]. Each measure stores **a pattern reference only**; scale, length, shuffle and flam come from the pattern. A track cannot use patterns from the other bank [OM p.13 note 2]. **CONFIRMED.**
- **Edit:** FWD/BACK or SHIFT + measure number + ENTER to navigate; replace, DEL (single or range or to end), INS, COPY of a range (the destination shifts later measures, and copying into the middle of the source range is not possible) [OM p.16, p.28 to p.31]. **CONFIRMED.** A known bug: single-measure copy in TRACK program did not work on v1, and is fixed by [REV] v5. **UNVERIFIED** (vendor claim).
- **Track play:**
  - Stops automatically at the end.
  - **CYCLE** makes it loop; pressing CYCLE during play stops at the end of the song.
  - STOP/CONT pauses and resumes.
  - Start from a set measure: SHIFT + number + ENTER, then STOP/CONT.
  - The main keys can be played live over the track [OM p.18].

  **CONFIRMED.**
- MIDI Song Select (track number 0 to 7 = Bank I tracks 1 to 4, Bank II tracks 1 to 4) and Song Position Pointer are sent and received in TRACK PLAY/STOP [SN p.15]. **CONFIRMED.**

### 2.10 External instrument mode ([OM p.37])
SHIFT + EXT INST toggles each pattern's **external layer**: 16 instrument rows plus an Accent row, sent from MIDI OUT. They share scale, last step and shuffle with the internal layer; there is no flam. The main keys map to notes; the keyboard figure shows keys 1 to 16 as consecutive semitones from the C two octaves below middle C, i.e. **notes 36 to 51** [DERIVED from the OM p.37 figure]. **CONFIRMED** (mode); note numbers **UNVERIFIED**.

---

## 3. Accent

### 3.1 The two accents ([OM p.5, p.25])
- **Accent A = "Total Accent"**: "set by each step. All the voices in that step will take on accents." Its strength is set by the TOTAL ACCENT knob. **CONFIRMED.**
- **Accent B = "Accent"** (per instrument): "available only for Bass Drum, Snare Drum, Low Tom, Mid Tom, Hi Tom, and Closed Hi-hat". "The intensity of this Accent cannot be changed." **CONFIRMED.**
- Interaction: "It is possible to set both Total Accent and Accent at the same time, but if the Total Accent Knob is set high, Accent may not be noticed" [OM p.25]. **CONFIRMED.** This points to **additive combination with saturation at the DAC ceiling**, which matches "the two accents are additive … the addition happens at the processor side" [EM-ACC] (**UNVERIFIED** secondary, but consistent with the hardware in §3.2).

### 3.2 How the accent voltage is made ([SN p.4, p.5, p.11]; [JV]; [EL-ACC])
- "Accent data on the CPU bus is latched into one of ACCENT latches (IC2–IC9) selected by Address Decoder (IC612, 613). Latched ACCENT code is converted to analog equivalent at the output of associated resistor array RM0621. The voltage is clamped to the level until it is replaced by the next incoming data." [SN p.5]. **CONFIRMED.**
- "Almost concurrent with ACCENT, TRIG is latched into IC1 or IC10, and appears as 5V positive going pulse on the correct output pin for **2ms**. TRIG is used either solely or in combination with ACCENT to reset generator(s) and to create various envelopes …" [SN p.5]. **CONFIRMED.**
- Latch/DAC map [SN p.4 block diagram]. Each is a 6-bit 4174 driving a 6-bit D/A with output "0–5V":
  - IC2 BD.
  - IC3 SD.
  - IC4 LT.
  - IC5 MT.
  - IC6 HT.
  - IC7 HI-HAT.
  - **IC8 "RS, CP"**.
  - **IC9 "CYMBAL"** (crash and ride).

  Trigger latches: **IC1** = BD, SD, LT, MT, HT; **IC10** = RS, HC, RIDE, CRASH, HH, CLOSE/OPEN. **All ten 4174 latches (IC1 to IC10)** have their reset (pin 1) on the common power-on RESET line from CN1 pin 7: "The RESET is also routed to … TRIG and ACCENT latches (IC1-IC10)" [SN p.6, p.11] (corrected by fact-check: was "IC1 has a RESET input"). **CONFIRMED** (and [JV]: "8 6-bit resistor ladder DAC's (IC2-IC9)"; [JV] counts three shared DACs because it treats CH/OH as a pair).
  - **Designator conflict (added by fact-check):** the p.4 block diagram labels IC8 "RS, CP" and IC7 "HI-HAT". On the p.11 circuit diagram the output of IC7's network RA6 runs straight up (one unbroken trace) to the RS accent buffer IC48b (+) and on to the HC buffer IC26a. That makes IC7 the RS/HC DAC and IC8 the hi-hat DAC, which is what [JV] says. Function is unaffected: RS and HC share one DAC, whichever number it carries. **CONFLICTING** (designators only); the circuit diagram is the more specific source.
- **RM0621 network** [SN p.11 legend]: six resistors **R, 2R, 4R, 8R, 16R, 32R with R = 5 kΩ** (5, 10, 20, 40, 80, 160 kΩ) summed into one node. This is a binary-weighted resistor DAC driven by CMOS 4174 outputs at 0/5 V. **CONFIRMED.**
  - [DERIVED] Unloaded output: V = 5 V · code/63, where the MSB drives R = 5 k. LSB ≈ **79 mV**. Source impedance = 5k ∥ 10k ∥ … ∥ 160k = **2.54 kΩ**.
  - **Fact-check correction:** the voice inputs checked do **not** load the node. The text was "every voice input that loads the node compresses the top of the range". On [SN p.11] each DAC node checked drives the (+) input of a non-inverting op-amp buffer with 10 kΩ feedback and 4.7 kΩ to ground:
    - BD: IC11b, R37/R38.
    - RS: IC48b, R406/R397.
    - HC: IC26a, R212/R206.
    - Toms: IC18b, R119/R120.

    The node therefore sits at the unloaded 5·code/63 V. Each buffer has a gain of 1 + 10/4.7 = **3.13**, so its output spans 0 to **15.6 V** nominal, which is more than the ±15 V op-amp can swing. If the output tops out at about +13 to +14 V, then **DAC codes above about 52 to 56 (of 63) clip in the buffer**. That gives a hardware mechanism for "Accent may not be noticed" at a high TOTAL ACCENT setting [OM p.25]. The SD, HH and cymbal accent inputs were not traced. [DERIVED]; the clip point depends on the op-amp's real swing and is **UNVERIFIED**.
- **Holding behaviour:** "If the instrument is not triggered, the accent control signal keeps the value of the last step in which the instrument was played" [EL-ACC]. This matches [SN p.5] "clamped … until replaced". **CONFIRMED.**
  - [DERIVED] Consequences:
    - A shared DAC (RS/HC, CRASH/RIDE) is rewritten whenever either partner fires, so a still-ringing partner's VCA/envelope input jumps at that moment. That only matters where the circuit reads the accent voltage continuously, not just at the trigger. The ride and crash anti-log VCAs do read it continuously (§3.3).
    - When RS and HC (or crash and ride) fire on the same step they **must share one accent level**.
- Measured old-versus-clone behaviour [EL-ACC]: on the 909, "the amplitude of the ENV2 without accent is half of the amplitude of the ENV2 with accent" (the Nava clone gave 2/3). **UNVERIFIED** (one forum measurement; the knob setting was not stated).
- [MEASURED], from the hand-drawn scope traces in [SN p.9], which were "Observed at MULTI OUT jack with all knobs set to center" (so TOTAL ACCENT at 12 o'clock), by pixel extent: BD with/without accent peak-to-peak ratio ≈ 2.6 (+8.4 dB), positive-peak ratio ≈ 2.3 (+7.1 dB); SD p-p ratio ≈ **2.2 (+6.9 dB)**, positive-peak ≈ 2.1 (+6.4 dB) (corrected by fact-check: SD was "≈ 2.5 (+8 dB)"; re-measured at full resolution on the 8110 px scan at three thresholds, giving 2.19 to 2.23; the BD figures reproduced at 2.62 and 2.25). These are drawings, not photographs, so treat them as ±2 dB at best.
- **Exact firmware codes** (normal / per-voice accent / total-accent knob mapping, and their sum) are **not documented anywhere I found**. **OPEN QUESTION.** Getting them needs either a ROM disassembly (µPD7811 code, the v1/v2 2764 dumps that circulate) or a measurement of the latch outputs (DAC node voltage per case) on a real unit.

### 3.3 Where the accent voltage goes in each voice ([SN p.4 block diagram, p.5 to p.6, p.11])
The block diagram draws a multiplier (⊗) of "Accent Level" × "Trig" for each analog voice. This is the TRIG-gated transistor switch that admits the held DAC voltage only during the 2 ms trigger. Per voice:

| Voice | Accent path (designators per [SN p.4, p.5, p.11]) | Effect |
|---|---|---|
| BD | "Bass Drum Accent Level" ⊗ "Bass Drum Trig" feeds ENV3 (pitch envelope), the Pulse Gen (Q8/Q9, attack click) and ENV1 (VCA Q12) | Accent changes pitch sweep depth, click and amplitude together (nonlinear) |
| SD | Drum: VCA Q50 "gain follows ENV 3 which is in turn controlled by an ACCENT coming through Q4 currently gated by the TRIG" [SN p.6]. Snappy: "ACCENT signal is gated through Q41 by the trigger from Q39 collector … coupled to the base of Q47 VCA as ENV 5" [SN p.6]; ENV4 start level via D60 [RW] | Both tone and snappy scale with accent |
| LT/MT/HT | "Tom Accent Level" ⊗ "Low Mid Hi Tom Trig" into ENV1 to ENV3 / VCA1 to VCA3 | Amplitude and envelopes |
| RS | "Rim Shot Accent Level" ⊗ "Rim Shot Trig" into ENV (Q64), then VCA Q65 | Amplitude; shares DAC IC8 with HC |
| HC | "Hand Clap Accent Level" ⊗ "Hand Clap Trig" via IC26a, Q35/Q36 | Amplitude; shares DAC IC8 with RS |
| HH (sample) | "HI-HAT Accent Level" into **Anti Log (Q84)** into VCA Q85, with ENV Q72 to Q74 | Gain after the 6-bit DAC; exponential (anti-log) law |
| CRASH/RIDE (sample) | "CYMBAL Accent Level" into **Anti Log (Q70/Q68)** into VCA Q71/Q69 (crash/ride) | Gain after the DAC; shared DAC IC9 |

**CONFIRMED** (block diagram and circuit description). The detailed transfer curves belong to files 01 and 02.

**Implementation guidance:**
- Model the accent as a **per-voice held control voltage** `Vacc[v]` (0 to 5 V, 6-bit quantised), written at each trigger of voice v and held until the next. For RS/HC and CRASH/RIDE use **one shared value per pair**.
- `code = clamp(baseCode + (voiceAccent ? kVoice : 0) + (totalAccentStep ? round(kTotal·knob) : 0), 0, 63)`, with baseCode, kVoice and kTotal **to be fitted** to targets: roughly 2× (+6 dB) ENV amplitude for a voice accent [EL-ACC], about +8.4 dB p-p (BD) and +6.9 dB p-p (SD) output at TOTAL ACCENT centre [SN p.9 MEASURED] (corrected by fact-check: was "+7 to 9 dB"), and "Accent may not be noticed" at a high knob (saturation) [OM p.25].
- Feed `Vacc` into the circuit model where the schematic does: excitation and envelope start for the analog voices, anti-log VCA control for the sample voices. Never apply it as a plain post-gain.

---

## 4. Timing internals

- **Clock source:** the internal tempo comes from the 12 MHz CPU, with the tempo pot read through the A/D converter [SN p.5, p.8]. This is unlike the 808/606, whose tempo comes from an analog oscillator. [MW] has a poster first claiming an analog tempo oscillator polled every 2 ms, then correcting himself: "It's the 606 and 808 that have the analogue oscillator for tempo. The 909 has an internal timer in the CPU." **CONFIRMED** (schematic: no RC tempo oscillator; VR601 goes to the AN input).
- **External time bases:**
  - MIDI clock (24 ppqn, F8).
  - DIN SYNC IN, "synced to the positive going edge of clock pulses from DIN jack" [SN p.15]. The rate is 24 ppqn, as on Roland's MC-4/MC-202 masters [OM p.35; DIN]. **CONFIRMED.**
  - Tape sync, FSK. "In normal PLAY MODE … The CPU changes frequency between 1200 and 2400 Hz at every half-period of T which is 1/24 of the time required … to process a quarter note"; in STOP a continuous 1200 Hz is sent [SN p.6]. **CONFIRMED.** [DERIVED]: one frequency change every T/2 = 1/48 beat.
- **Internal resolution:** [DERIVED] the shuffle grid of 1/48 beat [KVR], the 32nd-note shuffle pairing [OM p.26] and the 1/48-beat tape-sync toggle [SN p.6] all point to an internal **48 ppqn** time base. (Fact-check: this is a lower bound. The tape-sync signal is a 24-ppqn square clock with a 50 % duty cycle, i.e. one 1200/2400 Hz change every 1/48 beat, so it shows that the CPU schedules events at least every 1/48 beat. It does not exclude a finer internal grid.) When slaved to 24 ppqn MIDI/DIN, the CPU must interpolate the half-clock shuffle offsets; how it does so is **UNVERIFIED**. The "96 ppqn internal" figure in the brief appears in no primary source (the [KVR] post expresses delays in 96ths but they are all even). **UNVERIFIED.**
- **Trigger width:** 2 ms at 5 V from the 4174 latches [SN p.4, p.5]. **CONFIRMED.**
- **Jitter (measured by others):**
  - [GS-JIT], rimshot 16ths, MLA analysis:
    - Internal: **RMS 1.16 to 1.18 ms, peak 2.19 to 2.30 ms**.
    - DIN-sync slaved: RMS 1.16 to 1.26 ms, peak 2.16 to 2.33 ms.
    - MIDI-clock slaved: RMS 1.13 to 1.19 ms, peak 2.19 to 2.24 ms.

    **UNVERIFIED** (search excerpt; page blocked).
  - [LIT] Innerclock "Roland TR-909 Version 1.0 … maximum variance between consecutive Sixteenth Note intervals of 214 Samples [4.46 ms]" (internal, 48 kHz); also quoted as 307 samples (6.40 ms) on DIN sync and 374 samples (7.79 ms) on MIDI clock. **UNVERIFIED** (secondary quotes).
  - [ICS] Innerclock: the step trigger "has no higher interrupt priority than any other task … when the CPU has to deal with some other task everything else takes a back seat"; "a sloppy beast". Qualitative, **UNVERIFIED**.
  - [MW] repeats Colin Fraser's (unsourced) description of a cyclic ≈ 1 ms delay on alternate 16ths caused by 2 ms polling. **UNVERIFIED.**
  - Reading these together: the 909 jitters about as much as the 808 (≈ 2 ms peak; see mw808 research 03 §4). It has **no** long-term tempo drift internally, since the time base is a crystal.
- **MIDI-to-trigger latency:** no measurement found. **UNVERIFIED / OPEN.** [DERIVED] lower bound: a 3-byte Note On takes 3 × 10 bits / 31 250 baud = **0.96 ms** on the wire. Firmware scan granularity (≈ 2 ms, if the 2 ms TRIG period is also the output-update tick) adds 0 to 2 ms. Users report 10 to 20 ms subjective offsets (gearspace threads, unmeasured; **UNVERIFIED**).
- **Firmware timing bugs:**
  - [SN p.7]: "When synchronizing to MIDI clocks, there are glitches. TR-909 sometimes falls behind if STOP is pressed, then CONTINUE is pressed … Software revision 2 cures this". **CONFIRMED.**
  - [RW]: v4.0 fixes "TR-909 will rarely miss-read a $F8 (MIDI Clock) just after Start data" when Start-to-first-clock is ≈ 1 ms. **UNVERIFIED** (secondary).
  - [REV]: "The first MIDI clock is no longer missed (v1 bug) … no longer freezes when it is synchronized to DIN (v4 bug)". **UNVERIFIED** (vendor).
- **Start/Stop rules:** in MIDI or DIN tempo mode "only the external unit can start or stop the units" [OM p.34]. The unit always powers up in INT mode [OM p.34]. **CONFIRMED.**

**Recommendation for mw909:**
- Run sample-accurate by default, on a 96 ppqn internal grid so that 48-ppqn semantics are exact.
- Put shuffle on a (n−1)/48-beat table for the 16th scale and floor((n−1)/2)/48 for the 32nd scale.
- Make flam a fixed-ms offset.
- Offer an optional "Vintage timing" mode: a per-step onset offset uniform in 0 to 2 ms on a free-running 2 ms grid (all voices on a step move together). Add the flam's measured behaviour: the second hit occasionally lands 4 ms early, and never late (corrected by fact-check: was "the flam's measured ±2 ms spread"; see §2.6).

---

## 5. Connectivity

### 5.1 MIDI ([SN p.14 to p.16]; [OM p.38])

**Receive:**
- Note On (trigger), notes **35 to 51**. Velocity **1 to 127** controls the dynamics. Note Off and velocity 0 are ignored ("Note On message works as a trigger pulse").
- Mode messages 124 to 127.
- Timing Clock (only in TEMPO MODE = MIDI), Start/Continue/Stop, SPP and Song Select (TRACK PLAY/STOP), System Reset.
- Power-up: **channel 10, OMNI ON**. The OM says the receive channel shows "0" = Omni. Set the channel with SHUFFLE/FLAM + main key, which also sets OMNI OFF/POLY (Mode 3).

**CONFIRMED.**

**Transmit:**
- Note On for the internal pattern, notes **36 to 51**, velocity **64 to 96 "(accent min–max)"**, then Note Off as velocity 0.
- Mode messages (OMNI OFF + POLY ON when the transmit channel is set).
- Clock / Start / Continue / Stop. MIDI clock follows the active tempo source: internal, MIDI or DIN.
- SPP and Song Select.
- Power-up transmit channel **11**; set with LAST STEP + main key.

**CONFIRMED.**

**Thru:** "All valid MIDI IN messages are transferred to MIDI OUT except Timing Clock and System Exclusive". There are two MIDI OUT jacks wired in parallel (R600/R602 220 Ω) [SN p.12, p.15]. **CONFIRMED.**

| Note | Voice | | Note | Voice |
|---|---|---|---|---|
| 35, 36 | Bass Drum | | 45, 47 | Mid Tom |
| 37 | Rim Shot | | 46 | Open Hi-Hat |
| 38, 40 | Snare Drum | | 48, 50 | High Tom |
| 39 | Hand Clap | | 49 | Crash Cymbal |
| 41, 43 | Low Tom | | 51 | Ride Cymbal |
| 42, 44 | Closed Hi-Hat | | | |

"Two keys are for duplicating a sound with convenient key play" [SN p.14]. **CONFIRMED.** Which duplicate the 909 *transmits* is **UNVERIFIED**; the Tx range 36 to 51 excludes 35.

- **Velocity versus accent:** received velocity spans 1 to 127, and transmitted velocity spans "64–96 (accent min–max)" [SN p.15]. How Rx velocity maps onto the 6-bit accent codes (continuous or a few thresholds) is **UNVERIFIED**. Forum claims (https://gearspace.com/board/electronic-music-instruments-and-electronic-music-production/928151-tr909-accent-midi.html, blocked; search excerpt) say "velocity of 127 is the same as accent on full, lower velocities like lowering the accent amount". mw909 should map velocity continuously onto the accent code.
- **SysEx:** bank dump and load, Roland ID 0100 0001, op codes 0101 0000 (request), 0101 0001 (answer), 0101 0010 (data), 0101 0011 (ack), 0101 0100 (no data), abort 0111 0000. 16 blocks × 512 nibble-bytes plus checksum. TRACK PLAY/STOP only [SN p.16]. **CONFIRMED.**
- "While the Tape Interface is functioning (SAVE/LOAD/VERIFY), all MIDI routine is frozen" [SN p.15]. **CONFIRMED.**

### 5.2 DIN sync, tape sync, footswitch
- **SYNC IN** only: pin 1 Run/Stop, 2 GND, 3 Clock (Tempo), 5 Continue [SN p.1, p.12]. **CONFIRMED.** There is no DIN sync output; to clock other gear the 909 sends MIDI clock.
- **Tape sync:** SAVE/OUT emits the 1200/2400 Hz FSK while playing (1200 Hz steady when stopped; record at −10 to −3 VU). The TAPE SYNC button slaves the 909 to tape via LOAD/IN [OM p.35, p.36; SN p.6]. **CONFIRMED.**
- **Tape memory:** pilot tone ≈ 6 s, then ≈ 13 s of modulated data per bank. File numbers 0 to 999 [OM p.19, p.20, p.22]. Bits are coded by half-period length ("Under 300 µs … 0, Over 300 µs … 1"), and the save waveform shows a 3.2 kbaud D/A code [SN p.6]. **CONFIRMED.**
- **START/STOP** footswitch jack (DP-2) [OM p.38; SN p.12]. **CONFIRMED.**

### 5.3 Trigger output
- **TRIG OUT (RIM SHOT)**: "Trigger pulse of a positive 14V, **18ms** sent out in the same intervals as the Rim Shot written" [OM p.38], versus "+14V, **20 ms** pulse" [SN p.1; OM p.39]. **CONFLICTING (18 / 20 ms).** It follows the RS row, so programming RS with RS LEVEL at 0 gives a trigger track. The pulse is generated on the voicing board [SN p.7, p.11]. (Corrected by fact-check: was "buffered via R495 1 kΩ".) Reading the [SN p.11] circuit:
  - The RS TRIG line (the same node that feeds C111) drives Q66 through R392/R393 (1 MΩ/1 MΩ from S/N 415300; 22 kΩ/10 kΩ before).
  - Q66 discharges C114 (0.1 µF), which turns on the PNP Q67 through R409 (47 kΩ), with R410 (47 kΩ) across its base-emitter junction.
  - Q67's collector drives the jack through R413 (1 kΩ), with R412 (22 kΩ) to ground.
  - R495 (1 kΩ) is the **series feed from +15 V to Q67's emitter**, not an output buffer. It is a jumper before S/N 415300.

  [DERIVED] Output high ≈ 15 V − 0.65 V (R495) − V_CE(sat) ≈ **+14.1 V**, which matches "+14V". After the 2 ms TRIG, C114 recharges through R409 (τ = 4.7 ms). Q67 stays on for about another 13 to 14.5 ms, giving **≈ 15 to 17 ms** total at nominal values. With C114 tolerance, both printed figures are within reach, so this does not settle 18 versus 20 ms (UNVERIFIED estimate; it assumes Q67 is PNP, which is the only reading that gives a positive pulse).
- Production fix: C600 0.1 µF from the TRIG OUT hot to ground "for preventing RIM SHOT signal from being induced onto TAPE SYNC signal" [SN p.7, S/N 415300]. **CONFIRMED.**
- Plugin equivalent: a MIDI note-out on RS steps, or an audible 14 V/20 ms pulse on an aux output (as D16's products offer trigger outs).

### 5.4 Memory cartridge
M-64C, 2 banks. Supports save/load/play-from-cartridge. ENTER must be unlit (internal mode) during cartridge SAVE/LOAD [OM p.33; SN p.7, p.13]. **CONFIRMED.** (The OM spec page misprints it as "M-46C".)

---

## 6. Outputs and the mix bus ([SN p.11 voicing board "MASTER VOLUME", p.12 "MULTI JACK BOARD"])

**Topology (read from the 8110 px scan):**
```
voice amp out --R500..R527 1k--+--C500..C510 0.01µ (S/N 415300-up)--gnd
                               +--> MULTI OUT jack tip
                                    switched (normally-closed) contact --+-- R_R --> RMIX bus (CN6-45)
                                                                          +-- R_L --> LMIX bus (CN6-43)
LMIX --C154 1µ/50--> IC66a (−)  feedback VR28a (MASTER VOLUME, 50k B, dual) --R473 1k--C152 10µ--+
RMIX --C153 1µ/50--> IC66b (−)  feedback VR28b                               --R474 1k--C155 10µ--+
   (muting: Q82/Q83 2SC2878 shunt the outputs at power on/off, driven by Q79/D193-195, C143 47µ)
LOUT --R530 2.2k--+--C511 0.01µ--gnd , R531 10k--gnd --> L jack
ROUT --R532 2.2k--+--C512 0.01µ--gnd , R533 10k--gnd --> R(MONO) jack; with L unplugged, the L-jack switch ties the L node to the R node (mono sum)
```

**Mix resistors, from [SN p.12].** "Upper" and "lower" are the two resistors drawn per voice; the junction dots put every upper one on the line to CN-6 pin 45 and every lower one on the line to pin 43 (re-read by fact-check).

| Voice | Upper (→ pin 45) | Lower (→ pin 43) | Upper gain @VR28 = 50k | Lower gain | Upper − Lower |
|---|---|---|---|---|---|
| BD | R501 12k | R502 12k | +11.06 dB | +11.06 dB | 0 |
| SD | R504 12k | R505 12k | +11.06 | +11.06 | 0 |
| LT | R507 8.2k | R508 15k | +14.20 | +8.96 | **+5.25** |
| MT | R510 12k | R511 12k | +11.06 | +11.06 | 0 |
| HT | R513 15k | R514 8.2k | +8.96 | +14.20 | **−5.25** |
| RS | R516 12k | R517 12k | +11.06 | +11.06 | 0 |
| HC | R519 12k | R520 12k | +11.06 | +11.06 | 0 |
| HH | R522 15k | R523 8.2k | +8.96 | +14.20 | **−5.25** |
| CRASH | R525 12k | R526 10k | +10.93 | +12.52 | −1.58 |
| RIDE | R528 8.2k | R529 15k | +14.20 | +8.96 | **+5.25** |

(Corrected by fact-check. The gains were +11.7 / +14.7 / +9.9 / +13.2 dB and the pan differences ±4.8 and 1.45 dB. The old formula VR28/(R_bus + 1 k) gave every bus its own 1 kΩ, but R500 to R527 are **shared** by both mix resistors of a voice. Correct: node attenuation (R_up ∥ R_lo)/(1k + R_up ∥ R_lo) × VR28/R_bus, so the pan difference is simply 20·log(R_lo/R_up): 15/8.2 → 5.25 dB and 12/10 → 1.58 dB. Voice-amp output impedance is taken as 0.)

- **Which pin is which channel: CONFLICTING (added by fact-check).**
  - The multi-jack board [SN p.12] labels pin 43 "LMIX" and pin 45 "RMIX". Read that way, the upper resistors go to the right channel, which is how the earlier draft read it: LT/RIDE right, HT/HH/CRASH left.
  - The voicing board [SN p.11] labels the same pins the other way round: 43 "MIX IN (R)" and 45 "MIX IN (L)". Its drawn wiring agrees with its own labels:
    - pin 43 → C154 → IC66a → R473/C152 → pin 48 "MIX OUT (R)" → R532 → **Right/MONO jack**;
    - pin 45 → C153 → IC66b → R474/C155 → pin 47 "MIX OUT (L)" → R530 → **Left jack**.

    Both boards agree on the output pins 47 = L and 48 = R.
  - On the traced wiring, the upper resistors reach the **left** jack: **LT and RIDE louder left, HT, HH and CRASH louder right**.
  - Two of the three indications favour the trace, but only a recording of a real unit's L/R outputs settles it. **Verify on a unit.** The resistor values are **CONFIRMED** as read.
- Master volume: VR28 is the **feedback** resistor, so gain is proportional to rotation (linear B taper), and silence at MIN [DERIVED].

**Behaviours:**
- **Plugging a MULTI OUT removes that voice from both L and R** (switched jack; the mix resistors hang off the normally-closed contact). **CONFIRMED** from the schematic symbols. The individual output still follows that voice's LEVEL knob, because the level pot sits before the voice output amp [SN p.4 "Level"/"Amp" per voice].
- **HPF on the master [DERIVED]:** C153/C154 (1 µF) feed the op-amp virtual ground. The Thevenin source seen by each cap is all branches in parallel: (12k+1k)∥…≈ **1.22 kΩ (L), 1.24 kΩ (R)** with no jacks plugged. So f_c = 1/(2π·1.22k·1µ) ≈ **130 Hz**, first order, on every voice at the master out. (Fact-check: CONFIRMED. With each branch taken as R_bus + (1k ∥ R_other), R_th = 1.215 kΩ (pin 43 bus) gives 131.0 Hz and 1.236 kΩ (pin 45 bus) gives 128.8 Hz. The "(L)"/"(R)" labels here follow the jack-board labels, which are CONFLICTING; see the table note above.) Plugging multi-outs raises R_th and lowers f_c (for example BD alone: 13k gives 12 Hz). This matches a user report that "shorting out the 1 µF coupling capacitor (C153 and C154) … restore[s] the low end" (gearspace thread "Roland TR 909, noisy outputs" / "909 stereo out issue"; **UNVERIFIED** secondary). It is **design-critical for authentic master-bus tone**, and it is a good candidate for an "MS/Indiv-out tone" option.
- **LPF on the master [DERIVED]:** R473 1k + R530 2.2k into C511 0.01 µF ∥ R531 10k gives R_th = 3.2k ∥ 10k = 2.42 kΩ, so **f_c ≈ 6.6 kHz**, with a −2.4 dB divider (10/13.2). These caps were added from S/N 415300, or retro-fitted, to cure "Noise is high in OUTPUT" [SN p.7, p.12]. So the master out is noticeably darker on later units, which matches user reports of "an additional lowpass filter on the main out that makes it sound darker" (**UNVERIFIED** secondary). The R530/R531 values were read clearly; the C511/C512 values (".01") are printed beside them.
- **Multi-out LPF [DERIVED]:** 1 kΩ + C500 0.01 µF gives **15.9 kHz** into a high-Z load, on S/N ≥ 415300 units only. (Fact-check: CONFIRMED, 15.92 kHz. With the jack unplugged, the same cap also filters the mix path. The node then sees 1k ∥ (R_up ∥ R_lo), which puts the pole at **18.6 to 18.9 kHz** before the master LPF.)
- Output level: Master Out **6 Vp-p, 1 kΩ** [SN p.1; OM p.39]. Multi-out level is not specified (OM p.39 leaves "[ dB, Ω]" blank; [SN p.1] "See P.9", which holds only the scope drawings). **CONFIRMED** (spec); per-voice levels are an **OPEN QUESTION** for file 01.
- [DERIVED] Mono: with only R(MONO) plugged, the L-jack's normally-closed contact joins the L output node to the R output node, so both channels reach the R jack, each through its 2.2 kΩ, summed passively (each ≈ −6 dB relative to its stereo level at that node, before loading). The exact normalling is read from the jack symbol; **verify**.

---

## 7. Quirks, variance and mods

### 7.1 Unit-to-unit variance (production changes [SN p.7])

| From S/N | Change | Audible effect |
|---|---|---|
| 381500 | Tape-sync filter: C87 10 pF deleted; R306 4.7k→47k; R312 470k→4.7M | load reliability |
| 381500 | **BD C9 0.22 → 0.33 µF** "for expanding the TUNE range" | **BD pitch-envelope range differs on early units** |
| 393000 | Reset zener D701 RD6.8JB2 → RD5.6JB2 | fewer brown-out stops |
| 415300 | **RS R417 12k → 3.3k** "more realistic sound" ([SN p.9] shows both waveforms) | **two RS versions** |
| 415300 | **HH C134 10 µF → 0.01 µF** "rolling off unnecessary lower frequencies" | **early hats have more low end** |
| 415300 | TRIG OUT R495 1k; R393 10k→1M; R392 22k→1M; C600 0.1 µF | trig/tape-sync crosstalk fix |
| 415300 | Multi-jack C500 to C512 0.01 µF added ("Noise is high in OUTPUT" fix) | **darker master (≈ 6.6 kHz LPF), 15.9 kHz on multis** |
| 426700 | **Tom noise C54 0.0022 → 0.0047 µF; R194 47k → 100k** "emphasize attack of TOM TOMs" | **two tom-attack versions** |
| ROM | Group A (S/N 360100 to 393899, Rev 0/1) has a track-memory corruption bug; Group B (403900+). Ver. 2 ROM fixes the MIDI STOP/CONTINUE lag | sequencer behaviour |

All **CONFIRMED** [SN p.7]. mw909 could expose an "early/late unit" switch covering BD C9, RS R417, HH C134, tom C54/R194 and the output caps.

### 7.2 Noise floor
- The service manual acknowledges high output noise on pre-415300 units (fix: 0.01 µF caps) [SN p.7]. **CONFIRMED.** No numeric noise-floor spec or published spectrum was found. **UNVERIFIED / OPEN.**
- Known noise sources:
  - The shared 32-stage shift-register noise generator (IC31 to IC33) for the analog voices [SN p.6].
  - Leaky snappy VCA transistors (Q47/Q48 2SD1469) causing "excessive continual hissing when the Snappy pot was turned up" [RW].
  - Snappy C73 holding ≈ 0.3 V after notes [RW].

  **UNVERIFIED** as general properties (single-unit reports).
- Recommendation: a very low, voice-specific leakage noise (snappy path), optional and default off, with no fabricated broadband floor. Measure from CC0 recordings in file 05 if available.

### 7.3 Popular hardware mods
- **BD (Colin Fraser [CF], adopted by [RW]):**
  - **Extended decay**: 1 µF in parallel with C8 (the ENV1 cap on VCA Q12).
  - **Tune range**: +1 µF ∥ C9, 560 Ω ∥ R21, 6.2 kΩ ∥ R57. The pitch-sweep decay maximum becomes 4× and the minimum is unchanged; "standard range … around 30 to 120 milliseconds".
  - **Pitch**: R59 replaced by 100k lin + 10k.
  - **Drive**: R52 replaced by 10k pot + 1k, overdriving the triangle-to-sine diodes towards a square.
  - **Tune depth**: R27 1.5M removed, offset restored via 1M resistors from −15 V at IC12B; R28 150k→300k.

  **CONFIRMED** (primary mod author).
- **HH tune** [CF]: R492 replaced by 6k9 + 22 kΩ lin pot (as on crash/ride). **CONFIRMED.**
- **SD and HC** [RW]:
  - Exponential snappy decay with more low-mid; the 24 ms flat part is removed.
  - Extended tone and tune range (SD oscillators stop except for the onset pulse).
  - External inputs for snappy and clap noise.
  - Clap pseudo-reverb disable, Density and Intensity pots.
  - These are mounted in place of the cartridge slot.

  **CONFIRMED** (primary mod author).
- **Accent mods**: no easy one. "The only approach … would be to attenuate each individual accent line going into the individual voices" [EM-ACC]. **UNVERIFIED.**
- **Output mods:** removing C511/C512 and shorting C153/C154 to restore top end and bass (gearspace user reports; **UNVERIFIED**).
- **Timing / firmware mods:**
  - DIN-sync from a stable master (Innerclock Sync-Gen) to tighten timing [MW; ICS].
  - v4 ROM (MIDI-clock fix; RAM swap to NEC D4364C-15L; CR1/2AA lithium battery; C6 10 µF→1000 µF) [RW].
  - **Revolution 909 v5** (2024): sync fixes, 32 internal banks, mute, external mapping, shuffle and flam on all scales, v1/v4/v5 selectable [REV].

  **UNVERIFIED** (vendor and secondary).

---

## 8. Non-original sequencer features in the popular 909 re-creations (for parity planning)

| Feature | Original TR-909 | Roland TR-909 software [RC909] | Roland TR-09 [TR09] |
|---|---|---|---|
| Length | 1 to 16 per pattern | per-instrument LAST STEP, variations A to H | 1 to 16 + chain |
| Shuffle | 7 levels, 16th/32nd only | global SHUFFLE × per-instrument shuffle | SHUFFLE/FLAM + keys 1 to 7, fine adjust via TEMPO dial |
| Flam | 8 intervals, BD/SD/toms | TYPE1 0 ms, TYPE2 to 9 = 20 to 48 ms | keys 9 to 16, BD/SD/toms, fine adjust |
| Dynamics | total accent + voice accent | TOTAL ACCENT row + WEAK steps + sub-steps | as original |
| Trigger out | RS only | — | programmable TRIGGER OUT row |

**CONFIRMED** against the local manuals. mw909 should keep the original semantics as the default and add per-voice length, per-voice shuffle and sub-steps as clearly marked extras, as mw808 did.

---

## 9. Open questions

1. **Accent DAC codes:** the normal level, voice-accent increment, total-accent knob scaling, saturation, and the single-key voices' on-level. These need a ROM disassembly or a unit measurement.
2. **Flam spacing:** 10 to 38 ms or 20 to 48 ms? Also the relative level of the two hits, and whether accent applies to both.
3. **Internal resolution under external sync:** how half-clock (1/48) shuffle offsets are produced from a 24 ppqn clock.
4. **MIDI velocity to accent-code mapping**, which duplicate note numbers are transmitted, and how Tx velocity 64 to 96 maps to the internal states.
5. **MIDI-in to trigger latency**: no measurement found.
6. **Tap-write quantisation rule** (nearest step or floor).
7. **Mix-bus assignment and mono normalling:** the jack-board labels and the voicing-board labels/trace disagree on which bus is left (see §6). Confirm by recording a real unit's L and R outputs with a LT or RIDE hit.
8. **Trigger-out pulse length:** 18 or 20 ms. The fact-check's RC estimate is ≈ 15 to 17 ms nominal, so the width is analog and set by component tolerance, not by firmware.
9. **Noise floor:** no level or spectrum found.

---

## Verification log (2026-10-03)

- Read directly as images:
  - [OM] printed pp.4 to 18, 19 to 20, 22 to 31, 33 to 40.
  - [SN] pp.1, 4, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, from the 300 dpi scan and the 8110 px JPEG2000 set. Crops covered the block-diagram TRIG/ACCENT latches, the RM0621 legend, the CN3 accent pot wiring, the master-volume stage, the multi-jack board and the DIN board.
  - [SN p.5] port assignment and circuit description, via the text-layer PDF.
- Recomputed:
  - Mix gains and pans, HPF (130 Hz), LPF (6.6 kHz / 15.9 kHz), DAC LSB and source impedance, shuffle tables and swing %, bank size (4 KB) and cartridge capacity (2 banks), in `/tmp/claude-1000/mw909-research-sequencer/mix.py`.
  - Scope-trace pixel ratios from the [SN p.9] crop.
- Fetched: [KVR], [EL-FLAM], [EL-ACC], [MW] (archive), [CF], [RW] PDF, [NAVA] source, [JV], [REV].
- Not openable (HTTP 403 or no 909 row): the gearspace threads, the Litmus page and MPC-Forums. Their figures are tagged UNVERIFIED.

---

## Fact-check log (2026-10-03, independent adversarial pass)

Method: every value below was re-read from the primary scans and recomputed in my own scripts:
- Scans: the [SN] 6614 px and 8110 px JPEG2000 pages (p.1 = image 0000, p.4 = 0003, p.9 = 0008, p.11 = 0010, p.12 = 0011), the [SN] text-layer PDF, and [OM] pdf pages 3 to 38 rendered fresh.
- Scripts, in `/tmp/claude-1000/mw909-factcheck-03-system-sequencer/`: `factcheck.py`, `scope_measure3.py`, `crop.py`. Scratch only, not shipped.
- URLs re-fetched: [KVR], [EL-FLAM], [EL-ACC], [JV] and the [NAVA] source (`define.h`, `Clock.ino`). The archived [MW] text was re-read.

### Corrections

| # | Claim | Was | Now | Basis |
|---|---|---|---|---|
| 1 | [OM] page mapping | printed = pdf + 2 | printed = pdf + 1 | Rendered pdf pp.4, 22, 24, 25, 36, 37, 38 show printed 5, 23, 25, 26, 37, 38, 39; pdf p.3 is "Panel Description". The citations in the body were already in printed numbers and check out (p.24 scale, p.25 accent, p.26 shuffle/flam, p.38 trig 18 ms, p.39 spec). |
| 2 | 16th shuffle magnitudes tag | CONFIRMED | UNVERIFIED (single measurement) | Only [KVR] measures them; [MW] restates the level count. Under this file's own tag rules a single secondary source is UNVERIFIED. The quote was re-read verbatim at the URL. |
| 3 | Mix gains per bus | +11.7 / +14.7 / +9.9 / +13.2 dB | +11.06 / +14.20 / +8.96 / +12.52 dB (crash upper +10.93) | The 1 kΩ (R500…) is shared by both mix resistors. Gain = 50/R_bus × Rp/(1+Rp) with Rp = R_up∥R_lo. BD: Rp = 6k, 6/7 = 0.857, × 50/12 = 3.571 → +11.06 dB. LT: Rp = 5.302k, 0.8413 × 50/8.2 = 5.130 → +14.20 dB; × 50/15 = 2.804 → +8.96 dB. |
| 4 | Pan difference | 4.8 dB (LT, HT, HH, RIDE), 1.45 dB (CRASH) | **5.25 dB**, **1.58 dB** | Ratio = R_lo/R_up: 20·log(15/8.2) = 5.25 dB; 20·log(12/10) = 1.58 dB. |
| 5 | Pan side | LT/RIDE right, HT/HH/CRASH left (from the jack-board labels) | **CONFLICTING**; the traced wiring gives LT/RIDE left, HT/HH/CRASH right | Jack board: 43 = LMIX, 45 = RMIX. Voicing board: 43 = "MIX IN (R)", 45 = "MIX IN (L)". Trace: 45 → C153 → IC66b → R474/C155 → 47 "MIX OUT (L)" → R530 → Left jack; 43 → C154 → IC66a → R473/C152 → 48 → Right/MONO. Upper resistors are on the pin 45 line (junction dots re-read). |
| 6 | RS/HC accent DAC designator | IC8 (CONFIRMED) | CONFLICTING: IC8 on the p.4 block diagram, IC7 on the p.11 circuit and in [JV] | On the 8110 px p.11 scan, RA6 (on IC7) feeds a vertical trace at x ≈ 1625 px that runs unbroken from y = 4304 up to y = 2941 and joins the IC48b (+) RS accent input, which continues to IC26a (HC). |
| 7 | Latch reset | "IC1 has a RESET input" | All ten latches IC1 to IC10 have their reset on the common RESET line | [SN p.6] text: "The RESET is also routed to … TRIG and ACCENT latches (IC1-IC10)". p.11 shows pin 1 "R" on IC1, IC2…IC10 tied to CN1-7 "Reset". |
| 8 | Accent DAC loading | "every voice input that loads the node compresses the top of the range" | The DAC nodes checked feed high-Z op-amp (+) inputs. Buffer gain 1 + 10k/4.7k = 3.128 → 0 to 15.64 V nominal, so codes above ≈ 52 to 56/63 clip at a +13 to +14 V swing | [SN p.11]: BD IC11b R37 10k/R38 4.7k; RS IC48b R406 10k/R397 4.7k; HC IC26a R212 10k/R206 4.7k; tom IC18b R119 10k/R120 4.7k. Clip code = V_sat/3.128/5 × 63: 13.0 V → 52.4, 13.5 V → 54.4, 14.0 V → 56.4. SD/HH/cymbal not traced. |
| 9 | SD accent ratio, [SN p.9] scope drawing | p-p ≈ 2.5 (+8 dB) | p-p ≈ 2.2 (+6.9 dB), positive ≈ 2.1 (+6.4 dB) | Largest-component pixel extents on the 8110 px scan at thresholds 90/130/170: SD p-p 2.23/2.20/2.19. The BD figures reproduced: p-p 2.62 (+8.4 dB), positive 2.25 (+7.1 dB). The implementation-guidance target was updated to match. |
| 10 | Flam "(min)" spread | "≈ 4 ms spread fits the ≈ 2 ms CPU granularity"; §4 "±2 ms spread" | The second hit lands at two discrete positions, the listed value (more often) or 4 ms early, which suggests a ≈ 4 ms flam timer tick | [EL-FLAM] verbatim: "you actually see two pulses, which are due to the jitter of the sequencer; the second pulse of the second trigger is played way more often than the first one". |
| 11 | R495 | "buffered via R495 1 kΩ" | R495 is the +15 V series feed to Q67's emitter; output is via R413 1k with R412 22k to ground | [SN p.11] trigger-out circuit Q66/Q67/C114 (zoomed crop). |

### New derived figures (added, not corrections)

- **Trigger-out pulse width.** The stretcher is C114 0.1 µF, R409 47k and R410 47k, behind the 2 ms TRIG, so τ = 47k × 0.1 µF = 4.7 ms.
  - Q67 stays fully on while (14.35 − V_c)/47k exceeds Vbe/47k plus about 6 µA of base current. That holds for 4.7 · ln(14.25/0.93) ≈ 12.8 ms, and Q67 is fully off after 4.7 · ln(14.25/0.65) ≈ 14.5 ms.
  - Total pulse ≈ **14.6 to 16.7 ms** nominal.
  - Output level ≈ 15 − 0.65 − 0.15 ≈ **+14.1 V**, which matches "+14V". The 18 versus 20 ms question stays CONFLICTING.
- **Mix-path pole with the multi jack unplugged.** R_th = 1k ∥ (R_up ∥ R_lo): BD 0.857k → 18.6 kHz; LT 0.841k → 18.9 kHz.
- **Mono sum.** Millman at the tied node gives (2/3.2)/(2/3.2 + 1/5) = 0.7576 total, so 0.3788 per channel against 0.7576 in stereo: **−6.02 dB**. The pole stays at 6.57 kHz (1.212k with 20 nF). This confirms the "≈ −6 dB" in the body.

### Confirmations (recomputed or re-read, value unchanged)

- **RM0621** legend "R = 5K", with R, 2R, …, 32R in one box [SN p.11 crop]. Source R = 1/(1/5 + 1/10 + … + 1/160) = **2.540 kΩ**; LSB = 5/63 = **79.37 mV**. Six 4174 outputs (pins 15, 5, 12, 2, 7, 10) per DAC. The block diagram says "0–5V" [SN p.4].
- **TRIG** "5V positive going pulse … for 2ms"; accent "clamped to the level until it is replaced"; "Almost concurrent with ACCENT" [SN p.5 text layer, verbatim].
- **TRIG latch outputs:** IC1 has 5 used outputs (BD, SD, LT, MT, HT) and IC10 has 6 [SN p.11].
- **TOTAL ACCENT pot** EVJFDAF30B15 100 kΩ B [SN p.1 callout]. VR1 100K(B) on the voicing board connector, next to the tom section [SN p.11].
- **Crystal** 12.00 MHz [SN p.3 parts list]. Tempo ♩ = 37 to 290 [SN p.1; OM p.39].
- **Trigger out** "+14V, 20 ms pulse" [SN p.1; OM p.39] versus "positive 14V, 18ms" [OM p.38]; the conflict is real.
- **MIDI** Tx Note On "kkkkkkk = 36-51, vvvvvvv = 64 - 96 (accent min-max)" [SN p.15]. 3 bytes × 10 bits / 31 250 baud = **0.96 ms**.
- **Tape sync** "changes frequency … at every half-period of T which is 1/24 of the time required … to process a quarter note" [SN p.6]: a toggle every 1/48 beat, T = 20.83 ms at 120 BPM.
- **Shuffle arithmetic.**
  - Delay (n − 1)/48 beat = 0, 2, 4, 6, 8, 10, 12 /96. At 120 BPM that is 0, 10.42, 20.83, 31.25, 41.67, 52.08, 62.50 ms.
  - Swing % = 50 + 200·d: 50.0, 54.2, 58.3, 62.5, 66.7, 70.8, 75.0.
  - 32nd rule floor((n − 1)·6/12) = [0, 0, 1, 1, 2, 2, 3], which reproduces the [OM p.26] pairing "1(=2), 3(=4), 5=(6) and 7". 3/48 of a 6/48 step = 75 % swing.
  - [OM p.26] text re-read: "seven options for [16th] and four for [32nd]"; "1 gives no effect"; odd last step "will not take on the bounce".
- **Scale.**
  - [OM p.24] cycle icons, re-read at 400 dpi: 16th → 32nd → 8th-triplet (3) → 16th-triplet (6); auto last step 16/16/12/12.
  - Ticks @24 ppqn: 6, 3, 8, 4. Ticks @48 ppqn: 12, 6, 16, 8.
  - 16 × 32nd = ½ bar; 12 × 8th-triplet = 1 bar; 12 × 16th-triplet = ½ bar.
- **Flam.**
  - [EL-FLAM]: 10, 14 … 38 ms, with minimums 6 … 34 ms, re-read verbatim.
  - [RC909] p.5: "TYPE1: 0msec; TYPE2–9: … 20–48 msec in 4 msec units", from the local PDF text.
  - [NAVA] `define.h`: `{4999, 5999, 7999, 10999, 14999, 19999, 25999, 29999}` with prescaler 64 at 16 MHz = 4 µs per count gives (N + 1) × 4 µs = 20, 24, 32, 44, 60, 80, 104, 120 ms, which does not match its own comment. The doc's claim stands.
  - [OM p.26]: "the first sound of Flam is in time for a step, and the second one will follow". The 18/20 ms trigger and the 10/20 ms flam base remain CONFLICTING.
- **Tap-write rounding.** The [NAVA] `Clock.ino` line `if (ppqn % scale == scale/2) tapStepCount++` advances the tap step at half a step, i.e. rounds to nearest; the body's statement holds (still UNVERIFIED for the original).
- **[EL-ACC]** quotes re-read verbatim: "If the instrument is not triggered, the accent control signal keeps the value of the last step …"; ENV2 "without accent is half". No knob setting is given.
- **[JV]** says "8 6-bit resistor ladder DAC's (IC2-IC9)". It names IC7 = RS/HC and IC8 = HH, which conflicts with the [SN p.4] block diagram and agrees with [SN p.11] (correction 6).
- **Litmus** conversions: 214/48 kHz = 4.46 ms, 307 → 6.40 ms, 374 → 7.79 ms. Still UNVERIFIED as secondary quotes.
- **SysEx and cartridge.** 16 × 512 / 2 = 4096 bytes per bank. The cartridge holds 4 × 2048 = 8192 bytes = 2 banks.
- **Master HPF.** C153/C154 1 µF into the bus R_th of 1.215k / 1.236k gives 131.0 / 128.8 Hz (≈ 130 Hz). With BD alone on the bus, R_th = 12 + 1∥12 = 12.92k → 12.3 Hz (the body says 12 Hz).
- **Master LPF.** R_th = (1k + 2.2k) ∥ 10k = 2.424 kΩ with C511 10 nF → **6.57 kHz**. Divider 10/13.2 = **−2.41 dB**. The output coupling C152 10 µF into 13.2k → 1.2 Hz (negligible).
- **Multi-out pole.** 1/(2π·1k·10n) = **15.92 kHz**.
- **Multi-jack values** R501 to R529 and R530 to R533 re-read from the 8110 px p.12 scan; all match the table.

### Still unverified after this pass

- The 16th shuffle magnitudes (one measurement).
- Flam base, 10 or 20 ms.
- The trigger-out width.
- The mix L/R side.
- Accent DAC firmware codes.
- The µPD7811 A/D resolution (supported only by the µPD78C11A datasheet).
- The accent-buffer clip code (it depends on the op-amp's real swing).
- The SD/HH/cymbal accent input stages.
- All [GS-JIT]/[LIT] jitter figures.
