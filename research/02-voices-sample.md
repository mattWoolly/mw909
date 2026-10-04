<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# 02 — TR-909 sample voices (CH, OH, CY crash, RD ride): playback chain and the licensing question

Scope: circuit-level facts for the four PCM voices of the Roland TR-909 (closed hi-hat, open hi-hat,
crash cymbal, ride cymbal), written so the DSP can be built straight from it, followed by the
licensing analysis that decides what sound source mw909 ships. Values come from the Roland service
notes unless marked otherwise.

## Sources and notation

| Tag | Source | URL / local copy |
|---|---|---|
| **[SM]** | Roland, *TR-909 Service Notes*, First Edition, JUN. 15, 1984 (16 pages). p.5 "DIGITAL VOICE GENERATORS" circuit description, p.9 scope photos ("Observed at MULTI OUT jack with all knobs set to center"), p.11 voicing-board schematic (VG 73133090, PCB 2291084903 3/3). | https://archive.org/details/synth_Roland_TR-909_service_notes (text PDF: `Roland_TR-909_service_notes_text.pdf`, 600 dpi page scans: `TR-909_service_notes_archive_jp2.zip`); also https://archive.org/details/roland_Roland_TR-909_Service_Manual (`TR-909_Service_Manual.pdf`). Local copies in `.deps/sources/` (never shipped). |
| **[N909]** | network-909.de "HiHat and Cymbals" circuit description (a re-typing of [SM] p.5 with part references) | http://www.network-909.de/hihatand.htm |
| **[FRA]** | Colin Fraser, "Nine-Oh-Ninestein" 808/909 clone notes: ROM extraction, 32k ROMs, envelopes | http://www.colinfraser.com/tr909/my909.htm |
| **[FRA-MOD]** | Colin Fraser, "909 Hi-hat Tune" mod (R492 → 6k9 + 22k pot) | http://www.colinfraser.com/tr909/909mods/909mods.htm |
| **[RW]** | Robin Whittle, *TR-909 sound mods*, 10 Oct 2018 (pp.11–14 reproduce the HH/HCP/BD/SD schematic sections; HH tuning range) | https://www.firstpr.com.au/rwi/tr-909/TR-909-Sound-Mods.pdf (local: `.deps/sources/TR-909-Sound-Mods_RobinWhittle.pdf`) |
| **[MAME]** | MAME `roland_tr909.cpp` driver: ROM list with part numbers, IC positions and sizes | https://github.com/mamedev/mame/blob/master/src/mame/roland/roland_tr909.cpp |
| **[HOSH-A]** | Attack Magazine interview with Atsushi Hoshiai (2024): which cymbals, mic, recorder, computer | https://www.attackmagazine.com/features/interview/atsushi-hoshiai-the-man-behind-the-tr-909/ |
| **[HOSH-R]** | Roland Articles, "Atsushi Hoshiai and the TR-909" (2020) | https://articles.roland.com/atsushi-hoshiai-tr-909 |
| **[WIKI]** | Wikipedia, Roland TR-909 | https://en.wikipedia.org/wiki/Roland_TR-909 |
| **[TT]** | Tiptop Audio HATS909 manual | https://www.tiptopaudio.com/manuals/Tiptop_Audio_HATS909_ns.pdf (local: `.deps/sources/Tiptop_HATS909_manual.pdf`) |
| **[D16-2]** | D16 Group, *Drumazon 2 user manual* (parameter list) | https://m.d16.pl/pub/manuals/Drumazon%202-manual-gb.pdf (local: `.deps/sources/Drumazon_2-manual-gb.pdf`) |
| **[RC909]** | Roland Cloud TR-909 Software Rhythm Composer owner's manual (CC map incl. CH/OH TUNE) and product page | https://www.roland.com/global/products/rc_tr-909/ ; local `.deps/sources/RC_TR-909_OM_gear4music.pdf`; https://www.pluginboutique.com/product/1-Instruments/7-Drum-Machine/15284-TR-909 |
| **[ADM]** | MusicRadar review of AudioRealism ADM | https://www.musicradar.com/reviews/tech/audio-realism-adm-121891 |
| **[RD9]** | Behringer RD-9 user manual (local `.deps/sources/Behringer_RD-9_manual.pdf`) and Synthtopia shipping article with reader comments | https://gzhls.at/blob/ldb/5/c/d/a/f45872d020af698f563961b0c91d8a12ccca.pdf ; https://www.synthtopia.com/content/2021/05/31/tr-909-inspired-behringer-rd-9-drum-machine-finally-shipping/ |
| **[NAVA]** | e-licktronic Nava 909 clone coverage (ships EPROMs with the original samples) | https://www.matrixsynth.com/2018/02/roland-tr-909-replica-nava-909.html ; https://synthtopia.com/content/2016/04/13/build-your-own-roland-tr-909-inspired-drum-machine |
| **[ER99]** | ER-99 web 909 (GPL-3 code; hats/cymbals are sampled audio of undisclosed provenance) | https://github.com/matthewcieplak/er-99 |
| **[POLY]** | polynominal.com TR-909 page (claims "6 bits ... 18 kHz") | https://www.polynominal.com/m/roland-tr-909.htm |
| **[USC114]** | 17 U.S.C. § 114(b) (sound-alike recordings are not infringement) | https://www.law.cornell.edu/uscode/text/17/114 |
| **[BRIDGE]** | Bridgeport Music v. Dimension Films (6th Cir. 2005) | https://en.wikipedia.org/wiki/Bridgeport_Music,_Inc._v._Dimension_Films |
| **[VMG]** | VMG Salsoul v. Ciccone (9th Cir. 2016) | https://www.musicologize.com/case-law-library/vmg-salsoul-v-ciccone/ |
| **[PELHAM]** | CJEU C-476/17 Pelham v Hütter ("Metall auf Metall"), 29 July 2019 | https://ipwatchdog.com/2019/08/01/cjeu-backs-kraftwerk-music-sampling-copyright-case/ ; https://www.simkins.com/news/kraftwerk-revisited---cjeu-ruling-in-pelham-ii-clarifies-test-for-the-pastiche-exception |
| **[JPCR]** | Copyright Research and Information Center (Japan), neighbouring rights term | https://www.cric.or.jp/english/csj/csj4.html |
| **[UKCR]** | UK Copyright and Duration of Rights in Performances Regulations 2013 (CDPA s.13A, 70 years) | https://vlex.co.uk/vid/811930101 |
| **[VCSL]** | Versilian Community Sample Library (CC0): "Hi-Hat Cymbal", "Suspended Cymbal 1/2", "Clash Cymbals 1/2" under Idiophones/Struck Idiophones | https://github.com/sgossner/VCSL |
| **[FS]** | Freesound CC0 packs (see §9.5) | https://freesound.org/ |

Tags: **CONFIRMED** = primary source ([SM] scan or schematic read by me) or two independent good sources
agree. **UNVERIFIED** = single secondary source or my own reading of a small schematic detail that a
fact-checker should re-read. **CONFLICTING** = sources disagree; both values given.
**[DERIVED]** = my algebra from [SM] component values (formula and parts shown).
**[MEASURED]** = pixel measurement on the [SM] p.9 scope photos (600 dpi scan, see §7 for the
calibration and its ±10 % caveat). Analysis scripts were throwaway (`/tmp/claude-1000/mw909-research-sample/`).

Part references (IC/R/C/Q/VR/RA) are voicing-board numbers from [SM] p.11.

---

## 0. Design-critical facts (executive summary)

1. **Four voices are PCM: CH, OH, crash, ride.** Everything else on the 909 is analog. **CONFIRMED** [SM p.5][WIKI]
2. **Three mask ROMs, 32 KiB each, Hitachi HN61256P (32768 × 8), of which only the low six data bits are wired:** IC69 "C43" = hi-hat (shared by CH and OH), IC62 "C42" = crash, IC54 "C44" = ride. **CONFIRMED** ([SM] p.11 schematic: 15 address lines A0–A14, data pins 11,12,13,15,16,17 → 4174 latch; [MAME] lists all three as 0x8000 bytes; [FRA] "Each of the three ROMs are 32k in length").
3. **Sample lengths (in ROM samples):** crash 32768, ride 32768, **open hat 24576 (addresses 0x0000–0x5FFF), closed hat 8192 (0x6000–0x7FFF)**. The hat ROM is one 32K image with the CLOSED/OPEN line forcing A13/A14 high through diodes D196–D199 and the stop logic (IC72a) ending playback at counter 0x6000 (open) or 0x2000 (closed). **CONFIRMED** ([SM] p.5 text and address table; schematic). The CH is therefore a separate 8K recording at the top of the ROM, not a truncated OH.
4. **Sample clock:** a gated two-NAND-gate RC oscillator (4011UBP) divided by 2 by a 4013 flip-flop clocks a 4040 + 4520 address counter. [SM] p.5: "outputs about 60 kHz, which is divided by two" → **about 30 kHz sample rate** for the hats. **CONFLICTING**: the ideal CMOS-astable formula 1/(2.2·R492·C160) = 1/(2.2·12 kΩ·470 pF) gives 80.6 kHz → 40.3 kHz [DERIVED]; [FRA] chose 32 kHz as "pretty close to the rate used for the hats"; [POLY] claims 18 kHz (no source; treat as wrong). The [SM] p.9 crash trace ends ≈ 386 px ≈ **1.02 s** after its start (32768 samples), which supports **≈ 32 kHz (±10 % calibration: ≈ 29–35 kHz)** at the TUNE pot centre [MEASURED] (corrected by fact-check: was "≈1.1–1.2 s … supports ≈28–30 kHz"; see Fact-check log F1). Evidence now spans 30 kHz (SM text) to ≈ 32 kHz (drawing, [FRA]); default mw909 to 30–32 kHz nominal (fit parameter) with a per-unit variance of a few %.
5. **TUNE = clock-rate change.** Crash: VR25 10k(B) + R443 6.8k with C131 470p; ride: VR27 10k(B) + R441 6.8k with C130 470p; hat: fixed R492 12k with C160 470p. So f ∝ 1/(6.8k…16.8k): **crash/ride range 2.47 : 1 (15.7 semitones), ≈ +9.5 st above and −6.1 st below the pot centre (11.8k)**; the centre is within 2 % of the hat's fixed 12k, so all three run at the same nominal rate at centre. [DERIVED] from **CONFIRMED** parts. Sample duration scales inversely: crash/ride ≈ 1.09 s at 30 kHz, ≈ 0.62 s at max tune, ≈ 1.53 s at min tune.
6. **The hats have no TUNE on the original because R492 is a fixed resistor**; replacing it with 6.9k + 22k pot (Fraser mod) gives ≈ 0.43×–1.7× (−15 to +9 semitones). **CONFIRMED** [FRA-MOD][RW]. All popular plugins and the RD-9 add hat tune [D16-2][RC909 CC 61/80][RD9]; mw909 should too, as clock-rate change.
7. **6-bit DAC** = IC68/IC63/IC53 (4174 hex D latch, clocked each sample) driving **RA9/RA10/RA12 "RM0621"**, a six-resistor module with a common output node (a binary-weighted summing network; the same RM0621 is used as the accent DAC on IC2–IC9). **CONFIRMED** (schematic) that it is a 6-input resistor module; its internal weights are **UNVERIFIED** (R…32R assumed). Nonlinearity: CMOS output impedance and resistor tolerance give bit-weight errors on the order of 1–3 % of full scale, largest on the MSB; model as per-unit random bit weights. **UNVERIFIED (engineering estimate)**.
8. **Companding ("the 6-bit trick"):** the ROM content was amplitude-compressed before digitising so that it fills the six bits, and the natural decay is restored by the analog VCA. [SM] p.5: "the Hi-Hat sounds have been compressed before being digitalized … in order to have greater S/N ratio and higher digital resolution. The envelope … is controlled … with DECAY". [FRA]: "heavily compressed to make maximum use of the bits, and the original envelope is then restored by the vcas." **CONFIRMED.** Effective result: 6-bit mantissa × analog exponent. Any source material mw909 bakes into a ROM image must be normalised/AGC'd to a near-flat envelope first.
9. **Hat envelope (RC, decay knobs):** capacitor C135 1 µF is discharged at trigger and then **charges toward +15 V** through (R451 10k + VR21 100k(B)) in CLOSED mode or (R452 100k + VR23 1M(A)) in OPEN mode; the rising capacitor voltage closes the VCA through an "anti-log" stage: C135 → R453 220k → IC67a, whose DC feedback runs through R484 2.2k and the left half of Q84 (2SA798 dual PNP, a log/exp converter like IC52b/Q70 in the cymbals; C156 1000p is only loop compensation); Q84's right collector drives the base of the actual VCA transistor **Q85** through R485 10k (corrected by fact-check: was "IC67a buffer/LPF 723 Hz → Q84 … current-steering pair" as the VCA; see F5). **Time constants [DERIVED]: CH 9–100 ms (CH path ∥ OH path), OH 100 ms–1.1 s.** Audible result at knob centre [MEASURED, ±10 %]: **CH ≈ 65 ms (accent) / ≈ 100 ms (no accent) to −20 dB; OH ≈ 0.29 s to −14 dB, ≈ 0.5 s to −20 dB.** The SM notes that CH decay changes slightly with the OH knob because the paths are in parallel. **CONFIRMED** mechanism, fitted constants.
10. **Crash/ride envelope is address-driven:** the top six address bits (A9–A14; 64 steps of 512 samples = 17 ms each at 30 kHz) go through a second RM0621 (RA11 crash, RA13 ride) into IC52b/IC52a (R432 270k crash, R439 470k ride; C126/C125 1000p) and Q70/Q68 2SA798 "anti-log" converters that drive the VCA transistor Q71/Q69. [SM] p.5: "the same addresses are also converted to analog voltages through RA11, anti-log tapered by IC52b and Q70, and are applied to the base of Q71 (VCA) which is configured as a voltage controlled potentiometer". Consequence: **the decay always spans the whole sample and stretches with TUNE** [FRA] "an exponential decay with the same length as the sample regardless of tune setting". **CONFIRMED** mechanism; RA11 fed by A9–A14 (4040 Q10–Q12 + 4520 Q1–Q3) **CONFIRMED** by an independent fact-check re-read of the 600 dpi scan; RA13 (ride) **UNVERIFIED**.
11. **Reconstruction filter after the VCA = two unity-gain Sallen-Key low-passes (emitter-follower buffers Q80/Q81, Q75/Q76, Q77/Q78), identical in all three voices:** stage 1 R 5.6k/5.6k, C 1200p (feedback)/1000p (ground): **f0 = 25.9 kHz, Q = 0.55**; stage 2 R 5.6k/5.6k, C 2700p/390p: **f0 = 27.7 kHz, Q = 1.32**. Cascade: **−0.2 dB at 15 kHz, −0.6 dB at 20 kHz, −2 dB at 25 kHz, −5 dB at 30 kHz, −14 dB at 40 kHz, −3 dB point ≈ 27 kHz.** [DERIVED] from **CONFIRMED** parts. So this filter barely touches the first image band (15–30 kHz at fs = 30 kHz): **the aliasing/imaging sizzle above 15 kHz is part of the 909 hat sound** and must be reproduced (render the chain oversampled, then band-limit to the host rate).
12. **Accent** enters each VCA control node through 4.7k (R481 hat, R426 crash, R435 ride) from the per-voice ACC voltage (CPU accent code → RM0621 DAC on the IC1–IC10 latches, "clamped until replaced by the next data" [SM p.5]). It shifts the gain in the exponential domain: louder and slightly longer. [MEASURED] CH with accent 1.6 Vpp vs 0.96 Vpp without (+4.4 dB) at MULTI OUT, knobs centred. **CONFIRMED** mechanism.
13. **Choke:** there is one hat voice (one counter, one ROM, one VCA). A CH trigger while OH plays resets the counters, flips CLOSED/OPEN, jumps to the CH region and restarts the envelope: the open hat is cut by a retrigger, not by a separate gate. OH and CH can never sound together ([FRA] "the hi-hat can't be open and closed at the same time"; [TT] "capable of producing only one sound at a time"). **CONFIRMED.**
14. **Output levels at MULTI OUT, knobs centred [MEASURED, ±10 %]:** crash ≈ 2.3 Vpp, ride ≈ 2.2 Vpp, CH ≈ 1.6 Vpp (accent) / 0.96 Vpp (no accent), OH ≈ 1.2 Vpp. Level stages: inverting op-amp with 22k in and 50k(B) pot feedback (max gain ×2.27): IC65b/VR22 hat, IC65a/VR24 crash, IC64a/VR26 ride. **CONFIRMED** parts.
15. **Licensing (decides the design):** the ROM data are Roland's sound recordings (fixed 1983, published 1984) and are protected until at least the 2050s everywhere that matters (Japan/EU/UK 70 years from publication; US 95 years for a work made for hire). Copying any part of them, even bit-crushed, is infringement (EU: [PELHAM] "unless … in a modified form unrecognisable to the ear"; US 6th Cir.: no de minimis [BRIDGE]). **Imitation is explicitly lawful:** 17 U.S.C. § 114(b): the sound-recording right does "not extend to the making or duplication of another sound recording that consists entirely of an independent fixation of other sounds, even though such sounds imitate or simulate those in the copyrighted sound recording" [USC114]. **Recommendation (§9.7): ship the faithful chain + ROM images built from CC0 recordings of real cymbals (option a), with user sample import into the 6-bit slots (ADM's model), and keep a synthetic metal source (option b) as an optional later differentiator.** Never embed, fit to, or ship anything derived from a 909 ROM dump or a recording of a 909.

---

## 1. Which voices are PCM, the ROMs and the address map

### 1.1 The sampled voices
[SM] p.5: "Hi-Hat, Ride and Crash cymbals are reproduced out of digital sound memories which have
been sampled from an actual instrument, modified to be useful as data and stored into the ROM by
way of PCM. Circuit configurations and operations of these voices are basically the same."
**CONFIRMED.**

The recordings [HOSH-A] (Hoshiai, who made them): hi-hat = 14" Paiste Sound Edge top with a
14" A. Zildjian New Beat bottom; crash and ride = **the same Paiste 18" thin crash struck
differently**; Sony C-38 microphone, Pioneer reel-to-reel, recorded on the office floor at night for
"moderate ambience"; "No EQ or compression was applied during recording. The amplitude was processed
on a computer after digitization." [HOSH-R]: edited on a CP/M-80 computer; "He sampled the hi-hats at
6-bit". A Synthtopia reader claims the crash was a Paiste Sound Formula Thin Crash of 16" or 18"
[RD9 comments] — **CONFLICTING on the crash diameter (18" per Hoshiai himself, treat 18" as right).**

### 1.2 ROM chips
| Voice | ROM | Position | Device | Size | Data bits wired |
|---|---|---|---|---|---|
| Hi-hat (CH + OH) | "C43" | IC69 | HN61256P | 32768 × 8 | D0–D5 (pins 11,12,13,15,16,17) → IC68 4174 |
| Crash | "C42" | IC62 | HN61256P | 32768 × 8 | D0–D5 → IC63 4174 |
| Ride | "C44" | IC54 | HN61256P | 32768 × 8 | D0–D5 → IC53 4174 |

[MAME] ROM_START: `hn61256p__c43.ic69`, `hn61256p__c42.ic62`, `hn61256p__c44.ic54`, each `0x8000`.
The schematic shows 15 address inputs (ROM pins 10,9,8,7,6,5,4,3,25,24,21,23,2,26,27) and six data
outputs. ROM pins 18/19 (D6/D7) are unconnected. **CONFIRMED.** [FRA] agrees ("Each of the three ROMs
are 32k … The samples have 6 bit resolution … Each sample is shifted by two bits to occupy eight
bits(unsigned)").

Data format: unsigned 6-bit (0–63), mid-scale ≈ 32, since the DAC is a unipolar resistor sum of
0/5 V CMOS outputs and the result is AC-coupled (C157/C127/C128 10 µF). **CONFIRMED** by topology.

### 1.3 Address counters and the hat address map
Per voice: a 4040 12-stage ripple counter (IC71 hat, IC61 crash, IC55 ride) provides A0–A11, and one
half of a 4520 dual 4-bit counter (IC70, IC58a, IC58b) provides A12–A14 plus the overflow bit used to
stop. TRIG resets both counters (4040 pin 11, 4520 pin 15/7). **CONFIRMED** (schematic).

Hat stop logic [SM] p.5 (verbatim): "With the same bits applied from the address counters, a logic
(D196–199 OR gates) places ROM beginning and end at different locations according to H or L of the
CLOSED/OPEN as shown in the table. IC72a turns its output to L (stop) when the counter increments to:
110 0000 0000 0000 … in OPEN mode; 010 0000 0000 0000 … in CLOSED mode." The address table lists
OPEN HI-HAT from 000 0000 0000 0000, a "COMMON ADDRESS" boundary at 110 0000 0000 0000, and CLOSED
HI-HAT up to 111 1111 1111 1111.

Reading: in CLOSED mode the CLOSED/OPEN line pulls ROM A13 and A14 high through D196/D197 (pull-ups
R486/R487 10k), so the ROM is read at 0x6000 + counter, and IC72a stops when the counter
reaches 0x2000. In OPEN mode the ROM address equals the counter and the stop happens at 0x6000.
(Corrected by fact-check: was "IC72a (NAND of counter A13 OR'd with the closed flag, and counter A14
OR'd with the closed flag …)". That cannot be right: with both inputs OR'd, CLOSED mode would stop at
counter 0, whereas [SM] p.5 says the reset counter's "0's cause IC72a output to swing to H(run)
irrespective of a CLOSED/OPEN being applied to diode OR's". The logic consistent with both stop
addresses is IC72a = NAND(counter A13, counter A14 OR CLOSED); exact diode wiring **UNVERIFIED**, the
region map below stands on the SM text. See F4.)

| Region | ROM addresses | Samples | At 30 kHz | At 32 kHz |
|---|---|---|---|---|
| Open hat | 0x0000–0x5FFF | 24 576 | 819 ms | 768 ms |
| Closed hat | 0x6000–0x7FFF | 8 192 | 273 ms | 256 ms |
| Crash | 0x0000–0x7FFF | 32 768 | 1092 ms | 1024 ms |
| Ride | 0x0000–0x7FFF | 32 768 | 1092 ms | 1024 ms |

**CONFIRMED** (SM text + schematic logic). For the crash and ride, the stop comes from the 4520's
fourth bit (counter = 0x8000) through NAND IC59a / IC57a, i.e. the full 32K is played. **CONFIRMED**
(schematic).

Run/stop: TRIG (a 5 V, 2 ms pulse from the IC1/IC10 trigger latches [SM p.5]) is differentiated by
C159 100p / R491 100k (τ = 10 µs) into a short reset pulse for the address counters (and the
timing flip-flops). The counters at 0 put the stop NAND IC72a's output high, and **that output is the
"run" signal** (labelled RUN/STOP at IC72a pin 3 on [SM] p.11). It enables the oscillator (IC72c/d)
and, via IC72b, gates the latch clock; it goes low at the stop address. [SM] p.5: "Upon receiving this
'run' from IC72a …"; [N909] says the same. (Corrected by fact-check: was "differentiated … into the set
input of IC73a (4013); the stop NAND resets it; IC73a's Q is the 'run' signal". On the scan, IC73a's S
input (pin 6) is tied to ground; IC73a/IC73b form the "TIMING GEN" ÷2/clock-phase pair. See F3.)
Crash: C50 100p / R189 100k; ride: C40 100p / R188 100k, same arrangement around IC59/IC60 and
IC57/IC56. **CONFIRMED** parts; the per-pin routing of the reset pulse within IC60/IC56 is **UNVERIFIED**.
Fact-check reading, **UNVERIFIED**, no DSP impact: in the ride, the 4520 (IC58b) appears to be the
*low-order* counter (clocked from IC56a, carry from its pin 14 clocks the 4040 IC55), the reverse of
the hat and crash order. The full counter is still a plain binary counter.

When stopped, the 4174 latch (reset pin tied high) holds the last sample value; the AC coupling
removes the DC. On the next trigger the counters reset and address 0 is latched on the first clock,
so **every hit starts at ROM address 0 with the clock phase reset** (the oscillator is gated, not
free-running). **CONFIRMED** (SM: "Upon receiving this 'run' from IC72a, a combination of two gates
(IC72 c and d) starts oscillation").

---

## 2. Clock oscillator, TUNE, and the sample rate

### 2.1 Circuit
Two NAND gates of a 4011UBP (unbuffered) form the classic CMOS astable: gate c output → gate d
input; gate d output → C (470 pF polystyrene, "(S)" selected part) → node N; node N → R_t → gate c
output side… more precisely R_t sits between the gate c/d junction and node N, and a 33k series
resistor (R493 hat, R442 crash, R440 ride; the crash/ride ones changed to 10k from serial 392600, see
§6.2) from node N into gate c's input limits the current through the input protection diodes. The oscillator output clocks a 4013 D flip-flop wired as ÷2 (IC73b hat,
IC60a crash, IC56a ride), whose Q drives the 4040 clock (pin 10). [SM] p.5: "starts oscillation and
outputs about 60 kHz, which is divided by two and shaped up by IC73 flip-flop (TIMING GEN), clocking
the address counters". **CONFIRMED.**

| Voice | R_t | C | Notes |
|---|---|---|---|
| Hat | R492 12k (fixed) | C160 470p | no panel TUNE |
| Crash | R443 6.8k + VR25 10k(B) "TUNE" | C131 470p | linear pot |
| Ride | R441 6.8k + VR27 10k(B) "TUNE" | C130 470p | linear pot |

**CONFIRMED** (schematic, [RW] p.14 for the hat, [FRA-MOD] "the resistor to be replaced is R492").

### 2.2 Frequency
f_osc = k / (R_t · C). For the ideal CMOS astable with thresholds at V_DD/2 and no clamping,
T = 2·ln 3 · RC ≈ 2.2 RC, so k = 1/2.2 = 0.455. For R492 = 12k, C = 470p that gives 80.6 kHz,
÷2 → 40.3 kHz. The service notes say "about 60 kHz" (→ 30 kHz), which corresponds to k ≈ 0.34
(T ≈ 2.96 RC). (Corrected by fact-check: was "the difference is plausible from the 33k series
resistor (the node swing is clamped by the protection diodes, which lengthens the charge time),
unbuffered-gate thresholds and strays". Diode clamping through the series resistor *shortens* the
period. A time-step simulation of this exact topology (R_t from gate-c output to node N, C from N to
gate-d output, Rs from N to the clamped gate-c input) gives T/RC = 2.197 (no clamp), 2.171 (Rs 33k),
2.120 (Rs 10k). Threshold asymmetry of ±15 % of V_DD only raises it to 2.25. Nothing in the ideal model
reaches 2.96; gate output resistance and the slow transitions of the unbuffered 4011UB are untested
candidates. The gap is **UNVERIFIED/unexplained**. See F2.) **CONFLICTING:**

| Source | f_osc (hat) | sample rate |
|---|---|---|
| [SM] p.5 "about 60 kHz … divided by two" | 60 kHz | **30 kHz** |
| 1/(2.2RC) [DERIVED] | 80.6 kHz | 40.3 kHz |
| [FRA] "I chose an arbitrary sample rate of 32 kHz as this is pretty close to the rate used for the hats" | — | ≈ 32 kHz |
| [MEASURED] crash trace length ≈ 1.02 s (386 px) for 32768 samples at pot centre (§7) (corrected by fact-check: was "≈ 1.1–1.2 s … ≈ 27–30 kHz") | — | ≈ 32 kHz (29–35 kHz at ±10 %) |
| [POLY] "6 bits resolution and 18 kHz" | — | 18 kHz (no source; reject) |

Use **30 kHz nominal** (f_osc 60 kHz) or 32 kHz (fact-check: the corrected drawing measurement and
[FRA] both point nearer 32 kHz; keep it a fit parameter), let the serial-number variance perturb it by a few percent, and
re-fit against a reference recording once the A/B harness exists (the fold frequency fs/2 ≈ 15 kHz
is visible in a 909 hat spectrum as the mirror point of the image band). Whittle: "The actual range
will vary from one machine to the next" and "If the tuning is pushed too high, the oscillator will
stop" [RW] (a 4011 astable fails below some R_t).

### 2.3 TUNE ranges [DERIVED, from CONFIRMED parts]
Because f ∝ 1/R_t with C fixed:

| | R_t min | R_t centre | R_t max | ratio max/min | semitones |
|---|---|---|---|---|---|
| Crash / ride | 6.8k | 11.8k | 16.8k | 2.47 | 15.7 (−6.1 … +9.5 around centre) |
| Hat (stock) | 12k | — | — | 1 | 0 |
| Hat, Fraser mod (6.9k + 22k pot) | 6.9k | — | 28.9k | 4.19 | 24.8 (+9.6 … −15.2 vs stock) |

With the SM's 60 kHz at 12k: crash/ride clock = 52.9 kHz (max), 30.5 kHz (centre), 21.4 kHz (min).
Whittle quotes the Fraser hat mod as "from about 0.43 to about 1.7 of the original frequency" [RW],
i.e. 12/28.9 = 0.415 and 12/6.9 = 1.74. **CONFIRMED.**

Sample duration vs tune (32768 samples): 0.62 s at max, 1.07 s at centre, 1.53 s at min.
Pitch follows the clock exactly (resampling): +9.5 st at max. The crash/ride envelope stretches with
it (§5), the hat envelope does not (§4), which is why a hat-tune mod makes short hats "tick".

### 2.4 Why the hats have no TUNE on the original
Fact: R492 is fixed; otherwise the three circuits are identical. [SM] p.5 gives the design logic
indirectly: the crash and ride need the address-derived envelope "because tuning of the instrument is
possible. As the signal gets shorter at higher tunings, the decay-time has to change in the same
manner" [N909 re-typing of SM]; the hats instead have manual DECAY knobs, which require the RC
envelope, and an RC envelope does not track a clock change. So the panel got Decay for the hats and
Tune for the cymbals, one knob-pair each. That reading is **UNVERIFIED (inference)**; the component
fact is **CONFIRMED**. Every modern product adds hat tune (Drumazon 2: "Closed Hihats, Open Hihats,
Crash, Ride: Tune – Base frequency of the instrument, Decay" [D16-2]; Roland's own plugin has CC 61
CH TUNE and CC 80 OH TUNE [RC909]; RD-9 "HI HAT TUNE" [RD9]). mw909 ships hat tune as a clock-rate
change with the stock value at centre.

---

## 3. The 6-bit DAC and the companding trick

### 3.1 Latch and resistor module
Each voice: ROM D0–D5 → 4174 hex D flip-flop (IC68 hat, IC63 crash, IC53 ride; outputs pins
2,5,7,10,12,15) → six resistors in a module "RM0621" (RA9 hat, RA10 crash, RA12 ride) joined at a
common output node → 10 µF coupling capacitor (C157/C127/C128) → the VCA input stage. **CONFIRMED**
(schematic). The latch is clocked once per sample by IC72b/IC59d/IC57d (the ÷2 output gated by the run
latch), so the ROM output settles before it is latched; the DAC is a plain zero-order hold at the
sample clock. **CONFIRMED.**

The same RM0621 module appears as the DAC of each accent latch (RA1–RA8 on IC2–IC9): [SM] p.5
"Latched ACCENT code is converted to analog equivalent at the output of associated resistor array
RM0621." A six-resistor module with one common pin used as a code-to-voltage converter must be
binary-weighted (R, 2R, 4R, 8R, 16R, 32R) — a weighted-resistor DAC, not an R-2R ladder. The
internal values are **UNVERIFIED** (no datasheet found; "RM0621" is a Roland part number).

### 3.2 Nonlinearity
A weighted-resistor DAC driven by CMOS outputs has these error sources: (a) the 4174 output
resistance (hundreds of ohms, different for the high and low state) in series with each weight, which
matters most for the MSB (smallest resistor); (b) resistor-module tolerance (typically 1–2 %); (c)
output glitches when several bits change (the 4174 outputs do not switch simultaneously; the step at
mid-scale 011111→100000 shows a glitch of roughly one clock edge width). Expected static INL/DNL on
the order of 1–3 % of full scale, i.e. up to about one LSB at the MSB transition. **UNVERIFIED
(engineering estimate; no measurement found).** Model: per-unit random bit weights w_i = 2^i·(1 + ε_i),
ε_i ~ ±2 % for the MSB down to ±0.5 % for the LSB, plus an optional mid-scale glitch; keep it off by
default and tied to the serial-number variance.

### 3.3 Companding = effectively more than 6 bits
[SM] p.5 (verbatim): "The sound results at RA9 output has an envelope somewhat different from that of
actual Hi-Hat sound. This is because the Hi-Hat sounds have been compressed before being digitalized
and Pulse Code Modulated (PCM) in order to have greater S/N ratio and higher digital resolution. The
envelope of this Hi-Hat sound can be controlled manually with DECAY control (VR21 or VR23)." And for
the cymbals: "These voices also have unique envelopes that are quite different from actual sounds when
the data are directly reproduced … Restoration of the envelopes are made by the use of ROM addresses
as the envelope data." [FRA]: "they are heavily compressed to make maximum use of the bits, and the
original envelope is then restored by the vcas." **CONFIRMED.**

So the ROM holds a near-constant-amplitude "carrier" of the cymbal's spectral evolution, quantised to
64 levels, and the analog VCA supplies the ~40–60 dB of decay. The quantisation noise therefore stays
a fixed ~38 dB (6.02·6 + 1.76) below the carrier throughout the note and decays with it, instead of
swamping the tail as a straight 6-bit recording would. For mw909 this means: **whatever source is
baked into a ROM image must first be envelope-flattened (slow AGC / divide by its own smoothed
envelope) and peak-normalised to use all 64 levels, and the natural decay is then put back by the
modelled VCA.** Ideal quantiser (no dither) is the right default; the original's A/D was an in-house
converter [HOSH-A] with no documented dither.

---

## 4. Hi-hat: VCA, envelope, decay knobs, choke, accent

### 4.1 Signal flow (parts from [SM] p.11; [RW] p.14 reproduces this section)
Corrected by fact-check (see F5, F6); the original diagram routed the signal through Q84, which is
wrong:
```
signal:  IC69 ROM ─► IC68 4174 ─► RA9 RM0621 ─► C157 10µ ─► node X ─► Q85 (s) C–E ─► IC67b (−) (R483 47k fb) ─► R480 5.6k ─► LPF1 ─► LPF2 ─► C134 10µ ─► R444 22k ─► IC65b (VR22 50k(B) LEVEL) ─► HI-HAT out / mix
                                                        node X ─ R482 22 Ω ─ gnd
control: C135 envelope ─► R453 220k ─► IC67a (−) ; IC67a out ─► R484 2.2k ─► Q84 tied emitters
         Q84 left collector ─► IC67a (−)   (DC feedback: log/exp converter; C156 1000p out→(−) = compensation)
         Q84 left base ◄─ R481 4.7k (ACC), R489 150 Ω to gnd, R488 2.2k from the RUN/STOP line
         Q84 right base ─ R490 100 Ω ─ gnd ;  Q84 right collector ─► R485 10k ─► Q85 base (the VCA)
```
**CONFIRMED** parts, and **CONFIRMED** that Q85 is the VCA element driven from Q84's right collector:
the fact-check re-read of the 600 dpi scan agrees with [RW] p.10 ("current to flow out of the right
transistor's collector and so turn on Q85"; "the emitters of the dual PNP transistors Q84 being driven
strongly negative"). This mirrors the cymbals, where Q70 drives VCA transistor Q71. The DC polarity
and operating point of the converter are **UNVERIFIED**. R488 → RUN/STOP is a fact-check reading,
**UNVERIFIED**. R482 is printed "22" with no unit. On this schematic unit-less values are ohms (R489
"150", R490 "100", R428 "220", R429 "100" in the same circuits), so **R482 = 22 Ω**. It is the fixed
lower leg of the Q85 "voltage controlled potentiometer", not a bias resistor. The same applies to
R431/R438 "22" in the crash/ride. (Corrected by fact-check: was "almost certainly 22 kΩ (bias return
for Q85's base, fc = 1/(2π·10µ·22k) = 0.72 Hz)". The C157 coupling corner is set by C157 and the DAC's
source resistance (RM0621 values unknown), so it is **UNVERIFIED**.)

### 4.2 Envelope generator (CH DECAY / OH DECAY)
[SM] p.5 (verbatim): "CLOSED… A high CLOSED/OPEN on Q72 base removes a positive voltage from its
collector which in turn allows Q73 to charge DECAY capacitor C135 through R451 and VR21. Since this
charging path is 1/tenth the total resistance of R452 and VR23, the charging rate of C135 depends on
VR21 setting. OPEN… With low CLOSED/OPEN, CH charging path is disconnected from the DC supply source at
Q73 OH path becomes conductive." [N909] adds: "A disadvantage of this circuit is, that CH Decay is
changing slightly when turning OH-Decay (VR23)."

Parts: C135 1 µF/50 V; CH path R451 10k + VR21 100k(B) linear; OH path R452 100k + VR23 1M(A) log;
switch transistors Q72 (R447 22k, R448 10k) and Q73 (R450 10k, R449 22k); Q74 with R454 22k / R455 10k
resets the capacitor on TRIG (**UNVERIFIED** that Q74 is the reset switch; it is the only transistor
on the capacitor node and sits on the TRIG line). **CONFIRMED** values.

Charging time constants [DERIVED]:

| Mode | R_charge | τ = R·C135 |
|---|---|---|
| CLOSED, VR21 min | 10k ∥ (100k … 1.1M) | 9.1 – 9.9 ms |
| CLOSED, VR21 max | 110k ∥ (100k … 1.1M) | 52 – 100 ms |
| OPEN, VR23 min | 100k | 100 ms |
| OPEN, VR23 max | 1.1 M | 1.1 s |

(The CH path is only "1/10th" of the OH path at the knobs' extremes; at CH max and OH min the two are
equal, which is the SM's "changes slightly".) The envelope is the capacitor voltage **rising** from 0
toward +15 V; the VCA closes as it rises. The audible decay is shorter than τ because the exponential
control law drops the gain by ~8.7 dB per 26 mV of base-voltage difference at the pair (see §4.3), so
only the first volt or so of the ramp is audible; this is also why the SM's photos show the CH at
knob centre lasting ≈ 65–100 ms while τ ≈ 46–48 ms [MEASURED, §7]. (Corrected by fact-check: was
"τ ≈ 55 ms", which takes VR23 1M(A) at half its resistance. VR23 is an audio (log) taper, about 10–15 %
of 1 MΩ at mid-rotation, so the OH path is ≈ 200–250k and τ = (10k + 50k) ∥ (200…250k) · 1 µF =
46.2–48.4 ms. See F7.) The "control law" mechanism in this paragraph (gain falling
exponentially with V_C135) is **UNVERIFIED**. Per §4.1 the envelope enters as a *current* (V_C135/R453)
into the IC67a/Q84 converter.

### 4.3 VCA and anti-log law
(Corrected by fact-check, see F5: the signal does **not** pass through Q84. Q84 is in the control path
only. Its right-collector current drives the base of Q85, and Q85 plus R482 22 Ω form the "voltage
controlled potentiometer" that the audio passes through. IC67a with R453/C156 is not a "buffered,
723 Hz low-passed" stage: 1/(2π·220k·1000p) = 723 Hz is the R453/C156 integrator's unity-gain
frequency inside a transistor-feedback (log/exp) loop. The sigmoid below is kept as the original
author's working model, **UNVERIFIED**. For a converter of this topology, I_c,right ≈ (V_C135/R453)·
exp(ΔV_B/V_T) with ΔV_B set by ACC/RUN on the left base; fit the law to §7.)
Original text: Q84 is a 2SA798 matched PNP pair with tied emitters: a current-steering differential pair. The signal
current from the input stage enters the common emitter node; the fraction steered to the collector that
feeds IC67b's summing node is 1/(1 + exp(ΔV_B/V_T)), where ΔV_B is the base-voltage difference set by
the (buffered, 723 Hz low-passed) envelope capacitor voltage on one side and the ACC-derived offset on
the other. For ΔV_B ≫ V_T (26 mV) the gain is exp(−ΔV_B/V_T): the "anti-log" the SM refers to. The
SM calls the same Q71/Q69 arrangement in the cymbals "configured as a voltage controlled
potentiometer". **CONFIRMED** topology class (tied-emitter dual PNP, bases driven by envelope/ACC);
the divider constants that map the capacitor voltage and ACC onto ΔV_B are **UNVERIFIED** and should be
fitted to the §7 decay figures: g(t) = σ((a·V_ACC − b·V_C135(t))/V_T), V_C135(t) = 15 V·(1 − e^(−t/τ)).

Practical DSP form: gain_dB(t) = −(b/V_T)·8.686·V_C(t) dB above a floor, clamped at 0 dB; with the
measured CH "−20 dB at ≈ 65 ms, τ ≈ 55 ms", b ≈ 0.006 (i.e. ≈ 60 mV of base swing per 10 V of
capacitor). (Fact-check: the arithmetic reproduces, b = 20·0.026/(8.686·10.40 V) = 0.0058. With the
corrected τ ≈ 47 ms, V_C(65 ms) = 11.24 V and b ≈ 0.0053. Both are fit values for an **UNVERIFIED** law.) Expect the real circuit to add a short plateau while the sigmoid is saturated at accent
levels, which is why accented hats are both louder and a little longer.

### 4.4 Choke (open vs closed)
Single voice: TRIG resets IC70/IC71 and the run latch, the CLOSED/OPEN line (from the CPU, latched with
the trigger) selects the ROM region, Q74 resets C135 and the appropriate charge path is enabled. A
closed-hat hit therefore truncates an open hat instantly (one sample clock) and restarts from ROM
0x6000 with the CH envelope; an open-hat hit truncates a closed hat the same way. There is no
cross-fade and no separate "choke" circuit. **CONFIRMED** (schematic + [FRA] + [TT]). Behringer's RD-9
reproduces this so literally that muting CH also mutes OH [RD9 reviews] — mw909 should keep the single
voice but mute per instrument at the trigger level.

### 4.5 Accent
ACC (per-voice accent voltage from the accent latch DAC, [SM] p.5) enters through R481 4.7k into the
Q84 control node. Effect: a fixed offset in ΔV_B → the whole decay is louder and the audible part
longer. [MEASURED] at MULTI OUT, knobs centred: CH 1.6 Vpp with accent, 0.96 Vpp without (+4.4 dB);
durations to −20 dB ≈ 65 ms (accent) vs ≈ 100 ms (no accent) — the unaccented photo is longer, which
suggests the accent photo was taken on a different sweep position or that the plateau/knee moves;
treat the two durations as a 65–100 ms band. **CONFIRMED** mechanism, **UNVERIFIED** magnitudes.

---

## 5. Crash and ride: address-ramp envelope, anti-log, VCA

### 5.1 Signal flow (crash; ride in brackets)
```
IC62 [IC54] ROM ─► IC63 [IC53] 4174 ─► RA10 [RA12] RM0621 ─► C127 [C128] 10µ ─► Q71 [Q69] (s) ─► IC51b [IC51a] (R425 [R424] 47k fb) ─► R456 [R462] 5.6k ─► LPF1 ─► LPF2 ─► C132 [C133] 10µ ─► R445 [R446] 22k ─► IC65a [IC64a] (VR24 [VR26] 50k(B) LEVEL) ─► out
envelope: address bits A9–A14 ─► RA11 [RA13] RM0621 ─► R432 270k [R439 470k] ─► IC52b [IC52a] (C126 [C125] 1000p fb) ─► R430 [R436] 2.2k ─► Q70 [Q68] 2SA798 (R429 [R434] 100, R428 [R433] 220, R427 [R437] 2.2k) ─► Q71 [Q69] base (VCA)
accent: ACC ─► R426 [R435] 4.7k ─► Q70 [Q68] control node
```
**CONFIRMED** parts (schematic). [SM] p.5 (verbatim): "When CRASH sound data are read successively
from ROM (IC62) with correct addresses, the same addresses are also converted to analog voltages
through RA11, anti-log tapered by IC52b and Q70, and are applied to the base of Q71 (VCA) which is
configured as a voltage controlled potentiometer to give the incoming voltage the CRASH decay curve."
"Before being stored into the ROM, the envelope of CRASH is changed with the following compensation
measure taken into consideration."

### 5.2 The envelope DAC
RA11's six inputs come from the top of the address counter: 4040 Q10, Q11, Q12 (pins 14, 15, 1 =
A9, A10, A11) and 4520 Q0, Q1, Q2 (pins 3, 4, 5 = A12, A13, A14); pin 6 (A15 overflow) goes to the stop
NAND. **CONFIRMED** for the crash by an independent fact-check re-read of the 600 dpi scan (RA11 rows,
top to bottom: 4520 pins 5, 4, 3, then 4040 pins 1, 15, 14; pin 6 goes to the stop logic and ROM CS).
Ride RA13 **UNVERIFIED**. Result: a 64-step staircase,
one step per 512 samples (17.1 ms at 30 kHz, 9.7 ms at max tune, 24 ms at min tune), smoothed by
C126 1000p against 270k (τ = 0.27 ms; ride 0.47 ms) — i.e. the steps are not smoothed audibly; the
envelope is a 64-step exponential staircase. **[DERIVED]** (Fact-check: the R·C arithmetic is right.
But R432·C126 is the integrator constant of a closed transistor-feedback loop, not a smoothing time
constant. The closed loop is faster still, so the conclusion "steps not smoothed" stands.)

IC52b with only C126 in its feedback and Q70 in the loop forms, together with the pair, a log/anti-log
converter (the op-amp forces Q70's collector current to equal the DAC current V_ramp/R432, so the
emitter-node voltage is V_T·ln of the ramp; the other half of the pair re-exponentiates it with the ACC
offset and drives Q71). The net control law is **exponential in the address**, i.e. a straight line in
dB versus sample index, with the slope set by R432 (crash) vs R439 (ride) and the ladder weights.
**UNVERIFIED** detail; **CONFIRMED** behaviour ("anti-log tapered", [FRA] "an exponential decay with
the same length as the sample regardless of tune setting").

### 5.3 Decay figures [MEASURED, §7]
(Corrected by fact-check, see F8. The original said: "crash reaches ≈ −14 dB (20 % of peak) at
≈ 1.2 s, which is the end of the sample; ride reaches ≈ −14 dB at ≈ 1.4 s … roughly −14 to −20 dB over
the full 32768 samples … The ride's longer measured length than the crash … is CONFLICTING".)
Re-measured on the [SM] p.9 drawings (38 px/div, 0.1 s/div), relative to each trace's peak:
**crash** holds near full level for ≈ 0.1 s, then ≈ −4.8 dB at 0.43 s, −8.8 dB at 0.62 s, −12.5 dB at
0.82 s, −14 dB (20 %) at ≈ 345 px ≈ **0.91 s**, and the trace **ends abruptly at ≈ 386 px ≈ 1.02 s at
about −19 to −23 dB** (counter stop). **Ride** decays much faster at first: −8.8 dB at ≈ 0.13 s, −14 dB
(20 %) at ≈ 115 px ≈ **0.30 s**, −20 dB at ≈ 190 px ≈ **0.50 s**, ≈ −26 dB at 0.8 s, with residual
wiggle to ≈ 0.9–1.0 s near −30 dB. Crash and ride therefore have the **same length (≈ 1.0 s, consistent
with identical clocks at pot centre)**; there is no length conflict. Their envelope *shapes* differ a
lot (ride steep then slow, crash plateau then roughly linear in dB). Part of that is presumably the
ROM content's own residual envelope; the drawings are hand-drawn, so treat the shapes as ±3 dB.

Implementation: envelope_dB(n) = −K·floor(n/512)/63 with K fitted per voice (crash K ≈ 20–23 dB over
the sample; ride ≳ 30 dB with a much steeper start, so a single straight-line-in-dB law will not fit
the ride drawing; corrected by fact-check: was "crash K ≈ 14–20 dB … ride similar or slightly
smaller"; law **UNVERIFIED**), plus the accent offset; the staircase steps
are 17 ms apart at 30 kHz, which is audible as a faint 58 Hz "zipper" on long tails and is part of the
sound; the ACC offset shifts the whole curve up.

### 5.4 Accent
Same as the hat: ACC through 4.7k (R426 crash, R435 ride) into the pair's control node → gain offset.
[MEASURED] the SM photographs of crash/ride do not say whether accent was on; the LT photo in the same
row is "with accent", so assume the 2.3/2.2 Vpp figures are accented.

---

## 6. Reconstruction (anti-imaging) filters, level stage, output

### 6.1 Two Sallen-Key low-passes, emitter-follower buffered (identical in all three voices)
| Stage | Parts (hat / crash / ride) | R1, R2 | C1 (fb) | C2 (gnd) | f0 | Q |
|---|---|---|---|---|---|---|
| LPF1 | R480,R475 / R456,R457 / R462,R463; C148,C149 / C136,C137 / C144,C145; Q80 / Q75 / Q77 (R476/R458/R464 10k emitter) | 5.6k, 5.6k | 1200p | 1000p | **25.9 kHz** | **0.548** |
| LPF2 | R477,R479 / R459,R461 / R465,R467; C150,C151 / C138,C139 / C146,C147; Q81 / Q76 / Q78 (R478/R460/R466 10k) | 5.6k, 5.6k | 2700p | 390p | **27.7 kHz** | **1.316** |

[DERIVED]: f0 = 1/(2π·R·√(C1·C2)), Q = ½·√(C1/C2) for equal R and a unity-gain buffer.
Cascade response [DERIVED]: 1 kHz 0.0 dB; 10 kHz −0.06; 15 kHz −0.18; 20 kHz −0.56; 25 kHz −1.95;
30 kHz −5.1; 35 kHz −9.4; 40 kHz −13.7; 45 kHz −17.7; 60 kHz −27.8 dB; −3 dB at ≈ 27 kHz. Stage 2's
Q = 1.3 peaks +3.06 dB at 23.4 kHz (fact-check refinement: was "+3 dB near 25 kHz") and cancels stage 1's droop, so the pass-band is flat to 20 kHz.
**CONFIRMED** parts (both the 200 dpi and 600 dpi scans read 1200P/1000P and 2700P/390P in all three
voices; [RW] p.14 agrees for the hat).

Consequence: with fs ≈ 30 kHz, a 5 kHz component of the ROM data has its first image at 25 kHz
(−2 dB) and a 10 kHz component's image at 20 kHz (−0.6 dB). The filter removes the clock (30 kHz,
−5 dB — barely) and the higher images, not the first image band. **The 15–22 kHz content of a 909 hat
heard through a 44.1 kHz recording is mostly imaging of the 8–15 kHz ROM content.** The emulation must
run the DAC/VCA/filter at an oversampled rate (≥ 4× the ROM clock, e.g. 120–192 kHz internal) and
band-limit to the host rate with a proper anti-alias filter so that the 15–22 kHz images survive and
the >22 kHz ones do not fold again. This is the single most important "character" fact in this
document after the 6-bit quantisation itself.

### 6.2 Level and output
C134/C132/C133 10 µF → R444/R445/R446 22k → inverting op-amp with VR22/VR24/VR26 50k(B) as feedback
(gain 0 … −2.27, M5218L op-amps). Hat output goes to the MULTI JACK board "HI-HAT" and the mix bus.
**CONFIRMED.** [SM] p.11 note "SN 415300-UP 0.01" on C134: units from serial 415300 use 0.01 µF
instead of 10 µF for C134 (a high-pass at 1/(2π·0.01µ·22k) = 723 Hz on the hat output). Whittle: "I
also check that C134 … is 0.01 uF, rather than 10 uF" [RW]. **CONFIRMED** change; so late units have a
hat output HPF at ≈ 720 Hz (which also kills the DAC's DC step on retrigger). The equivalent note on
the crash/ride level stages is not present on the scan (**UNVERIFIED** whether C132/C133 changed).
Also "SN 392600 UP 10K" on R442 (crash clock series resistor, 33k → 10k from serial 392600) and the
same 33k struck through to 10k on R440 (ride) **CONFIRMED** on the scan; a smaller series resistor
lets the input protection diodes clamp node N harder, which shortens the period slightly (fact-check
simulation of the ideal astable: T/RC 2.171 → 2.121 at R_t 11.8k, i.e. ≈ 2.4 % higher clock, ≈ +0.4
semitone, for 10k vs 33k; the real magnitude is still **UNVERIFIED**; the same CONFLICTING-k question as §2.2 — early and late units may differ by a few
percent in cymbal pitch at the same knob position). The hat's R493 stayed 33k.

---

## 7. Service-manual check figures (scope photos, [SM] p.9)

The 909 service notes have no table of Vpp/decay values for these voices (unlike the 808's); they
give oscilloscope photographs "Observed at MULTI OUT jack with all knobs set to center". I measured
the trace extents on the 600 dpi scan (page `..._0008.jp2`). Calibration: the hand-clap photo
(10 ms/div) shows its four bursts 38, 39, 37 px apart; the 909 clap spacing is ≈ 10 ms (sibling doc
01), so **1 div ≈ 38 px (±10 %)** at that scale; all photos appear to share the same magnification.
(Fact-check: the burst spacing re-measures at 38/39/37 px. Note that these are **hand-drawn line
renderings of scope traces with no graticule**, not photographs. The "≈ 10 ms" clap spacing rests on
sibling doc 01, which did not exist in `research/` when this was checked, so the time calibration is
**UNVERIFIED**.)
Trace line thickness (~6 px) limits the −20 dB (10 %) readings; the −14 dB (20 %) readings are more
reliable.

| Photo | Scale | Peak-to-peak | Length to 20 % | Length to 10 % |
|---|---|---|---|---|
| CRASH | 500 mV/div, 0.1 s/div | 176 px → **≈ 2.3 Vpp** | ≈ 345 px → **≈ 0.91 s**; trace ends at 386 px → **≈ 1.02 s** (corrected by fact-check: was 457 px → ≈ 1.2 s) | trace ends first (≈ 1.02 s, at ≈ −19…−23 dB) (was "same") |
| RIDE | 500 mV/div, 0.1 s/div | 167 px → **≈ 2.2 Vpp** | ≈ 115 px → **≈ 0.30 s** (corrected by fact-check: was 522 px → ≈ 1.4 s) | ≈ 190 px → **≈ 0.50 s** (was 530 px); residual to ≈ 0.9–1.0 s |
| HI-HAT CLOSED with accent | 500 mV/div, 20 ms/div | 120 px → **≈ 1.6 Vpp** | 101 px → ≈ 53 ms | 124 px → **≈ 65 ms** |
| HI-HAT CLOSED w/o accent | 500 mV/div, 20 ms/div | 73 px → **≈ 0.96 Vpp** | 165 px → ≈ 87 ms | 195 px → **≈ 100 ms** |
| OPEN | 500 mV/div, 0.1 s/div | 89 px → **≈ 1.2 Vpp** | 112 px → **≈ 0.29 s** | 186 px → ≈ 0.49 s |
| HAND CLAP (calibration) | 500 mV/div, 10 ms/div | 232 px → ≈ 3.0 Vpp (fact-check re-measure: 223 px → 2.9 Vpp) | | |

All **[MEASURED]**, ±10 % from the calibration plus ±1 px/div. The clap's 3.0 Vpp versus the SM's
"ADJUSTMENT LEVEL: Center MULTI OUT 2 Vp-p" for the clap trim (p.11) suggests the calibration could
be up to 1.5× too generous for amplitude but not for time (the crash would then be shorter than its
own sample); treat Vpp as "about" and use ratios (accent +4.4 dB; crash ≈ ride ≈ 1.4–1.9× CH-accent)
as the firmer facts. (Corrected by fact-check: the ratio is ≈ 1.4× (crash 177/121 = 1.46, ride
168/121 = 1.39), not "1.4–1.9×"; see F9. Also, a crash ending at ≈ 1.02 s rather than 1.2 s weakens
the parenthetical argument: a time scale 1.5× too generous would put the end at ≈ 0.68 s, i.e.
fs ≈ 48 kHz, which is above even the ideal astable's 40 kHz.) The implementer should tabulate these against the render (brief's fidelity
rule) and against reference recordings.

---

## 8. Implementation notes for mw909 (what the chain model must contain)

1. **ROM images:** three 32768 × 6-bit arrays (store as uint8 0–63; 96 KiB total). Hat map per §1.3.
   Built at build time or at init from the shipped source material by a reproducible `rombuild` step
   (envelope flatten → normalise → resample to the nominal ROM rate → 6-bit quantise), see §9.
2. **Trigger:** reset read index to the region start, reset the clock-phase accumulator, latch
   CLOSED/OPEN, reset the hat envelope capacitor, latch ACC. Never reset anything else (filters, VCA
   state keep running; brief rule). Retrigger of the hat = choke.
3. **Clock:** f_clk = f_nom · 12k/(R_t) with f_nom = 30 kHz (±3 % per-unit), R_t = 6.8k + pot for
   crash/ride, 12k·2^(−tune/12) for the added hat tune (stock at 0). Phase accumulator at the
   oversampled internal rate; zero-order hold of the 6-bit value (no interpolation: the DAC is a
   hold). Optional DAC bit-weight errors (§3.2). Stop at the region end and hold the last value.
4. **VCA:** gain = σ((a·ACC − b·env)/V_T) (hat) or exp(−K·addr/32768 + c·ACC) (crash/ride), applied
   to the AC-coupled DAC output; include the 10 µF DAC coupling (corner set by C157 and the unknown
   DAC source resistance, a few Hz at most; corrected by fact-check: was "10 µF/22k 0.72 Hz", since
   R482 is 22 Ω, see §4.1) and, for late units, the
   0.01 µF/22k 720 Hz hat output HPF as a "serial ≥ 415300" variance option.
5. **Filters:** two biquads per §6.1 at the internal rate, then decimate to the host rate with a
   steep anti-alias filter (the brief's "latencySamples()" slot covers its delay).
6. **Level:** × up to 2.27 (pot), calibrate so that centre knobs give the §7 Vpp figures within a
   few percent from 44.1 to 192 kHz.
7. **Accent:** per-voice ACC in the control domain, not a post-gain.
8. **Variance:** clock ±3 %, C135 ±10 %, bit weights, LPF caps ±5 %, from the serial seed.
9. **CPU:** trivial (one table read + two biquads per oversampled sample per voice); keep the
   oversampled section only while a voice is active.

---

## 9. THE LICENSING QUESTION

### 9.1 What is protected and for how long
- The ROM contents are fixations of actual cymbal recordings made by a Roland employee in 1983 and
  published with the TR-909 in 1984 [HOSH-A][WIKI]. In copyright terms they are **sound recordings /
  phonograms** owned by Roland (work made for hire / producer's right), independent of any copyright in
  the circuit or firmware. **CONFIRMED** (nature of the data; ownership by Roland is the obvious
  default, no assignment to anyone else is known).
- Term: **Japan** — phonogram producers' right lasts 70 years from publication [JPCR] → ~2054.
  **EU/UK** — 70 years from publication for published sound recordings (Directive 2011/77/EU; CDPA
  s.13A as amended by the 2013 Regulations [UKCR]) → ~2054. **US** — sound recordings fixed after
  15 Feb 1972 have federal copyright; a work made for hire runs 95 years from publication
  (17 U.S.C. § 302(c)) → ~2079. **CONFIRMED** terms, **UNVERIFIED** exact expiry years (depend on the
  publication date Roland would assert).
- Copying the data is infringement regardless of transformation: the CJEU held that a phonogram
  producer can prohibit even "a very short sound sample" being taken "unless that sample is included
  in a modified form unrecognisable to the ear" [PELHAM]; in the US the 6th Circuit allows no de
  minimis defence for sound recordings [BRIDGE] while the 9th Circuit does [VMG] — a 32768-sample copy
  is not de minimis anywhere. Bit-crushing, resampling, re-enveloping or "running the ROM through our
  model" does not create a new work. **CONFIRMED.**
- **Imitation is lawful.** 17 U.S.C. § 114(b): "The exclusive rights of the owner of copyright in a
  sound recording … do not extend to the making or duplication of another sound recording that
  consists entirely of an independent fixation of other sounds, even though such sounds imitate or
  simulate those in the copyrighted sound recording." [USC114] The EU/UK phonogram right likewise
  protects the fixation, not the sound of a cymbal, a microphone or a room. There is no copyright in a
  timbre. **CONFIRMED.**
- Consequences for mw909: (1) never obtain, keep, analyse or fit against a ROM dump (the brief's
  rule; also a clean-room hygiene point — a developer who has studied the dump and then "synthesises"
  something that matches it waveform-for-waveform has copied it); (2) recordings *of* a 909
  (sample packs, CC0 reference sets, YouTube rips) contain the ROM content and are equally unshippable
  — they may be used for listening tests and spectral/temporal comparisons only, exactly as the brief
  says for reference recordings; (3) matching statistics (spectral envelope, decay, density) is fine;
  matching samples is not; (4) brand names stay out of everything shipped (existing rule).

### 9.2 How the professional products handle it
| Product | Approach | Source / tag |
|---|---|---|
| Roland Cloud TR-909, TR-8S, TR-09, TR-8 | The owner: "Powered by ACB technology with original sample ROM and circuit modeling"; the plugin adds CH TUNE / OH TUNE (CC 61 / 80). | [RC909] **CONFIRMED** |
| AudioRealism ADM | "Out of respect for Roland's copyright, genuine 909 samples aren't provided … (some convincing replacements are included instead), though you can import your own." Users load .wav/.aiff into the drum parts. | [ADM] **CONFIRMED (secondary)** |
| D16 Drumazon / Drumazon 2 | Markets "the 6-bit sampled hi-hats and cymbals" as emulated "with every subtle nuance"; parameters Tune + Decay for CH/OH/crash/ride; the origin of its sample data is not disclosed anywhere I found. | [D16-2] **UNVERIFIED source** |
| Behringer RD-9 | Manual: "Digital (Sampling): Closed hat, Open hat, Crash, Ride", with HI HAT TUNE, CRASH TUNE, RIDE TUNE. No public statement on the sample origin; reader consensus is that an unlicensed clone "has to use similar but not exact" samples and several users report "The high-hats sound completely wrong", consistent with new recordings. | [RD9] **UNVERIFIED (new samples likely)** |
| Wave Alchemy Revolution / Transistor Revolution / 909 Drums | Multisampled recordings of real 909 hardware (so they embed the ROM audio); sold for years without known action — tolerance, not a licence. | attackmagazine/bedroomproducersblog coverage **CONFIRMED approach, UNVERIFIED licence status** |
| Softube Heartbeat | "Inspired by, rather than a direct emulation"; "the Kick, Snare and HiHats instruments mix sampled waveforms in" (origin undisclosed). | MusicRadar review **UNVERIFIED source** |
| Arturia Spark | Analog modelling + samples + physical modelling; 909 kit sample origin undisclosed. | **UNVERIFIED** |
| Tiptop Audio HATS909 / CYMBL909 | Hardware replicas that play "the original … sample ROM" through the original analog chain (fact-check: that phrase is **not** in the local HATS909 manual, which says "a low-fi sample based generator" whose "sampled sound pass through the original analog filters and VCA of the 909 machine"; quote **UNVERIFIED**); licensing status not stated (Tiptop sells them under its own name with explicit TR-909 references). | [TT] **UNVERIFIED licence** |
| e-licktronic Nava | DIY clone shipped with "3x program EPROM 27C256 with the sound of HiHat, Crash and Ride as those present in the original TR-909". No licence known. | [NAVA] **CONFIRMED claim, UNVERIFIED licence** |
| Open source: ER-99 (GPL-3), HTML-909 (MIT), 9W9, Faust 909 | Hats/cymbals are sample files of undisclosed provenance (ER-99 ships `hh.wav` 44.1 kHz/24-bit 0.46 s, `crash.wav` 0.98 s, `ride.wav` 1.07 s — recordings of a 909, not synthesis). Their code licences do not license the audio. **Do not reuse.** | [ER99] **CONFIRMED files, UNVERIFIED provenance** |

Lesson: only Roland ships the real thing; the serious third parties either (i) include substitute
recordings and let users import (ADM), or (ii) say nothing about where their data came from. There is
no precedent of Roland licensing the ROM to a competitor.

### 9.3 Option (a): original recordings of real cymbals through the faithful chain
- **What:** record (or source under CC0) real hi-hats and a crash/ride, process into three 6-bit ROM
  images (§3.3, §8.1), play through the modelled DAC/clock/VCA/filter chain.
- **Legal:** cleanest practical option — an independent fixation [USC114]; no Roland material anywhere.
  Keep a `PROVENANCE.md` with each source's URL, author, licence text and the processing script so
  the chain of title is auditable. CC0 is the only Creative Commons licence suitable for an AGPL
  plugin's binary data without attribution plumbing; CC-BY would also work but needs the credits in
  the about box.
- **How close:** the chain reproduces the lo-fi *character* exactly (6-bit, ~30 kHz imaging, zero-order
  hold, clock-rate tune, address-staircase envelope, anti-log VCA, 27 kHz filters, choke). The *timbre*
  will be "a 909-style hat from these cymbals", not the 909's hat: different cymbals, mic, room. The
  originals were a 14" Paiste Sound Edge top / 14" Zildjian New Beat bottom pair and one 18" Paiste
  thin crash used for both crash and ride [HOSH-A]; choosing similar cymbals (bright, medium-weight
  14" hats; a thin 18" crash, ride = the same crash hit on the bow with a soft decay) and a close dry-ish
  mono mic in a small room gets the family resemblance. Closed hat needs a tight "chick" (pedal closed),
  open hat a half-open sizzle; the 909 OH is only 0.8 s of ROM at nominal rate.
- **Candidate CC0 sources (vetted for licence tag only, not yet auditioned):**
  - Freesound pack "14" Zildjian Constantinople Hi-Hats" by trivialAccapella (CC0) —
    https://freesound.org/people/trivialAccapella/packs/24125/
  - Freesound pack "Zildjian A Custom Mastersound Hi-Hats" by DanVGC++ (CC0) —
    https://freesound.org/people/DanVGC++/packs/38575/
  - Freesound pack "hi-hats" by Logicogonist (CC0) — https://freesound.org/people/Logicogonist/packs/43426/
  - Freesound pack "crashes" by Logicogonist (CC0) — https://freesound.org/people/Logicogonist/packs/29712/
  - Freesound pack "Cymbal" by Sajmund (CC0) — https://freesound.org/people/Sajmund/packs/8322/
  - Freesound CC0 search for ride cymbals (per-file licences; check each):
    https://freesound.org/search/?q=ride+cymbal&f=license%3A%22Creative+Commons+0%22
  - VCSL (CC0, "even make commercial software"): Idiophones/Struck Idiophones "Hi-Hat Cymbal",
    "Suspended Cymbal 1", "Suspended Cymbal 2", "Clash Cymbals" — orchestral playing, so useful mainly
    for the crash/ride wash [VCSL].
  - Not usable: Pianobook (no redistribution), BPB "Cassette 909" (royalty-free music use only, and it
    is a 909 recording), Freesound CC-BY-NC files, any "909" pack.
- **Best variant:** the owner records his own cymbals (or commissions a drummer for an hour): total
  control of the chick/open/crash/ride articulations, mono, 48 kHz, no licence questions at all.
- **Effort:** low–medium: curation/audition day(s), the `rombuild` script (envelope flattening is the
  only subtle part: a 20–50 ms RMS tracker divided out, then peak normalise, resample to 30 kHz with a
  12–13 kHz anti-alias LPF like a 1983 converter would, quantise), listening iteration.
- **What ships:** 96 KiB of 6-bit ROM data derived from CC0/own recordings, plus the chain. Optionally
  the pre-ROM source wavs for transparency (they are CC0/own).

### 9.4 Option (b): synthesise the metal sources, render into the 6-bit ROM at init
- **What:** a cymbal/hat source synthesiser (e.g. a bank of 6–12 detuned square/pulse oscillators
  through resonant band-passes as in the 808 but denser; or an inharmonic modal bank — 40–80 partials
  with stretched/irregular ratios, fast-decaying highs plus a shaped noise burst, "crash" = broad noise
  wash that brightens then dulls, "ride" = ping + bell partials + sustained wash), rendered once at
  init (or offline) to the same 32K/6-bit images, then played through the identical chain.
- **Legal:** cleanest of all — no third-party audio exists. The hygiene rule still applies: tune it by
  ear/metrics against reference recordings, never by waveform fitting to a 909 recording.
- **How close:** the closed hat is the easiest (short, noisy, the 6-bit chain dominates); the open hat
  is plausible with a good partial bank; **a convincing crash and ride are hard** — real-cymbal washes
  have hundreds of modes with nonlinear energy cascade; synthetic ones tend to sound like 808/DX hats.
  Realistically "good synthetic 909-ish cymbals", not "the 909".
- **Effort:** high for crash/ride quality; medium for hats. Pays back as a differentiator ("metal"
  parameters: partial density, brightness, stretch) that no competitor's fixed ROM offers.
- **What ships:** code only. Deterministic rendering (seeded) keeps renders reproducible.

### 9.5 Option (c): hybrid
- Ship (a) as the default ROM images; add a **user ROM slot** per voice (drop a wav onto the hat/
  crash/ride; it is envelope-flattened, resampled and 6-bit quantised into the slot and saved with the
  preset) — this is ADM's precedent and puts the choice of a "real 909" sample in the user's hands
  (their own content, their own responsibility; we never ship it and never suggest where to get it);
  keep (b) as a later optional source ("synthetic metal" page) that renders into the same slot.
- Legal: as clean as (a) for what we ship; the import path is a generic feature.
- Effort: (a) + a small import/convert path (the same `rombuild` code, run in the plugin) + preset
  state for the 96 KiB (store compressed; the StateCodec size guard must allow it).

### 9.6 Comparison
| | Legal defensibility | Closeness to the original hat/cymbal sound | Effort | Ships |
|---|---|---|---|---|
| (a) real recordings (CC0/own) + faithful chain | High (independent fixation; auditable provenance) | Character exact; timbre "sibling" (medium-high) | Low–medium | 96 KiB ROM data + chain |
| (b) synthesis → ROM | Highest (no third-party audio) | Hats medium-high, crash/ride medium-low | High | code only |
| (c) hybrid: (a) default + user import (+ (b) later) | High | As (a); user can get closer with their own material | Medium | ROM data + chain + import |

### 9.7 Recommendation and what to present to the user
Recommend **(c) with (a) as the shipped default**, in this order: (1) build and validate the chain
model first (it carries most of the recognisable 909-ness and is fully specified by this document);
(2) ship ROM images built from CC0 recordings by a reproducible script, with a provenance file;
(3) add user sample import into the 6-bit slots; (4) leave a synthetic source as an optional later
feature. Present to the owner:
- the legal basis in two lines (ROM = Roland's phonogram, protected into the 2050s; imitation by
  independent recording is explicitly lawful, § 114(b)) and the hygiene rule (no dumps, no 909
  recordings in anything shipped or fitted against);
- the three options with the table above;
- the concrete ask: **does he want to record his own cymbals (best result, zero licence questions),
  or accept curated CC0 Freesound/VCSL material (good, auditable), and does he want the user-import
  slot in v1?**
- the expectation to set: no legal approach reproduces the exact original ROM; every competitor except
  Roland is in the same position, and the chain model is where the fidelity budget should go.

---

## 10. Open questions / UNVERIFIED items

1. **Exact nominal sample clock** (SM "about 60 kHz"/2 vs 1/(2.2RC) 40 kHz vs Fraser 32 kHz vs
   ≈ 32 kHz from the corrected crash-drawing length; corrected by fact-check: was "my ≈28–30 kHz from
   the photos"): fit against a reference recording's image-fold frequency once the A/B
   harness exists; needs a clean 909 CH recording at ≥ 44.1 kHz (test data only).
2. **RM0621 internal weights** (assumed R…32R) and the DAC's real INL; a photo of a 909 hat output on a
   scope at the DAC node would settle both.
3. **Node-level topology of the Q84/Q70/Q68 current-steering VCAs** (which base takes ACC, divider
   constants); the control law's a, b, K constants must be fitted to §7.
4. **Which address bits feed RA13** (RA11 = A9–A14 confirmed by the fact-check re-read; RA13 and the
   ride's apparently reversed 4520/4040 counter order still to confirm from the paper schematic).
5. **Whether C132/C133 changed to 0.01 µF** like C134 on late units; and the effect of R442 33k → 10k
   (SN 392600 up) on the ride clock.
6. **Behringer's RD-9 sample origin** (no statement found); **D16's data origin** (none found).
7. **Scope-drawing calibration** (38 px/div from the clap spacing; ±10 %; the drawings are hand-drawn,
   with no graticule, and the 10 ms clap-spacing assumption needs sibling doc 01); the Vpp values are "about".
8. **Hat retrigger behaviour mid-sample on the real unit** (any click from the DAC DC step on early
   units with the 10 µF C134?) — expected audible; late units' 0.01 µF kills it.

## Local files (not shipped)
`.deps/sources/`: `TR-909_Service_Manual.pdf` (16 p. scan), `Roland_TR-909_service_notes_text.pdf`
(OCR text layer), `TR-909_service_notes_archive_jp2.zip` (600 dpi page scans; p.11 = `_0010.jp2`,
p.9 = `_0008.jp2`), `TR-909-Sound-Mods_RobinWhittle.pdf`, `Tiptop_HATS909_manual.pdf`,
`Drumazon_2-manual-gb.pdf`, `RC_TR-909_OM_gear4music.pdf`, `Behringer_RD-9_manual.pdf`,
`TR-909_OM.pdf` (owner's manual, image-only). No ROM dumps were downloaded or kept.

## Fact-check log

Adversarial fact-check, 2026-10-03. Primary source: [SM] 600 dpi scans
(`TR-909_service_notes_archive_jp2.zip`: p.11 schematic = `_0010.jp2`, p.9 traces = `_0008.jp2`) and
the OCR text of p.5. Every part value below was re-read from full-resolution crops of the scan, and
every figure was recomputed in Python. Scripts are in `/tmp/claude-1000/mw909-factcheck-02-voices-sample/`
(`recompute.py`, `astable.py`, `meas3.py`–`meas5.py`; throwaway, not shipped). URLs checked: [FRA] and
[FRA-MOD] (curl), [N909] (curl), [HOSH-A] (WebFetch). Local copies checked: [RW], [TT], [RC909],
[D16-2], [RD9].

### Corrections

- **F1 (crash length → sample clock), §0.4, §2.2, §10.1.** Was: crash ≈ 1.1–1.2 s → fs ≈ 27–30 kHz.
  Now: the crash trace starts at x = 6220 px and its last column with any wiggle (extent ≥ 6 px above
  the 3 px baseline) is 386 px later. 386/38 × 0.1 s = 1.016 s, and 32768/1.016 s = 32.3 kHz. With
  ±10 % calibration (34.2–41.8 px/div) that is 28.7–35.1 kHz. The old 457 px is not reproducible as
  either the 20 % or the end point; the drawn baseline runs 523 px.
- **F2 (oscillator physics), §2.2.** Was: the 33k series resistor's diode clamping "lengthens the
  charge time", which explains 60 kHz. Now: clamping shortens the period. Simulation (V_DD 5 V,
  threshold V_DD/2, ideal 0.6 V clamps, 2 ns step) gives T/RC = 2.197 (no clamp), 2.171 (Rs 33k), 2.120
  (Rs 10k); threshold at 0.35 or 0.65 V_DD gives 2.254. 60 kHz with 12k/470p needs T/RC =
  1/(60e3·5.64e-6) = 2.955, so the gap is unexplained (**UNVERIFIED**).
- **F3 (run signal), §1.3.** Was: the TRIG differentiator sets IC73a and IC73a's Q is "run". Now:
  "run" is IC72a's output (SM p.5 "Upon receiving this 'run' from IC72a"; [N909] the same; the
  schematic labels IC72a pin 3 RUN/STOP). IC73a pin 6 (S) is grounded on the scan.
- **F4 (stop NAND), §1.3.** Was: NAND of (A13 OR CLOSED) and (A14 OR CLOSED). That would stop CLOSED
  mode at counter 0, which contradicts SM p.5 ("0's cause IC72a output to swing to H(run) irrespective
  of a CLOSED/OPEN"). Logic consistent with both stop addresses: NAND(A13, A14 OR CLOSED). The region
  map itself (OH 0x0000–0x5FFF, CH 0x6000–0x7FFF) is unchanged.
- **F5 (hat VCA topology), §0.9, §4.1, §4.3.** Was: the signal passes through Q84, and IC67a is a
  "buffer/LPF 723 Hz". Now, read from the scan: C157 node → Q85 "(s)" → IC67b (−) carries the audio.
  Q84's right collector → R485 10k → Q85 base. Q84's left collector → IC67a (−), and the tied emitters
  → R484 2.2k → IC67a output, so IC67a/Q84 form a log/exp converter fed by C135 via R453. Corroborated
  by [RW] p.10 (right collector current "turn[s] on Q85"; "emitters of … Q84 … driven strongly
  negative"). 723 Hz = 1/(2π·220k·1000p) is correct arithmetic, but it is R453/C156's integrator
  unity-gain frequency, not a filter corner.
- **F6 (R482 value), §4.1, §8.4.** Was: "22" is almost certainly 22 kΩ, giving fc = 0.72 Hz. Now:
  22 Ω, because unit-less values on this schematic are ohms (R489 150, R490 100, R428 220, R429 100,
  R433 220, R434 100), and it is the fixed leg of the Q85 "voltage controlled potentiometer". For
  reference, 1/(2π·10µ·22k) = 0.72 Hz but 1/(2π·10µ·22) = 723 Hz. Neither is the coupling corner,
  which depends on the DAC source resistance (**UNVERIFIED**). The same applies to R431/R438.
- **F7 (CH τ at knob centre), §4.2.** Was 55 ms. Now 46–48 ms. VR21 100k(B) at mid = 50k, so the CH
  path = 60k. VR23 1M(A) at mid ≈ 100–150k, so the OH path = 200–250k. 60k∥200k = 46.2k and 60k∥250k
  = 48.4k, × 1 µF → 46.2–48.4 ms. (55 ms = 60k∥600k, which assumes a linear VR23.) Refit b ≈ 0.0053
  (was 0.0058 ≈ 0.006).
- **F8 (crash/ride decay figures), §5.3, §7.** Was: crash −14 dB at 1.2 s; ride −14 dB at 1.4 s and
  −20 dB at 530 px; a ride-vs-crash length "CONFLICTING". Envelope extents per 25 px bin, peak-relative:
  - Crash (peak 177 px): 170 177 171 159 142 129 102 86 81 64 56 50 42 42 20 13 | 2 (baseline).
    That gives 20 % (35 px) at ≈ 345 px = 0.91 s and the end at 386 px = 1.02 s, at 13–20 px
    (−19 to −23 dB).
  - Ride (peak 168 px): 168 105 61 42 34 28 22 17 17 13 10 8 8 6 5 4. That gives 20 % (34 px) at
    ≈ 115 px = 0.30 s, 10 % at ≈ 190 px = 0.50 s, and ≥ 6 px wiggle to 335 px (0.88 s).
  - Both traces are ≈ 1.0 s long, so there is no length conflict. The K estimates were changed to
    crash 20–23 dB and ride ≳ 30 dB.
- **F9 (cymbal/CH ratio), §7.** Was 1.4–1.9×. Now ≈ 1.4× (177/121 = 1.46; 168/121 = 1.39; also
  2.3/1.6 = 1.44 and 2.2/1.6 = 1.38).
- **F10 (minor), §6.1.** Was: stage-2 peak "+3 dB near 25 kHz". Now: +3.06 dB at 23.4 kHz.
- **F11 (minor), §7.** Clap p-p re-measures 223 px (2.93 Vpp) vs 232 px. The p.9 figures are hand-drawn
  traces without a graticule, and the 10 ms clap-spacing calibration cites a sibling doc 01 that was
  not present (**UNVERIFIED**).
- **F12 (licensing table), §9.2.** Tiptop "the original … sample ROM" is not in the local HATS909
  manual (**UNVERIFIED** quote). The "only one sound at a time" quote is confirmed.

### Confirmations (re-read and recomputed)

- **ROMs and counters.** IC69/IC62/IC54 HN61256P C43/C42/C44; 4174 latches IC68/IC63/IC53 (outputs
  2,5,7,10,12,15; R̄ tied high); RA9/RA10/RA12 RM0621; C157/C127/C128 10µ/16; 4040 outputs (pins
  9,7,6,5,3,2,4,13,12,14,15,1 = A0–A11); hat 4520 IC70 Q1–Q3 (pins 11–13) with D196–D199 and
  R486/R487 10k; SM stop addresses 110 0000 0000 0000 (OPEN) and 010 0000 0000 0000 (CLOSED).
- **Region durations.** 24576/30k = 819 ms and /32k = 768 ms; 8192 → 273/256 ms; 32768 → 1092/1024 ms.
- **Clock parts.** Hat R492 12K, R493 33K, C160 470P polystyrene "(S)"; crash VR25 10K(B) + R443 6.8K,
  C131 470P, R442 33K struck → 10K "SN392600 UP"; ride VR27 10K(B) + R441 6.8K, C130 470P, R440 33K
  struck → 10K. 1/(2 ln3 · 12k · 470p) = 80.7 kHz → fs 40.3 kHz. k for 60 kHz = 0.338.
- **TUNE range.** 16.8/6.8 = 2.471 = 15.66 st; +9.54 / −6.12 st about the 11.8k centre; 11.8/12 =
  0.983. fs at 30k-hat scaling = 52.94 / 30.51 / 21.43 kHz; durations 0.619 / 1.074 / 1.529 s;
  512-sample step 9.67 / 16.78 / 23.89 ms; 58.6 Hz step rate at 30 kHz.
- **Fraser hat mod.** 12/28.9 = 0.415 (−15.2 st), 12/6.9 = 1.739 (+9.6 st). [RW] text confirmed
  ("about 0.43 to about 1.7", "R492, which is 12k", "the oscillator will stop", C134 0.01 uF).
- **Hat envelope.** C135 1µ/50; R451 10K + VR21 100K(B); R452 100K + VR23 1M(A); Q72 (R447 22K,
  R448), Q73 (R450 10K, R449 22K), Q74 (R454 22K, R455 10K) with its collector on the C135/R453 node.
  τ: CH-min 9.09–9.91 ms, CH-max 52.4–100 ms, OH 100 ms–1.1 s. C159·R491 = 10 µs.
- **Cymbal envelope parts.** R432 270K / C126 1000P / IC52b / R430 2.2K / Q70 2SA798 / R429 100 /
  R428 220 / R427 2.2K / R426 4.7K; ride R439 470K / C125 1000P / IC52a / R436 2.2K / Q68 / R434 100 /
  R433 220 / R437 2.2K / R435 4.7K; VCAs Q71/Q69 "(s)" with R431/R438 "22", into IC51b/IC51a with
  R425/R424 47K. R·C = 0.27 / 0.47 ms. Hat ACC R481 4.7K.
- **Reconstruction filters, all three voices.**
  - LPF1: 5.6K/5.6K, 1200P (fb), 1000P (gnd), giving f0 = 25.94 kHz and Q = 0.548.
  - LPF2: 5.6K/5.6K, 2700P/390P, giving f0 = 27.70 kHz and Q = 1.316.
  - Emitter followers Q80/Q81, Q75/Q76, Q77/Q78 with 10K loads.
  - Cascade response: −0.06 dB at 10 kHz, −0.18 at 15k, −0.56 at 20k, −1.95 at 25k, −5.10 at 30k,
    −9.35 at 35k, −13.69 at 40k, −17.73 at 45k, −27.79 at 60k; −3 dB at 27.00 kHz.
- **Level and output.** R444/R445/R446 22K, VR22/24/26 50K(B) in feedback: gain max 2.273, centre
  1.136. C134 "SN 415300-UP 0.01" gives 1/(2π·0.01µ·22k) = 723 Hz. C132/C133 show no SN note.
  [SM] p.11 clap note "ADJUSTMENT LEVEL: Center MULTI OUT 2Vp-p".
- **Scope-drawing amplitudes and hat timings** at 38 px/div:
  - Amplitudes: crash 177 px = 2.33 Vpp; ride 168 = 2.21; CH accent 121 = 1.59; CH no accent
    74 = 0.97; OH 90 = 1.18. Accent 20·log(1.6/0.96) = 4.44 dB.
  - CH accent: 20 %/10 % at ≈ 85–100 / 110–125 px.
  - CH no accent: ≈ 150–165 / 195–200 px.
  - OH: ≈ 100–110 / 175–185 px.
  - These are consistent with the doc's 53/65 ms, 87/100 ms and 0.29/0.49 s.
- **Quantisation.** 6-bit SQNR 6.02·6 + 1.76 = 37.9 dB.
- **Quotes checked verbatim.**
  - [FRA]: "Each of the three ROMs are 32k in length", "can't be open and closed at the same time",
    "heavily compressed … restored by the vcas", "exponential decay with the same length as the
    sample regardless of tune setting", "arbitrary sample rate of 32kHz … pretty close to the rate
    used for the hats", "shifted by two bits".
  - [FRA-MOD]: "the resistor to be replaced is R492".
  - [N909]: "As the signal gets shorter at higher tunings, the decay-time has to change in the same
    manner", "CH Decay is changing slightly when turning OH-Decay (VR23)".
  - [HOSH-A]: 14" Paiste Sound Edge top / 14" A. Zildjian New Beat bottom, Paiste 18" Thin Crash for
    crash and ride, Sony C-38, Pioneer reel-to-reel, office floor, "No EQ or compression".
  - Products: [RC909] CC 61 CH TUNE / CC 80 OH TUNE; [D16-2] "Tune - Base frequency of the
    instrument"; [RD9] "Digital (Sampling) Closed hat, Open hat, Crash, Ride"; [TT] "capable of
    producing only one sound at a time".
