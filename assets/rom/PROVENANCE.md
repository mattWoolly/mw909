<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# Factory ROM sources: provenance

The factory 6-bit ROM images of the four sample voices (`libs/mw909-core/src/rom/RomData.cpp`)
are built from the four recordings in `sources/`. All four come from the Versilian Community
Sample Library (VCSL), which is released under CC0 1.0. They are recordings of real cymbals.
None of them is a recording of the original machine, and no data from the original machine's
ROMs was downloaded, kept, analysed or fitted against at any point.

## Licence

- Library: Versilian Community Sample Library, by Versilian Studios LLC (Sam Gossner),
  https://github.com/sgossner/VCSL
- Licence: Creative Commons CC0 1.0 Universal (public domain dedication),
  https://creativecommons.org/publicdomain/zero/1.0/. The full text is the repository's
  `LICENSE` file at the commit below (sha256 `a2010f343487d3f7618affe54f789f5487602331c0a8d03f49e9a7c547cf0499`).
  The README says: "This collection is under a Creative Commons 0 license. Essentially it's
  Public Domain- you can do whatever you want with these sounds (even make commercial
  software), no royalties, no credit, no special terms."
- Commit: `c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e` (HEAD of `master` on 2026-10-03)
- Retrieved: 2026-10-03, over HTTPS from `raw.githubusercontent.com` at that commit

## Files

Source paths are relative to `Idiophones/Struck Idiophones/` in the repository. Each source file
is 44.1 kHz, 16-bit, stereo.

| `sources/` file | Source file | Articulation | Source sha256 | Output sha256 |
|---|---|---|---|---|
| `hat_closed.wav` | `Hi-Hat Cymbal/HiHat_HitC_v4_rr2_Mid.wav` | closed hi-hat, stick, loudest layer | `98325d2327a7165b12840951de3fd2297e59fcdd0a6a58176a5cb4a3ed1f2958` | `ad8e70331e1eb298834d28923eaa810c4e7c70c2ffc40e9b696d9f26af5c3f85` |
| `hat_open.wav` | `Hi-Hat Cymbal/HiHat_HitO_rr2_Mid.wav` | open hi-hat, stick | `f71f21883f6fe5466d15610ef0e9241ba3851c821858a958f8a6a75bff774760` | `785de80272db152c67c03ff47f76541be1b762ca94bd630787ba8aab64d628de` |
| `crash.wav` | `Suspended Cymbal 2/susCymb2_hit_stick_mf1.wav` | suspended cymbal, stick, mezzo-forte | `665e7cad4a0deb1931a0f26589ea8af177527a47b65926586fd0b819f3c6362b` | `a28629db85ddb735c9f9265b88d89c4cc956a5904fc8e4fd0947d2b7562f59dc` |
| `ride.wav` | `Suspended Cymbal 2/susCymb2_hit_stick_mp1.wav` | the same cymbal, stick, mezzo-piano | `16c523bae5b58e17eefcc207a91317ec51683941b26398c2473dafc92a08999e` | `194b42592319039e8a3ef6ee17996b1d391309031ab500685f961fca1863365a` |

The crash and the ride are the same cymbal struck harder and softer. The original machine also
used one cymbal for both.

## Processing

1. Trim to mono (sox 14.4). Each output is a bit-exact copy of the left channel over the given
   span. No gain change, no dither, no resampling.
   ```
   sox HiHat_HitC_v4_rr2_Mid.wav    hat_closed.wav remix 1 trim 0.0075 0.400
   sox HiHat_HitO_rr2_Mid.wav       hat_open.wav   remix 1 trim 0.0078 1.000
   sox susCymb2_hit_stick_mf1.wav   crash.wav      remix 1 trim 0 1.100
   sox susCymb2_hit_stick_mp1.wav   ride.wav       remix 1 trim 0 1.100
   ```
2. Voicing EQ in `tools/rombuild` (RBJ biquads at 44.1 kHz). The VCSL cymbals are orchestral
   instruments, darker and heavier in the low mids than a drum-kit cymbal. The EQ moves their
   band balance towards the published spectral statistics of the original machine's output
   (research/05 §10.2). It matches statistics only, never waveforms.
   - closed hat: none
   - open hat: high-pass 800 Hz, Q 0.5
   - crash: high-pass 1 kHz, Q 0.707, then a +4 dB high shelf at 6 kHz
   - ride: high-pass 1 kHz, Q 0.707
3. `mw909::romFromAudio` (`libs/mw909-core/src/rom/RomBuild.cpp`):
   - remove the mean and trim to the onset
   - low-pass and resample to the region's clock: 31.9 kHz for the hats with a 12.8 kHz corner,
     37.7 kHz for the cymbals with a 17 kHz corner
   - flatten the envelope by dividing out a 25 ms RMS tracker, floored 40 dB down
   - normalise to the 99.9th percentile, then quantise to 6 bits with no dither
   - fade the last 2 ms to mid-scale and pad with mid-scale

## Regenerating

```
cmake --build <build-dir> --target mw909-rombuild
<build-dir>/tools/rombuild/mw909-rombuild assets/rom/sources libs/mw909-core/src/rom/RomData.cpp
```

The tool can also be configured and built on its own, which builds only the ROM library and the
tool (the rest of the core is excluded from that build):

```
cmake -S tools/rombuild -B <dir> && cmake --build <dir>
<dir>/mw909-rombuild assets/rom/sources libs/mw909-core/src/rom/RomData.cpp
```

The output
is deterministic: the same sources give the same file on the same toolchain. The generated file
records the inputs and an FNV-1a checksum of each image. The current checksums are hat
`207e5a5b`, crash `000f4ba4` and ride `e2e155c1`.
