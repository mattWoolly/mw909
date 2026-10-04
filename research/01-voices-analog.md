<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# 01 — TR-909 analog voices (BD, SD, LT/MT/HT, RS, HC) and the shared noise generator

Scope: circuit-level behaviour of the nine analog voice circuits of the TR-909 and the digital noise source they share, written so the DSP can be built from it. Every value was read off the 600 dpi scan of the voicing-board schematic [SM p.11] at native resolution, then checked against my own measurements of a real unit (AudioRealism 2025 pack). The sample voices (HH, crash, ride) are in 02; the accent DAC, trigger latches, mix bus and outputs are in 03; DSP technique is in 05.

## Sources and notation

| Tag | Source | URL / local copy |
|---|---|---|
| **[SM p.N]** | Roland, *TR-909 Service Notes*, First Edition, JUN. 15, 1984, printed page N. p.1 spec + panel pot call-outs, p.2 parts list (pots), p.4 block diagram, p.5 "ACCENT & TRIG", **p.6 "ANALOG VOICE GENERATORS" (snare text + IC36 supply figure) and "NOISE"**, p.7 change information, **p.9 scope drawings ("Observed at MULTI OUT jack with all knobs set to center")**, **p.11 voicing-board circuit diagram (VG 73133090, PCB 2291084903 3/3)**. Read from the 600 dpi JPEG2000 scans (p.9/p.11 are 8110 × 5732 px). | https://archive.org/details/synth_Roland_TR-909_service_notes ; local `.deps/sources/TR-909_service_notes_archive_jp2.zip` (image n = page n+1), `Roland_TR-909_service_notes_text.pdf` |
| **[OM p.N]** | Roland TR-909 Owner's Manual, printed page N (p.4 "Panel Description") | https://static.roland.com/assets/media/pdf/TR-909_OM.pdf ; local `.deps/sources/TR-909_OM.pdf` |
| **[RW p.N]** | Robin Whittle, *TR-909 sound mods — BD, SD, HH Tuning and HCP*, 10 Oct 2018, printed page N (BD p.3, SD p.4–7, HCP p.8–9) | https://www.firstpr.com.au/rwi/tr-909/ ; local `.deps/sources/TR-909-Sound-Mods_RobinWhittle.pdf` |
| **[CF]** | Colin Fraser, "909 Sound Mods" (BD Tune/Decay/Pitch/Drive/Tune-Depth mods), 18 May 2004 | http://www.colinfraser.com/tr909/909mods/909mods.htm (cached copy read locally; the live site fails TLS) |
| **[KVR-CP]** | KVR forum, "Emulating the TR-909(808) clap in detail" (forum estimate "1140 Hz, Q 1.95") | https://www.kvraudio.com/forum/viewtopic.php?p=8606994 |
| **[R03]** / **[R05]** / **[R02]** | mw909 research 03 (system/sequencer/accent), 05 (emulation, measurements), 02 (sample voices) | `research/03-system-sequencer.md`, `research/05-emulation-techniques.md`, `research/02-voices-sample.md` |
| **[808-02]** | mw808 research 02 (808 metal/noise voices; 808 clap) | `/home/mwoolly/projects/mw808/research/02-voices-metal-noise.md` |
| **[AR]** | AudioRealism "TR-909 Ultimate Sample Pack" (2025): one original unit, 96 kHz / 24-bit, individual outs, part level 50 % | https://audiorealism.se/TR-909-SamplePack.html ; local `.deps/ref-samples/audiorealism-2025/` (never shipped) |

**Tags.**
- **CONFIRMED**: read in the primary source (schematic, circuit text, parts list), or two independent sources agree, or a measurement agrees with a derivation within the stated tolerance.
- **UNVERIFIED**: my reading of a dense schematic region, a single secondary source, or an inference.
- **CONFLICTING**: sources (or derivation vs. measurement) disagree; both values given.
- **[DERIVED]**: my own algebra from component values; formula and parts shown.
- **[MEASURED]**: numbers I measured in this session from [AR] with throwaway scripts in `/tmp/claude-1000/mw909-research-01/an/` (`lib.py`, `bd_acc.py`, `bd_decay.py`, `bd_click.py`, `shaper.py`, `sd_fit.py`, `sd_pitch.py`, `snappy.py`, `tone.py`, `tonefit.py`, `tom_fit.py`, `tomh3.py`, `tomdec.py`, `rs.py`, `clap2.py`, `lfsr.py`, `filt.py`, `scope.py`). Unless stated, measurements are on the AR unit, LEVEL 50 %.

General derivation assumptions: op-amp saturation ±13.5 V on ±15 V rails (4558-class; the exact part is not printed on p.11) — **UNVERIFIED**; all resistors 5 %, capacitors 5–10 % (not stated in [SM]); diode drop 0.6 V; transistor V_BE 0.6 V.

---

## 0. Design-critical facts (executive summary)

1. **One VCO architecture serves BD, SD and all toms** [SM p.6, p.11] **CONFIRMED**: an op-amp integrator (R = 47 kΩ into C) driven by a ±V_cv square, plus an op-amp Schmitt comparator; the trigger shorts the integrator cap (transistor + 10 kΩ) and forces the comparator, so **every hit starts at the same phase** (triangle at 0 V, mid-slope). Frequency is **linear in V_cv**: `f = V_cv / (4·V_th·R·C)`.
   - BD: V_cv comes from an op-amp (IC12a) whose sign is flipped by Q10 at the comparator rate; V_th = V_sat·R50/(R49+R50) ≈ 4.3 V.
   - SD and toms: V_cv is **the supply of a CMOS 4069UB inverter** (VDD/VSS from two op-amp followers, an 18 V zener across them); V_th = V_sat/2 ≈ 6.75 V. **Toms modulate only VSS during the pitch envelope**, so the triangle is skewed early in the note (strong H2) and the frequency follows `f = 2·f_s / (1 + VDD/|VSS|)`.
2. **The analog VCA is one "selected" (S) transistor per path**: signal attenuated to ≈ ±12–18 mV (diode-clipped node → 1.5–2.2 kΩ → 47 Ω) into the emitter, envelope capacitor discharging **into the base through a 220–470 kΩ resistor** (its only or main discharge path), collector into the voice's summing virtual ground [SM p.11], [RW p.6–7]. Gain ∝ base current ∝ `max(0, V_env − 0.6 V)/R_b`: **linear above a 0.6 V threshold**, which is what gives the BD and snappy their "cliff" endings. **CONFIRMED** by the BD and snappy decay fits below.
3. **Bass drum** [SM p.11]:
   - TUNE (VR2 100 kΩ A) sets only the **pitch-sweep time constant** τ = C9·((R57+VR2)‖R26) = **6.8–29.4 ms** [DERIVED] (measured 7.6–29.1 [R05]). The sweep amplitude is **accent-independent** (C9 is charged from the unaccented +15 V trigger pulse via Q2/D7): [MEASURED] Δf = 225–237 Hz with no trend over TOTAL ACCENT 0–100 %, f_b = 49.6–49.7 Hz, τ = 28.8–29.2 ms (TUNE 100).
   - Base pitch: `f_b = (R28/R27)·15 V / (4·V_th·R59·C14)` = 56.0 Hz at V_th 4.32 V [DERIVED] vs **49.2–49.7 Hz measured**; a 4.9 V triangle peak reconciles both the pitch and the measured harmonics (item 4).
   - Amplitude: C8 (0.33 µF) is held at a **clamped ≈ 2.6 V** (D8 + R36/R22 divider) for a **fixed hold** set by C7 (0.033 µF) / R18 (470 kΩ) / Q3 (charged from the unaccented pulse) — [MEASURED] **61–63 ms at every accent level** — then decays through DECAY VR5 (1 MΩ A) + R58 47 kΩ in parallel with R54 470 kΩ into Q12's base. [DERIVED] t(−20 dB) = **79 ms (DECAY min) / 248 ms (max)**. *(Corrected by fact-check: was "vs 75 / 235 ms measured", CONFIRMED. Re-measured on the AR files with the hold plateau as 0 dB: **≈ 91–101 ms (DECAY 0) / 298–299 ms (DECAY 100)**, so the nominal-part model runs 17–22 % short: **CONFLICTING**. The implied VR5 taper (11 / 19 / 39 / 85 %) was inverted from the 75–235 ms figures and is now **UNVERIFIED**. See Fact-check log F1.)*
   - Waveshaper: triangle → R52 10 kΩ → anti-parallel 1SS133 diodes D10/D11, loaded by R53 2.2 kΩ + R43 47 Ω. With 1N4148-class diode parameters a **4.9 V-peak triangle reproduces the measured H3/H5/H7 (−25.7/−41.0/−40.9 dB) within 1.3 dB** [DERIVED fit].
   - ATTACK (VR3 500 Ω B, rheostat into R55 47 Ω) mixes a resonant pulse (SK low-pass 3.7 kHz, Q ≈ 3) and ≈ 500 Hz low-passed noise into a second transistor VCA Q6. Range **7.7 dB** [DERIVED] vs **+7 dB** measured on the first-2 ms peak [R05].
4. **Snare** [SM p.6, p.11]:
   - Two VCOs, C69 0.01 µF / C71 0.0068 µF (nominal ratio 1.47; [MEASURED] **1.500 ± 0.002 after ≈ 40 ms** at every TUNE and TONE. *Corrected by fact-check: was "1.573–1.576", which is an artefact of a 40 ms fit made during the bend. See F2.*). TUNE VR6 10 kΩ B gives an **exact 2:1 range** (1.25–2.5 V). Measured settled f1 = **117.4 / 173.5 / 230.7 Hz** at TUNE 0/50/100 *(corrected by fact-check: was 113 / 169 / 226, the fit values)*. Derived 98.5/148/197 is **−15 to −16 %, CONFLICTING**.
   - Pitch bend: C66 (0.01 µF) charged to ≈ 13.3 V by the trigger *(corrected by fact-check: was ≈ 12.6 V; (14.2·570k + 1.875·47k)/617k = 13.26 V)*, decays τ = C66·(R248+R249) = **5.7 ms** [DERIVED]. [MEASURED] τ = **4.8–5.5 ms** and the bend is a **constant +216 Hz on VCO-1 at onset at every TUNE** (k = 1.91/1.28/0.95 × f1). The p.6 figure (VDD 2 V → 5 V) agrees.
   - Drum VCAs: ENV3 τ = C83·(R291+R290) = 24.2 ms (VCO-1), ENV2 τ = C82·(R295+R293) = 20.7 ms (VCO-2) [DERIVED]; [MEASURED] 24–26 ms and 25–31 ms; VCO-2 sits **−9 dB** below VCO-1 (derived −8.4 dB).
   - Snappy: noise → R286/R285 (÷4) → C85 → **unity Sallen-Key low-pass 8.49 kHz, Q 1.94** (IC40a) → two paths: (a) **1st-order HPF 1.52 kHz** → Q48 VCA on ENV4 (TONE; C67 0.47 µF, hold ≈ 24 ms, then decays toward −0.1·V_acc through VR7 500 kΩ B + R254 100 kΩ, ‖ R297 220 kΩ into the base); (b) **Sallen-Key HPF 4.05 kHz, Q 1.31** → Q47 VCA on a ≈ 2 ms accent pulse (ENV5). Both sum into IC39b, whose feedback is **SNAPPY VR9 10 kΩ B** (gain ∝ knob). [MEASURED] the snappy spectrum matches path (a) within 3 dB from 500 Hz to 16 kHz, and the snappy **cut-off times 99/179/225/255/295 ms at TONE 0–100 are fitted within 9 ms** by the ENV4 model with V0 = 10.3 V, V_end = −0.52 V.
5. **Toms** (LT/MT/HT identical topology) [SM p.11]: **three VCOs each**, cap ratios **1 : 0.667 : 1.8** (VCO1 : VCO2 : VCO3); TUNE 10 kΩ B gives an exact **2:1** range; settled VCO1 [DERIVED] LT 61–122 Hz, MT 75–149 Hz, HT 90–179 Hz. *(Corrected by fact-check: was "measured LT 62–122, MT 77–147, HT 89–173: within 8 %". Those values were read at 150–250 ms, while the slow pitch term still adds 5–8 %. The asymptotic f_s, fitted with the derived rail law over 120–600 ms, is **LT 57.0 / 85.8 / 117.2, MT 71.9 / 106.6 / 141.3, HT 82.8 / 125.4 / 166.7 Hz**, i.e. **4–8 % below** the derived values. See F3.)* Ratios [MEASURED] 0.673–0.679 and 1.77–1.94.
   - Pitch envelope: two caps (0.1 µF/2.2 MΩ = 220 ms, 0.047 µF/470 kΩ = 22 ms) charged by the +15 V trigger pulse push **VSS only** → [MEASURED] VCO1 starts **29–38 % sharp** and relaxes with an apparent τ ≈ 42–55 ms (the rail law stretches the 22 ms RC); the excess falls with TUNE as derived. *(Added by fact-check: the slow 220 ms term **is** visible. f1 keeps falling ≈ 7 % from 150 to 600 ms, and a free fit gives a slow amplitude of 0.57–0.66 V (derived 0.645 V) with τ 155–264 ms (derived 220 ms). See F3.)*
   - **VCO1 is overdriven into its clipping diodes for the first ≈ 20–30 ms** (Q18/Q23/Q56 bypass the 10 kΩ feed with 1 kΩ, timed by 0.01/0.0068/0.0056 µF × 1 MΩ): [MEASURED] H3 −11.6 → −17 dB and H5 −19 → −31 dB from 0–30 ms to 150–250 ms; H2 −7 → −27 dB (rail skew).
   - VCO3 + tom noise run through a short ENV3 (≈ 22–26 ms); VCO2 (0.67×) has a **soft-attack ENV2** (≈ 24–34 ms rise) set by DECAY (VR 500 kΩ B). DECAY changes t(−20 dB) only from 165 → 213 ms (LT), 103 → 138 (MT), 108 → 160 (HT), saturating above ≈ 40–60 % [MEASURED].
6. **Rim shot** [SM p.11]: an impulse rings **three 808-style bridged-T band-passes** (2.2 kΩ shunt, 470/330/470 kΩ bridge): **F1 495 Hz Q 7.3 τ 4.7 ms, F2 219 Hz Q 6.1 τ 8.9 ms, F3 1053 Hz Q 7.3 τ 2.2 ms** [DERIVED; the schematic itself annotates T≈2 / 4.5 / 1 ms] vs [MEASURED] **481 / 222 / 1019 Hz, τ 4.6 / 9.6 / 2.8–3.0 ms**. Their outputs (12k/12k/**3.3k** since S/N 415300, 12k before) are summed into an **anti-parallel diode clipper** (intermod lines at 723, 1288, 1975 Hz measured), then VCA Q65 (accent only scales gain), LEVEL, then a ×2 Sallen-Key HPF at 229 Hz, Q 1.58.
7. **Hand clap** [SM p.11]: digital noise → **MFB band-pass f0 = 963 Hz, Q = 2.13, gain 1.6** [DERIVED] ([MEASURED] spectral peak 1031 Hz in the bursts, 938 Hz in the tail; the forum's "1140 Hz, Q 1.95" is wrong). Two paths, as on the 808: (a) **BA662A OTA** driven by an AN6912 sawtooth burst generator through a Q38 exponential converter, output into the level amp through a **1.94 kHz first-order high-pass** (R237 82 kΩ / C58 1 nF); (b) "reverb" through a **440 Hz low-pass** (R204/C57) into transistor VCA Q37 with a two-capacitor swell envelope (C59, C61 0.1 µF; 470 kΩ each; slow eigen-τ 123 ms derived, ≈ 90 ms measured [R05]). Burst onsets 0 / 9.8 / 21.5 / 31.4 ms; each burst falls ≈ 25 dB in 8–9 ms (τ ≈ 2.6–3.3 ms) [MEASURED]. **Only analog-voice trimmer on the board: TM 10 kΩ B, set for "2 Vp-p at MULTI OUT, LEVEL center"** [SM p.11].
8. **Noise generator** [SM p.6, p.11] **CONFIRMED**: two 4006 18-stage shift registers chained (36 stages), **XOR feedback from stages 13 and 31** into stage 1 → `x^31 + x^13 + 1`, which is primitive [DERIVED, checked by computer]: **period 2^31 − 1 clocks ≈ 1.3–2 hours**. Clock: 4070 RC oscillator (R184 10 kΩ, C47 100 pF), ≈ 0.3–0.46 MHz. Output: 0/+15 V CMOS bits from IC33 pin 9 to **SD, BD (ATTACK), tom-noise and HC** — one stream, so SD and HC noise are identical sample-for-sample (the AR "phasing" note). Rim shot uses no noise. Model as white noise defined by PSD: one-sided ≈ 2·(7.5 V)²/f_clk ≈ **2.5·10⁻⁴ V²/Hz (15.7 mV/√Hz) at the bus** for f_clk = 455 kHz [DERIVED].
9. **Accent** (see [R03 §3]): each voice's 6-bit DAC voltage (0–5 V) is buffered ×3.13 (4.7 k/10 k non-inverting) and **gated by a PNP switch during the 2 ms trigger** into the voice's envelope capacitors, never applied as an output gain. Per voice: BD → click pulse and ENV1 (clamped, so accent saturates: +6.2 dB span measured); SD → all four VCA envelopes and the snappy end-voltage; toms → ENV1/ENV2/ENV3 (no clamp: +7.3–7.7 dB); RS → VCA envelope only (+3.9 dB); HC → burst generator (+2.7 dB). **Pitch envelopes (BD C9, SD C66, toms C16/C17…) and VCO resets come from unaccented +15 V pulses** [SM p.11] — **CONFLICTING with [R03 §3.3]**, which lists BD accent as feeding ENV3 (pitch); the measurement in item 3 settles it.
10. **Panel controls** [OM p.4, SM p.1, p.2, p.11] **CONFIRMED**: BD TUNE 100 kΩ(A), ATTACK 500 Ω(B), DECAY 1 MΩ(A), LEVEL 100 kΩ(B); SD TUNE 10 kΩ(B), TONE 500 kΩ(B), SNAPPY 10 kΩ(B), LEVEL 50 kΩ(B); LT/MT/HT TUNE 10 kΩ(B), DECAY 500 kΩ(B), LEVEL 50 kΩ(B); RS LEVEL 100 kΩ(B); HC LEVEL 50 kΩ(B). **Every LEVEL pot is the feedback resistor of the voice's summing amp** (output ∝ rotation; 50 % = −6 dB; silent at 0). RS LEVEL also forms a level-dependent low-pass with C120 220 pF (7.2 kHz at full, 14.5 kHz at half).
11. **No level/decay table exists for the 909** — unlike the 808's p.14 table. The only check figures are the p.9 drawings (V/div and time/div annotations, no graticule), the HC trimmer target (2 Vpp), and the master-out spec 6 Vpp [SM p.1]. My pixel estimates from the drawings (±30 %): BD 1.2 / 0.6 Vpp with/without accent, SD 2.1 / 1.0 Vpp, LT 1.9 / 1.2 Vpp (all knobs centre).

### 0.1 Derived vs. measured (validation table)

| Quantity | Derived from parts [DERIVED] | Measured [MEASURED unless cited] | Verdict |
|---|---|---|---|
| BD base pitch f_b | 56.0 Hz (V_th 4.32 V); 49.3 Hz if V_th = 4.9 V | 49.2–49.7 Hz [R05]; 49.6–49.7 (accent sweep) | CONFLICTING by 13 % at nominal V_th; consistent with the shaper fit |
| BD sweep τ, TUNE 0 / 100 | 6.8 / 29.4 ms | 7.6 / 29.1 ms [R05] | CONFIRMED |
| BD sweep Δf vs accent | constant (C9 from unaccented Q2) | 225–237 Hz, no trend, accent 0–100 % | CONFIRMED |
| BD hold | ≈ 45–50 ms (C7 τ 15.5 ms; Q3 β-dependent), accent-independent | 61–63 ms at all 11 accent levels | independence CONFIRMED; length −20 % (UNVERIFIED mechanism detail) |
| BD t(−20 dB), DECAY min / max | 79 / 248 ms | 75 / 235 ms [R05]; **≈ 91–101 / 298–299 ms re-measured by fact-check** | *corrected by fact-check: was CONFIRMED (≤ 6 %)*; **CONFLICTING** (model 17–22 % short; F1) |
| BD ATTACK range | 7.7 dB | +7 dB (first-2 ms peak) [R05] | CONFIRMED |
| BD shaper H3/H5/H7 | −25.3 / −39.6 / −39.1 dB (A = 4.9 V, 1N4148-class) | −25.7 / −41.0 / −40.9 [R05] | CONFIRMED as a fit (diode parameters assumed) |
| SD VCO-1 settled, TUNE 0/50/100 | 98.5 / 147.8 / 197.0 Hz | 112.9 / 168.8 / 226.4 Hz (fit); **117.4 / 173.5 / 230.7 Hz (spectral peak, 50–250 ms; fact-check)** | ratio 1.97 (≈ 2.0) CONFIRMED; absolute **−15 to −16 %** CONFLICTING *(corrected by fact-check: was −13 %)* |
| SD f2/f1 | 1.471 (C69/C71) | **1.500 ± 0.002 after ≈ 40 ms, every TUNE/TONE (fact-check)**; 1.573–1.576 in a 40 ms fit | *corrected by fact-check: was "1.573–1.576, CONFLICTING by 7 %"*; CONFLICTING by +2 %, with an exact 3:2 lock (mechanism UNVERIFIED; F2) |
| SD bend τ | 5.7 ms | 4.8–5.5 ms | CONFIRMED |
| SD bend size | TUNE-independent absolute ΔV ≈ 2.0 V (k ≈ 1.7/1.1/0.8) *(corrected by fact-check: was 1.9 V, k 1.6/1.0/0.7)* | Δf = 215–216 Hz at every TUNE (k = 1.91/1.28/0.95) | form CONFIRMED; size +12–25 % |
| SD drum VCA τ (VCO-1/VCO-2) | 24.2 / 20.7 ms | 24–26 / 25–31 ms | CONFIRMED |
| SD VCO-2 level re VCO-1 | −8.4 dB | −8.9 to −9.6 dB | CONFIRMED |
| Snappy spectrum (path a) | SK LP 8.49 kHz Q 1.94 × HP 1.52 kHz | octave bands within 3 dB, 500 Hz–16 kHz | CONFIRMED |
| Snappy cut-off vs TONE | ENV4 model (fit V0 10.3 V, V_end −0.52 V) | 99/179/225/255/295 ms | CONFIRMED (fit ≤ 9 ms) |
| Tom VCO1 settled, TUNE 0/50/100 | LT 61/92/122, MT 75/112/149, HT 90/134/179 Hz | LT 62/90/122, MT 77/112/147, HT 89/132/173 [R05] (at 150–250 ms); asymptotic **LT 57/86/117, MT 72/107/141, HT 83/125/167** (fact-check fit) | *corrected by fact-check: was CONFIRMED (≤ 4 %)*; measured is **4–8 % low**, which is the same order as the SD offset (F3) |
| Tom slow pitch term (C16/R60) | +0.645 V on VSS, τ 220 ms | fitted 0.57–0.66 V, τ 155–264 ms (fact-check) | **CONFIRMED** (added by fact-check) |
| Tom VCO2/VCO1, VCO3/VCO1 | 0.667/1.833 (LT), 0.667/1.80 (MT), 0.682/1.829 (HT) | 0.679/1.88, 0.673/1.77, 0.675/1.94 | CONFIRMED (HT VCO3 +6 %) |
| Tom initial sharpness k(0), TUNE 0/50/100 | 0.47 / 0.37 / 0.31 (fast term only); **0.52 / 0.42 / 0.35 with both terms** (fact-check) | 0.35–0.38 / 0.29–0.32 / 0.25 | trend CONFIRMED; size +25–45 % (UNVERIFIED) |
| RS partials | 495 / 219 / 1053 Hz | 481 / 222 / 1019 Hz | CONFIRMED (≤ 3 %) |
| RS partial τ | 4.7 / 8.9 / 2.2 ms | 4.6 / 9.6 / 2.8–3.0 ms | CONFIRMED |
| HC band-pass | 963 Hz, Q 2.13 | peak 1031 Hz (bursts), 938 Hz (tail); fact-check re-measure 965 / 911 Hz (15 Hz-smoothed, 19 hits) | CONFIRMED |
| HC tail τ | 123 ms (slow eigenmode) | ≈ 90 ms [R05] | same order; UNVERIFIED |
| Noise sequence | x^31+x^13+1 primitive | — | [DERIVED] |

---

## 1. Common machinery

### 1.1 Trigger, gating and accent entry [SM p.4, p.5, p.11; R03 §3]
- TRIG is a **5 V, 2 ms** pulse per voice from latches IC1/IC10 [SM p.5]. **CONFIRMED.**
- Every analog voice has the same front end [SM p.11] **CONFIRMED** (read):
  1. an NPN trigger switch (22 kΩ base resistor, 10 kΩ base–emitter): BD Q1, SD Q39, LT Q16, MT Q25, HT Q61, RS Q63 (driven through C111 0.018 µF, so it fires on the edge), HC Q35;
  2. a **PNP "+15 V pulse" switch** with a 100 Ω emitter resistor to +15 V: BD Q2, SD Q40, LT Q15, MT Q24, HT Q60. This **unaccented** pulse resets the VCOs, forces the comparators and charges the pitch-envelope caps;
  3. a **PNP accent switch** whose emitter sits on the accent buffer output (BD Q5 and Q4, SD Q41 and Q42, LT Q17, MT Q29, HT Q62, RS Q64, HC Q36). It passes the held accent voltage into the envelope caps while the trigger (or a hold timer) is active.
- Accent buffers: non-inverting op-amps with 10 kΩ feedback and 4.7 kΩ to ground, **gain 3.13** (BD IC11b R37/R38, SD IC34b R241/R240, LT IC18b R119/R120, MT IC23b R179/R180, HT IC45b R382/R383, RS IC48b R406/R397, HC IC26a R212/R206) [SM p.11]. **CONFIRMED.** The toms also have 220 kΩ from the buffer (+) input to the −15 V rail (R118, R178, R381), which offsets the DAC node by ≈ −0.17 V (−0.54 V at the buffer output) [DERIVED from the 2.54 kΩ DAC source impedance in R03]. HC has R496 100 kΩ in the same position on S/N ≤ 371499 units (drawn "SN UP to 371499, surface mounted"). **UNVERIFIED** (that the arrow is −15 V).
- Effective envelope start voltage ≈ V_acc − 1.2 V (PNP V_CE(sat) ≈ 0.1–0.6 V plus one diode) [DERIVED]. Accent has no effect below ≈ 1.2–1.8 V (V_acc plus the VCA's 0.6 V threshold).

### 1.2 The triangle VCO [SM p.6 text, p.11]
- [SM p.6]: "VCO-1 comprises a hytheresis comparator IC37a, inverting buffers configured as voltage-dependent resistor (in IC36) and an integrator consisting of IC37b and C69 with Q44 switcher. In this arrangement VCO-1 generates triangle waveform." "SNARE, BASS, TOM TOM operate basically in the same manner." **CONFIRMED.**
- Topology, SD/tom form (read for every tom VCO) [SM p.11]:
  - Comparator: op-amp; the triangle enters (−) through 100 kΩ (SD R269); (+) has 100 kΩ to the output and 100 kΩ to ground, so **V_th = ±V_sat/2 ≈ ±6.75 V**. A diode + 10 kΩ (SD D62/R275; LT D13/R62, D33/R89, D32/R88) from the +15 V pulse line forces the comparator during the trigger.
  - Square → 100 kΩ → three paralleled 4069UB inverters (they clamp the ±13.5 V comparator swing to their own rails) → 47 kΩ → integrator. The inverter output swings between VDD and VSS, so **integrator current = VDD/47 kΩ one way, |VSS|/47 kΩ the other**.
  - VDD = op-amp follower of the tune CV through 1 kΩ (SD IC35a/R250; LT IC14a/R68; MT IC20a/R129 (label partly legible); HT IC42a/R333). VSS = inverting op-amp (100 kΩ/100 kΩ) through 1 kΩ (SD IC35b/R253; LT IC14b/R65; MT R126; HT R338). An **RD18JB2 18 V zener** sits across VDD–VSS (SD D57, LT D16, MT D34, HT D72).
  - Reset: an NPN with 1 MΩ base resistor from the +15 V pulse line and **10 kΩ** from collector to the integrator output shorts the integration cap (SD Q44/R263, Q45/R268; LT Q13/R71, Q14/R77, Q22/R84; MT Q30, Q31, Q32; HT Q53, Q54, Q55).
- [DERIVED] Symmetric rails: `f = V_cv / (4·V_th·R·C)`; with V_th = 6.75 V and R = 47 kΩ, `f = V_cv / (1.269·10⁶·C)` (C in F). Asymmetric rails: `f = 1 / (2·V_th·R·C·(1/VDD + 1/|VSS|))`.
- [DERIVED] Start state: the cap is shorted for as long as the +15 V pulse lasts (≤ 2 ms), so the triangle starts at 0 V (mid-slope), in a fixed direction. [MEASURED] hit-to-hit start correlation 0.91–1.00 [R05].
- BD variant: see §3.2 (op-amp sign switch instead of inverters, V_th = V_sat·47/147).

### 1.3 The transistor VCA [SM p.11; RW p.6–7]
- Same cell in BD (Q12, Q6), SD (Q50, Q51, Q47, Q48), toms (VCO1/2/3: LT Q19/Q20/Q21, MT Q26/Q27/Q28, HT Q57/Q58/Q59), tom-noise (Q33), RS (Q65), HC reverb (Q37) [SM p.11]. Most are marked **(S)** = selected part. **CONFIRMED** (read).
  - Signal: waveform → 10 kΩ → anti-parallel 1SS133 pair to ground (soft clip at ≈ ±0.5–0.6 V; the schematic annotates "0.6V" and a sine) → 1.5–2.2 kΩ → **47 Ω to ground** = emitter node. Peak signal at the emitter ≈ ±12–18 mV [DERIVED].
  - Control: envelope capacitor → 220–470 kΩ → **base**. Whittle: "the current from R297 (220k) into the base of Q48, at about 0.4 to 0.6 volts"; "when their input voltage … is below about 0.4 volts, they do not let any signal through"; "a capacitor is discharged through a resistor into the base of the transistor, with this being the only discharge path. Therefore, the current is naturally exponential, whatever the critical voltage of the transistor" [RW p.6–7]. **CONFIRMED.**
  - Output: collector into the summing op-amp's virtual ground.
- [DERIVED] Model: `i_out = G·I_B·v_e`, `I_B = max(0, V_env − V_BE)/R_b`, V_BE ≈ 0.4–0.6 V (temperature- and part-dependent). The transistor is saturated (both junctions forward), so v_e of ±0.45·V_T is not small: expect **even-harmonic distortion that is independent of envelope level**. That matches the BD's level-independent H2 = −28 dB [R05]. The H2 attribution is **UNVERIFIED**.
- Consequences:
  - An envelope that discharges **only** into the base (SD drum ENV2/ENV3, tom ENV1/ENV3, RS, HC reverb) gives an exactly exponential gain decay, τ = R_b·C.
  - An envelope that also discharges to ground or a negative rail (BD ENV1, SD ENV4) crosses the threshold at a finite time: an accelerating dB slope and a hard end. Both are measured (§3.4, §4.5).

### 1.4 Output stage and LEVEL [SM p.11]
- Each voice sums its VCA collector currents into an inverting op-amp whose **feedback element is the LEVEL pot** (linear B): BD IC11a VR4 100 kΩ, SD IC40b VR8 50 kΩ, LT IC18a VR12 50 kΩ, MT IC23a VR15 50 kΩ, HT IC45a VR18 50 kΩ, RS IC50a VR19 100 kΩ ‖ C120 220 pF, HC IC28a VR20 50 kΩ [SM p.11]. **CONFIRMED.** Output ∝ rotation: 50 % = −6.0 dB, 0 % = silence [DERIVED].
- RS only: after the level amp, a ×2 Sallen-Key high-pass (§6.4).
- Then 1 kΩ (+ 0.01 µF on S/N ≥ 415300) to the multi-out jack and the mix bus [R03 §6].

### 1.5 Service-manual check figures [SM p.1, p.9, p.11]
- **There is no level or decay table in the 909 notes.** The spec says the multi-out level is "See P.9", and p.9 holds only hand-drawn traces. **CONFIRMED** (all 16 pages checked).
- p.9 annotations **CONFIRMED** (read): "Observed at MULTI OUT jack with all knobs set to center". BD 200 mV/div, 5 ms/div, with and without accent. SD 0.5 V/div, 2 ms/div, with and without. LT 0.5 V/div, with and without (with a "noise" arrow at the onset). LT, and MT/HT, with-accent envelopes at 50 ms/div. RS 2 ms/div, two traces for R417 = 3.3 k and 12 k. HAND CLAP 500 mV/div, 10 ms/div.
- [MEASURED from the drawings] The drawings have no graticule. I calibrated 47 px/div at 8110 px from the BD trace (the last drawn BD period, 188 px, equals the 20.1 ms period at that point of the measured sweep) and assumed square divisions:

  | Voice (all knobs centre) | with accent | without | ratio |
  |---|---|---|---|
  | BD | ≈ 1.2 Vpp | ≈ 0.61 Vpp | 2.0 (6.0 dB) |
  | SD | ≈ 2.1 Vpp | ≈ 1.0 Vpp | 2.0 (6.2 dB) |
  | LT | ≈ 1.9 Vpp | ≈ 1.2 Vpp | 1.6 (3.9 dB) |

  **UNVERIFIED** (±30 % in level, ±2 dB in ratio). [R03] measured the same BD/SD drawings at 2.6 / 2.2 by a different pixel method: **CONFLICTING** within the drawing tolerance. The HC drawing's burst spacing implies a different px/div, so the drawings are not on one scale.
- The only numeric calibration for an analog voice is the **HC trimmer TM: "ADJUSTMENT LEVEL: Center, MULTI OUT, 2Vp-p"** next to a drawing of the burst train [SM p.11]. **CONFIRMED.**
- For relative levels between voices use [R05 §10.1] (AR, LEVEL 50 %): BD 0 dB, SD +3.6, LT −0.6, MT +0.3, HT +0.4, RS +0.3, CP +9.0 dB Vpp. All analog voices are negative-peak dominant (+/− ratio 0.53–0.79).

---

## 2. Noise generator [SM p.6 "NOISE", p.11 "NOISE" block]

### 2.1 Circuit
- [SM p.6]: "a quasi-random noise generator having two shift registers (IC32, IC33) connected in cascade making up 32 stages … Two Ex-OR gates of IC31 clock the shift registers at a higher frequency … On power-up, a trigger is applied into pin 1 of IC32 via D48 for starting running." **CONFIRMED.**
- Schematic reading [SM p.11] **CONFIRMED** (read at native resolution):
  - IC32 and IC33 are **4006** (18-stage static shift registers: 4 + 5 + 4 + 5) with pins 13→4, 12→5, 10→6 linked, so each is a full 18-stage chain from pin 1 to pin 9. IC32 pin 9 → IC33 pin 1: **36 stages**.
  - Feedback: XOR IC31c (4070) inputs from **IC32 pin 10 (stage 13)** and **IC33 pin 10 (stage 18 + 13 = 31)**; its output → R187 47 kΩ → IC32 pin 1.
  - Start-up kick: IC31d with C46 4.7 µF, R183 10 kΩ, R186 100 kΩ, R182 10 kΩ, D49 → D48 into pin 1. It is needed because an XOR LFSR locks up in the all-zero state.
  - Clock: IC31a/b (one gate wired as an inverter, the other as a buffer), R184 10 kΩ, R185 33 kΩ, C47 100 pF, to both pin-3 clock inputs. Supply +15 V (C48 0.1 µF).
  - Output: **IC33 pin 9 (stage 36)** → noise bus.
- So the "32 stages" of the text is the feedback length (stage 31), while the physical chain is 36. Whittle's "two 18 stage shift registers" [RW p.5] is also right. **CONFLICTING in wording only**; the schematic is definitive.
- [DERIVED, computed in `lfsr.py`] `x^31 + x^13 + 1` is irreducible, hence **primitive** (2^31 − 1 is prime). The period is **2^31 − 1 ≈ 2.15·10⁹ clocks: 79 min at 455 kHz, 2 h at 300 kHz**. A simulation of the exact 36-stage circuit gives a ones-density of 0.502 and autocorrelation < 0.004 at lags 1–31.
- Clock frequency ≈ 1/(2.2·R184·C47) = 455 kHz [R05] [DERIVED]; [R02] found the same formula 25 % high on the hat oscillator, so expect **0.3–0.46 MHz**. **UNVERIFIED.**

### 2.2 Spectrum and level [DERIVED]
- A random NRZ bit stream at f_clk with 0/15 V levels: mean 7.5 V, σ = 7.5 V. One-sided PSD `S(f) = 2·σ²/f_clk · sinc²(f/f_clk)` → **2.47·10⁻⁴ V²/Hz (15.7 mV/√Hz) at 455 kHz**, flat to −0.06 dB at 20 kHz (f_clk 300 kHz).
- Do not emulate the LFSR at audio rate [R05 §3]. A seeded white PRNG with this PSD (σ_sample = √(S·fs/2)) reproduces everything audible. Hit-to-hit noise differs because the sequence free-runs. The 1–2 h period is inaudible.
- Who uses it (one shared stream) [SM p.6, p.11] **CONFIRMED**:

  | Consumer | Entry |
  |---|---|
  | SD snappy | R286 10 kΩ / R285 3.3 kΩ |
  | BD ATTACK | R10 12 kΩ / R9 4.7 kΩ → C15 |
  | TOM NOISE | R190 22 kΩ |
  | HC | R208 47 kΩ |

  RS uses none. Because SD and HC filter **the same bit stream**, their outputs are partially correlated. AR recorded "Snare + Clap … from the master output to capture the phasing interference" (AR Readme). mw909 must feed SD, BD, toms and HC from one generator.

---

## 3. Bass drum (BD) [SM p.11 "BASS DRUM" block; CF; RW p.3]

### 3.1 Signal flow (read from [SM p.11]; **CONFIRMED** except where tagged)

```
TRIG ─R1 22k/R2 10k─► Q1 ──(collector low during TRIG)──┬─ R3 100k/D4 ─► Q2 (PNP, +15 V) ─► +15 V PULSE (R19 2.2k load)
                                                         ├─ D5/R34 100k ─► Q5 gate ─┐
                                                         └─ C1 0.068µ/R12 100k ─► Q7 ─► R47 330k → Q11 (integrator reset, R51 2.2k)
                                                                                 └─ R48 22k/D12 → force comparator
DAC ─► IC11b ×3.13 (R37 10k/R38 4.7k) = V_acc ──► Q5 (gated) ─► R6 22k ─► Q8 ─► PULSE (click)  [+ R5 100k from +15 V pulse]
                                            └──► Q4 (gated by Q3 hold) ─► R35 1.8k ─► D9 ─► C8 0.33µ = ENV1  (clamp D8 → R36 47k/R22 10k)
+15 V PULSE ─► D7/R21 2.2k ─► C9 0.33µ = ENV3 (pitch) ── R57 22k + VR2 100k(A) TUNE to gnd
           └─► D6/R20 6.8k ─► C7 0.033µ ─ R18 470k ─► Q3 (R17 470k) ─► R32 47k ─► Q4 base   (HOLD)
ENV3 ─R26 330k─► IC12a (Σ, R28 150k fb, R27 1.5M from +15 V, R25 100k) ─► IC12b ±1 (R29/R30/R31 100k; Q10 via R23 100k/R24 22k from comparator)
     ─► R59 47k ─► IC13a integrator (C14 0.033µ) ═ triangle ═► IC13b comparator (R49 100k / R50 47k)
triangle ─► R52 10k ─► D10/D11 ─► R53 2.2k ─► R43 47Ω ║ Q12 emitter ; Q12 base ◄─ R54 470k ◄─ C8 (ENV1)
PULSE: Q8 (C5 0.0068µ, R14 22k) ─► C2 0.0033µ ─► R7 100k ─ R15 100k ─ R16 39k ─► Q9 follower (C4 100p, C3 0.0047µ) ─► C11 0.47µ ─► R44 22k ─┐
NOISE ─► R10 12k / R9 4.7k ─► C15 0.47µ ─► R46 4.7k ─ C13 0.1µ ─► R45 4.7k ───────────────────────────────────────────────────────┤ node N (R56 330Ω)
node N ─► VR3 500Ω(B) ATTACK ─► R55 47Ω ║ Q6 emitter ; Q6 base ◄─ R42 470k ◄─ C12 0.033µ ◄─ R41 22k ◄─ ENV2 source
Q12, Q6 collectors ─► IC11a (−) ; feedback VR4 100k(B) LEVEL ─► BD out
```
Notes:
- R495 150 kΩ from Q6's emitter to ground is fitted on "SN UP to 371499, surface mounted" units. It is negligible against R55 47 Ω.
- The ENV2 source line (R41's left end) is **UNVERIFIED**. It is most likely the Q5 gated-accent pulse.

### 3.2 VCO and pitch envelope
- Integrator IC13a, R59 47 kΩ, C14 0.033 µF.
- Comparator IC13b: R49 100 kΩ feedback, R50 47 kΩ to ground, so V_th = ±V_sat·47/147 = **±4.32 V** at V_sat 13.5 V [DERIVED].
- IC12b is a **±1 amplifier switched by Q10**: R29, R30, R31 are all 100 kΩ, and Q10 grounds the (+) input when the comparator is high. Its output is ±V_cv, so the integrator ramps up and down at the same rate: a symmetric triangle [DERIVED]. **CONFIRMED** (read).
- **V_cv** = |IC12a out| = R28·(15 V/R27 + V_C9/R26) = **1.50 V + 0.4545·V_C9** [DERIVED]. [CF] confirms that "R27 … adds a fixed voltage offset to the output of IC12A which sets the minimum frequency".
- Base frequency: f_b = 1.5 / (4·4.32·47k·0.033µ) = **56.0 Hz** [DERIVED] against **49.2–49.7 Hz** [MEASURED, R05 and here]. **CONFLICTING (−12 %).** It would be resolved by V_th = 4.9 V (V_sat·0.36), which also fits the shaper harmonics (§3.5). Either the op-amp swing or R49/R50 differ from my assumption. For the model, **calibrate f_b = 49.5 Hz** and keep the derived structure.
- Sweep: C9 charges from the +15 V pulse through D7 + R21 (τ_charge = 0.73 ms, full in 2 ms) to ≈ 14.4 V. It discharges through R57 + VR2 to ground, in parallel with R26 into IC12a's virtual ground:
  - τ = C9·((R57 + VR2)‖R26) = **6.8 ms (TUNE min) to 29.4 ms (max)** [DERIVED] against 7.6–29.1 ms [MEASURED, R05]. **CONFIRMED.**
  - [CF]: "The Tune control is really a decay control for a simple envelope generator that provides the initial falling pitch sweep … from around 30 to 120 milliseconds." **CONFIRMED** (≈ 4τ).
  - [RW p.3]: "The TR-909's Tune control does not control the overall frequency of the Bass Drum oscillator." **CONFIRMED.**
- Sweep depth [DERIVED]: Δf = f_b·0.4545·V_C9(0)/1.5 = 49.5 × 4.36 = **216 Hz** at V_C9 = 14.4 V, against **225–247 Hz measured**. **CONFIRMED** (within 10 %).
- **Accent does not change the sweep** [MEASURED, `bd_acc.py`, TUNE/ATTACK/DECAY 100]:

  | TOTAL ACCENT | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 | 100 |
  |---|---|---|---|---|---|---|---|---|---|---|---|
  | f_b (Hz) | 49.65 | 49.67 | 49.66 | 49.64 | 49.66 | 49.62 | 49.62 | 49.68 | 49.72 | 49.73 | 49.70 |
  | Δf (Hz) | 226 | 226 | 234 | 228 | 234 | 225 | 226 | 236 | 232 | 237 | 234 |
  | τ (ms) | 29.1 | 29.0 | 29.0 | 29.1 | 29.0 | 29.2 | 29.2 | 29.0 | 28.9 | 28.8 | 28.9 |
  | hold (ms) | 61.6 | 61.7 | 62.7 | 61.2 | 62.8 | 61.7 | 61.4 | 62.4 | 62.9 | 62.6 | 62.9 |
  | Vpp (dBFS) | −4.6 | −4.7 | −3.1 | −5.1 | −1.7 | −0.4 | 0.8 | 1.0 | 1.3 | 1.6 | 1.6 |

  This contradicts the [SM p.4] block diagram as summarised in [R03 §3.3] ("Accent … feeds ENV3 (pitch envelope)"). **CONFLICTING → resolved by the schematic (D7 is fed from Q2's unaccented +15 V pulse) and the measurement.**
- Reset: Q7 (driven through C1 from Q1's collector) turns on Q11, which shorts C14 through R51 2.2 kΩ, and pulls the comparator (+) high through R48 22 kΩ / D12 [SM p.11]. The edge coupling through C1 suggests the reset lasts at most the 2 ms trigger. **UNVERIFIED.** [MEASURED] In the onset-trimmed AR files the tone and the click start within 0.5 ms of each other, and the first half-cycle is negative-going.
- Early-unit change: **C9 was 0.22 µF below S/N 381500** "for expanding the TUNE range" [SM p.7]. That gives τ = 4.5–19.6 ms [DERIVED]. **CONFIRMED.**

### 3.3 TUNE taper
- VR2 is 100 kΩ (A) [SM p.2, p.11]. **CONFIRMED.**
- The resistance implied by the measured τ is 2.8 / 10.3 / 18.1 / 28.7 / 98.3 kΩ at 0/25/50/75/100 % [R05]: an audio taper. The model should map knob → τ through a fitted A-curve, then f(t) = f_b + Δf·e^(−t/τ).

### 3.4 Amplitude: hold, clamp and thresholded decay (ENV1)
- **Clamp** [SM p.11, read at 3× zoom] **CONFIRMED**:
  - R36 47 kΩ (from +15 V) and R22 10 kΩ form node Y = 15·10/57 = **2.63 V** [DERIVED].
  - D8 runs from node X (the R35/D9 junction) to Y, so X ≤ 3.2 V, and C8 charges through D9 to **≈ 2.6 V** however large the accent [DERIVED]. This is why BD accent saturates (Vpp flat from 80 % up in the table above).
- **Hold**: C7 (0.033 µF) is charged from the unaccented +15 V pulse through D6/R20 6.8 kΩ. It discharges through R18 470 kΩ into Q3's base (R17 470 kΩ to ground). Q3 holds Q4's base low (R32 47 kΩ), so Q4 keeps feeding V_acc into C8 [SM p.11].
  - [DERIVED] C7 → 0.6 + 13.4·e^(−t/15.5 ms) falls below the ≈ 1.3–1.45 V Q3 needs at t ≈ 43–46 ms; the 0.5 dB envelope drop adds ≈ 4 ms. Total ≈ **47–50 ms**.
  - [MEASURED] **61–63 ms**, flat within 1 ms over accent 0–100 % and independent of DECAY [R05].
  - Independence: **CONFIRMED.** Length: CONFLICTING (−20 %); depends on Q3's β and C7 tolerance. **Calibrate the hold to 62 ms.**
- **Decay**: after the hold, C8 discharges through R_d = VR5 (1 MΩ A, wiper tied to one end) + R58 47 kΩ to ground, and through R54 470 kΩ into Q12's base (≈ 0.6 V).
  - [DERIVED] τ = C8·(R_d‖R54) = **14.1 ms (DECAY min) – 107 ms (max)**. V_C8 heads for V∞ = 0.6·R_d/(R_d + R54) < 0.6 V, so the gain (V − 0.6) reaches zero in finite time: the "thresholded" tail of [R05].
  - [DERIVED] t(−20 dB) = hold + τ·ln((2.63 − V∞)/(0.80 − V∞)) = **79.4 ms / 248 ms** at min/max against **75 / 235 ms measured** [R05]. *Corrected by fact-check: was **CONFIRMED**. Re-measured (400 Hz LP, 20 ms-smoothed Hilbert envelope, 0 dB = mean of 20–50 ms, TUNE 0/50/100, ATTACK 0): t(−20 dB) = **91–101 / 141–145 / 171–176 / 203–212 / 298–299 ms** at DECAY 0/25/50/75/100. A per-cycle Vpp track gives 288–297 ms at DECAY 100. R05's 75–235 ms could not be reproduced by any method tried. The nominal-part model is 17–22 % short: **CONFLICTING** (F1). Inverting the model on the new numbers would need VR5 ≈ 1.85 MΩ at full rotation, so the shortfall is in the model (hold, threshold or C8), not the taper.*
  - [DERIVED from the measured t(−20 dB) at 25/50/75 %: 115 / 135 / 175 ms] implied VR5 = 114 / 192 / 388 kΩ (11 / 19 / 39 % of 1 MΩ): an audio taper. *Corrected by fact-check: was **CONFIRMED**; now **UNVERIFIED**, because the input times are the R05 values not reproduced above.* That VR5 is "(A)" remains CONFIRMED from the parts list [SM p.2] and the schematic.
  - [CF]: "The amplitude decay curve (ENV 1) is provided by capacitor C8, which is charged up when the drum is triggered via D9, and discharges through VR5 and R58. The voltage on C8 controls the VCA - Q12." **CONFIRMED.** [R05]'s "whether R54 really loads C8" is now **CONFIRMED**: R54 is Q12's base resistor.
- Model:
  - `V8 = 2.63·a(V_acc)` held for 62 ms, where a(·) is the accent law below the clamp;
  - then `dV8/dt = −V8/(R_d·C8) − max(0, V8 − 0.6)/(R54·C8)`;
  - `gain = max(0, V8 − 0.6)` with a soft knee of ≈ 20 mV.

### 3.5 Waveshaper (triangle → near-sine) [SM p.11; CF; RW p.3]
- Triangle (±4.3–4.9 V) → R52 10 kΩ → node with **D10, D11 (1SS133, anti-parallel to ground)**, loaded by R53 2.2 kΩ + R43 47 Ω (2.247 kΩ to ground) → Q12 emitter at 47/2247 of the node voltage [SM p.11]. **CONFIRMED.**
- [RW p.3]: "rounded somewhat to make it more like a sine wave, by two back-to-back diodes, which turn on around 0.5 to 0.6 volts above and below ground". [CF]'s "Drive" mod replaces R52 with a 10 kΩ pot + 1 kΩ. **CONFIRMED.**
- Transfer curve [DERIVED]: solve `(v_tri − u)/10k = u/2247 + 2·I_s·sinh(u/(n·V_T))` for u. Node peak ≈ 0.50 V.

  | Triangle peak | H3 | H5 | H7 |
  |---|---|---|---|
  | 4.32 V | −32.8 | −37.7 | −43.5 dB |
  | **4.9 V** | **−25.3** | **−39.6** | **−39.1 dB** |
  | measured [R05] | −25.7 | −41.0 | −40.9 dB |

  H3 depends strongly on the drive level (a near-null near 4.3 V). The fit uses I_s = 2.5 nA, n = 1.75; 1SS133 data were not available.
- Even harmonics (H2 −28 dB) and the +/− asymmetry (0.45–0.64) are not produced by a matched diode pair. Attribute them to the saturated-transistor VCA (§1.3) or diode mismatch. **UNVERIFIED.** Implement as a static curve, then a small even-order term after the VCA; fit H2 to −28 dB.

### 3.6 ATTACK (click) [SM p.11; CF]
- [CF]: "Attack - controls the level of an initial noise burst and click". **CONFIRMED.**
- **Pulse path** [SM p.11; DERIVED]:
  - Q8 emitter follower of (V_acc via R6 22 kΩ + the +15 V pulse via R5 100 kΩ), with C5 0.0068 µF and R14 22 kΩ to ground.
  - C2 3.3 nF differentiates into R7 100 kΩ / R15 100 kΩ.
  - **Unity Sallen-Key low-pass** (R7, R16 39 kΩ, C3 4.7 nF to the Q9 emitter, C4 100 pF to ground): **f0 = 3.72 kHz, Q = 3.08**, ignoring R15 loading.
  - → C11 0.47 µF → R44 22 kΩ → node N.

  The click is a differentiated, ringing pulse whose size follows accent. The R5/R6 weighting is my reading: **UNVERIFIED**.
- **Noise path** [DERIVED]: R10 12 kΩ / R9 4.7 kΩ (×0.28) → C15 0.47 µF (HPF 26 Hz) → R46 4.7 kΩ, C13 0.1 µF (**LPF ≈ 513 Hz**, Thevenin 3.1 kΩ) → R45 4.7 kΩ → node N, which has R56 330 Ω to ground (×0.066).
- **ATTACK** VR3 500 Ω (B) is a rheostat from N to Q6's emitter (R55 47 Ω): attenuation 47/(347 + R_VR3) → **−17.4 dB to −25.1 dB, a 7.7 dB range** [DERIVED] against +7 dB measured [R05]. **CONFIRMED.** Which pot end is clockwise is **UNVERIFIED**; minimum resistance must be ATTACK 100 %.
- **ENV2** (Q6 base): C12 0.033 µF, R41 22 kΩ, R42 470 kΩ. τ is 0.7 ms if R41's source returns to 0 V, 15.5 ms if it floats [DERIVED]. **UNVERIFIED.**
- [MEASURED, ATTACK 100 minus ATTACK 0, `bd_click.py`]:
  - click peak at 1.5–1.6 ms; −6 dB at 1.9 ms; −20 dB at 2.5–3.8 ms;
  - peak −6.6 to −7.5 dB re the whole hit;
  - roughly flat −13 to −23 dB from 500 Hz to 8 kHz (relative to the residual low band, which subtraction contaminates).

  So the click lasts about as long as the 2 ms trigger.

### 3.7 Level and accent
- Q12 and Q6 sum into IC11a; LEVEL VR4 100 kΩ (B) is the feedback resistor [SM p.11]. **CONFIRMED.**
- Accent enters at three points: click amplitude (Q5 → R6 → Q8), ENV1 start (Q4 → C8, clamped at 2.6 V), and ENV2 (if R41 is fed from Q5; UNVERIFIED).
- Pitch and hold are accent-independent (§3.2). [MEASURED] TOTAL ACCENT span **+6.2 dB**, saturating at ≥ 80 % [R05].

### 3.8 BD summary for the model
| Element | Value |
|---|---|
| VCO | symmetric triangle, f(t) = 49.5 Hz + Δf·e^(−t/τ), Δf ≈ 230 Hz (no accent dependence), τ = 6.8–29.4 ms (A taper; 4.5–19.6 ms for S/N < 381500) |
| Shaper | 10 kΩ into a 1SS133 pair ‖ 2.25 kΩ; drive ≈ 4.9 V peak |
| ENV1 | start = min(V_acc − 1.2 V, 2.6 V); hold 62 ms; then C8 = 0.33 µF into R_d = 47 kΩ + VR5 (1 MΩ A) ‖ 470 kΩ to 0.6 V |
| VCA | gain ∝ max(0, V8 − 0.6) |
| Click | pulse (SK LP 3.7 kHz, Q 3) + noise (LP 513 Hz), attenuated 7.7 dB over ATTACK, ≈ 2 ms gate |
| Level | linear 0–100 % |

---

## 4. Snare drum (SD) [SM p.6 text + figure, p.11 "SNARE DRUM" block; RW p.4–7]

### 4.1 Signal flow (read from [SM p.11]; **CONFIRMED** except where tagged)

```
TRIG ─R239 22k─► Q39 ─R242 10k/R246 22k─► Q40 (PNP, R245 100Ω to +15 V) = +15 V PULSE
  +15 V PULSE ─► D62/R275 → force IC37a ; R258 → Q44 (reset C69) ; R267 1M → Q45 (reset C71) ; D63/R274 → force IC38a
             ─► R281 100k → Q46 (mutes Q50 base node) ; R292 100k → Q49 (mutes Q51 base node) ; R266 100k → Q43 (ENV5 node)
             ─► D56 ─► R247 47k ─► C66 0.01µ = ENV1 (pitch bend)
TUNE: +15 V ─R287 100k─ VR6 10k(B) ─R284 10k─ gnd ; wiper ─R249 100k─► node A ─R248 470k─ C66
node A ─► IC35a follower ─R250 1k─► VDD of IC36 (4069UB) ; IC35b inverter (R251/R252 100k) ─R253 1k─► VSS ; D57 RD18JB2 across
VCO-1: IC37a comparator (R260/R262 100k; triangle via R269 100k) ─R259 100k─► IC36 (9,11,13→8,10,12) ─R261 47k─► IC37b + C69 0.01µ
VCO-2: IC38a comparator (R272/R271 100k; via R273 100k) ─R270 100k─► IC36 (1,3,5→2,4,6) ─R264 47k─► IC38b + C71 0.0068µ
VCO-1 ─R283 10k─ D65/D66 ─R288 1.5k─ R289 47Ω ║ Q50 ; base ◄ R290 22k ◄ (C70 0.0047µ) ◄ R291 220k ◄ C83 0.1µ ◄ D68 ◄ GATED ACC
VCO-2 ─R305 10k─ D69/D70 ─R304 2.2k─ R303 47Ω ║ Q51 ; base ◄ R293 220k ◄ (C79 0.01µ) ◄ R295 220k ◄ C82 0.047µ ◄ D67 ◄ GATED ACC
DAC ─► IC34b ×3.13 (R241 10k/R240 4.7k) = ACC ─► Q41 (gated by TRIG) = GATED ACC
                                             └─► Q42 (held by C68 0.0068µ/R256 1M, D61) ─► D60 ─► C67 0.47µ = ENV4 (D59 clamp)
ACC ─R255 100k─► IC34a (R243 10k fb) = −0.1·ACC ─► R254 100k ─ VR7 500k(B) TONE ─ ENV4
NOISE ─R286 10k─┬─R285 3.3k─gnd ; ─C85 0.015µ─┬─R302 220k─gnd ; ─R301 22k─┬─R300 22k─► IC40a(+) (C84 220p to gnd), C81 0.0033µ fb
 IC40a ─C80 0.022µ─R299 4.7k─ R298 47Ω ║ Q48 (2SD1469) ; base ◄ R297 220k ◄ ENV4                    (path a, main snappy)
 IC40a ─C76 0.0022µ─C75 0.0022µ─► IC39a(+) (R277 47k gnd; R276 6.8k fb) ─R278 2.2k─ R279 47Ω ║ Q47 (2SD1469) ; base ◄ R280 220k ◄ C73 470p ◄ D64 ◄ GATED ACC (path b, ENV5)
 Q47, Q48 collectors ─► IC39b (−) ; feedback VR9 10k(B) SNAPPY ‖ C77 10p ─► C78 0.47µ ─► R294 47k ─┐
 Q50, Q51 collectors ──────────────────────────────────────────────────────────────────────────────┴► IC40b (−) ; VR8 50k(B) LEVEL ─► SD out
```
- [SM p.6] says "controlled by an ACCENT coming through Q4 currently gated by the TRIG". On the schematic the gated accent feeding ENV2, ENV3 and ENV5 is **Q41's** output (Q4 belongs to the BD). Read "Q4" as a misprint for Q41. **CONFLICTING (text vs schematic); schematic preferred.**

### 4.2 VCOs and TUNE
- C69 = 0.01 µF (VCO-1) and C71 = 0.0068 µF (VCO-2) [SM p.11]; [SM p.6]: "charging capacitors C69 and C71 have different capacitance … VCO-1 runs at lower frequency". **CONFIRMED.**
- TUNE (VR6 10 kΩ B): wiper = 15·(10k + x·10k)/120k = **1.25–2.50 V**, linear in rotation, **exactly 2:1** [DERIVED]. At rest node A = wiper, VDD = +V_A, VSS = −V_A.
- [DERIVED] f = V_A/(1.269·10⁶·C):

  | TUNE | VCO-1 derived | VCO-2 derived | VCO-1 measured | VCO-2 measured | ratio measured |
  |---|---|---|---|---|---|
  | 0 % | 98.5 Hz | 144.9 Hz | 112.9 | 178.0 | 1.576 |
  | 50 % | 147.8 | 217.3 | 168.8 | 265.5 | 1.573 |
  | 100 % | 197.0 | 289.7 | 226.4 | 356.3 | 1.574 |

  Measured values are from the `sd_fit.py` two-VCO fit, TONE 0, SNAPPY 0; [R05] spectral peaks agree (114/171/230, 180/262/346).
  - *Fact-check re-measurement (parabolic-interpolated spectral peaks, 50–250 ms, SNAPPY 0, TONE 0/50/100 identical to 0.05 Hz):*

    | TUNE | 0 | 25 | 50 | 75 | 100 |
    |---|---|---|---|---|---|
    | f1 (Hz) | 117.4 | 144.7 | 173.5 | 200.9 | 230.7 |
    | f2 (Hz) | 176.3 | 217.1 | 260.2 | 301.3 | 346.1 |
    | f2/f1 | 1.5017 | 1.4998 | 1.4999 | 1.5002 | 1.5003 |

    The spectrum is f1/2 (−40 dB), f1, 1.5·f1, 2·f1, 3·f1 …, so the combined waveform is periodic at f1/2. Sliding 30 ms windows show a higher ratio (≈ 1.55–1.58) during the first ≈ 40 ms, settling to 1.50.
  - The **2.0 range is CONFIRMED** (measured 1.97).
  - The absolute frequency is **CONFLICTING**. *Corrected by fact-check: was −13 %; the derived values are **−15 to −16 %** below the settled 117.4/173.5/230.7 Hz.* The SD comparator or op-amp swing differs from the tom VCOs, which are themselves 4–8 % low (§5.2, corrected).
  - *Corrected by fact-check: was "The VCO-2/VCO-1 ratio is 1.574 against C69/C71 = 1.471 (CONFLICTING, +7 %)".* The settled ratio is **exactly 1.500** at every TUNE, against 1.471 nominal (+2 %). An exact 3:2 at five TUNE settings is not cap tolerance; it looks like **mode-locking**, plausibly through the shared IC36 VDD/VSS rails (1 kΩ feeds). The mechanism is **UNVERIFIED**. The 1.574 figure is a fit artefact from the bend region.
  - Calibrate f1 to the measurement and **lock VCO-2 at 3:2** once the bend has decayed.
- Waveform: triangle → D65/D66 (VCO-1) or D69/D70 (VCO-2) soft clip, annotated "0.6V" with a sine [SM p.11]. [RW p.4]: "rounded to a rough approximation of a sine wave by back-to-back diodes D65 and D66". **CONFIRMED.**

### 4.3 Pitch bend (ENV1, C66)
- [SM p.6]: "The outputs of IC35 gives the buffers output amplitude proportional to ENV-1 as shown in figure; the charging rate of C69 also continuously changes for about 20ms." The figure shows **VDD 2 V → 5 V and VSS −2 V → −5 V, rising during 2 ms and relaxing over ≈ 20 ms**. **CONFIRMED.**
- [DERIVED]:
  - The Q40 pulse charges C66 through D56 + R247 47 kΩ to V_B ≈ (14.4·570k + V_w·47k)/617k ≈ **13.3–13.5 V** *(corrected by fact-check: was ≈ 12.6 V; with V_w = 1.875 V the formula gives 13.45 V at a 14.4 V pulse and 13.26 V at 14.2 V)*. τ_charge = 0.43 ms, so it is full within the 2 ms trigger.
  - Node A = (V_w·470k + V_B·100k)/570k, so the excess on A is (V_B − V_w)·100/570 ≈ **2.0 V** *(corrected by fact-check: was 1.9 V)*, independent of TUNE. At TUNE centre A goes 1.88 → 3.9 V, against 2 → 5 V in the figure.
  - Relaxation τ = C66·(R248 + R249) = **5.7 ms**.
- [MEASURED, `sd_fit.py`] Model `f_i(t) = f_i,s·(1 + k·e^(−t/τ))`, two reset-phased oscillators with exponential amplitudes, fitted to the first 40 ms:
  - **τ = 5.5 / 5.1 / 4.8 ms** at TUNE 0/50/100;
  - k = 1.91 / 1.28 / 0.95, so **k·f1,s = 216 / 216 / 215 Hz**: a constant absolute bend, exactly as derived.

  [R05]'s "1.13–1.18 bend ratio" describes the 12–25 ms tail of this bend, not its start. Use **f1(t) = f1,s + 216 Hz·e^(−t/5 ms)** and scale VCO-2 by the same factor (1 + k·e^(−t/τ)).
- [RW p.4]: the bend pulse "is the same for all SD notes, and is not related to the accent level". **CONFIRMED** (it comes from Q40).

### 4.4 Drum VCAs (ENV3 → Q50, ENV2 → Q51)
- [DERIVED] ENV3: C83 0.1 µF discharging only through R291 + R290 = 242 kΩ into Q50's base: **τ = 24.2 ms**. C70 4.7 nF at the R291/R290 junction smooths the attack (≈ 1 ms). ENV2: C82 0.047 µF through R295 + R293 = 440 kΩ: **τ = 20.7 ms**, with C79 0.01 µF smoothing.
- [MEASURED] amplitude τ VCO-1 **24.0 / 25.1 / 25.7 ms**, VCO-2 **30.9 / 25.8 / 25.5 ms**. **CONFIRMED.**
- [DERIVED] Relative level: injection 47/1547 (VCO-1) vs 47/2247 (VCO-2) = +3.2 dB; base drive 1/242k vs 1/440k = +5.2 dB, so VCO-2 is **−8.4 dB** re VCO-1. [MEASURED] a2/a1 = 0.33–0.36 (**−8.9 to −9.6 dB**). **CONFIRMED.**
- During the trigger Q46 and Q49 clamp the VCA base nodes ("cuts off VCA Q50, muting unwanted noises" [SM p.6]). **CONFIRMED.** The drum tone therefore starts at the end of the trigger with both oscillators phased.

### 4.5 Snappy [SM p.6; RW p.4–7]
- [SM p.6]: "ENV 5 determines the amount of high frequency noise components in the SNAPPY which becomes articulate when noises passing through a high pass filter (IC39a and associated RC's) are combined with the noises from the low pass filter at IC39b." **CONFIRMED.**
- Input conditioning [DERIVED, `filt.py`]:
  - R286/R285 divide the 0/15 V noise by 4 (3.7 V p-p). [RW p.4]: "R286 drove R285 (3.3k) to reduce the signal level to 1/4 of the 15 volt digital signal".
  - C85/R302 high-pass at 47.7 Hz.
  - **IC40a unity Sallen-Key low-pass**: R301 = R300 = 22 kΩ, C81 3.3 nF (feedback), C84 220 pF (to ground) → **f0 = 8.49 kHz, Q = 0.5·√(C81/C84) = 1.94** (+5.8 dB peak). *Fact-check refinement: these are the ideal-source figures. With the real source (R286‖R285 = 2.48 kΩ, C85, R302) solved as a nodal network, the gain peak is at **7.43 kHz, +5.1 dB** (the ideal filter peaks at 7.91 kHz, +6.0 dB). The adding of 2.48 kΩ to R301 alone gives f0 8.05 kHz, Q 1.93.*
- **Path (a), main snappy, ENV4/TONE** [DERIVED]:
  - Filter: C80 0.022 µF into R299 4.7 kΩ + R298 47 Ω → **first-order HPF 1.52 kHz** → Q48 emitter. [RW p.7]: "C80 … forms a high-pass filter for the noise signal which is sent to this pathway 4, via the Q48 VCA". **CONFIRMED.**
  - ENV4: C67 0.47 µF, charged by Q42/D60 to ≈ V_acc − 1.2 V.
  - Hold: Q42 is kept on by C68 0.0068 µF / R256 1 MΩ (τ 6.8 ms; ≈ 3.5τ ≈ 24 ms). [RW p.6]: "It stays at its maximum level for about 24 ms … C68 is removed, which eliminates the 24 ms flat start". **CONFIRMED.**
  - Decay: through R254 100 kΩ + VR7 (TONE, 500 kΩ B rheostat) **toward IC34a's output, −0.1·V_acc**, and through R297 220 kΩ into Q48's base. [RW p.5]: "the discharge path (VR7 500k – the Tone pot – in series with R254, 100k) being to a negative voltage". **CONFIRMED.** D59 stops C67 going negative.
  - So V4 → V_eq = (V_end/R_t + 0.6/R297)·(R_t‖R297) with τ = C67·(R_t‖R297) = **32 ms (TONE 0) – 76 ms (TONE 100)**. The VCA **cuts off** when V4 = 0.6 V: the "steam valve" envelope [RW p.6].
  - [MEASURED, `tone.py`; 3–12 kHz band, TUNE 50, SNAPPY 100] cut-off (first −40 dB crossing) **99 / 179 / 225 / 255 / 295 ms** at TONE 0/25/50/75/100. The level falls only 6–22 dB before a cliff of 30–40 dB.
  - [DERIVED fit, `tonefit.py`] linear B taper, fitted V0 = 10.3 V and V_end = −0.52 V → 108 / 176 / 224 / 260 / 289 ms. **CONFIRMED** (≤ 9 ms). With nominal guesses (V0 6.8 V, V_end −0.8 V) the model runs 10–18 % short. V0 depends on accent (UNVERIFIED code mapping), which also lengthens the snappy at high accent [RW p.4].
  - Whittle on Q48 saturation: "In about the top half of the voltage range, such as above 8 volts, I think there is little change in VCA output, due to the transistor Q48 being very much turned on" [RW p.5]. **UNVERIFIED** (single source); model as a soft saturation of I_B.
- **Path (b), "articulate" HF crack, ENV5** [DERIVED]:
  - Filter: **unity Sallen-Key HPF** C76 = C75 = 2.2 nF, R276 6.8 kΩ (feedback), R277 47 kΩ (ground) → **f0 = 4.05 kHz, Q = 0.5·√(R277/R276) = 1.31** (formula checked symbolically in `filt.py`).
  - Then R278 2.2 kΩ / R279 47 Ω → Q47. ENV5 = gated accent → D64 → C73 470 pF → R280 220 kΩ → base: **τ = 0.10 ms after the 2 ms gate**, so it is a ≈ 2 ms accent-scaled burst. Q43 (R266 100 kΩ / R265 22 kΩ) also acts on this node (UNVERIFIED function).
  - [RW p.4]: "quiet and very brief on its own … I think inaudible in the presence of the next signal". **UNVERIFIED** (opinion).
- **SNAPPY** VR9 10 kΩ (B) is IC39b's feedback resistor (‖ C77 10 pF): snappy amplitude ∝ rotation, silent at 0 [SM p.11]. **CONFIRMED.** The snappy then enters IC40b through C78 0.47 µF + R294 47 kΩ (HPF 7.2 Hz).
- [MEASURED, `snappy.py`; 50–120 ms, TUNE 50, SNAPPY 100] octave bands (250 Hz … 48 kHz) re max:

  | | 250 | 500 | 1k | 2k | 4k | 8k | 16k | 24k–48k |
  |---|---|---|---|---|---|---|---|---|
  | TONE 50 | −6.3 | −10.1 | −5.4 | −3.0 | **0.0** | −5.8 | −21.6 | −34.5 |
  | Path (a) derived | −16.4 | −11.1 | −7.0 | −4.1 | **0.0** | −3.3 | −16.7 | −26.8 |

  - 500 Hz–8 kHz agree within 3 dB. **CONFIRMED.**
  - The 250 Hz band holds the drum oscillators.
  - The extra roll-off above 16 kHz is the S/N ≥ 415300 multi-out RC (1 kΩ/0.01 µF, 15.9 kHz [R03]) plus the 96 k recording chain.
  - The measured spectral peak (4.8–7.0 kHz) moves up with TONE because the tail is analysed.
- Known unit defects [RW p.7]: leaky Q47/Q48 (2SD1469-R) cause continuous hiss; C73 retaining ≈ 0.3 V gives "a slight fizzly sound … for a second or two after each note". Single-unit reports, **UNVERIFIED** as general. Optional "aged unit" switch, default off.

### 4.6 SD accent
- Accent enters ENV2, ENV3 and ENV5 (Q41, during the trigger) and ENV4 (Q42, held ≈ 24 ms). The snappy end-voltage −0.1·V_acc also moves.
- No clamp, but the [R03] buffer-clip (> ≈ 13.5 V, codes above ≈ 52–56) applies.
- [MEASURED] TOTAL ACCENT span +4.8 dB [R05]; [EL-ACC in R03] "ENV2 without accent is half of ENV2 with accent" (UNVERIFIED).

---

## 5. Toms (LT, MT, HT) [SM p.11 "LOW TOM", "MID TOM", "HI TOM", "TOM NOISE" blocks]

### 5.1 Topology (identical for the three; designators per tom below) **CONFIRMED** (read)
```
+15 V PULSE (Q15/Q24/Q60) ─► VCO reset + comparator forcing (all three VCOs) ; ─► D/C pitch caps (2) ; ─► overdrive timer (C24/C38/C101)
GATED ACCENT (Q17/Q29/Q62) ─► D ─► C_ENV1 ─R 470k─► VCO1 VCA base
                          ─► D19/D30/D75 ─► C_A (0.68/0.47/0.47µ) ─R 100k─► C_ENV2 (0.68/0.47/0.47µ) ─R 220k─► VCO2 VCA base ; ─R 220k─► VCO1 VCA base
                                        C_A ─ DECAY VR 500k(B) ─ 56k ─ gnd
                          ─► D ─► C_ENV3 ─R 470k─► VCO3 VCA base
TUNE: +15 V ─68k─ VR 10k(B) ─10k─ gnd ; wiper ─► follower ─1k─► VDD ;  VSS = −(VDD + V_C1·100k/2.2M + V_C2·100k/470k) via inverter, ─1k─► VSS ; 18 V zener
VCO1/2/3 ─10k─ diode pair ─2.2k─ 47Ω ║ VCA ; VCO1's 10k bypassed by 1k + transistor while the overdrive timer runs
TOM NOISE ─22k─► VCO3 VCA emitter node
VCA collectors ─► summing amp ; LEVEL 50k(B) feedback
```

| Part | LT | MT | HT |
|---|---|---|---|
| VCO1 int. cap / R / reset | C18 0.022 µF / R73 47k / Q13 | C32 0.018 µF / R134 47k / Q30 | C97 0.015 µF / R364 47k / Q53 |
| VCO2 | C19 0.033 µF / R78 / Q14 | C33 0.027 µF / R139 / Q31 | C102 0.022 µF / R368 / Q54 |
| VCO3 | C20 0.012 µF / R75 / Q22 | C34 0.01 µF / R136 / Q32 | C103 0.0082 µF / R366 / Q55 |
| VCO op-amps | IC15, IC17, IC19 | IC21, IC24, IC25 | IC43, IC46, IC47 |
| Inverters (4069UB) | IC16 | IC22 | IC44 |
| TUNE | VR10 10k(B), R113 68k, R112 10k | VR13, R172 68k, R173 10k | VR16, R386 68k, R384 10k |
| CV amps / zener | IC14a/b, R68/R65 1k, R64/R66 100k, D16 | IC20a/b, R126 1k, R125/R127 100k, D34 | IC42a/b, R333/R338 1k, R337/R335 100k, D72 |
| Pitch caps (slow / fast) | C16 0.1 µF·R60 2.2M / C17 0.047 µF·R61 470k (D15/D14) | C30·R121 / C31·R122 (D37/D36) | C95·R331 / C96·R332 (D83/D82) |
| VCO1 VCA | Q19; R103 10k, D20/21, R104 2.2k, R99 47 | Q26; R164, D38/39, R165, R159 | Q57; R358, D77/78, R357, R351 |
| VCO1 overdrive | Q18, R102 1k, C24 0.01 µF, R101 1M, D17 | Q23, R163 1k, C38 0.0068 µF, R162 1M, D31 | Q56, R359 1k, C101 0.0056 µF, R360 1M, D76 |
| ENV1 (VCO1) | C22 0.22 µF, R107 470k, D24 | C36 0.15 µF, R168 470k, D29 | C99 0.15 µF, R354 470k, D74 |
| ENV2 share to VCO1 | R108 220k | R169 220k | R353 220k |
| VCO2 VCA / ENV2 | Q20; R105, D22/23, R106, R98; R109 220k ← C23 0.68 µF | Q27; R166, D40/41, R167, R160; R170 ← C37 0.47 µF | Q58; R356, D79/80, R355, R350; R352 ← C100 0.47 µF |
| DECAY network | VR11 500k(B), R111 56k, R100 100k, C21 0.68 µF, D19 | VR14, R171, R161, C35 0.47 µF, D30 | VR17, R385, R341, C98 0.47 µF, D75 |
| VCO3 VCA / ENV3 | Q21; R117, D27/28, R116, R115; R110 470k ← C25 0.056 µF, D25 | Q28; R181, D46/47, R176, R175; R174 ← C39 0.056 µF, D43 | Q59; R388, D85/86, R380, R379; R378 ← C106 0.047 µF, D84 |
| Tom-noise inject | R114 22k | R177 22k | R387 22k |
| Accent buffer / switch | IC18b, Q17 (R96 100), Q16 trig | IC23b, Q29, Q25 | IC45b, Q62 (R348 100), Q61 |
| Sum / LEVEL | IC18a, VR12 50k(B) | IC23a, VR15 50k(B) | IC45a, VR18 50k(B) |

Small labels (a few 100 kΩ comparator resistors, the MT VDD resistor "R12?") are partly illegible at 600 dpi; those designators are **UNVERIFIED**. Values are **CONFIRMED**.

### 5.2 Frequencies and TUNE [DERIVED vs MEASURED]
- TUNE wiper = 15·(10k + x·10k)/88k = **1.705–3.409 V**, linear, **exactly 2:1** [DERIVED]. [MEASURED] ratios LT 1.97, MT 1.91, HT 1.94 [R05]. **CONFIRMED.**
- Settled frequency (symmetric rails) f = VDD/(1.269·10⁶·C):

  | Voice | VCO1 (TUNE 0/50/100) | VCO2 | VCO3 | VCO1 measured, settled [R05] |
  |---|---|---|---|---|
  | LT | 61.1 / 91.6 / 122.1 Hz | 40.7 / 61.1 / 81.4 | 112 / 168 / 224 | 62 / 90 / 122 |
  | MT | 74.6 / 111.9 / 149.2 | 49.8 / 74.6 / 99.5 | 134 / 202 / 269 | 77 / 112 / 147 |
  | HT | 89.6 / 134.3 / 179.1 | 61.1 / 91.6 / 122.1 | 164 / 246 / 328 | 89 / 132 / 173 |

  *Corrected by fact-check: was **CONFIRMED** (≤ 4 %; HT TUNE 100 −3.4 %).* The measured column is f1 at ≈ 150–250 ms, which still carries the slow pitch term (+5–8 %, §5.3). Fitting the derived rail law (both terms) to f1(t) over 120–600 ms gives the asymptotic f_s:

  | Voice | f_s measured (TUNE 0/50/100) | measured/derived |
  |---|---|---|
  | LT | 57.0 / 85.8 / 117.2 Hz | 0.934 / 0.937 / 0.959 |
  | MT | 71.9 / 106.6 / 141.3 | 0.964 / 0.952 / 0.947 |
  | HT | 82.8 / 125.4 / 166.7 | 0.924 / 0.933 / 0.931 |

  The derived values are **4–8 % high: CONFLICTING (small)**. The measured/derived ratio is constant within ±1 % from 150 to 600 ms, so the rail-law shape is right and only the scale (V_th, op-amp swing or caps) is off. That offset has the same sign as the SD's, but it is smaller.
- Ratios [MEASURED, `tom_fit.py`, three-oscillator fit, DECAY 100]: VCO2/VCO1 = **0.679 (LT), 0.673 (MT), 0.675 (HT)** against 0.667/0.667/0.682 derived; VCO3/VCO1 = **1.87–1.89, 1.76–1.78, 1.94–1.96** against 1.833/1.800/1.829. **CONFIRMED** (HT VCO3 +6 %). [R05]'s weak "3×" partial is VCO1's third harmonic, not a separate oscillator.

### 5.3 Pitch envelope (VSS-only, two time constants)
- [DERIVED] The +15 V pulse charges C16/C17 (LT) through diodes to ≈ 14.2 V. They discharge only into IC14b's virtual ground: **τ_fast = C17·R61 = 22.1 ms**, **τ_slow = C16·R60 = 220 ms**. VSS = −(VDD + 0.2128·V_C17 + 0.04545·V_C16) → extra **3.02 V (fast) + 0.65 V (slow)** at t = 0, independent of TUNE.
- [DERIVED] With asymmetric rails `f = f_s·2/(1 + VDD/|VSS|)`. At LT TUNE 50 this gives 130 Hz at 0 ms, 121 Hz at 12 ms, 107 Hz at 40 ms, 97 Hz at 180 ms (f_s 91.6). The compressive law makes the fast 22 ms term look like a ≈ 30–45 ms decay in f.
- [MEASURED, `tom_fit.py`] fit `f1 = f∞·(1 + k_a·e^(−t/τa) + k_b·e^(−t/τb))`:

  | | TUNE 0 | TUNE 50 | TUNE 100 |
  |---|---|---|---|
  | LT k_a / τa | 0.365 / 45.5 ms | 0.287 / 48.6 ms | (unstable fit) |
  | MT | 0.379 / 45.1 | 0.290 / 47.7 | — |
  | HT | 0.350 / 55.0 | 0.318 / 41.5 | 0.254 / 41.9 |

  The slow term is ≤ 0.1 and poorly determined. Derived k(0) = 0.47 / 0.37 / 0.31 (fast term only; **0.52 / 0.42 / 0.35 with both terms**, added by fact-check). **CONFIRMED** in form and trend. *Corrected by fact-check: was "±25 %"; against the both-term derivation the measured k_a is 25–45 % smaller.*
  *Corrected by fact-check: was "The derived slow term (+5–8 % at 120–250 ms) is not seen: **CONFLICTING**, possibly C16/R60 tolerance", with a slow-term weight of ≈ 0–0.3 of the derived value.* The slow term **is** present. Zero-crossing f1 of every tom at TUNE 0/50/100, DECAY 100, keeps falling from 150 to 600 ms (e.g. LT TUNE 0: 62.2 → 60.6 → 60.0 → 58.5 → 57.8 Hz at 150/215/300/425/600 ms, a 7.1 % drop, against 6.9 % predicted). A least-squares fit with the slow amplitude free gives **0.57–0.66 V** (derived 0.645 V; one outlier, LT TUNE 50, 0.26 V). With τ also free it gives 155–264 ms (derived 220 ms). **CONFIRMED.** The earlier fit missed it because its "f∞" absorbed the slow term. **Implement the rail model** with fast τ ≈ 22 ms and the **derived slow term (0.645 V, 220 ms) at full weight**, scaled to the measured f_s (§5.2).
- Rail skew: only VSS moves, so early cycles ramp faster down than up, giving **even harmonics that fade as the envelope relaxes**. [MEASURED, `tomh3.py`, TUNE 50, DECAY 100] VCO1 H2: LT −6.7 → −27.4 dB, MT −8.5 → −20.8, HT −7.4 → −20.1 (0–30 ms → 150–250 ms). **CONFIRMED.**

### 5.4 VCO1 attack overdrive
- [SM p.11; read] Q18 (LT) and R102 1 kΩ shunt R103 10 kΩ between VCO1 and its clipping diodes, so the diodes see **≈ 10× the drive** while Q18 conducts. C24 0.01 µF, charged by D17 from the +15 V pulse, drives Q18's base through R101 1 MΩ. **CONFIRMED** (topology); the switch polarity is **UNVERIFIED**.
- [DERIVED] Q18 conducts until V_C24 ≈ 0.6 V: ≈ 3.2·τ = **32 ms (LT, 10 ms), 22 ms (MT, 6.8 ms), 18 ms (HT, 5.6 ms)**, fading as the base current falls.
- [MEASURED] VCO1 H3/H5 at 0–30 ms vs 150–250 ms: LT −11.6/−18.7 → −17.2/−31.0 dB, MT −13.0/−20.1 → −17.0/−30.6, HT −14.7/−24.4 → −17.2/−30.6. **CONFIRMED** (near-square at onset). The [SM p.9] LT drawing shows flat-topped early cycles.

### 5.5 Envelopes, DECAY, noise
- [DERIVED] ENV1 (VCO1): τ = 0.22 µF·470k = **103 ms (LT)**, 0.15 µF·470k = **70.5 ms (MT, HT)**. VCO1 also receives ENV2 through 220 kΩ.
- [DERIVED] ENV2 (VCO2, part of VCO1): a two-capacitor network.
  - C_A is charged directly; C_ENV2 fills through 100 kΩ: **rise τ ≈ 100k·(C_A‖series C_ENV2) = 34 ms (LT), 23.5 ms (MT/HT)**, so VCO2 swells in after the attack (the ENV2 symbol on [SM p.11] is drawn with a rounded rise).
  - C_A decays through DECAY VR + 56 kΩ: **τ = 38–378 ms (LT), 26–261 ms (MT/HT)** over the pot range.
  - C_ENV2 also drains into two 220 kΩ bases (110 kΩ: 75 ms LT, 52 ms MT/HT).
  - Net behaviour is **UNVERIFIED**; fit it.
- [DERIVED] ENV3 (VCO3 + noise): **26 ms (LT, MT), 22 ms (HT)**.
- [MEASURED, three-VCO fit, DECAY 100] amplitude τ: VCO1 166–180 ms (LT), 114–118 (MT), 101–119 (HT); VCO2 272–302, 158–167, 163–173 ms; VCO3 43–58 ms (LT/MT), 13–44 (HT, poorly determined). VCO2/VCO1 level 0.34–0.43 (−7 to −9 dB); VCO3/VCO1 0.35–0.73.
- [MEASURED, `tomdec.py`, TUNE 50] t(−10/−20 dB) in ms:

  | DECAY | 0 | 20 | 40 | 60 | 80 | 100 |
  |---|---|---|---|---|---|---|
  | LT | 74/165 | 49/184 | 76/212 | 76/213 | 76/213 | 76/213 |
  | MT | 35/103 | 34/112 | 35/112 | 34/138 | 34/138 | 35/138 |
  | HT | 50/108 | 68/137 | 72/138 | 51/144 | 68/160 | 68/160 |

  **DECAY has a small, early-saturating effect** on the overall −20 dB time. It mostly sets the length of the low VCO2 "bloom". The knob value is therefore mainly audible as the VCO2 tail level after ≈ 150 ms.
- **TOM NOISE** (shared by the three toms) [SM p.11, read; **CONFIRMED** values]:
  - Noise → R190 22 kΩ / C161 4.7 nF (LPF 1.54 kHz) → C51 = C52 = 4.7 nF Sallen-Key HPF (R191 10 kΩ feedback, R192 220 kΩ ground) on IC27b: **f0 = 722 Hz, Q = 2.35** [DERIVED] for an ideal source. Net: a resonant band around ≈ 0.7–1.5 kHz. *Corrected by fact-check: the HPF is driven from the R190/C161 node, whose impedance (22 kΩ ‖ C161, ≈ 15 kΩ at 700 Hz) is comparable to R191 and damps the resonance. A nodal solution of the whole network gives a **broad band-pass peaking at ≈ 904 Hz, only +1.6 dB, −3 dB band ≈ 625–1600 Hz**. The ideal cascade would peak at 750 Hz, +6.7 dB. Do not implement it as "LP 1.54 kHz × SK HP Q 2.35"; implement the loaded network.*
  - → C53 0.047 µF → R195 1 kΩ / R199 47 Ω → Q33 VCA → IC27a (R193 22 kΩ).
  - Q33's base: C54 (0.0022 µF; **0.0047 µF from S/N 426700**) charged by D26/D44/D45 from any tom's +15 V pulse, through R200 100 kΩ. **τ ≈ 0.22 ms (0.47 ms late units)**: a sub-millisecond noise "tick".
  - **R194 47 kΩ (100 kΩ from S/N 426700)** feeds IC27b's output straight into IC27a. That adds a steady noise component at gain −0.47 (−0.22 late), which each tom gates with ENV3.
  - Q34 (R196 22 kΩ/R197 10 kΩ from D1/D2/D3) acts on C54 through R198 100 kΩ (function **UNVERIFIED**).
  - [SM p.7]: "TOM Noises … C54 0.0022 → 0.0047µF; R194 47k → 100k … emphasize attack of TOM TOMs". **CONFIRMED.** [SM p.9] marks "noise" at the LT onset.
  - Each tom adds tom-noise through 22 kΩ into its VCO3 emitter node (R114/R177/R387), so the noise is gated by ENV3 (≈ 22–26 ms).

### 5.6 Tom accent
- Gated accent → ENV1, ENV2 (via C_A) and ENV3. No clamp; pitch is not accented.
- [MEASURED] TOTAL ACCENT span +7.3 (LT), +7.7 (MT), +7.4 dB (HT) [R05]. The [SM p.9] drawing ratio (LT, my pixel estimate) is 1.6×.

---

## 6. Rim shot (RS) [SM p.9, p.11 "RIM SHOT" block, p.7]

### 6.1 Signal flow (**CONFIRMED**, read)
```
TRIG ─► C111 0.018µ ─┬─ R399 22k (R398 10k) ─► Q63 ─► D90/R400 47k ─► Q64 base (R402 1M) ; Q64 emitter = IC48b ×3.13 accent (R406 10k/R397 4.7k)
                     │                                Q64 collector ─► R401 2.7k ─► C119 0.047µ = ENV ─► R403 1M ─► Q65 base
                     └─ D89 ─► R396 22k / R395 22k (÷2) = KICK ─► (+) inputs of IC48a, IC49a, IC49b
F1 IC48a: bridged-T C113 = C112 = 0.01µ, R394 2.2k (shunt), R407 470k (bridge) ─► R408 12k ─┐
F2 IC49a: C116 = C115 = 0.027µ, R411 2.2k, R414 330k ─────────────────────────► R415 12k ─┤ Σ node, D91/D92 anti-parallel to gnd
F3 IC49b: C118 = C117 = 0.0047µ, R404 2.2k, R416 470k ─────────────────────► R417 3.3k* ┘ ─► R423 12k ─ R418 680Ω ║ Q65 emitter
Q65 collector ─► IC50a (−), feedback VR19 100k(B) LEVEL ‖ C120 220p ─► C121 0.01µ ─ C122 0.01µ ─► IC50b(+) (R420 220k gnd; R422 22k to (−) node) ; IC50b gain 2 (R421 / R419 4.7k) ─► RS out
* R417 = 12k before S/N 415300 [SM p.7]
TRIG OUT: Q66/Q67 pulse stretcher (C114 0.1µ, R409/R410 47k, R412 22k, R413 1k, R392, R393, R495 1k)
```
- The resonators are the 808's **bridged-T, excitation on the (+) input** (mw808 01 §0.2). Same formulas: `f = 1/(2π√(R1R2C1C2))`, `Q = √(R2/R1)/(√(C1/C2) + √(C2/C1))`, τ = Q/(π·f).
- The schematic prints, above the RS block, "F1 T1≈2ms 30Vp-p, 20ms"; "F2 T2≈4.5ms 15Vp-p, 25ms"; "F3 T3≈1ms 30Vp-p, 5ms" [SM p.11]. **CONFIRMED.** These are periods, peak-to-peak ringing amplitudes and visible ring lengths. The rails clip the filters at the start.

### 6.2 Resonators [DERIVED vs MEASURED, `rs.py`]
| Filter | f (derived) | Q | τ (derived) | Schematic T | f (measured) | τ (measured, −10→−20 dB) |
|---|---|---|---|---|---|---|
| F2 | **218.8 Hz** | 6.12 | 8.9 ms | 4.5 ms (222 Hz) | **222 Hz** | 9.6 ms |
| F1 | **494.9 Hz** | 7.31 | 4.7 ms | 2 ms (500 Hz) | **481 Hz** | 4.6 ms |
| F3 | **1053 Hz** | 7.31 | 2.2 ms | 1 ms (1 kHz) | **1019 Hz** | 2.8–3.0 ms |

- **CONFIRMED** (frequencies ≤ 3.2 %, τ within 25 %).
- Other measured lines: 723 Hz (−20 dB), 1288 Hz (−28), 1975 (−33), 2446 (−35), 2799 (−40). They are the same at accent 0, 100 and single shots. I attribute them to intermodulation in the D91/D92 clipper. **UNVERIFIED.**
- Band peaks occur at 1.7 ms (F3), 5.5 ms (F1) and 9.2 ms (F2): the clipper hides each band until the stronger ones decay.
- The excitation is the edge-differentiated trigger (C111 with 44 kΩ: τ 0.8 ms, ≈ 2.2 V), **not accent-scaled** [DERIVED]. *(Corrected by fact-check: was ≈ 2.5 V. (5 V − 0.6 V D89)·22k/44k = 2.2 V, before the extra loading of R399/Q63.)*

### 6.3 Clipper, VCA, accent
- [DERIVED] Filter outputs of 7–15 V peak into 12 kΩ/12 kΩ/3.3 kΩ drive milliamps into D91/D92, so the sum is hard-limited to ≈ ±0.6 V. The output is a clipped (square-ish) mix; the weights matter only as the filters decay into the clipper's linear region.
- [SM p.9] draws both RS versions (R417 = 3.3 k and 12 k) with square-topped cycles inside a decaying envelope. **CONFIRMED.** The 3.3 kΩ version (S/N ≥ 415300, "more realistic sound" [SM p.7]) weights F3 (1 kHz) by 3.6×. Expose an early/late switch.
- VCA Q65: emitter from the clipper through R423 12 kΩ / R418 680 Ω (±0.6 V → ±32 mV). Base from C119 through R403 1 MΩ: **τ = 47 ms** [DERIVED], long against the filter decays. C119 charges through R401 2.7 kΩ (τ 0.13 ms) to ≈ V_acc − 0.6 V while Q63 conducts (≈ 0.5–1 ms).
- Accent changes level only: [MEASURED] partial frequencies and τ are identical at accent 0 and 100; TOTAL ACCENT span **+3.9 dB** [R05]. RS shares its accent DAC with HC [R03].
- [MEASURED] whole-voice t(−10/−20/−30 dB) = 9.6/17/21.7 ms (accent 0), 9.7/21/26 ms (accent 100) [R05].

### 6.4 Output filter and level
- IC50a: LEVEL VR19 100 kΩ (B) ‖ C120 220 pF → **level-dependent low-pass, f = 1/(2π·R_VR19·220 pF) = 7.2 kHz at full, 14.5 kHz at 50 %** [DERIVED].
- IC50b [SM p.11, read at 3×]: C121 = C122 = 0.01 µF, R420 220 kΩ to ground, R422 22 kΩ from the capacitor junction to the (−) node; R421/R419 4.7 kΩ set gain 2. Since V(−) = V(+), this is a **unity Sallen-Key HPF followed by ×2**: **f0 = 229 Hz, Q = 0.5·√(220k/22k) = 1.58** (+4.3 dB peak) [DERIVED]. **CONFIRMED** (topology). F2 (219 Hz) sits on this peak.

---

## 7. Hand clap (HC) [SM p.11 "HAND CLAP" block; RW p.8–9; 808-02 §5]

### 7.1 Signal flow (values **CONFIRMED**; burst-generator wiring partly **UNVERIFIED**)
```
NOISE ─► R208 47k ─┬─ R209 10k ─ gnd                MFB band-pass on IC26b: C42 = C43 = 0.0047µ, R207 150k (fb)
                   ├─ C42 ─► IC26b (−) ; C43 ─► IC26b out
IC26b ─► C55 0.47µ ─┬─► C56 0.022µ ─► R202 39k ─┬─ R201 1k ─ gnd ─► IC30 BA662A (OTA) pin 2 (R236 1k on pin 3) ─► pin 6 ─┬─ R237 82k ─ gnd
                    │                            (÷40)                                                               └─ C58 0.001µ ─► IC28a (−)
                    └─► R204 1k ─┬─ C57 0.47µ ─ gnd ─► R205 3.3k ─ R203 47Ω ║ Q37 emitter ; Q37 collector ─► IC28a (−)
                                                     Q37 base ◄ R234 470k ◄ C61 0.1µ (D53) ◄ R233 470k ◄ C59 0.1µ ◄ D52 ◄ (pulse)
IC28a feedback VR20 50k(B) LEVEL ─► HC out
Burst generator: DAC ─► IC26a ×3.13 (R212 10k / R206 4.7k; R496 100k on S/N ≤ 371499) ─► Q36 (gated by Q35: R215 22k, R216 10k; R210 22k, R211 10k)
  ─► R213 22k / R218 22k ─► IC29 AN6912 quad comparator a–d ; ladder R219 / R217 / R214 4.7k ; C62 0.027µ with R229 1M (gate timer, τ 27 ms)
  comparator c ─► D54 (R220 82k) ─► C65 0.47µ "ENV1" ─ R225 1M ; comparator d (C63 220p, R221 18k, R222 10k, R227 2.7k, R228 5.6k, C64 0.001µ, R226 68k, D55)
  ─► Q40 (slams C65 through R223 330Ω) ; C65 node ─► R224 470k / R238 15k ─► Q38 (dual PNP exponential converter, R235 15k, R230 2.2k)
  Q38 ─► BA662 control current ; other Q38 base ◄ R494 47k ◄ TM 10k(B) trimmer
```
- Whittle [RW p.8]: "The VCA is driven with a series of short pulses, like a few cycles of a sawtooth wave … controlled in the TR-909 by a trimpot (TM) which drives (via R494) one base of the dual PNP transistor Q38 (which functions along the lines of a log to linear converter …)". **CONFIRMED.** "R208 (47k) … the junction of R209 (10k to ground) and the two filter capacitors C42 and C43 … its impedance at that point is supposed to be 47k in parallel with 10k (8.2k)" [RW p.9]. **CONFIRMED.**
- This is the 808's multi-clap architecture carried over almost part for part [808-02 §5.3]: AN6912 comparators, a 0.47 µF cap slammed through 330 Ω by a transistor (808: C144/R367/Q73; 909: C65/R223/Q40), an exponential converter into a BA662 OTA, and a parallel transistor "reverb" VCA with a two-capacitor envelope. **CONFIRMED** (parts); the exact 909 comparator wiring is **UNVERIFIED**.

### 7.2 Band-pass [DERIVED, `filt.py`]
- MFB (Deliyannis) with R1 = R208 47 kΩ, R2 = R209 10 kΩ, R3 = R207 150 kΩ, C1 = C2 = 4.7 nF:
  - **f0 = 1/(2πC·√(R3·(R1‖R2))) = 963 Hz**
  - **Q = ½·√(R3/(R1‖R2)) = 2.13**
  - centre gain −R3/(2R1) = −1.60
- [MEASURED, `clap2.py`, 19 hits] spectral peak **1031 Hz** (bursts, 0–34 ms) and **938 Hz** (tail, 60–250 ms). **CONFIRMED.**
- [KVR-CP]'s "1140 Hz, Q 1.95" is **not** supported by the parts. *(Corrected by fact-check: was "(it reproduces only if R209 is ignored and the input is taken as 47 kΩ ‖ 150 kΩ …)". That reading gives 462 Hz, Q 1.02, not 1140 Hz. Q 1.94 does follow if R208 is dropped (R1‖R2 → 10 kΩ), but then f0 = 874 Hz. No simple misreading gives both numbers. The forum poster himself wrote "Not sure whether it's correct". The derivation is UNVERIFIED.)* This is the same pattern as the 808, where the quoted number was also wrong (808 clap BPF ≈ 1.27 kHz, Q 1.5 [808-02 §5.2]).

### 7.3 Burst path
- Input to the OTA: C56 22 nF / (R202 + R201) high-pass at 181 Hz, ÷40 [DERIVED].
- Output: the BA662 output current splits between R237 82 kΩ (to ground) and C58 1 nF into IC28a's virtual ground → **first-order high-pass, 1/(2π·82k·1n) = 1.94 kHz** [DERIVED]. It brightens the bursts (cf. the 808's C150/R378 1.59 kHz pole [808-02 §5.6]).
- [MEASURED] burst spectrum octave bands (125 Hz … 48 kHz) re max: −22 / −12 / **0 / 0** / −6 / −12 / −19 / −26 / −35 dB. The tail is −16 / −9 / **0** / −4 / −14 / −21 / −28 / −36 / −44. Centroid (0.1–20 kHz): **2.45 kHz (bursts), 1.42 kHz (tail)**.
  - [R05] quoted a centroid of ≈ 8 kHz: **CONFLICTING** by method; its band limits were likely wider (to 48 kHz, including the recording floor). Use the band table.
- Burst timing [MEASURED, R05; re-checked]: onsets **0, 9.8, 21.5, 31.4 ms** (±0.4 ms); the 4th merges into the tail.
- [MEASURED] Burst envelope (0.2 ms-smoothed Hilbert, mean of 19 hits): each burst jumps to within 1–3 dB of the peak and decays nearly linearly in dB, **≈ 25 dB in 8–9 ms (2.6–3.4 dB/ms, τ ≈ 2.6–3.3 ms)**, to ≈ −25 to −29 dB just before the next burst. After burst 4 the level holds at −8 to −13 dB (tail). That is consistent with an exponential converter driven by an exponentially recovering C65.
- [DERIVED] C62 0.027 µF × R229 1 MΩ = 27 ms gate timer, matching the ≈ 30 ms burst window (cf. the 808's 30 ms gate [808-02 §5.3]). The 9.8–11.7 ms burst period's RC is **UNVERIFIED** (candidates: C65 with R221 18 kΩ, 8.5 ms; or with R220 82 kΩ, 38.5 ms, the 808's 36 ms).
- **TM** (10 kΩ B, the only analog-voice trimmer) sets the Q38 offset, i.e. the OTA gain and how far the bursts close between pulses. Calibration: **2 Vpp at MULTI OUT with LEVEL centre** [SM p.11]. Whittle: with a Density pot in its place, "ACW there are almost no pulses and CW the pulses join to be one long pulse" [RW p.8]. A strong unit-variance point.

### 7.4 Reverb path [DERIVED]
- Filter: R204 1 kΩ + C57 0.47 µF → **low-pass 339 Hz unloaded, 440 Hz with the 3.35 kΩ load**, then R205 3.3 kΩ / R203 47 Ω (×0.014) into Q37. The tail is darker than the bursts. [MEASURED] above 1 kHz it falls ≈ 7–10 dB/oct against ≈ 6 dB/oct for the bursts.
- Envelope: D52 charges C59 0.1 µF; C59 → R233 470 kΩ → C61 0.1 µF (D53 clamps negative) → R234 470 kΩ → Q37's base. Equal-RC ladder (RC = 47 ms): eigen-τ **18 ms (rise) and 123 ms (decay)**, so the reverb **swells in** and peaks ≈ 25–30 ms. [MEASURED, R05] tail slope −96 dB/s (τ ≈ 90 ms); t(−20 dB) 77–85 ms. Same order; **UNVERIFIED** (what charges D52 and for how long).

### 7.5 Accent and level
- Accent enters only the burst generator (via IC26a/Q36 → R213/R218 → IC29). [MEASURED] span **+2.7 dB** (noisy) [R05]. HC shares its DAC with RS [R03].
- LEVEL VR20 50 kΩ (B) is the IC28a feedback; both paths sum there. **CONFIRMED.**

### 7.6 909 clap vs 808 clap
| | 808 [808-02 §5] | 909 (this file) |
|---|---|---|
| Noise | analog transistor noise (own circuit, trimmed) | shared 31-bit LFSR, **same stream as SD** (SD+CP correlate) |
| Band-pass | MFB ≈ 1.27 kHz, Q ≈ 1.5 (input RC tilt) | MFB **963 Hz, Q 2.13**, no input tilt |
| Burst path tilt | C150 1 nF + R378 100k into IC19 (pole 1.59 kHz) | R237 82k / C58 1 nF into IC28a (**1.94 kHz**) |
| Burst timing | 0 / 10.8 / 22.1 / 29.7 ms (drawing) | **0 / 9.8 / 21.5 / 31.4 ms** (measured) |
| Reverb filter | Q70 collector LP ≈ 884 Hz | R204/C57 LP **≈ 440 Hz** |
| Reverb envelope | C139 18n → 1M → C138 47n → 1M (≈ 12 / 71–84 ms) | C59 0.1µ → 470k → C61 0.1µ → 470k (≈ 18 / 123 ms derived; ≈ 90 ms measured) |
| Trim | TM3 offset, 6 Vpp / 0.5 Vpp | TM, **2 Vpp at MULTI OUT, LEVEL centre** |
| Level / accent | shared CP/MA LEVEL 100k(A) | own LEVEL 50k(B); accent DAC shared with RS |

---

## 8. Unit-to-unit variance points

| Voice | Point | Effect | Source |
|---|---|---|---|
| BD | **C9 0.22 µF (S/N < 381500) vs 0.33 µF** | sweep τ 4.5–19.6 ms instead of 6.8–29.4 ms | [SM p.7] CONFIRMED |
| BD | R27/R28/R26, R49/R50, op-amp V_sat | f_b (two units agree within 0.3 % [R05]; keep σ ≈ 0.5 %) | [DERIVED] |
| BD | D10/D11 match, triangle level | H3 is very sensitive (−25 to −33 dB over 4.3–4.9 V); H2 | [DERIVED] |
| BD | C7, Q3 β | hold length (≈ 47–62 ms) | [DERIVED/MEASURED] |
| BD | R36/R22/D8 clamp | maximum (accent-saturated) level | [DERIVED] |
| BD | R495 150 kΩ on S/N ≤ 371499 | negligible | [SM p.11] |
| SD | C69/C71 ratio | measured 1.500 (locked 3:2) vs nominal 1.471 *(corrected by fact-check: was 1.574)*; with a 3:2 lock, tolerance may not change the ratio at all | [MEASURED] |
| SD | C66, R247/R248/R249 | bend size and τ | [DERIVED] |
| SD | Q47/Q48 leakage, C73 residual | hiss / post-note fizz | [RW p.7] UNVERIFIED |
| SD | Q48 V_BE temperature drift | snappy cut-off time | [RW p.6] |
| Toms | VCO cap ratios (HT VCO3 +6 %) | partial ratios | [MEASURED] |
| Toms | **C54 0.0022 → 0.0047 µF, R194 47k → 100k (S/N 426700)** | tom-noise tick longer, steady noise lower | [SM p.7] CONFIRMED |
| Toms | C16/R60 slow pitch term | ≈ 7 % pitch fall from 150 to 600 ms; on the AR unit it is visible at full derived weight *(corrected by fact-check: was "not visible on the AR unit")* | [MEASURED] |
| RS | **R417 12k → 3.3k (S/N 415300)** | F3 weight ×3.6, "more realistic" | [SM p.7, p.9] CONFIRMED |
| HC | **TM trimmer** | burst density/level; the only analog-voice trim | [SM p.11], [RW p.8] |
| HC | R496 100 kΩ on S/N ≤ 371499 | accent offset | [SM p.11] UNVERIFIED |
| All | VCA transistors marked (S) | threshold 0.4–0.6 V → envelope end times | [SM p.11], [RW p.5–7] |
| All | Multi-out 1 kΩ/0.01 µF from S/N 415300 | 15.9 kHz roll-off | [SM p.7], [R03] |
| Noise | clock R184/C47 | inaudible (white to 20 kHz) | [DERIVED] |

Default to the **late configuration** (C9 0.33 µF, R417 3.3k, C54 0.0047 µF/R194 100k, output caps fitted), matching the AR unit's measured values. Offer an "early unit" switch. Keep variance as a seeded per-instance offset, never per-hit randomness [R05 §7].

---

## 9. Implementation checklist (per voice)

1. **Shared**:
   - Trigger produces the 2 ms +15 V pulse (VCO reset, pitch caps, comparator force) and the accent-gated pulse (V_acc − 1.2 V into the envelope caps).
   - All envelopes are RC states with exact exponential coefficients [R05 §2.7].
   - VCA = `g·max(0, V − 0.6)` with a ≈ 20 mV knee.
   - LEVEL is linear gain.
   - One noise generator (PSD-defined) feeds SD, BD, toms and HC.
2. **BD**: §3.8 table.
3. **SD**:
   - VCO-1 f1 = (117–231 Hz, linear in TUNE)·(1 + k·e^(−t/5 ms)) with k·f1 = 216 Hz; VCO-2 = **1.500** × VCO-1 once settled (≈ 1.55–1.58 during the first ≈ 40 ms), same bend; both reset to the same phase at the trigger end. *(Corrected by fact-check: was 113–226 Hz and 1.574×.)*
   - Diode-rounded triangles; VCAs τ 24 ms / 21 ms (VCO-2 −9 dB), scaled by accent.
   - Snappy (a): SK LP 8.49 kHz Q 1.94 → HP 1.52 kHz → VCA on ENV4 (24 ms hold; decay toward V_eq with τ = 0.47 µF·((100k + 500k·TONE)‖220k); hard cut at 0.6 V).
   - Snappy (b): SK LP → SK HP 4.05 kHz Q 1.31 → VCA on a 2 ms accent gate.
   - Sum × SNAPPY (linear).
4. **Toms**:
   - Three VCOs per tom (ratios 1 : 0.67 : 1.8–1.95), asymmetric-rail law with VSS = −(VDD + 3.0 V·e^(−t/22 ms) + 0.65 V·e^(−t/220 ms)), with f_s scaled to the measured 4–8 % below derived. *(Corrected by fact-check: was "+ slow term", fitted at 0–0.3 weight.)*
   - VCO1 overdriven into its diodes for ≈ 20–30 ms.
   - ENV1 (103 / 70 ms) + ENV2 share on VCO1; ENV2 (soft rise, DECAY-controlled) on VCO2; ENV3 (22–26 ms) on VCO3 + tom-noise.
   - Tom-noise: the loaded R190/C161 + SK HP network (broad peak ≈ 900 Hz, +1.6 dB; *corrected by fact-check: was "LP 1.54 kHz + SK HP 722 Hz Q 2.35"*), with a 0.47 ms tick and a steady component.
5. **RS**: kick (≈ 2.2 V, τ 0.8 ms) into three bridged-T resonators (f/Q above) → weighted sum (1, 1, 3.6) → hard diode clip ±0.6 V → VCA (τ 47 ms, accent) → level-dependent LP (220 pF) → SK HP 229 Hz Q 1.58, ×2.
6. **HC**:
   - Noise → MFB BPF 963 Hz Q 2.13.
   - (a) OTA × exp(burst sawtooth) with onsets 0/9.8/21.5/31.4 ms, per-burst ≈ 3 dB/ms decay → HP 1.94 kHz.
   - (b) LP 440 Hz → VCA on a two-pole swell (≈ 18 / 90–123 ms).
   - Sum; TM sets burst depth (calibrate to the AR envelope).

---

## 10. Open questions / UNVERIFIED

1. **SD absolute pitch −15 to −16 %** and VCO-2/VCO-1 = **1.500 (an exact 3:2 lock)** vs 1.471 *(corrected by fact-check: was −13 % and 1.574)*. Is the SD comparator divider or op-amp different from the toms' (the toms are 4–8 % low)? What couples VCO-2 into a 3:2 lock: the shared IC36 rails? A second unit's SD recording would settle it.
2. **BD f_b −12 %**: is the triangle peak really ≈ 4.9 V (op-amp swing, R49/R50)? A scope reading of IC13a's output would settle it.
3. **BD hold 62 ms measured vs ≈ 47–50 ms derived** (C7/R18/Q3 β); the BD ENV2 (attack VCA) source and time constant; and *(added by fact-check)* **BD t(−20 dB) ≈ 96 / 298 ms measured vs 79 / 248 ms derived**.
4. Tom ENV2 two-capacitor dynamics; exact DECAY law. *(Fact-check: the tom slow pitch term is no longer open. It is measured at the derived 0.645 V / 220 ms.)*
5. **HC burst generator**: which RC sets the 9.8–11.7 ms period, how the AN6912 comparators sequence, what charges D52 (reverb) and for how long. Tracing IC29's pins on a board, or a 909 clap clone schematic, would settle it.
6. **VCA distortion**: is H2 (BD −28 dB) from the saturated VCA transistor or the shaper diodes? Needs a transistor-level simulation of the (S) cell.
7. **Analog-voice levels in volts**: no table exists; the p.9 drawings give only ±30 % estimates. A calibrated multi-out recording of one unit (LEVEL max, accent off/on) would fix the absolute scale.
8. **Accent code mapping** (inherited from [R03]): normal/accent/TOTAL ACCENT DAC codes, needed to place V_acc in each envelope model.
9. **Noise clock** 0.3–0.46 MHz (inaudible; only for completeness).
10. **Trigger-to-sound latency** of the analog voices (reset held for ≤ 2 ms?). Onset-trimmed recordings cannot show it; a multi-voice recording with a common time base (e.g. BD + HH on the same step) would.

---

## Sources

- Roland, *TR-909 Service Notes*, 1st ed., 15 Jun 1984: https://archive.org/details/synth_Roland_TR-909_service_notes ; https://archive.org/details/roland_Roland_TR-909_Service_Manual (local: `.deps/sources/TR-909_service_notes_archive_jp2.zip`, `Roland_TR-909_service_notes_text.pdf`, `TR-909_SERVICE_NOTES_text.pdf`, `TR-909_Service_Manual.pdf`; voicing-board PNGs `TR-909-schematic-voicing-board-firstpr.png`, `…-BD-only-firstpr.png`)
- Roland, *TR-909 Owner's Manual*: https://static.roland.com/assets/media/pdf/TR-909_OM.pdf (local `.deps/sources/TR-909_OM.pdf`)
- R. Whittle, *TR-909 sound mods* (2018): https://www.firstpr.com.au/rwi/tr-909/ (local `.deps/sources/TR-909-Sound-Mods_RobinWhittle.pdf`)
- C. Fraser, *909 Sound Mods* (2004): http://www.colinfraser.com/tr909/909mods/909mods.htm
- KVR forum, "Emulating the TR-909(808) clap in detail": https://www.kvraudio.com/forum/viewtopic.php?p=8606994
- AudioRealism, *TR-909 Ultimate Sample Pack* (2025): https://audiorealism.se/TR-909-SamplePack.html (local only, not redistributable)
- mw909 research 02, 03, 05; mw808 research 01 (bridged-T formulas) and 02 (808 clap)

## Measurement and reading log (2026-10-03)

- Pages decoded from the JPEG2000 set (image n = printed page n+1). The p.11 voicing board was read in tiles at 1.0–3.0× native 8110 px (scratch `/tmp/claude-1000/mw909-research-01/c/`), section by section: BD, SD, LT, MT, HT, tom noise, noise generator, RS, HC.
- AR measurements: 96 kHz files, DC removed from the last 50 ms; zero-phase Butterworth filtering; nonlinear least squares (`scipy.optimize.least_squares`) for the SD two-VCO and tom three-VCO models (sine partials with exponential amplitudes and a shared pitch-envelope factor, phase-reset at t ≈ 0). Fit residuals 0.30–0.44 relative RMS (the partials are shaped triangles, not sines). TUNE-100 and DECAY-0 tom fits were unstable and are marked as such.
- No source code from any emulation was consulted or reproduced.

## Fact-check log

Independent check, 2026-10-03. The adversarial pass re-read every designator used below from the p.11 voicing-board scan. Scan file `Roland_TR-909_service_notes_0010.jp2`, 8110 × 5732 px. Crops were read at 1.0–2.5× in `/tmp/claude-1000/mw909-factcheck-01/`. Each figure was recomputed in `an/derive.py`, and the AR recordings were measured with `an/bd*.py`, `sd*.py`, `tom*.py`, `rshc.py`, `tomnoise.py` and `snapnet.py`. The text PDF was used for the p.6 and p.9 quotes, the p.7 scan for the change list, and the Whittle PDF for the [RW] quotes.

### Corrections

- **F1. BD t(−20 dB): CONFIRMED → CONFLICTING.**
  - Derived, recomputed and correct: τ = 0.33 µF·(R_d‖470k) = 14.1 / 107.0 ms. V∞ = 0.6·R_d/(R_d + 470k) = 0.055 / 0.414 V. t = 62 + τ·ln((2.63 − V∞)/(0.803 − V∞)) = 62 + 17.4 = **79.4 ms**, and 62 + 186.3 = **248.3 ms**.
  - Measured on the AR files (400 Hz LP, 20 ms Hilbert, 0 dB = 20–50 ms plateau): DECAY 0/25/50/75/100 → 91–101 / 141–145 / 171–176 / 203–212 / 298–299 ms, over TUNE 0/50/100.
  - A per-cycle Vpp track gives 288–297 ms at DECAY 100.
  - R05's 75 / 235 ms was not reproduced. The model is 17–22 % short.
  - Inverting the model on 298 ms would need R_d ≈ 1.9 MΩ (VR5 1.85 MΩ), so the VR5-taper inference (11/19/39/85 %) is now UNVERIFIED.
- **F2. SD VCO frequencies and ratio.**
  - Parabolic-interpolated spectral peaks, 50–250 ms, SNAPPY 0: f1 = 117.4 / 144.7 / 173.5 / 200.9 / 230.7 Hz and f2 = 176.3 / 217.1 / 260.2 / 301.3 / 346.1 Hz at TUNE 0–100.
  - **f2/f1 = 1.4998–1.5017**, identical at TONE 0/50/100, against the file's 1.573–1.576 (a 40 ms fit during the bend; sliding 30 ms windows show ≈ 1.55–1.58 before ≈ 40 ms).
  - Spectral lines sit at 0.5, 1, 1.5, 2, 3 × f1, which points to a 3:2 lock. The mechanism is UNVERIFIED.
  - Derived f1 = V_w/(4·6.75·47k·C69) = 98.5 / 147.8 / 197.0 Hz. Against the settled values that is −16.1 / −14.8 / −14.6 % (was −13 %).
  - Range 230.7/117.4 = 1.965.
- **F3. Tom VCO1 settled pitch and slow term.**
  - Derived f_s = 15·(10k + x·10k)/88k / (1.269·10⁶·C) = LT 61.1/91.6/122.1, MT 74.6/111.9/149.2, HT 89.5/134.3/179.1 Hz (correct).
  - Rail law f = f_s·2/(1 + VDD/|VSS|), with VSS = VDD + 3.02·e^(−t/22.1 ms) + 0.645·e^(−t/220 ms). The coefficients are 14.2 V·100k/470k and 14.2 V·100k/2.2M.
  - Fitted to zero-crossing f1(t) over 120–600 ms, the scale is A = 0.924–0.964, so asymptotic f_s = LT 57.0/85.8/117.2, MT 71.9/106.6/141.3, HT 82.8/125.4/166.7 Hz.
  - With the slow amplitude free: 0.57–0.66 V (8 of 9 settings).
  - The file's "≤ 4 % CONFIRMED" compared derived f_s with values read at 150–250 ms. Its "slow term not seen" is wrong; the term is CONFIRMED.
  - Derived k(0) with both terms = 0.518 / 0.417 / 0.350. The file's 0.47/0.37/0.31 is the fast term alone.
- **F4. SD bend peak.** V_B = (14.2·570k + 1.875·47k)/617k = 13.26 V (13.45 V at a 14.4 V pulse); the file had 12.6 V. Excess on node A = (13.26 − 1.875)·100/570 = 2.00 V (was 1.9 V). k at V_w 1.25/1.875/2.5 = 1.69 / 1.07 / 0.76 (was 1.6/1.0/0.7).
- **F5. Tom-noise filter.** The ideal SK HP gives f0 = 1/(2π·4.7n·√(10k·220k)) = 722 Hz and Q = 0.5·√22 = 2.35, which is correct as an ideal-source figure. Nodal analysis with the R190 22k / C161 4.7n source gives a peak at 904 Hz, +1.6 dB, and a −3 dB band of 625–1596 Hz. The resonance is mostly damped.
- **F6. HC: KVR explanation.** The file said KVR's 1140 Hz / Q 1.95 reproduces with "47k ‖ 150k". That reading gives 462 Hz, Q 1.02. With R1‖R2 → 10 kΩ the result is 874 Hz, Q 1.94. No simple reading gives both numbers. The thread (fetched) shows the poster's own "Not sure whether it's correct".
- **F7. RS kick amplitude.** (5 − 0.6)·22k/44k = 2.2 V (was ≈ 2.5 V).
- **F8. Snappy LP refinement** (not an error). The ideal SK gives 8.49 kHz, Q 1.936. With the 2.48 kΩ source and C85/R302 the response peaks at 7.43 kHz, +5.1 dB; the ideal filter peaks at 7.91 kHz, +6.0 dB.

### Confirmations

Schematic values were read independently; the arithmetic is from `derive.py`.

- **BD**
  - Comparator: R49 100k / R50 47k; the triangle goes into IC13b (−) and (+) is tapped on R49/R50. V_th = 13.5·47/147 = 4.316 V.
  - f_b = 1.5/(4·4.316·47k·0.033µ) = 56.0 Hz; with V_th = 4.9 V it is 49.3 Hz. Measured f_b = 49.3–49.9 Hz (DECAY 100), so the 56.0 Hz conflict stands.
  - Sweep τ = 0.33µ·(22k‖330k) = 6.81 ms and 0.33µ·(122k‖330k) = 29.39 ms. Measured: 6.9 / 11.3 / 28.6 ms at TUNE 0/50/100.
  - 0.22 µF (S/N < 381500, p.7 scan: "For expanding the TUNE range"): 4.54 / 19.60 ms.
  - Sweep depth: R28/R26 = 0.4545, and 49.5·0.4545·14.4/1.5 = 216 Hz. Measured Δf = 228 / 238 Hz at accent 0 / 100 (TUNE 100).
  - Clamp node 15·10/57 = 2.632 V; D8 runs anode X → cathode Y (read at 2.2×).
  - C7 τ = 15.5 ms.
  - Click SK LP (R7 100k, R16 39k, C3 4.7n, C4 100p): 3717 Hz, Q 3.08.
  - Noise divider 0.281. C15 HPF 25.8 Hz. C13 LPF 513 Hz (3.10 kΩ).
  - ATTACK: VR3 is a rheostat (read). Node N source 317 Ω, so −17.8 to −25.3 dB, a 7.5 dB range (the file says 7.7 dB; same within rounding).
  - ENV2 τ = 0.69 / 15.5 ms.
  - Hold: measured −1 dB point at 59–72 ms; the file's 61–63 ms is consistent.
- **SD**
  - TUNE wiper 1.250 / 1.875 / 2.500 V (R287 100k, VR6 10k, R284 10k).
  - C69 0.01 µF, C71 0.0068 µF, R261 / R264 47k.
  - Comparator: (+) on R262/R260 100k/100k (V_sat/2).
  - Bend τ = 0.01µ·570k = 5.70 ms; charge τ 0.434 ms.
  - ENV3 = 0.1µ·242k = 24.2 ms; ENV2 = 0.047µ·440k = 20.7 ms.
  - VCO-2 level −20·log(2247/1547) − 20·log(440/242) = −8.44 dB.
  - C80 HPF 1/(2π·4747·22n) = 1524 Hz. C85 HPF 47.7 Hz.
  - Path (b): SK HP C75 = C76 = 2.2n, R276 6.8k feedback, R277 47k ground → 4047 Hz, Q 1.315.
  - ENV4 τ 32.3–75.7 ms. C68 τ 6.8 ms. ENV5 τ 0.103 ms. C78 HPF 7.2 Hz.
- **Toms**
  - Caps read: LT C18 0.022 / C19 0.033 / C20 0.012 µF; MT C32 0.018 / C33 0.027 / C34 0.01 µF; HT C97 0.015 / C102 0.022 / C103 0.0082 µF.
  - VCO ratios 0.667 / 1.833, 0.667 / 1.800, 0.682 / 1.829. Measured VCO2/VCO1 0.674–0.686.
  - Pitch caps C16 0.1µ·R60 2.2M = 220 ms and C17 0.047µ·R61 470k = 22.1 ms.
  - Overdrive: C·1M·ln(14.4/0.6) = 31.8 / 21.6 / 17.8 ms.
  - ENV1 103.4 / 70.5 ms. ENV3 26.3 / 22.1 ms.
  - ENV2 rise 34.0 / 23.5 ms. C_A decay 38–378 / 26–261 ms. Drain 74.8 / 51.7 ms.
  - Tom-noise: LP 1539 Hz; C54 τ 0.22 / 0.47 ms; R194 gain −0.468 / −0.22. The S/N 426700 change was confirmed on the p.7 scan.
- **RS**
  - F1 (0.01µ, 2.2k, 470k): 494.9 Hz, Q 7.31, τ 4.70 ms. F2 (0.027µ, 330k): 218.8 Hz, Q 6.12, τ 8.91 ms. F3 (0.0047µ, 470k): 1053.1 Hz, Q 7.31, τ 2.21 ms.
  - Measured lines 222 / 481 / 1018 Hz plus 723, 1288 and 1976 Hz (10 shots).
  - Output HP (C121 = C122 = 0.01µ, 22k / 220k): 228.8 Hz, Q 1.58.
  - VR19 ‖ 220p: 7234 / 14469 Hz. C119 τ 47 ms.
  - R417 12k → 3.3k at S/N 415300 (p.7 scan).
- **HC**
  - MFB (read: R208 47k in, R209 10k to ground at the C42/C43 junction, R207 150k across, C 4.7 nF): f0 = 1/(2π·4.7n·√(150k·8.246k)) = 962.9 Hz, Q = 0.5·√(150/8.246) = 2.133, gain −1.596.
  - Burst HP 1/(2π·82k·1n) = 1941 Hz. OTA input HP 180.9 Hz.
  - Reverb LP 339 Hz unloaded, 440 Hz loaded.
  - Two-cap ladder eigen-τ 17.95 / 123.0 ms. Gate 27 ms. C65 with R221 / R220: 8.5 / 38.5 ms.
  - Measured burst onsets 0 / 10.3 / 21.9 / 31.8 ms (1 kHz-band envelope; within 0.5 ms of the file); burst fall ≈ 23 dB in 9 ms.
  - "ADJUSTMENT LEVEL: Center, MULTI OUT, 2Vp-p" was read beside TM.
- **Noise**
  - Read: two 4006s, XOR IC31c on IC32 pin 10 and IC33 pin 10, R187 47k, R184 10k, C47 100p.
  - x^(2^31−1) ≡ 1 mod (x^31 + x^13 + 1) was verified by computer, so the polynomial is primitive.
  - f_clk = 1/(2.2·10k·100p) = 454.5 kHz. PSD 2·7.5²/f_clk = 2.47·10⁻⁴ V²/Hz = 15.7 mV/√Hz. Period 78.7 min at that clock, 1.99 h at 300 kHz.
- **Common**
  - Accent buffer gain 1 + 10/4.7 = 3.128.
  - Multi-out RC 15.9 kHz.
  - The [SM p.6] quotes ("hytheresis", "32 stages", "about 20ms", "muting unwanted noises", "ENV 5 determines …") and the p.9 caption "Observed at MULTI OUT jack with all knobs set to center" were confirmed in the text PDF.
  - [RW] quotes (BD Tune, 24 ms, "only discharge path", "1/4 of the 15", "0.4 to 0.6 volts", "almost no pulses", "8.2k", "two 18 stage") were confirmed in the PDF.
  - The firstpr.com.au page is Whittle's mods page; it is now dated 20 May 2024, while the PDF is dated 10 Oct 2018.

### Not checked or still open

- The [CF] quotes; the live site fails TLS, and no local copy was read.
- The SD bend size of 216 Hz was not re-measured.
- The snappy TONE cut-off times were not re-measured.
- The p.9 pixel Vpp estimates were not re-measured.
- New observation: [SM p.6] says the D62 pulse turns IC37a's output **low**, but on the schematic R275 enters the node drawn as (+). This is a minor CONFLICTING detail with no effect on the numbers.
