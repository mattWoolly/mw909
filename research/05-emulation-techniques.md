<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->
# 05: How TR-909 voices are emulated, how to validate them, and the reference recordings

Scope: modelling technique for every TR-909 voice type (switched-integrator triangle VCOs, pitch envelopes, the BD waveshaper, the shared digital noise source, RC envelopes and transistor VCAs, the clap burst generator), the 6-bit PCM chain (DAC, zero-order hold at a variable clock, reconstruction filter, companding and VCAs) and how to resample it without aliasing, oversampling and CPU cost, analog variance, validation, and the reference recordings. Circuit-level detail of each voice lives in 01 (analog voices) and 02 (sample voices); this file covers the DSP side and adds my own measurements.

Tags:
- **CONFIRMED**: primary source (the 1984 service notes) or two independent good sources agree.
- **UNVERIFIED**: single secondary source, or my reading of a hard-to-read schematic region.
- **CONFLICTING**: sources disagree; both values given.
- **[DERIVED]**: my own algebra from component values (formula and parts shown) or my own simulation.
- **[MEASURED]**: numbers I measured in this session from the reference recordings in §9 with throwaway scripts in `/tmp/claude-1000/mw909-research-emu/`. Unless stated otherwise the source is the AudioRealism 2025 pack ("AR"): one unit, mono, **96 kHz / 24-bit**, individual outputs, part level 50 %, onset-trimmed.

Source keys:

| key | source | URL |
|---|---|---|
| [SM] | Roland, *TR-909 Service Notes*, 1st ed., 15 Jun 1984. p.5 "Accent & Trig" and "Digital Voice Generators", p.6 "Analog Voice Generators" and "Noise", p.9 scope figures ("Observed at MULTI OUT jack with all knobs set to center"), p.11 voicing-board schematic (VG 73133090). Read from the 600 dpi scans. Local: `.deps/sources/TR-909_Service_Manual.pdf`, `TR-909_service_notes_archive_jp2.zip` | https://archive.org/details/synth_Roland_TR-909_service_notes ; https://archive.org/details/roland_Roland_TR-909_Service_Manual |
| [FRA] | Colin Fraser, 909 clone notes (ROMs, DAC, VCAs, envelopes) | http://www.colinfraser.com/tr909/my909.htm |
| [R02] | mw909 research 02 (sample voices) | `research/02-voices-sample.md` |
| [R03] | mw909 research 03 (system and sequencer) | `research/03-system-sequencer.md` |
| [AR] | AudioRealism "TR-909 Ultimate Sample Pack" (2025) | https://audiorealism.se/TR-909-SamplePack.html |
| [RR] | Rob Roy Recordings TR-909 sample set (1995) | http://machines.hyperreal.org/manufacturers/Roland/TR-909/samples/TR909.TXT |
| [W14] | Werner, Abel, Smith, *A Physically-Informed, Circuit-Bendable, Digital Model of the Roland TR-808 Bass Drum Circuit*, DAFx-14 | https://dafx14.fau.de/papers/dafx14_kurt_james_werner_a_physically_informed,_ci.pdf |
| [BLAMP] | Esqueda, Välimäki, Bilbao, *Rounding Corners with BLAMP*, DAFx-16 | https://dafx.de/paper-archive/details/xRM0GiuF-JmeN0_aaP9VNg |
| [MINBLEP] | Brandt, *Hard Sync Without Aliasing*, ICMC 2001 | https://www.cs.cmu.edu/~eli/papers/icmc01-hardsync.pdf |
| [JOS-RS] | J. O. Smith, *Digital Audio Resampling Home Page* (bandlimited interpolation) | https://ccrma.stanford.edu/~jos/resample/ |
| [ADAA] | Parker, Zavalishin, Le Bivic, *Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time Convolution*, DAFx-16 | https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA |
| [VAFD] | V. Zavalishin, *The Art of VA Filter Design* (TPT/ZDF) | https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf |
| [TT] | Tiptop Audio HATS909 manual (local `.deps/sources/Tiptop_HATS909_manual.pdf`) | https://www.tiptopaudio.com/manuals/Tiptop_Audio_HATS909_ns.pdf |
| [D16] | D16 Group, Drumazon 2 manual (local `.deps/sources/Drumazon_2-manual-gb.pdf`) | https://m.d16.pl/pub/manuals/Drumazon%202-manual-gb.pdf |

No source code from any project is reproduced here; open-source designs are described at the technique level only.

---

## 0. Design-critical facts (executive summary)

1. **The 909 is a hybrid with three kinds of signal source, and each needs a different discretisation.**
   - Analog voices use **switched-integrator triangle VCOs with a linear control voltage**, RC envelopes and transistor VCAs.
   - The shared noise is a **32-stage digital shift-register (LFSR) clocked at a few hundred kHz**.
   - HH, crash and ride are **6-bit PCM** from three 32 K mask ROMs, played at an RC-oscillator clock.
   - [SM p.5, p.6] **CONFIRMED.**
2. **Every analog VCO is phase-reset by the trigger. This is a circuit mechanism, not a software reset:** [SM p.6] SD "Q44 … discharges C69 … resets VCO-1 to the starting point".
   - [MEASURED]: the first 4–10 ms of different hits correlate at 0.91–1.00 (BD, SD, toms).
   - Model the reset switch (integrator capacitor shorted for the trigger pulse). Keep every other state (envelope capacitors, filters, noise) running, so retriggers stay authentic.
3. **BD pitch envelope [MEASURED]:** for every knob setting, `f(t) = f_b + Δf·e^(−t/τ)` with an RMS fit error ≤ 0.4 Hz.
   - **f_b = 49.2–49.6 Hz**, **Δf ≈ 230–250 Hz**, so f(0) ≈ 280–295 Hz.
   - τ grows with TUNE: **τ = 7.6, 9.7, 11.8, 14.5, 29.1 ms** at TUNE 0/25/50/75/100 %.
   - [DERIVED] τ = C9·((R57 + VR2)‖R26) = 0.33 µF·(22 k‖330 k) = **6.8 ms** at minimum and 0.33 µF·(122 k‖330 k) = **29.4 ms** at maximum. Both match.
   - **TUNE sets the sweep time, not the base pitch.**
   - A second unit [RR] settles at 49.2–49.6 Hz too.
4. **BD amplitude:**
   - **A ≈ 55–65 ms hold**: flat within 0.5 dB, independent of DECAY.
   - Then a decay that steepens in dB as it nears zero, a sign of a thresholded VCA.
   - [MEASURED] time to −20 dB: **75–86 ms (DECAY 0), 115–121, 135–158, 174–179, 235–238 ms (DECAY 100)**, over TUNE 0/50/100.
5. **BD waveshaper:**
   - The triangle is shaped **before** the VCA, by the anti-parallel diodes D10/D11 behind R52 = 10 k [SM p.11].
   - [MEASURED] the harmonic signature is **independent of output level over 20 dB**: H2 −28, H3 −25.7, H4 −44.6, H5 −41, H6 −46.5, H7 −41 dB re H1. A plain triangle would have H3 at −19.1 dB and no even harmonics.
   - Implement it as a memoryless curve on a fixed-amplitude triangle, then the VCA.
6. **Triangle VCOs need no oversampling.**
   - [DERIVED by simulation] A naive phase-accumulator triangle at 48 kHz has an alias error of −88 dB at 49 Hz, −65 dB at 291 Hz, and −76 dB over the whole BD sweep. Soft-shaped, it reaches −81 to −102 dB.
   - Add a 2-point polyBLAMP at the corners [BLAMP] and a polyBLEP at the reset discontinuity. That removes the last of it for about 0.03 % of a core per VCO.
7. **Noise:**
   - It is an LFSR clocked at about 0.3–0.46 MHz [DERIVED: 4070 XOR RC oscillator, R184 = 10 k, C47 = 100 pF]. Across the audio band it is white noise, flat within 0.1 dB to 20 kHz.
   - Define it **by spectral density**: per-sample σ ∝ √fs. Do not use unit RMS at the host rate.
   - SD, BD (attack), toms and clap share this one source [SM p.6].
8. **PCM chain facts:**
   - ROM: 32768 × 6 bit each [SM][FRA][R02]. Data are **companded**, with the envelope restored by analog VCAs [SM p.5][FRA].
   - Reconstruction filter (two unity-gain Sallen-Key stages): 25.9 kHz/Q 0.55 and 27.7 kHz/Q 1.32, a near-Butterworth 4-pole. **−3 dB at 27 kHz; −0.56 dB at 20 kHz** [DERIVED].
   - **It does not remove the first image band.**
   - [MEASURED] the 909 output carries strong ZOH images. The 24–48 kHz band of the hats sits only **−20 dB below the strongest band**.
9. **Sample clocks [MEASURED from the exact ZOH sinc null, 63–77 dB deep, cross-checked against ROM playback length]:**
   - **HH 31.8–32.0 kHz.** That is CONFLICTING with the "about 60 kHz ÷ 2 = 30 kHz" of [SM p.5]; this unit is 6 % fast.
   - **Crash/ride 28.3 kHz (TUNE 0) → 37.0 kHz (50 %) → ≈ 56.6 kHz (100 %).** These fit `f_clk ≈ 56.6 kHz / (2 − k)`, where k = knob 0..1: within 2 % from 30 to 80 %, 3–4 % at 10–20 %, and the 90 % point (from playback length) is ≈ 7 % above it.
   - At maximum tune the clock is **above 48 kHz**, so the resampler must also decimate.
   - [R02]'s derived ratio of 2.47:1 does not match the measured 2.0:1 (CONFLICTING; §5.1).
10. **Resample the PCM chain with band-limited step insertion (BLEP) at exact sub-sample edge times. Do not use naive oversampling.**
    - [DERIVED by simulation, 9 kHz tone at f_clk = 31.9 kHz]:
      - Naive sample-and-hold at 1× folds the 40.9 kHz image to 7.1 kHz at **−13 dB**. At 4× it is still −58 dB, because edge-time quantisation leaves a jitter floor.
      - A 32-tap BLEP residual table (256 phases) holds every fold-back alias below **−100 dB**.
    - [MEASURED in C on the target Xeon]: 0.16–0.27 % of one core per PCM voice, scalar.
11. **Accent [MEASURED]:**
    - TOTAL ACCENT from 0 to 100 % raises Vpp by **+6.2 dB (BD), +4.8 (SD), +7.3–7.7 (toms), +3.9 (RS), +6.8 (CH), +7.2 (OH), +7.9 (CR), +7.7 (RD)**.
    - The curve is flat from 0 to 10 % and roughly linear in dB above that.
    - A hit with accent and total accent at 0 matches the no-accent hit within 0.5 dB for most voices (crash: 1.2 dB).
12. **Clap [MEASURED, 19 hits]:**
    - Bursts start at **0, 9.8, 21.5 and 31.4 ms (±0.4 ms)**, so the spacing is 9.8 / 11.7 / 9.9 ms. The reverb tail follows.
    - Tail decay τ ≈ 90 ms (−96 dB/s). Spectral peak **≈ 0.95–1.05 kHz**, centroid ≈ 8 kHz.
13. **Sequencer by-products [MEASURED, cross-check of R03]:**
    - Shuffle n (1..7) delays the even 16ths by **(n−1)·2/96 beat**; measured 0.00, 2.01, 4.04, 6.04, 7.88, 9.95, 11.89 ticks.
    - Internal-clock jitter is **1.0–1.3 ms RMS, up to ±3.6 ms**.
    - The flam second hit comes **≈ 20–47 ms** after the first, which supports the 20–48 ms flam table in R03's CONFLICTING item.
14. **Reference recordings:**
    - **No CC0 one-shot recording of a real 909 exists** that I could find (search log §9.1).
    - The best material is the AR pack: free for personal and commercial use, **no redistribution**. It holds 848 files at 96 k/24, with BD/SD 5×5×5 grids, tom 11×11 grids, cymbal tune sweeps, 11 accent levels per voice, flams and shuffles.
    - A second unit, [RR] 1995, is free for non-commercial use only and normalised.
    - Both are in `.deps/ref-samples/` for local tests only, with LICENSE files.

---

## 1. Architecture for an emulator

| block | 909 implementation | emulation technique (recommended) | rate |
|---|---|---|---|
| Triangle VCO (BD, SD×2, each tom ×2–3) | op-amp integrator + hysteresis comparator, capacitor reset by a transistor switch on trigger [SM p.6, p.11] | phase accumulator with explicit reset state; polyBLAMP corners; CV linear in Hz | 1× |
| Pitch envelope | RC discharge (BD: C9 via R57+VR2 ‖ R26; SD: ENV-1 changing CMOS buffer supply for ≈ 20 ms [SM p.6]) | exact one-pole `e^(−1/(τ·fs))` | 1× |
| Waveshaper (BD) | R52 10 k into anti-parallel D10/D11 [SM p.11] | memoryless curve (table or closed form), ADAA optional | 1× |
| VCA (analog voices) | single transistors (Q12 BD, Q50/Q47 SD, Q6 BD attack) gated by RC envelopes [SM p.6, p.11] | gain = g(V_env) with a cut-off threshold; smooth knee | 1× |
| Hold generators | RC timers (BD C7 0.033 µ with R18/R17 470 k each; three time annotations "T1≈2 ms, T2≈4.5 ms, T3≈1 ms" on the BD section) [SM p.11] | comparator on an RC state, smooth crossing | 1× |
| Noise | two 4006 in cascade = 32 stages, XOR feedback, clocked by a 4070 RC oscillator [SM p.6, p.11] | white PRNG scaled by √fs (PSD-defined), shared, seeded | 1× |
| Clap burst | separate burst/envelope logic around IC29 (see 01) | burst timer + RC envelopes on filtered shared noise | 1× |
| PCM voices | counter → 32 K ROM → 6-bit latch (4174) → resistor DAC (RM0621) → VCA → 4-pole LPF [SM p.5, p.11] | ROM image (own data, §5.7) → per-unit DAC LUT → exact-edge BLEP resampler with LPF folded into the kernel → VCA model | events at f_clk, output at fs |
| Cymbal envelope | 6 MSBs of the address → second RM0621 → anti-log transistor → VCA [SM p.5] | 64-step staircase in log domain smoothed by τ = R432·C126 = 270 k·1 nF = 0.27 ms [DERIVED] | per step |

What the commercial 909 plugins disclose is thin: D16 mentions only a "global quality setting" that affects CPU [D16 p.2]. The Tiptop HATS909 replaces the hat sample-rate generator with a VCO, and its 909 mode passes the sample through "the original analog filters and VCA" [TT]. See 04 for the product survey; none of them publishes its algorithm.

---

## 2. Analog voices

### 2.1 The VCO: switched integrator + comparator (all analog voices)

- [SM p.6], SD as the representative circuit: "VCO-1 comprises a hysteresis comparator IC37a, inverting buffers configured as voltage-dependent resistor (in IC36) and an integrator consisting of IC37b and C69 with Q44 switcher. In this arrangement VCO-1 generates triangle waveform." **CONFIRMED.**
  - "SNARE, BASS, TOM TOM operate basically in the same manner and share the same Noise Generator." **CONFIRMED.**
  - BD: integrator IC13a with C14 = 0.033 µF and switch Q11; comparator IC13b with R49 = 100 k / R50 = 47 k hysteresis [SM p.11].
- **Frequency is linear in the control voltage.** The integrator slope is ∝ V_cv/R, so f ∝ V_cv.
  - Evidence: the BD trajectory fits `f_b + Δf·e^(−t/τ)` exactly (§2.2). An exponential converter driven by an RC envelope would give an exponential-of-exponential, which does not fit.
  - So model the VCO as `phase += (f_b + k·V_env(t))/fs`, not `f = f0·2^(V)`.
- **Trigger reset** (SD, [SM p.6]):
  - "a) one input of IC37a via D62 … IC37a turns its output to low. b) The base of Q44 which discharges C69, canceling VCO-1 output. The combination of a) and b) resets VCO-1 to the starting point at which VCO-2 also starts oscillation, phasing the initial waveforms of both VCOs." **CONFIRMED.**
  - Implementation: while the trigger is high (2 ms [SM p.5]), hold the integrator at its reset value and force the comparator state; on release, start ramping. Both the output discontinuity and the end of the clamp are band-limited with polyBLEP/polyBLAMP.
  - [MEASURED] start repeatability, measured as the correlation of the first 4–10 ms between hits with different knob settings: BD 0.91–1.00, SD 0.999–1.000, LT 0.67–1.00 (the lower value is the decay-100 file, where the envelope differs).
- **SD pitch bend:**
  - Mechanism [SM p.6]: "VCO-1 also sees the effects of trigger pulse from Q40 at VDD and VSS terminals of buffer IC36 through the control voltage generator … the charging rate of C69 also continuously changes for about 20 ms. The resultant effect is a pitch bend." The figure shows VDD/VSS jumping from ±2 V to ±5 V in 2 ms, then relaxing back over ≈ 20 ms.
  - So the snare's "VCO CV" is the CMOS buffer supply: charging current ∝ (V_supply − V_threshold)/R_on. Model it as f = f_b·(1 + a·e^(−t/τ)) with τ ≈ 6–8 ms (20 ms ≈ 3τ).
  - [MEASURED] (snappy 0, VCO-1 band, zero crossings): TUNE 0: 133 → 117 Hz; TUNE 50: 196 → 171–176 Hz; TUNE 100: 272 → 227–235 Hz. The bend settles within 20–25 ms and its ratio is ≈ 1.13–1.18.

**Discretisation.** Use a phase accumulator in double precision. Derive the triangle from it, `4·|φ − ⌊φ + ½⌋| − 1`, and apply a 2-point polyBLAMP (polynomial integral of the polyBLEP residual, [BLAMP]) at each corner: the slope changes by ±8·f/fs per corner.

[DERIVED by simulation] Alias-error energy relative to a 64× oversampled, brick-wall-decimated reference (`tri2.py`):

| f0 | 44.1 kHz naive | 48 kHz naive | 48 kHz, after tanh(2x) | 48 kHz, after hard clip (1.6x) |
|---|---|---|---|---|
| 49.4 Hz | −82 dB | −88 dB | −102 dB | −90 dB |
| 113 Hz | −76 | −78 | −93 | −79 |
| 208 Hz | −69 | −70 | −89 | −71 |
| 291 Hz | −64 | −65 | −81 | −67 |
| 504 Hz | −57 | −58 | −77 | −60 |
| BD sweep 289 → 49 Hz | −76 | −76 | −78 | −78 |

Conclusion: 1× is enough for every 909 analog VCO. polyBLAMP is cheap insurance for the SD and the high tom settings. The **reset** step (SD: output cancelled while C69 is shorted) is a value discontinuity and needs polyBLEP, not polyBLAMP.

### 2.2 Bass drum

**Pitch envelope (ENV5)** [SM p.11, BD block]:
- Q1 charges C9 = 0.33 µF through D7 + R21 = 2.2 k on the trigger.
- C9 discharges through R57 = 22 k + VR2 = 100 k(A) TUNE, loaded by R26 = 330 k into the summing amp IC12a. IC12a also has R27 = 1.5 M to the rail (the base-pitch offset) and R28 = 150 k feedback.
- IC12b/Q10 then set the integrator current.

[MEASURED] Least-squares fit of `f(t) = f_b + Δf·e^(−t/τ)` to per-cycle zero-crossing frequencies, t = 3–350 ms, ATTACK 0 and 100, DECAY 100:

| TUNE | f_b | Δf | τ | f(0) | RMS fit error |
|---|---|---|---|---|---|
| 0 % | 49.2 Hz | 247 Hz | 7.6 ms | 296 Hz | 0.1 Hz |
| 25 % | 49.3 | 245 | 9.7 | 294 | 0.1 |
| 50 % | 49.4 | 238 | 11.8 | 287 | 0.1 |
| 75 % | 49.4 | 235 | 14.5 | 284 | 0.1 |
| 100 % | 49.5 | 229 | 29.1 | 279 | 0.2 |

- ATTACK does not change the pitch fit: the ATTACK 100 fits agree within 2 %.
- [DERIVED] Time constant of the discharge path, `τ = C9·((R57 + VR2·x)‖R26)`:
  - x = 0: 0.33 µF × (22 k‖330 k = 20.6 k) = **6.8 ms**, against 7.6 measured.
  - x = 1: 0.33 µF × (122 k‖330 k = 89.1 k) = **29.4 ms**, against 29.1 measured.
- Implied VR2 resistance from the measured τ, `VR2 = 1/(1/(τ/C9) − 1/R26) − R57`: 0 %: 2.8 k (end resistance plus tolerance); 25 %: 10.3 k (10 %); 50 %: 18.1 k (18 %); 75 %: 28.7 k (29 %); 100 %: 98.3 k [DERIVED]. That is a log-type taper, milder than the textbook 10–15 %-at-centre A curve, consistent with the "(A)" marking on VR2.
- **The base pitch barely depends on TUNE** (49.2 → 49.6 Hz): only the R26 loading shifts the DC operating point.
- [MEASURED, second unit] [RR] BD at TUNE 0/3/7/10 settles at 49.23–49.27, 49.30–49.32, 49.43–49.48 and 49.61–49.62 Hz. The two units agree within 0.3 %.
- The service notes' own scope figure (BD with accent, 5 ms/div) shows the first cycle at roughly 2–3 ms with periods lengthening to about 8 ms by the fifth cycle, in line with the fit [SM p.9]. That is a qualitative read of a drawing.

**Amplitude envelope (ENV1 + hold)** [SM p.11]:
- C8 = 0.33 µF is charged by D9/R35 = 1.8 k and discharged by VR5 = 1 M(A) DECAY + R58 = 47 k, with R54 = 470 k and Q12 on the node.
- A hold timer (Q3/Q4/Q5 with C7 = 0.033 µF, R18 = R17 = 470 k) keeps the envelope up after the 2 ms trigger.
- [DERIVED] C7 time constant ≈ 0.033 µF × 470 k = 15.5 ms per resistor. The circuit-level hold length is for 01 to pin down.
- [MEASURED] (5 ms RMS, first crossing after the peak; TUNE 50, ATTACK 0):

| DECAY | −3 dB | −10 dB | −20 dB | −30 dB | −40 dB |
|---|---|---|---|---|---|
| 0 % | 43 ms | 73 | 75 | 132 | 152 |
| 25 % | 43 | 74 | 115 | 156 | 158 |
| 50 % | 43 | 93 | 135 | 183 | 215 |
| 75 % | 44 | 95 | 175 | 236 | 287 |
| 100 % | 43 | 114 | 235 | 336 | 429 |

  - The −3 dB at 43 ms (23 ms at TUNE 0, 62–68 ms at TUNE 100) comes from the waveform change as the pitch drops, not the envelope.
  - Per-cycle Vpp stays within 0.5 dB for the first 54 ms at every DECAY setting.
  - At DECAY 100 the per-cycle level falls 1.6 dB per 20 ms at first and 3.7 dB per 20 ms near −45 dB. This accelerating decay in dB is the signature of `out ∝ max(0, V_env − V_th)` with an exponential V_env.
  - A pure exponential after the hold fits worse: 0.66–1.1 dB RMS against 0.25–0.61 dB for hold + thresholded exponential, at DECAY ≥ 50 % (one outlier, TUNE 0 / DECAY 75, fits both at 1.96 dB).
- [DERIVED] DECAY range from C8: at minimum 0.33 µF × (47 k‖470 k) = 14 ms; at maximum 0.33 µF × (1.047 M‖470 k) = 107 ms. The fitted τ at DECAY 100 is 117–125 ms with threshold k ≈ 0.03, so the maximum agrees within 15 %. Whether R54 really loads C8 is **UNVERIFIED** (my reading of p.11).

**Waveshaper.**
- R52 = 10 k feeds D10/D11 (anti-parallel to ground), then R53 = 2.2 k / R43 = 47 Ω into the Q12 VCA [SM p.11].
- [MEASURED] Harmonics re H1 of the settled tone (49 Hz), TUNE 50 and TUNE 0, DECAY 100, in four windows from 60 to 400 ms while the level falls from −17 to −38 dBFS:

| H2 | H3 | H4 | H5 | H6 | H7 |
|---|---|---|---|---|---|
| −28.1 ± 0.4 | −25.7 ± 0.2 | −44.6 ± 0.5 | −41.0 ± 0.2 | −46.5 ± 0.3 | −40.9 ± 0.1 |

- **The shaping is level-independent, so it comes before the VCA.**
  - A pure triangle would be H3 −19.1, H5 −27.9, H7 −33.8 dB with no even harmonics.
  - H2 at −28 dB means the shaper is asymmetric: diode mismatch or a DC offset on the triangle.
  - The whole hit is asymmetric too: positive/negative peak ratio 0.45–0.64 [MEASURED].
- Implementation:
  - Use a static curve `y = s(x)` on the normalised triangle, fitted to these six numbers. A diode-pair form `y = V_T·asinh(x/(2·I_s·R))`-like, or a tanh with an offset, are good starting shapes.
  - Fit the asymmetry with a small DC offset before the shaper.
  - The input is band-limited and low-frequency, so ADAA [ADAA] is optional.

**Attack (click).**
- PULSE (Q9 / C11 0.47 µF / R8 10 k / R44 22 k) and the shared NOISE (via C15 0.47 µF, R46 4.7 k, C13 0.1 µF, R56 330 Ω) are mixed by VR3 = 500 Ω(B) ATTACK into the Q6 VCA, gated by ENV2 (C12 = 0.033 µF, R41 22 k: τ ≈ 0.73 ms [DERIVED]) [SM p.11].
- [MEASURED] ATTACK 0 → 100 raises the first-2 ms peak from −14.7 to −7.7 dBFS (+7 dB) and the >1 kHz energy in the first 5 ms by +8 dB. Overall Vpp rises by 2.7 dB.
- The click is a short, noise-bearing transient summed with the VCO path. It does not change pitch.

**Recipe.**
1. Trigger (2 ms) → reset the VCO integrator; charge C9 (pitch) and C8 (amplitude); start the hold.
2. `f = f_b + K·V_C9`; triangle with polyBLAMP; asymmetric static shaper.
3. VCA `g = max(0, V_C8 − V_th)^p` with p ≈ 1 and a smooth knee; hold from the C7 timer.
4. Add the attack transient (pulse + shared noise × ENV2) scaled by ATTACK.
5. Output stage. Accent scales the trigger/envelope voltages, not the output gain: [MEASURED] the waveform asymmetry changes with accent (0.45 at accent 0 against 0.49 at full accent; TUNE/ATTACK/DECAY 100).

### 2.3 Snare drum

Structure [SM p.6] **CONFIRMED**:
- DRUM = VCO-1 + VCO-2 (C69 ≠ C71, "VCO-1 runs at lower frequency"). VCO-1 passes through VCA Q50 on ENV3; the amount is set by ACCENT through Q4.
- SNAPPY = shared noise. "ENV 5 determines the amount of high frequency noise components in the SNAPPY which becomes articulate when noises passing through a high pass filter (IC39a and associated RC's) are combined with the noises from the low pass filter at IC39b."

[MEASURED] Settled spectral peaks, 20–80 ms, snappy 0, tone 0:

| TUNE | VCO-1 | VCO-2 | ratio | early (0–12 ms) VCO-1 |
|---|---|---|---|---|
| 0 % | 114 Hz | 180 Hz | 1.58 | ≈ 133 Hz |
| 25 % | 141 | 221 | 1.57 | — |
| 50 % | 171 | 262 | 1.53 | ≈ 196 Hz |
| 75 % | 199 | 302 | 1.52 | — |
| 100 % | 230 | 346 | 1.50 | ≈ 272 Hz |

[MEASURED] SNAPPY sweep (TUNE 50, TONE 50):

| SNAPPY | Vpp | −10 / −20 / −30 / −40 dB | centroid 0–50 ms | centroid 100–200 ms |
|---|---|---|---|---|
| 0 % | −4.8 dBFS | 27 / 72 / 117 / 141 ms | 1.09 kHz | 3.98 kHz |
| 50 % | −2.9 | 29 / 90 / 152 / 200 | 5.11 kHz | 8.30 kHz |
| 100 % | −1.7 | 41 / 120 / 176 / 217 | 6.46 kHz | 8.43 kHz |

[MEASURED] TONE sweep (SNAPPY 100):
- TONE lengthens the noise decay: t(−20 dB) = 78, 110, 120, 140, 143 ms for TONE 0/25/50/75/100.
- Low-band (200–1000 Hz) noise content falls from −7 to −17 dB re the strongest band (6–12 kHz).
- The 12–20 kHz band sits at −12 dB and the 20–48 kHz band at −23 dB. The snappy noise is broadband well past 20 kHz, so it cannot be a narrow band-pass.

Technique: two reset-synchronised triangle VCOs (§2.1) with the supply-modulated bend, each through its own VCA, plus shared noise into an HPF/LPF pair with VCA'd mixing. Use TPT SVF / one-pole sections [VAFD] at 1×. Every filter corner lies far below fs/2 (circuit values in 01).

### 2.4 Toms

[MEASURED] Spectral peaks over time (DECAY 100; strongest first):

| voice | TUNE | 0–25 ms | 25–60 ms | 120–250 ms (settled) | weaker partials (settled) |
|---|---|---|---|---|---|
| LT | 0 % | 75 Hz | 64 | 62 | ≈ 0.68× / 3× |
| LT | 50 % | 115 | 105 | 90 | 61 (0.68×), 270 (3×) |
| LT | 100 % | 151 | 130 | 122 | 83 (0.68×), 366 (3×) |
| MT | 0 % | 105 | 86 | 77 | 52, 232 |
| MT | 50 % | 146 | 123 | 112 | 76, 337 |
| MT | 100 % | 180 | 160 | 147 | 99, 441 |
| HT | 0 % | 122 | 109 | 89 | 60, 267 |
| HT | 50 % | 166 | 145 | 132 | 89, 396 |
| HT | 100 % | 208 | 189 | 173 | 117, 520 |

Reading:
- A dominant triangle VCO sweeps down by 1.2–1.35× over about 60 ms. Its 3rd harmonic sits at −17 dB, close to a triangle's −19 dB.
- A second oscillator sits at **0.67–0.68×** the dominant frequency, 4–5 dB down.
- The schematic labels VCO1, VCO2 and VCO3 per tom [SM p.11], plus a TOM NOISE block, and [SM p.9] marks a "noise" spike at the LT onset.
- Level and DECAY [MEASURED]: t(−20 dB) at TUNE 50 is LT 165 / 210 / 214 ms, MT 103 / 145 / 139 ms and HT 109 / 139 / 161 ms for DECAY 0 / 50 / 100.

Technique: the same VCO machinery as the SD (shared tom-noise source, per-tom VCOs at fixed ratios, RC pitch envelope). Exact ratios and circuit values come from 01.

### 2.5 Rim shot

[MEASURED] (accent 0 / 50 / 100):
- Spectral peaks at **223, 480, 728, 1018, 1292 Hz**, the same at every accent.
- Time to −10 / −20 / −30 dB: 9.6 / 17 / 21.7 ms (accent 0); 9.7 / 21 / 26 ms (accent 100).
- Vpp −6.2 / −4.0 / −2.2 dBFS.
- The schematic shows three filter blocks F1–F3 [SM p.11], and [SM p.9] shows how the RS waveform depends on R417 (3.3 k against 12 k).
- Technique: impulse-excited resonant band-passes (TPT SVF) at 1×. The non-harmonic partials fit the ≈ 223/480/1018 Hz trio.

### 2.6 Hand clap

[MEASURED] Burst onsets, the rise of a 0.4 ms RMS envelope on 0.5–5 kHz band-passed audio, over 11 accent levels and 8 single shots (19 hits):

| burst | onset (ms) | spread |
|---|---|---|
| 1 | 0 (trim point) | — |
| 2 | 9.8 | 9.4–10.2 |
| 3 | 21.5 | 21.2–21.8 |
| 4 | 31.4 | 30.9–31.8 (merges into the tail) |

- The spacing is **9.8, 11.7, 9.9 ms**. This deterministic pattern repeats on every hit, so the burst clock is not free-running.
- After the fourth burst comes the "reverb" tail. 1 ms envelope: −16 dB at 60 ms, −26 at 100, −34 at 140, −44 at 200, −47 at 300 ms. The slope from 50 to 300 ms is −96 dB/s, **τ ≈ 90 ms**.
- Time to −20 dB is 77–85 ms, to −30 dB 143–151 ms.
- Spectrum (30–150 ms): peaks at **0.92–1.05 kHz** and centroid ≈ 8.0 kHz. There is a band-pass near 1 kHz plus a broadband high-frequency component.
- Level: accent changes the Vpp by only 2.7 dB (noisy; the clap is noise-driven).
- [SM p.9] scope (10 ms/div): four sharp spikes, then a decaying tail. That agrees.
- Technique: shared noise → band-pass ≈ 1 kHz (and a parallel HF path; values in 01) → VCA driven by a burst envelope (4 fast attack/decay pulses at the measured onsets) plus a tail envelope (τ ≈ 90 ms).
  - Use RC-shaped (exponential) bursts.
  - Hold burst times to ±0.5 ms per unit; unit variance belongs in the RC values, not in per-hit randomness.

### 2.7 Envelopes and VCAs: how to discretise them

- **RC charge/discharge (every 909 envelope):** `v[n+1] = v_target + (v[n] − v_target)·e^(−1/(τ·fs))`.
  - Compute the coefficient exactly in prepare or on parameter change. Do not use the Euler form 1 − 1/(τ·fs): it is wrong by 1–2 % at τ = 0.73 ms at 44.1 kHz.
  - Diode charge paths are asymmetric τs. The fast charge through a small R (BD: 2.2 k into 0.33 µF, τ = 0.73 ms) must be modelled, because it shapes the click.
- **Sub-sample trigger timing:** start envelopes at the fractional event position (one-pole: pre-advance by the fraction), so timing does not depend on the host rate.
- **Hold timers:** compare an RC state to a threshold. Do not use a sample counter: the RC version is rate-independent and keeps the circuit's dependence on accent voltage.
- **Transistor VCAs:**
  - The analog voices use single transistors driven at the base or emitter by the envelope. Model them as `g = softplus_k(V_env − V_th)` (linear above a soft threshold).
  - [MEASURED] evidence: the BD thresholded tail (§2.2), and the hold-then-fall shapes.
  - The PCM voices use a 2SA798 differential pair (Q84 hat) and "anti-log" transistors Q70/Q68 (cymbals) [SM p.5, p.11]. Those are exponential in the control voltage, so their dB decay is linear in V.
- **Exponential vs. linear decay in dB:** RC + exponential VCA gives a linear dB slope only if V decays linearly. RC + linear VCA gives a linear dB slope only above threshold. The measured curves (§2.2, §10) fix which applies to each voice. Fit `V_th` per voice, not one global value.

---

## 3. The shared noise source

- [SM p.6]: "a quasi-random noise generator having two shift registers (IC32, IC33) connected in cascade making up 32 stages … Two Ex-OR gates of IC31 clock the shift registers at a higher frequency, allowing them to create noises that contain favorable higher frequency contents. On power-up, a trigger is applied into pin 1 of IC32 via D48 for starting running." **CONFIRMED.**
- Clock [DERIVED]: the 4070 gates a, b form an RC astable with R184 = 10 k (timing), R185 = 33 k (input series) and C47 = 100 pF.
  - The ideal CMOS-astable approximation gives f ≈ 1/(2.2·R184·C47) = **455 kHz**.
  - [R02] found that the same formula over-predicts the hat oscillator by about 25 %, so take **≈ 0.3–0.46 MHz**.
  - The 4006s run from +15 V [SM p.11 NOISE block].
- Consequence: an LFSR at ≥ 300 kHz is white across the audio band. Its sinc² envelope is −0.06 dB at 20 kHz for f_clk = 300 kHz, and the sequence repeats only after hours. **Do not emulate the LFSR bit-exactly at audio rate.** Running it at 44.1 kHz would put a sinc null at 44.1 kHz and droop of −3.9 dB at fs/2; that is wrong.
- **Contract:** define the noise by one-sided PSD `N0` (V²/Hz). Each sample is `x = √(N0·fs/2)·u` with u unit-variance white.
  - Downstream filters then see the same spectral density at every host rate, so filtered noise levels are identical at 44.1 and 192 kHz.
  - This is the mw808 lesson in the brief: there, every voice had to rate-compensate on its own.
- Sharing: SD, BD (attack), toms (TOM NOISE) and the clap tap one source [SM p.6, p.11]. [AR] recorded "Snare + Clap … from the master output to capture the phasing interference that occurs when both use the same noise generator" (AR Readme).
  - So mw909 must feed SD and CP from **one** noise stream. Correlated noise at the mix is part of the sound.
- Seeding: one PRNG (xoshiro/PCG) seeded from (instance seed, serial). It runs continuously, so successive hits differ. Offline renders are deterministic.

---

## 4. Oversampling and CPU

| voice | nonlinearity / bandwidth | choice | evidence |
|---|---|---|---|
| BD, SD, toms | triangle ≤ 350 Hz + static shaper, VCAs at LF | **1×**, polyBLAMP corners, polyBLEP resets | §2.1 table |
| RS | resonant band-passes ≤ 1.3 kHz | 1× | — |
| CP | filtered noise, VCAs | 1× | noise has nothing to alias except through the VCA; burst edges are RC-smoothed |
| HH, CR, RD | ZOH at 28–57 kHz, images to > 100 kHz | **exact-edge BLEP resampler** at host rate (no oversampling) | §5.5 |
| output stage / bus drive | soft saturation | ADAA [ADAA] or 2× locally | — |

[MEASURED, C microbenchmark, E5-2699 v4, -O2, scalar, 20 s at 48 kHz]:
- BLEP ZOH resampler (32 taps, 256 phases, linear phase interpolation): **0.157 %** of one core at f_clk = 31.9 kHz, **0.273 %** at 56 kHz. That is 49 ns per DAC step.
- Three triangle VCOs with polyBLAMP: **0.10 %**.
- For contrast, a naive 4× ZOH with 2 biquads and a 48-tap decimator: 0.47 %. It is also worse (§5.5).

Estimate [DERIVED]: the whole kit (11 voices) at 48 kHz Standard quality comes to well under 2 % of one core before SIMD. That leaves the brief's 8 % budget for the bus compressor, limiter and UI metering. The real risks are denormals in long RC tails (use FTZ/DAZ, and sleep below −120 dB) and allocation, not arithmetic.

Quality modes:
- **Standard:** everything as above at the host rate.
- **High (offline):** 64-tap BLEP kernels and 2× for the bus saturation.
- An "Eco" mode is unnecessary.

---

## 5. The sample chain

### 5.1 Facts the model must reproduce

| item | value | status |
|---|---|---|
| ROMs | 3 × HN61256P (32768 × 8), 6 data bits wired; HH (IC69, C43), crash (IC62, C42), ride (IC54, C44) | **CONFIRMED** [SM p.11][FRA][R02] |
| Hat regions | OH counts 0 → 0x6000 (24576 samples); CH plays 0x6000–0x7FFF (8192) | **CONFIRMED** [SM p.5 address table][R02] |
| Companding | "the Hi-Hat sounds have been compressed before being digitalized and PCM in order to have greater S/N ratio and higher digital resolution" [SM p.5]; "heavily compressed to make maximum use of the bits, and the original envelope is then restored by the vcas" [FRA] | **CONFIRMED** |
| DAC | 4174 hex latch → RM0621 six-resistor module to a common node (RA9 hat) | module **CONFIRMED**; internal weights **UNVERIFIED** [R02 §0.7] |
| Recon filter | 2 × unity-gain Sallen-Key: R480 = R475 = 5.6 k, C148 = 1200 pF (feedback), C149 = 1000 pF (ground): f0 = 25.9 kHz, Q = 0.548; R477 = R479 = 5.6 k, C150 = 2700 pF (fb), C151 = 390 pF (gnd): f0 = 27.7 kHz, Q = 1.316 | [DERIVED] from **CONFIRMED** parts, read from the HH section of [SM p.11]; R02 reads identical filters in all three voices |
| Recon response | −0.06 dB at 10 kHz, −0.22 at 16, **−0.56 at 20**, −1.54 at 24, −3 at 27.0, −6.7 at 32, −13.7 at 40, −20 at 48 kHz | [DERIVED], `f0 = 1/(2π√(R1R2C1C2))`, `Q = √(R1R2C1C2)/(C2(R1+R2))` |
| Cymbal envelope | "the same addresses are also converted to analog voltages through RA11, anti-log tapered by IC52b and Q70, and are applied to the base of Q71 (VCA)" [SM p.5]; "exponential decay with the same length as the sample regardless of tune setting" [FRA] | **CONFIRMED** |
| Envelope smoothing | IC52b integrator R432 = 270 k, C126 = 1000 pF: τ = 0.27 ms (crash) | [DERIVED] [SM p.11] |
| Start | every hit restarts at address 0 with the gated oscillator restarting: deterministic first edge | **CONFIRMED** [R02 §2] |

**Sample clocks [MEASURED].** Method: the ZOH impulse response is a rectangle of width 1/f_clk. Its transform, sinc(f/f_clk), is exactly zero at f_clk whatever the data, so the 96 kHz recordings show a single 63–77 dB-deep notch at f_clk. Welch PSD with 8192-sample segments and 32 K FFT, minimum of a 200 Hz-smoothed spectrum.

Cross-check: ride playback length × f_clk = 31,150–32,320 samples (median ≈ 31,500) for TUNE 0–70 %, which is 32768 less the last ≈ 1,200 samples that have decayed below the −87 dBFS floor. That confirms both the notch method and the 32 K length.

| voice / knob | f_clk (notch) | other |
|---|---|---|
| CH (DECAY 0 / 100), OH (accent 0 / 100) | 32.6 (weak notch) / 31.97 / 31.98 / 31.81 / 31.84 kHz | → **HH ≈ 31.9 kHz** |
| Crash TUNE 0 / 10 / 20 / 30 / 40 / 50 / 60 / 70 / 80 % | 28.27 / 28.53 / 30.37 / 32.63 / 34.54 / 36.93 / 39.34 / 42.43 / 46.98 kHz | 90–100 %: notch above the 48 kHz recording Nyquist |
| Ride TUNE 0 … 80 % | 28.29 / 28.81 / 30.32 / 33.53 / 35.26 / 37.05 / 40.20 / 43.67 / 46.49 kHz | from length/31,500: ≈ 54.9 kHz (90 %), 56.6 kHz (100 %) |

- **Fit [MEASURED]:** `f_clk(k) = 56.6 kHz / (2 − k)` predicts 28.3, 29.8, 31.4, 33.3, 35.4, 37.7, 40.4, 43.5, 47.2, 51.5, 56.6 kHz. Measured points lie within 2 % from 30 to 80 % and within 3–4 % at 10–20 %; the 90 % value from playback length (≈ 54.9 kHz) is ≈ 7 % above the fit, so the top end needs a better fit or a better measurement.
  - The effective law is `f ∝ 1/(R_fix + R_pot·(1 − k))` with **R_fix ≈ R_pot**.
- **CONFLICTING:** the schematic values R443 = 6.8 k + VR25 = 10 k(B) [SM p.11; R02 §0.5] predict a ratio of 16.8/6.8 = 2.47 : 1 (15.7 semitones). This unit measures 2.0 : 1 (12.0 semitones).
  - The hat is at 31.9 kHz while crash/ride at 50 % are at 37 kHz. The schematic would put both near 12 k, at the same rate.
  - Possible causes, all **UNVERIFIED**: the serial-number change R442 33 k → 10 k (the input series resistor of the crash oscillator, marked "SN392600 UP 10K" [SM p.11]) altering the CMOS-astable timing; the pot's end resistance; or a service trim.
  - **Recommendation:** use the measured law for the default unit, and expose the R-values in the variance model.
- **CONFLICTING with [SM p.5] / [R02]:** the hat clock is 31.9 kHz here against "about 60 kHz ÷ 2 = 30 kHz" in the notes. [FRA] chose 32 kHz as "pretty close to the rate used for the hats". Default to **31.9 kHz** (measured, matches [FRA]) and let unit variance cover about ±6 %.

### 5.2 The continuous-time model

The hardware output of one PCM voice is

```
y(t) = h_LPF(t) * [ g(t) · Σ_k  D(c_k) · rect((t − t_k)/T_k) ]
```

- `c_k` is the 6-bit code and `D(·)` the DAC transfer (§5.3).
- `t_k` is the k-th clock edge and `T_k = t_{k+1} − t_k` (the clock may move if TUNE moves mid-hit).
- `g(t)` is the VCA gain: the hat RC envelope × accent; for cymbals, the address-staircase envelope × accent.
- The VCA sits between DAC and LPF [SM p.11: Q84 diff-pair, then IC67b, then the Q80/Q81 filter].

For a constant clock, the spectrum is `Y(f) = H_LPF(f) · sinc(f/f_clk) · Σ_m S(f − m·f_clk)`. That is the baseband S (0..f_clk/2) plus images at m·f_clk ± f, weighted by sinc and H_LPF.

[DERIVED] for f_clk = 31.9 kHz:
- A 9 kHz component produces images at 22.9 kHz (**−9.2 dB** re the tone) and 40.9 kHz (−27.5 dB).
- Baseband content at 12–16 kHz has its first image at 15.9–19.9 kHz, in the audible band at almost the same level.
- This is the 909 hat "sizzle" and must be reproduced. [MEASURED] the hat recordings carry 16–24 kHz energy at −9 dB and 24–48 kHz at −20 dB re the strongest band (5–8 kHz).

At host rate fs, what the plugin must output is `y` band-limited to fs/2:
- keep baseband and images below fs/2;
- remove every image above fs/2, never fold it;
- apply sinc(f/f_clk) (the ZOH droop: −1.45 dB at 10 kHz and −3.9 dB at f_clk/2 for 31.9 kHz) and the analog H_LPF.

### 5.3 The 6-bit DAC model

- Codes 0..63 are unsigned with mid-scale ≈ 32 [R02 §1]. Output `D(c) = Σ_i b_i(c)·w_i + offset`, with nominal weights w_i = 2^i/63.
- Nonlinearity sources:
  - **resistor-weight tolerance** inside RM0621 (unknown spec; assume 1–2 %);
  - **CMOS output resistance of the 4174** in series with each weight, which matters most for the MSB's small resistor;
  - **supply ripple** on the latch VDD.
  - Engineering estimate: bit-weight errors of 1–3 % of full scale, largest on the MSB [R02 §0.7]. **UNVERIFIED.**
- Model: a 64-entry LUT per voice per unit, `D_unit(c) = Σ_i b_i·w_i·(1 + ε_i)`, with ε drawn from the serial seed (§7). The DNL then sits at the major carries (31→32, 15→16, …).
  - Cost: zero (a LUT lookup per step).
  - Audible effect: small even-order distortion and code-dependent noise, mostly hidden by the 6-bit quantisation.
- **No DAC glitch model is needed.** The latch updates all bits on one clock edge and RC settling is ≪ 1 µs.
- The 6-bit quantisation noise is part of the data (§5.7), not of the DAC model.
  - Because of the companding, the quantisation noise follows the envelope: SNR ≈ 30–36 dB re the instantaneous signal throughout the decay, rather than a fixed floor.
  - **This envelope-modulated grain is a signature of the 909 cymbals**, and a model that quantises *after* applying the envelope gets it wrong.

### 5.4 The variable clock

- The clock is a gated RC oscillator divided by 2 [SM p.5; R02 §2]. It starts at the trigger, so `t_0` is fixed relative to the trigger plus a small, deterministic start-up delay.
- Model the clock as a phase accumulator in seconds: `t_{k+1} = t_k + 1/f_clk(k_tune, unit)`. A TUNE change mid-hit changes the spacing of the following steps, and its pitch effect is instantaneous, as on the hardware.
- Per-unit clock spread: §7. Jitter of the RC oscillator: unknown, assume none (**UNVERIFIED**).

### 5.5 Anti-aliasing: methods compared

Exact solution: write the DAC output as a sum of steps, `Σ_k Δ_k·u(t − t_k)` with `Δ_k = D(c_k) − D(c_{k−1})`, so that

```
y(n/fs) = Σ_k Δ_k · S(n/fs − t_k)
```

where `S` is the step response of `h_LPF * h_AA` (h_AA = the host-rate band-limiting kernel).

Split S into the ideal step plus a short residual: `S = u + r`, with r ≈ 0 outside ±T/2 host samples. Each DAC step then costs **T multiply-adds at its exact fractional position**, and the hold value carries the rest. This is the BLEP / minBLEP idea [MINBLEP] applied to a stepped signal, and it is equivalent to bandlimited interpolation [JOS-RS] with a ZOH-convolved kernel.

- r is tabulated once per (fs, kernel) at P phases. Linear interpolation between phases is enough at P = 256.
- The analog LPF is folded into the table: h_LPF is fixed, so it costs nothing extra. For the hat (fixed f_clk) one could equally run H_LPF as an IIR at host rate, but folding it in also covers f_clk > fs.
- When f_clk > fs (crash/ride TUNE > ~70 % at 44.1/48 kHz) several steps land in one host sample. The method still works because each residual is band-limited at the host rate.

[DERIVED by simulation], test 1: a 6-bit, 3 kHz-high-passed white code stream. In-band (< 19 kHz, zero-phase filtered) waveform error re a 256× oversampled reference with a 96-tap Kaiser(14) decimator, best of ±3-sample alignment, 48 kHz host:

| f_clk | naive S&H 1× | linear interp 1× | naive 2× | naive 4× | naive 8× | BLEP T = 8 | BLEP T = 16 | **BLEP T = 32** |
|---|---|---|---|---|---|---|---|---|
| 28.3 kHz | −6.8 dB | −6.5 | −14.7 | −21.1 | −27.8 | −25.2 | −36.2 | **−52.4** |
| 31.9 kHz | −6.5 | −4.7 | −13.1 | −20.1 | −26.3 | −24.1 | −34.9 | **−51.5** |
| 37.0 kHz | −6.0 | −5.2 | −12.7 | −19.8 | −25.9 | −23.1 | −33.6 | **−50.3** |
| 56.0 kHz | −3.3 | −8.1 | −9.8 | −16.3 | −23.4 | −21.6 | −31.9 | **−48.6** |

- At 44.1 kHz the same test saturates at −30 to −35 dB for BLEP T = 32. The 0.45·fs kernel cut-off (19.8 kHz) falls inside my 19 kHz evaluation band, so passband-edge differences between kernels dominate; that is a limit of the test, not of the method.
- At 96 kHz the T = 32 result also saturates near −41 dB, against the reference's own LPF discretisation.
- Read the trend, not the absolute floor: **naive oversampling improves only about 6 dB per doubling. Exact-edge BLEP improves about 13–16 dB per doubling of taps.**

[DERIVED by simulation], test 2: a 9 kHz tone at f_clk = 31.9 kHz. Levels re the tone; the hardware would put its images at 22.9 kHz = −9.2 dB and 40.9 kHz = −27.5 dB:

| method | 48 kHz: image 22.9 k (should be ≈ −9 to −20 dB after the AA edge) | fold of 40.9 k → 7.1 k (should be absent) | fold of 54.8 k → 6.8 k | 44.1 kHz: fold 40.9 k → 3.2 k |
|---|---|---|---|---|
| naive S&H 1× | −8.1 | **−13.2** | −15.6 | −13.2 |
| linear interpolation | −16.2 | −26.3 | −31.4 | −26.3 |
| naive 2× + LPF + FIR | −32.3 | −55.4 | −55.4 | −51.8 |
| naive 4× + LPF + FIR | −29.9 | −58.2 | −61.2 | −66.5 |
| BLEP T = 16 | −19.5 | −102.7 | −101.1 | −102.7 |
| **BLEP T = 32** | −24.0 | **−111.3** | −100.8 | **−106.0** |
| 64× naive + 96-tap FIR | −48.0 | −65.2 | −63.9 | −64.4 |

Why naive oversampling fails:
- At M× the step edges are quantised to 1/(M·fs). f_clk and fs are incommensurate, so the timing error is a deterministic pattern that produces **spurs, not noise**: about −55 to −65 dB even at 64×.
- Exact-edge BLEP has no timing error.
- Linear interpolation is the worst of all for this signal: it destroys the ZOH spectrum, removing the images the hardware has, and still folds.

**Recommended design (Standard quality):**
1. Clock phase accumulator in double precision, in host-sample units. Steps at `τ_k = t_k·fs`, with the first edge at the trigger's sub-sample offset.
2. For each step: `Δ = (D_unit(c_k) − D_unit(c_{k−1}))·g(t_k)`. Apply the VCA at the step for the hat; between steps g changes slowly, so either apply g at each step or apply g to the output sample when the envelope is slow compared with 1/f_clk (true for every 909 envelope above 0.27 ms). Add `Δ·r(n − τ_k)` into a ring buffer for n = ⌈τ_k⌉ − T/2 … +T/2, and update the hold value.
3. Output `y[n] = hold[n] + ring[n]`. Latency T/2 = 16 samples (0.33 ms at 48 kHz). Report it through the `latencySamples()` slot the brief asks for. Alternatively use a minimum-phase residual (minBLEP) for zero latency at a slightly worse response.
4. Kernel: Kaiser-windowed sinc with cut-off `f_c = min(0.45·fs, 21 kHz)` and β ≈ 9, convolved with the analog 4-pole step response, normalised so S(∞) = 1. At fs ≥ 88.2 kHz, set f_c = 0.45·fs, which keeps the 22.9 kHz image the hardware has.
5. T = 32 for Standard and T = 64 for High.

Do not apply sinc-droop compensation: the droop **is** the hardware sound, and the BLEP formulation reproduces it automatically.

### 5.6 Envelopes after the DAC

- **Hats:** one RC envelope C135 = 1 µF. CH: R451 = 10 k + VR21 = 100 k(B). OH: R452 = 100 k + VR23 = 1 M(A). The CLOSED/OPEN line through Q72/Q73 selects the path [SM p.5, p.11]. The 2SA798 differential pair then acts as an exponential gain cell.
  - Choke: a CH trigger restarts the same counter at 0x6000 and the same VCA [R02 §0.13]. Model **one** hat voice.
  - [MEASURED] CH t(−20 dB) = 20 / 54 / 65 / 79 / 88 ms at DECAY 0 / 25 / 50 / 75 / 100; OH (DECAY 100) 242 ms.
- **Cymbals:** the envelope is a function of the address, so it is tied to the playback position:
  - `g_k = A_acc · 10^(−α·⌊k/512⌋/64 · …)` in the log domain, with 64 steps of 512 samples each if RA11 takes the top 6 address bits (which bits: **UNVERIFIED**, [R02 §0.10]);
  - smoothed by τ = 0.27 ms [DERIVED].
  - The decay therefore stretches with TUNE. [MEASURED] crash t(−20 dB) = 792 / 607 / 457 ms at TUNE 0 / 50 / 100; the clock ratio is 2.0 and the time ratio 1.73. Ride: 662 / 564 / 381 ms.
  - Compute g per DAC step, so staircase and clock stay locked at every host rate.
  - I looked for a 512-sample staircase in the crash envelope (expected 55–92 Hz modulation). I found modulation at f_clk/357 instead, most likely from the recorded cymbal content; the staircase is **not confirmed by measurement**.
- **Accent:** a voltage into the VCA control node (R481 4.7 k for the hat [SM p.11]). [MEASURED] its effect is +6.8 to +7.9 dB from total accent 0 to 100 % (§10.3).

### 5.7 Building the ROM images (the licensing constraint, technique only)

The decision belongs to 02 (§0.15, §9.7) and must be put to the user. Whatever source is chosen (original CC0 metal recordings or synthesis), the emulation pipeline for building a 6-bit image should mirror what the 1983 engineers did [SM p.5]:
1. Record or synthesise the source at ≥ 96 kHz. Resample it to the nominal f_clk with a high-quality band-limited resampler.
2. Measure its envelope (RMS, 2–5 ms) and divide it out (compand), leaving a nearly flat-envelope signal.
3. Quantise to 6 bits, unsigned, mid-scale 32, with no dither (the original has none; the grain is the point).
4. At playback the analog VCA (§5.6) restores a decay. For the cymbals the restored envelope is whatever the address-DAC law gives, so step 2 should flatten to that law's inverse.
5. Never fit, align or calibrate against a 909 ROM dump or a 909 recording at the waveform level. Match only statistics: spectra, decay, centroid.

---

## 6. Sample-rate independence (rules that make 44.1 → 192 kHz agree within a few %)

1. Specify everything in seconds, Hz, volts and ohms. Compute coefficients from fs in `prepare()`.
2. One-poles: `e^(−1/(τ·fs))`. SVFs: TPT with `g = tan(π·fc/fs)` [VAFD]. Never use bilinear-without-prewarp for resonances.
3. VCOs: phase increments in cycles per sample, from the CV in Hz. Fractional-sample trigger offset applied to phase and envelopes.
4. Noise defined by PSD (§3).
5. PCM: clock and envelope staircase in seconds and samples-at-f_clk (§5.4–5.6), output through the BLEP resampler. The only fs-dependent part is the band-limit, and that matches the hardware below min(fs/2, ~20 kHz).
6. Test: render each voice at 44.1, 48, 96 and 192 kHz. Compare the f(t) fit (Δf_b < 0.5 %, Δτ < 2 %), t(−20 dB) (< 2 %), Vpp (< 0.2 dB) and the 1/3-octave spectrum up to 16 kHz (< 0.5 dB). The PCM voices additionally need a 16–20 kHz image level within 1 dB at 48 kHz and above.

---

## 7. Analog variance (909-specific)

Physical sources, in order of audibility:
1. **Deterministic per-hit variation that is not randomness:**
   - shared noise realisation (SD, CP, toms, BD click);
   - retrigger interactions (envelope capacitors not discharged: a flam's second hit starts from the first hit's state);
   - **no** VCO phase randomness, because the VCOs reset (§2.1).
   - This makes the 909 much more repeatable hit-to-hit than the 808, whose metal bank free-runs.
2. **Unit-to-unit component values**, from a stored serial seed:
   - BD base pitch: two units, 25+ years and an ocean apart, agree within 0.3 % (49.2–49.6 Hz both) [MEASURED]. Keep the BD f_b spread small (σ ≈ 0.5 %).
   - BD pitch-envelope τ follows C9 (0.33 µF, tolerance unknown): σ ≈ 5 %.
   - Sample clocks: the hat measured 31.9 kHz against the notes' "about 30 kHz", and the crash/ride law differs from the schematic (§5.1). Allow σ ≈ 3–4 % on each of the three clocks, **independently**. Crash and ride were 0.3 % apart here.
   - DAC weights (§5.3), envelope τs (σ ≈ 5–10 %), VCA thresholds (σ ≈ 20 mV equivalent).
   - The 909's component tolerances are not stated in [SM]. Every σ above is a design choice, **UNVERIFIED**.
3. **Timing:**
   - [MEASURED] internal-clock jitter of 1.0–1.3 ms RMS, max ±3.6 ms, with intervals clustering in two groups about 3.9 ms apart (129.2–130.5 and 133.1–135.1 ms at a nominal 131.8 ms 16th). That points to a quantised CPU loop. Cause **UNVERIFIED**; see 03.
   - Offer it as a switchable "machine timing" feature, default off in host sync. That is a design choice.
4. **Noise floor / hum:**
   - [MEASURED] the AR recordings' floor is −101 dBFS RMS broadband, plus a −86 dBFS DC offset and 50 Hz odd harmonics (150/350/450/750 Hz lines; the recording was made on 50 Hz mains).
   - The level pots were at 50 %, so the hardware floor is at least about 95 dB below the BD accent peak. If modelled at all, keep the floor ≤ −95 dB re BD peak and off by default.

Keep the faithful variance model (serial seed, stored with the preset) separate from any "humanise" feature, as mw808 did.

---

## 8. Validation

### 8.1 Harness
- Use the existing mw808 pattern: an `mw909-render voice` CLI renders one hit per knob grid point at a chosen fs. Python or C++ metric code compares it against the reference set when present, with exit code 77 when `.deps/ref-samples/` is missing.
- Align by onset: the first sample above −40 dB re peak. For deterministic voices (BD, SD with snappy 0, toms with no noise), refine with cross-correlation over the first 5 ms. The AR files are trimmed with the onset at sample 0–10.
- Normalise the level per voice once, at a reference setting (§10.1), because AR's part level is an unknown 50 % pot position. After that, the **relative** levels across knob and accent grids must match.

### 8.2 Metrics and targets

| metric | method | target |
|---|---|---|
| Pitch trajectory (BD, SD, toms) | per-cycle zero crossings on a low-passed signal (1.5 kHz BD), sub-sample interpolation; fit f_b, Δf, τ | f_b ± 0.5 %; Δf ± 5 %; τ ± 5 %; RMS residual < 1 Hz |
| Multi-VCO voices | STFT peak tracking (25 / 35 / 60 / 130 ms windows, 2^18 zero-padded FFT) | partial frequencies ± 2 %, partial levels ± 2 dB |
| Decay | 5 ms RMS envelope, **first** crossing after the peak at −10 / −20 / −30 dB (not the last: the AR files have late clicks at about −47 dBFS); BD also per-cycle Vpp | ± 10 % per grid point after fitting the pot tapers |
| Envelope shape | BD hold length (Vpp within 0.5 dB); clap burst onsets; CH/OH | hold ± 5 ms; bursts ± 0.5 ms |
| Spectrum (noise and metal voices) | Welch PSD in 1/3-octave bands 30 Hz – 20 kHz over fixed windows; centroid | < 1.5 dB per band; centroid ± 5 % |
| Harmonic signature (BD shaper) | H2..H7 re H1 in a 80 ms window | ± 1.5 dB |
| PCM clock | depth and position of the sinc null in a 96 kHz render | f_clk ± 0.5 % of the target unit |
| PCM images | 16–24 kHz and (at 96 k) 24–48 kHz band level re the strongest band | ± 1.5 dB |
| Alias test | a tone-content ROM image at a high clock; look for non-harmonic lines | < −90 dB |
| Accent | Vpp vs total accent 0..100 % in 10 % steps | shape ± 0.7 dB, span ± 1 dB |
| SR invariance | §6 | as listed |

### 8.3 Null tests: where they work
- **Within mw909**:
  - Null renders of the same seed across builds (golden renders; tolerance-based because of SIMD and compiler differences, gated by `BLESS_REASON`).
  - Null the BLEP resampler against a 256× exact-edge reference (§5.5) in a unit test. Expect < −90 dB alias content.
- **Against the hardware**:
  - Only the deterministic, reset-started parts can null: the BD with ATTACK 0 and its first ~60 ms, the snappy-0 SD drum, the toms without noise.
  - Expect residuals of −20 to −30 dB at best. Phase drift from a 0.3 % pitch error accumulates to 90° in about 80 cycles, and unit variance does the rest.
  - Use the null as a diagnostic of the *trajectory* (the residual grows where the model diverges), not as a pass/fail gate.
  - Noise and PCM voices cannot be nulled: use the spectral and envelope metrics.

### 8.4 A/B listening
- Loudness-match (LUFS-S or RMS over the hit) and randomise blind ABX with ≥ 16 trials per pair. Use the same knob grid points as §10.
- Test in context too: a 16-step house pattern at 125 BPM rendered both from the samples and from the plugin, because shuffle, accent and choke interactions are audible only in patterns.
- **The AR files must not leave the machine.** Listening sessions happen locally; share only the metric reports.

---

## 9. Reference recordings

### 9.1 Search log (what exists, with licences)

| set | URL | licence | unit | what's covered | verdict |
|---|---|---|---|---|---|
| **AudioRealism "TR-909 Ultimate Sample Pack"** (909 Day, Sep 2025) | https://audiorealism.se/TR-909-SamplePack.html | "Free for personal & commercial use under the included license"; the archive's Readme says "may not be distributed in whole or in parts without permission". **Not CC0.** | one original unit; serial not stated | 848 WAVs (page says 853), mono 96 kHz/24-bit (recorded 32-bit float), individual outs, part volume 50 %, no clipping. BD and SD: tune × attack/tone × decay/snappy at 0/25/50/75/100 % (125 each); LT/MT/HT: tune × decay at 0..100 % in 10 % steps (121 each); CH decay ×5; crash and ride tune ×11; 11 accent levels for every voice; 8 flam modes for BD/SD/LT/MT/HT; RS 7 shuffles at "112" BPM; OH cut by CH × 7 shuffles (master out); SD+CP from master out; 8 single CP and RS shots | **Downloaded**: best measurement source by far |
| **Rob Roy Recordings set (1995)** | http://machines.hyperreal.org/manufacturers/Roland/TR-909/samples/ | "feel free to copy or distribute … DO NOT change samples … You may not distribute these samples for profit!!" **Not CC0.** | Manna Studios' unit, Minneapolis | 160 samples, mono 44.1 kHz/16-bit through an EMU ESI-32 sampler, **normalised**, trimmed: BD tune 0/3/7/10 × attack 0/10 × decay 0/3/7/10; SD 52; toms 16 each; RS 2; CP 2; CH/OH 6 each (decay 0..10); crash/ride tune 0..10; OH/CH combos | **Downloaded** as a second unit (pitch and timing only; levels meaningless) |
| Same set re-uploaded | https://archive.org/details/rided-0 ; https://github.com/tidalcycles/Dirt-Samples/tree/master/909 | no licence stated | — | subsets of the Rob Roy files | not used |
| geikha/tidal-drum-machines `RolandTR909` | https://github.com/geikha/tidal-drum-machines | **no licence** (GitHub reports none) | unknown | one-shots for 11 voices | not usable |
| Wikimedia Commons patterns | https://commons.wikimedia.org/wiki/File:TR-909_sound_example.ogg (CC BY-SA 3.0); https://commons.wikimedia.org/wiki/File:TR-909.ogg (CC BY 1.0); https://commons.wikimedia.org/wiki/File:909_house_pattern.ogg (CC BY-SA 4.0); https://commons.wikimedia.org/wiki/File:TR-909_House_Beat.wav (CC BY-SA 4.0) | CC BY / BY-SA (attribution) | unknown | short patterns, lossy Ogg except the last | usable for pattern-level listening only, with attribution; not one-shots |
| Freesound, filtered to CC0 + tags `tr909` / `tr-909` / `909` | https://freesound.org/search/?q=&f=tag%3Atr-909+license%3A%22Creative+Commons+0%22 | CC0 | — | 57 results: emulations (JD-Xi "Classic TR 909", "TR-909 emulation" by menegass), processed loops, no documented original-unit multisamples | none suitable |
| WaivOps RTRO-DRM / EDM-TR9 datasets | https://zenodo.org/records/8327650 | CC BY 4.0 | not hardware: "curated using an internal drum sample dataset" | loops | not a hardware reference |
| Colin Fraser ROM-derived WAVs | http://www.colinfraser.com/tr909/my909.htm | Roland's copyrighted ROM data | — | the three ROM images | **must not be used** (02 §0.15) |

**Conclusion: no CC0 recording of an original 909 with per-knob coverage exists that I could find.** The AR pack is legally usable for local testing (personal and commercial use is allowed; redistribution is not) and is far richer than anything else. Keep it local, never commit it, and never quote its URL or name in shipped files (branding audit).

### 9.2 What is on disk
- `.deps/ref-samples/README.md`: summary table and rules.
- `.deps/ref-samples/audiorealism-2025/`: 848 WAVs, the original `Readme.txt`, `LICENSE` (terms copied verbatim, archive sha256 `ff00d206…b560`), `SHA256SUMS`.
- `.deps/ref-samples/robroy-1995/`: the original zips and texts unmodified, an unpacked `wav/`, `LICENSE`, `SHA256SUMS`.

---

## 10. Measurements [MEASURED] (AR pack unless noted; 96 kHz / 24-bit mono)

### 10.1 Levels at the 50 % level setting (relative; there is no absolute volt calibration)

Each voice at knobs 50 % (crash/ride TUNE 50; OH accent 0 / DECAY 100; CH DECAY 50; RS and CP accent 0). DC is removed using the last 50 ms.

| voice | +peak | −peak | Vpp | Vpp re BD | +/− ratio |
|---|---|---|---|---|---|
| BD | −14.6 dBFS | −10.8 | −6.5 dBFS | 0.0 dB | 0.64 |
| SD | −10.0 | −7.9 | −2.9 | +3.6 | 0.78 |
| LT | −14.7 | −11.7 | −7.1 | −0.6 | 0.71 |
| MT | −14.4 | −10.5 | −6.2 | +0.3 | 0.64 |
| HT | −14.5 | −10.3 | −6.1 | +0.4 | 0.62 |
| RS | −15.4 | −9.9 | −6.2 | +0.3 | 0.53 |
| CP | −4.6 | −2.5 | +2.6 | +9.0 | 0.79 |
| CH | −9.9 | −12.1 | −4.9 | +1.6 | 1.29 |
| OH | −14.3 | −18.0 | −9.9 | −3.5 | 1.55 |
| crash | −16.9 | −21.1 | −12.7 | −6.3 | 1.62 |
| ride | −21.7 | −27.4 | −18.1 | −11.6 | 1.93 |

- These Vpp ratios carry the level-pot tolerance at 50 %. Use them as a cross-check of the service manual's level table (01/02 own the calibration in Vpp, per the brief), not as the calibration itself.
- **Every analog voice is negative-peak dominant (ratio 0.5–0.8). The PCM voices are positive-peak dominant (1.3–1.9).** A model calibrated on symmetric peaks will be off by 2–5 dB on one polarity.
- The highest peak anywhere in the set is 0.0 dBFS (CP accent 100). No file was found clipped flat (I checked peaks only, not runs of equal samples).

### 10.2 Per-voice tables
- BD pitch: §2.2 table. BD decay: §2.2 table. BD waveshaper harmonics: §2.2. BD attack: §2.2.
- SD: §2.3 tables. Toms: §2.4 table and decays. RS: §2.5. CP: §2.6.
- Hats and cymbals (Vpp, times from onset):

| voice | setting | peak | t(−10 / −20 / −30 / −40 dB) | centroid | strongest band | 16–24 kHz | 24–48 kHz |
|---|---|---|---|---|---|---|---|
| CH | DECAY 0 | −10.5 dBFS | 10 / 20 / 30 / 39 ms | 9.7 kHz | 5–12 kHz | −9 dB | −20 dB |
| CH | DECAY 50 | −9.9 | 30 / 65 / 98 / 121 | 9.8 kHz | 5–12 kHz | −10 | −20 |
| CH | DECAY 100 | −9.9 | 33 / 88 / 110 / 137 | 9.9 kHz | 5–12 kHz | −9 | −20 |
| OH | DECAY 100 (accent 0/50/100) | −14.3 / −10.8 / −7.3 | 95 / 242 / 381 / 520 | 10.2 kHz | 5–8 kHz | −9 | −19…−20 |
| crash | TUNE 0 / 50 / 100 | −16.2 / −16.9 / −18.0 | 439/792/1016/1093; 354/607/776/835; 239/457/545/572 | 7.9 / 8.9 / 10.6 kHz | 2–5 / 5–8 / 5–12 kHz | −13 / −18 / −7 | −25 / −17 / −23 |
| ride | TUNE 0 / 50 / 100 | −20.7 / −21.7 / −23.5 | 252/662/979/1098; 194/564/761/838; 149/381/525/559 | 9.8 / 11.3 / 13.8 kHz | 5–8 / 8–12 / 12–16 kHz | −10 / −13 / −3 | −28 / −15 / −17 |

- The OH decay times do not change with accent, which supports accent as a gain offset in the exponential VCA domain.
- The crash/ride decay times shrink with TUNE (clock) as the address-envelope mechanism predicts.

### 10.3 Accent (Vpp in dBFS, TOTAL ACCENT 0 → 100 % in 10 % steps, accent on)

| voice | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 | 100 | span |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| BD | −4.6 | −4.7 | −3.1 | −5.1 | −1.7 | −0.4 | 0.8 | 1.0 | 1.3 | 1.6 | 1.6 | 6.2 |
| SD | −1.0 | −1.4 | 1.5 | 1.6 | 1.1 | 2.3 | 3.0 | 3.2 | 3.8 | 3.7 | 3.8 | 4.8 |
| LT | −6.9 | −6.4 | −5.1 | −4.2 | −3.9 | −2.7 | −2.0 | −1.3 | 0.6 | 1.3 | 0.4 | 7.3 |
| MT | −6.4 | −6.2 | −4.5 | −6.7 | −3.0 | −2.2 | −0.5 | −0.4 | 0.4 | 1.1 | 1.3 | 7.7 |
| HT | −5.9 | −6.0 | −4.2 | −3.4 | −2.9 | −1.5 | −1.7 | −0.3 | 1.6 | 0.9 | 1.4 | 7.4 |
| RS | −6.2 | −6.2 | −5.4 | −6.5 | −4.5 | −4.0 | −3.6 | −3.1 | −2.6 | −2.3 | −2.2 | 3.9 |
| CH | −4.6 | −4.7 | −3.8 | −3.1 | −2.3 | −1.3 | −0.5 | 0.4 | 1.2 | 2.2 | 2.2 | 6.8 |
| OH | −9.9 | −9.8 | −8.8 | −8.3 | −7.4 | −6.4 | −5.0 | −4.5 | −3.6 | −2.6 | −2.8 | 7.2 |
| crash | −13.9 | −13.8 | −12.8 | −12.3 | −11.2 | −10.1 | −9.1 | −8.1 | −7.1 | −6.0 | −6.0 | 7.9 |
| ride | −17.9 | −18.2 | −17.0 | −16.6 | −15.6 | −14.3 | −13.3 | −12.4 | −11.3 | −10.4 | −10.2 | 7.7 |

- The no-accent grid files match accent 0 within 0.5 dB for BD, the toms, CH and ride. The crash is 1.2 dB louder in the grid file, and SD differs by 1.0 dB (the files have different TONE settings).
- The PCM voices are the clean reference for the accent DAC law: **flat from 0 to 10 %, then ≈ 0.95–1.05 dB per 10 %, flat again from 90 to 100 %.** That is consistent with a clamped DAC voltage into an exponential VCA.
- The analog voices are noisier (hand-set knobs, hit-to-hit noise).

### 10.4 Sequencer by-products (inputs to 03)
- **Shuffle** (RS at the nominal 112 BPM; the measured 16th was 131.82 ms, so the actual tempo was ≈ 113.8 BPM): even-16th delays of 0.00, 11.05, 22.21, 33.18, 43.27, 54.67, 65.30 ms. That is **0.000, 0.084, 0.169, 0.252, 0.328, 0.415, 0.495 of a 16th = (n−1)·2/96 beat** within 0.12 tick. This **CONFIRMS** R03's (n−1)/48-beat rule independently. Method: cross-correlation template matching with parabolic peak refinement.
- **Jitter** (same files, residuals from a linear fit of every other hit): 1.0–1.3 ms RMS, 2.4–3.6 ms max.
- **Flam** (BD, from the pitch reset at the second hit; each value is bracketed by a ≈ 4–6 ms cycle period):

  | flam | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|---|---|
  | second hit | 19–25 ms | 19–25 | 20–25 | 26–32 | 26–32 | 34–41 | 42–49 | 42–49 |

  Onset detection on the toms gives 19.4, 23.6, 27.1, …, 47.9 ms (LT) and 19.1, 23.5, …, 47.4 ms (MT), with outliers.
  - This favours the **20–48 ms in 4 ms steps** branch of R03's CONFLICTING item over 10–38 ms.
  - The BD pairs (1/2, 4/5, 7/8) look identical at this resolution, which needs a cleaner method (**UNVERIFIED**).
  - In the flam samples the first hit is much quieter than the second.

### 10.5 Recording-chain facts
- AR noise floor: −101 dBFS RMS (0–48 kHz) and a −86 dBFS DC offset. Lines at 150, 350, 450 and 750 Hz (50 Hz mains harmonics).
- Late clicks of up to about −47 dBFS appear in some BD tails (e.g. at about 180–340 ms at −58 dB RMS in DECAY 0). **Use first-crossing decay metrics.**

---

## 11. Open questions

1. **Crash/ride clock law:** measured 28.3–56.6 kHz (2.0 : 1), against 2.47 : 1 from R443 + VR25. Is the difference the R442 serial change, or something else? A second unit's 96 kHz cymbal sweep, or a scope reading on IC60 pin 1, would settle it. The hat clock is 31.9 kHz on this unit against "about 30 kHz" in the notes.
2. **RM0621 internal weights** (binary-weighted, or not), and therefore the DAC nonlinearity.
3. **Which address bits feed the cymbal envelope DAC** (RA11/RA13), and whether a 64-step staircase is audible. My modulation search was inconclusive.
4. **BD VCA law and hold mechanism:** the measured hold of 55–65 ms and the thresholded tail are clear. The transistor-level origin (Q3/Q4/Q5 with C7) needs 01's circuit analysis to replace my behavioural fit.
5. **BD shaper curve:** D10/D11 type and the triangle amplitude into R52 set the exact H3/H5. Fit to §2.2.
6. **Snare/tom VCO frequency ratios and the third tom oscillator.** The spectra show 0.67–0.68× and 1.5–1.58× ratios; 01 should derive them from C69/C71 and the tom capacitors.
7. **Flam table:** two of the eight BD flam modes seem to coincide in pairs at my resolution; check with 03's sources.
8. **Noise clock:** 300–460 kHz derived. A scope or spectrum measurement would confirm it, though it is irrelevant to the audio band.
9. **Absolute level calibration:** the AR pack is at part volume 50 % with an unknown interface gain. Vpp calibration must come from the service-manual level table (01/02).

---

## Sources

- Roland, *TR-909 Service Notes* (1984): https://archive.org/details/synth_Roland_TR-909_service_notes ; https://archive.org/details/roland_Roland_TR-909_Service_Manual
- C. Fraser, 909 clone notes: http://www.colinfraser.com/tr909/my909.htm
- AudioRealism, TR-909 Ultimate Sample Pack: https://audiorealism.se/TR-909-SamplePack.html (archive https://audiorealism.se/downloads/TR-909-96kHz-24bit.zip)
- Rob Roy Recordings TR-909 set: http://machines.hyperreal.org/manufacturers/Roland/TR-909/samples/TR909.TXT
- Wikimedia Commons 909 audio: https://commons.wikimedia.org/wiki/File:TR-909_sound_example.ogg ; https://commons.wikimedia.org/wiki/File:TR-909.ogg ; https://commons.wikimedia.org/wiki/File:909_house_pattern.ogg ; https://commons.wikimedia.org/wiki/File:TR-909_House_Beat.wav
- Freesound CC0 search: https://freesound.org/search/?q=&f=tag%3Atr-909+license%3A%22Creative+Commons+0%22
- WaivOps RTRO-DRM: https://zenodo.org/records/8327650
- tidal-drum-machines: https://github.com/geikha/tidal-drum-machines ; Dirt-Samples: https://github.com/tidalcycles/Dirt-Samples ; archive.org re-upload: https://archive.org/details/rided-0
- Werner, Abel, Smith, DAFx-14 (808 BD; methodology): https://dafx14.fau.de/papers/dafx14_kurt_james_werner_a_physically_informed,_ci.pdf
- Esqueda, Välimäki, Bilbao, *Rounding Corners with BLAMP*, DAFx-16: https://dafx.de/paper-archive/details/xRM0GiuF-JmeN0_aaP9VNg
- E. Brandt, *Hard Sync Without Aliasing* (minBLEP), ICMC 2001: https://www.cs.cmu.edu/~eli/papers/icmc01-hardsync.pdf
- J. O. Smith, bandlimited interpolation / resampling: https://ccrma.stanford.edu/~jos/resample/
- Parker, Zavalishin, Le Bivic, ADAA, DAFx-16: https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA
- V. Zavalishin, *The Art of VA Filter Design*: https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf
- Tiptop Audio HATS909 manual: https://www.tiptopaudio.com/manuals/Tiptop_Audio_HATS909_ns.pdf
- D16 Drumazon 2 manual: https://m.d16.pl/pub/manuals/Drumazon%202-manual-gb.pdf
- mw909 sibling research: `research/02-voices-sample.md`, `research/03-system-sequencer.md`
