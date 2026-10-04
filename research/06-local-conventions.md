<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- mw909: classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio. -->

# 06: Local conventions of the mwAudio line, applied to mw909

Scope: a read-only survey of this machine, run on 2026-10-03 between 13:05 and 13:20 local time. mw808 (`/home/mwoolly/projects/mw808`) is the primary template. The older survey in `/home/mwoolly/projects/mw808/research/06-local-conventions.md` (written 2026-10-02, before mw808 existed) covers the wider sibling line: mwStime, mw140, mw160, ssl4k-2, mw90 and mw303. This document does not repeat that background. It records what mw808 actually ended up doing and what mw909 has to change.

There are no web sources here. Every claim cites the local file it came from, as `path:line` where a line helps. Tags:

- **CONFIRMED**: read directly from the cited file, or reproduced by a command whose output is quoted.
- **INFERRED**: my reasoning from the cited files. It was not observed directly.
- **UNVERIFIED**: not checked, or seen only in another session's scratch files.
- **[DERIVED]**: computed by me, with the formula shown.

Brand names appear in this research file only. Elsewhere in mw909 they are forbidden (§6).

**Concurrency note.** While I was surveying, another agent was scaffolding mw909 by copying mw808. Between 13:07 and 13:13 these appeared under `/home/mwoolly/projects/mw909/`: `CMakeLists.txt`, `plugin/`, `libs/mw909-core/`, `tools/`, `tests/`, `scripts/`, `build/linux-release` and `~/.config/mwAudio/mw909/Presets/`. Where I quote that scaffold, I mark it "scaffold snapshot 13:13". It may have changed since.

---

## 0. Design-critical facts (executive summary)

| Item | Value for mw909 | Source / status |
|---|---|---|
| Product name | `mw909` (`MW_PRODUCT_NAME`, set in `plugin/CMakeLists.txt`, not top level) | mw808 pattern `mw808/plugin/CMakeLists.txt:10-12`. CONFIRMED |
| Company / manufacturer code | `mwAudio` / `mwAu` | `mw808/plugin/CMakeLists.txt:20,22`. CONFIRMED |
| Plugin code | **`mw99`**. Unused in every project source tree and every installed binary (§1.3, §1.4). `mw90` is free as a code but is the *product name* of the mw90 reverb (bundle `com.mwaudio.mw90`), so avoid it. | grep + byte scan below. CONFIRMED |
| Bundle id / CLAP id | `com.mwaudio.mw909` | mw808 pattern `plugin/CMakeLists.txt:21,102`. CONFIRMED |
| LV2 URI | `https://github.com/mattWoolly/mw909` | mw808 pattern `plugin/CMakeLists.txt:33`, `tools/validators/lib.sh:24`. CONFIRMED |
| VST3 component class ID | `ABCDEF01 9182FAEB 6D774175 6D773939` | [DERIVED] from JUCE `juce_VST3Interface.h:185-215`: word0 `0xABCDEF01`, word1 `0x9182FAEB`, then the manufacturer and plugin codes as big-endian ASCII. `mwAu` = `6D774175`, `mw99` = `6D773939`. The same rule reproduces mw808's installed CID `ABCDEF019182FAEB6D7741756D773830`. |
| Licence / header | AGPL-3.0-or-later, © mwAudio. Code files: `// SPDX-License-Identifier: AGPL-3.0-or-later` / `// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.` (em dash). Markdown uses `<!-- ... -->` with a colon (`mw909: classic ...`), as mw808's README does. | `mw808/tools/check_license_headers.sh:25-26`, `mw808/README.md:1-2`. The checker only reads code, CMake and shell files, so the md form is free. CONFIRMED |
| Deps | `.deps/{JUCE 8.0.13 @7c9d378, clap-juce-extensions @51a9359 (clap 1.2.7), Catch2 v3.7.1 @fa43b77}` are already in `mw909/.deps`, clean git checkouts. `cmake/Deps.cmake` points `FETCHCONTENT_SOURCE_DIR_<NAME>` at `.deps/<dir>` when `<dir>/CMakeLists.txt` exists, and otherwise fetches the same pin. | `git describe`/`rev-parse` run on 2026-10-03. CONFIRMED |
| Toolchain | g++ 13.3.0, CMake 3.28.3 (presets schema 6 needs ≥ 3.25), Ninja 1.13, python3 with numpy 1.26.4 and scipy 1.11.4, xvfb-run, strip, unzip, curl. **Not installed:** pluginval and clap-validator on PATH, lv2lint, sord_validate, lv2_validate, carla-single. | `which` / `--version` output. CONFIRMED |
| Validator pins | pluginval **v1.0.4**: zip `c01c49d8…b55352`, binary `d3d342ea…e10ba`. Run at strictness 10, in-process, `--repeat 3 --randomise --timeout-ms 120000`. clap-validator **0.4.1** by default: zip `49edadcf…ad90`, binary `d9a78bfe…0dbf`. 0.3.2 alternative: tgz `1476ed68…9cb1`, binary `d25b9f06…7f4c`. | `mw808/tools/validators/run_pluginval.sh:28-36`, `run_clap_validator.sh:26-44`. The digests match the cached files in both `mw808/build` and `mw909/build`. CONFIRMED |
| Validator contract | Exit 0 pass, 1 fail (a checksum mismatch is a hard fail), 77 skip (CTest `SKIP_RETURN_CODE 77`). LABEL `validators`, `RUN_SERIAL`. Cache in `<repo>/build/validator-tools/`, seeded from `build/tools/pluginval`. | `tools/validators/lib.sh:5-14`, `tools/ProjectChecks.cmake:29-45`. CONFIRMED |
| Install | `tools/install-user.sh <build-dir>` installs Release only. It removes exactly `~/.{vst3,clap,lv2}/mw909.*`, then `cp -a`, then `strip --strip-unneeded` on the installed copy, then `touch`. Also takes `--dry-run`, `--uninstall`, `--formats` and `MW909_INSTALL_ROOT`. It never touches mw808. | `mw808/tools/install-user.sh`. CONFIRMED |
| Collisions | No `mw909`, `mw99` or `com.mwaudio.mw909` anywhere in `~/.vst3`, `~/.clap`, `~/.lv2` or the REAPER plugin caches. `~/.config/mwAudio/mw909/Presets/{Kits,Patterns,Full}` already exists (created 13:13 by the scaffold's test run). | §2. CONFIRMED |
| Forbidden tokens | mw808's list already catches `Roland`, `TR-909`, `TR909`, `TR-8S`, `TR-09`, `RD-9`, `Drumazon`, `Nepheton`, `AudioRealism`, `Behringer` and `Arturia` (each pattern tested in §6.2). For mw909 add at least `Softube`, `Wave Alchemy`, `D16 Group`, the space-separated `TR 909` and `Phoscyon`. Optionally add `ReDominator` and the text-only `ADM`. | §6. CONFIRMED (tests run) |
| State/preset tags | Root ValueTree `mw909`, preset root `mw909preset`, extension `.mw909`. Default unit seed `kDefaultUnitSeed = 909` (mw808 uses 808). Start fresh at `stateVersion = 1`. The pattern size is new, so `patternSize` differs and mw808 blobs are refused. | `StateCodec.h:37-42`, `PresetManager.cpp:22`, `PresetManager.h:82`, `PatternModel.h:65`. CONFIRMED |

Biggest traps when copying (§8, §9):

1. A blind `s/mw808/mw909/` is necessary but not enough. Bare `808` literals (the unit seed, the wordmark `num = "808"`, test RNG seeds) and the `MW808_V2` SIMD macro need manual edits. So does the header *text* "classic analog drum machine", which becomes "classic hybrid drum machine". The test CMake files hard-code `MW808_PRODUCT_NAME="mw808"` because `MW_PRODUCT_NAME` lives in `plugin/`'s directory scope.
2. The voice roster is the 808's (16 voices, including congas, claves, maracas, cowbell and the analog metal bank). Everything indexed by `VoiceId` must be re-derived from the 909 roster: params, `MidiMap`, buses, factory content and golden cases.
3. The plugin tests and the UI tool create the real `~/.config/mwAudio/<product>/Presets` (§10, L23). Set `MW909_PRESET_DIR` in their CTest `ENVIRONMENT`.

---

## 1. Identity for mw909

### 1.1 The identity block (mw808 pattern, CONFIRMED)

`/home/mwoolly/projects/mw808/plugin/CMakeLists.txt`:

```cmake
set(MW_PRODUCT_NAME "mw808")                       # :10
string(TOLOWER "${MW_PRODUCT_NAME}" MW_PRODUCT_ID) # :11
set(MW_PLUGIN_CODE "mw80")                         # :12
set(MW808_PLUGIN_FORMATS VST3 LV2 Standalone)      # :15  one line to drop LV2
juce_add_plugin(mw808
    PRODUCT_NAME "${MW_PRODUCT_NAME}"  VERSION "${PROJECT_VERSION}"
    COMPANY_NAME "mwAudio"  BUNDLE_ID "com.mwaudio.${MW_PRODUCT_ID}"
    PLUGIN_MANUFACTURER_CODE mwAu  PLUGIN_CODE ${MW_PLUGIN_CODE}
    DESCRIPTION "Classic analog drum machine with step sequencer"
    FORMATS ${MW808_PLUGIN_FORMATS}
    IS_SYNTH TRUE  NEEDS_MIDI_INPUT TRUE  NEEDS_MIDI_OUTPUT TRUE  IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE          # text-entry fields
    VST3_CATEGORIES Instrument Drum
    LV2URI "https://github.com/mattWoolly/${MW_PRODUCT_NAME}"
    NEEDS_CURL FALSE  NEEDS_WEB_BROWSER FALSE  COPY_PLUGIN_AFTER_BUILD FALSE)
clap_juce_extensions_plugin(TARGET mw808 CLAP_ID "com.mwaudio.${MW_PRODUCT_ID}"
    CLAP_FEATURES instrument drum-machine stereo)                         # :100-103
```

The compile definitions are `JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0` (PUBLIC) and `MW808_PRODUCT_NAME`, `MW808_VERSION_STRING` (PRIVATE) (`:69-78`). For mw909, change only the three `set()` lines, the `DESCRIPTION` ("Classic hybrid drum machine with step sequencer"), and the `MW808_` → `MW909_` variable and target names.

The scaffold snapshot 13:13 (`/home/mwoolly/projects/mw909/plugin/CMakeLists.txt:10-12,21,33,102`) already has `mw909` / `mw99` / `com.mwaudio.mw909` / LV2URI `.../mattWoolly/mw909`. CONFIRMED.

### 1.2 Who uses which 4-character code: project sources

Method: grep `PLUGIN_CODE`, `MW_PLUGIN_CODE`, `PLUGIN_MANUFACTURER_CODE`, `PLUG_UNIQUE_ID`, `PLUG_MFR_ID` and `JucePlugin_PluginCode` over every `CMakeLists.txt`, `*.cmake`, `config.h`, `*.jucer`, `*.h` and `*.json` under `/home/mwoolly/projects`. The search excluded `_deps`, `build*`, `.deps`, JUCE, iPlug2, vst3sdk and clap-juce-extensions trees, and folded agent worktrees into their parent project. 261 raw hits, deduplicated (script and raw list in `/tmp/claude-1000/mw909-research-conventions/`). CONFIRMED.

| Project (source) | Mfr code | Plugin code | Product / bundle id | Notes |
|---|---|---|---|---|
| mw140 | `mwAu` | `mw14` | `com.mwaudio.mw140` | |
| mw160 | `mwAu` | `mw16` | `com.mwaudio.mw160` | the *installed* binary is older and carries `MwAu`/`Mw16` (§1.3) |
| ssl4k-2 (mw4000) | `mwAu` | `mw40` | `com.mwaudio.mw4000` | |
| mwVnylSim | `mwAu` | `mwVn` | `com.mwAudio.mwVnylSim` | builds with the VST2-compatible CID |
| mw303 (product `mw12F`) | `mwAu` | **`mw12`** (`mw303/CMakeLists.txt:7`) | `com.mwaudio.mw12f` | was `m12F` in mw808's survey and has changed since |
| mw303VinylSim (+ ~40 `.claude/worktrees` copies) | `Mw3x` | `Vs03` | | |
| mwStime | `MwSt` | `MwS1` | `com.mattwoolly.mwstime` | off-standard identity |
| **mw808** | `mwAu` | **`mw80`** | `com.mwaudio.mw808` | |
| **mw909** (scaffold 13:13) | `mwAu` | **`mw99`** | `com.mwaudio.mw909` | the only hit for `mw99` |
| mw90 | n/a (raw VST3 SDK) | n/a | `com.mwaudio.mw90` (`mw90/mw90/config.h:6`) | own FUID `0x4D573930 0x50434D39 0x30524556 0x52425052` = ASCII "MW90PCM90REVRBPR" (`mw90/mw90/mw90_vst3.cpp:25-26`) |
| postscript-stuff/postscript-backup/mw101 | `Mwly` | `Mw01` | | backup tree |
| postscript-stuff/postscript-backup/303 | `Mwau` | `Ac33` | | backup tree |
| postscript-stuff/postscript-backup/mwStime | `MwSt` | `MwS1` | | backup tree |

A whole-tree search for a standalone token `mw99`, or for `PLUGIN_CODE "?mw9`, outside build, deps and SDK trees returned only `mw909/plugin/CMakeLists.txt`. A search for `PLUGIN_CODE mw90`, `PLUG_UNIQUE_ID 'mw90` or `"mw90"` returned only the mw90 project's own files: its `CMakeLists.txt`, `config.h` and three plan/spec docs. Neither code is used as a JUCE/iPlug plugin code. CONFIRMED.

### 1.3 Installed binaries: what codes they carry

JUCE builds the VST3 class ID from the two codes (§0 row "VST3 component class ID"), so the installed `moduleinfo.json` files show the codes directly. I decoded bytes 8-15 of each CID:

| Installed bundle | Vendor | Component CID | bytes 8-11 / 12-15 |
|---|---|---|---|
| `~/.vst3/mw140.vst3` | mwAudio | `ABCDEF019182FAEB6D7741756D773134` | `mwAu` / `mw14` |
| `~/.vst3/mw160.vst3` | mwAudio | `ABCDEF019182FAEB4D7741754D773136` | `MwAu` / `Mw16` (older build than the source) |
| `~/.vst3/mw4000.vst3` | mwAudio | `ABCDEF019182FAEB6D7741756D773430` | `mwAu` / `mw40` |
| `~/.vst3/mw808.vst3` | mwAudio | `ABCDEF019182FAEB6D7741756D773830` | `mwAu` / `mw80` |
| `~/.vst3/mwStime.vst3` | mattWoolly | `ABCDEF019182FAEB4D7753744D775331` | `MwSt` / `MwS1` |
| `~/.vst3/mwVnylSim.vst3` | mwAudio | `5653546D77566E6D77766E796C73696D` | VST2-style: "VST" + `mwVn` + "mwvnylsim" |
| `~/.vst3/TAL-BassLine-101.vst3` | TAL | `5653546241733174616C20626173736C` | third party |
| `~/.vst3/mw90.vst3` | (no moduleinfo) | `4D57393050434D393052455652425052` per REAPER cache | "MW90PCM90REVRBPR" |
| `~/.vst3/Vital.vst3` → `/usr/lib/vst3/Vital.vst3` | | not decoded | third party |

**Byte scan for the codes** (`/tmp/claude-1000/mw909-research-conventions/bytescan.py`, output `bytescan.txt`). For every installed `.so`/`.clap` under `~/.vst3`, `~/.clap` and `~/.lv2`, I counted occurrences of each code as big-endian text and as a little-endian int32 immediate, which is how `JucePlugin_PluginCode` lands in x86 code. `mw99`: 0/0 in all 14 binaries. `mw90`: 162 text hits in `mw90.so` only (its product name), 0 elsewhere. CONFIRMED.

**`strings` scan** over the same 14 binaries for `mw`-prefixed 4-character tokens. The only hits are `mw90` (mw90.so); `MW80` (in the three mw808 binaries); `MW1Z` (mwStime LV2); `MWXd` (mw160, mwVnylSim); `MWVR`/`MWbK`/`MW8I` (Vital); `mwxz`/`mwst` (TAL); and `mwlb` (most JUCE binaries, chance bytes). No line matched `mw99`, `99wm`, `MW99` or `mw909`. CONFIRMED (`strings_report.txt`).

**CLAP ids installed:** `com.mwaudio.mw808`, `com.mattwoolly.mwstime`, `com.mattwoolly.ssl4k` (from `strings`). CONFIRMED.

### 1.4 Verdict on the code

- `mw99` is unused by every source tree and every installed plugin, so mw909's VST3 CID `ABCDEF019182FAEB6D7741756D773939` [DERIVED] collides with nothing. CONFIRMED.
- `mw90` would not technically collide either. The mw90 reverb uses its own FUID and no JUCE codes, so a JUCE `mwAu`/`mw90` CID would be `…6D7741756D773930`, which differs from `4D573930…` [DERIVED]. But `mw90` is a shipped mwAudio product name, and a code equal to a sibling's name invites confusion in host caches and support. **Use `mw99`**, as the brief says. INFERRED.

---

## 2. What is installed now, and what could collide with mw909

Listing taken at 2026-10-03 13:10 (`ls -la ~/.vst3 ~/.clap ~/.lv2`). CONFIRMED:

| Dir | Entries (mtime) |
|---|---|
| `~/.vst3` | mw140 (04-26), mw160 (04-12), mw4000 (04-14), **mw808 (10-03 09:23)**, mw90 (04-14), mwStime (07-07), mwVnylSim (04-16), TAL-BassLine-101 (05-24), `Vital.vst3` → `/usr/lib/vst3/Vital.vst3` (symlink) |
| `~/.clap` | **mw808.clap (5.2 MB, 10-03 09:23)**, mwStime.clap (116 MB, unstripped), SSL4K.clap |
| `~/.lv2` | **mw808.lv2/** (`libmw808.so`, `manifest.ttl`, `dsp.ttl`, `ui.ttl`; URI `<https://github.com/mattWoolly/mw808>`), mwStime.lv2/ |
| system | `/usr/lib/vst3/Vital.vst3`, `/usr/lib/lv2/Vital.lv2.disabled`. Nothing in `/usr/lib/clap` or `/usr/local/lib/{vst3,clap,lv2}` |

The installed mw808 binaries are `stripped` (`file`) and about 5 MB each, the result of `install-user.sh`'s strip step. Compare mwStime's 116 MB unstripped CLAP. CONFIRMED.

Possible collisions for mw909, checked one by one:

| Namespace | mw909 will use | Already present? |
|---|---|---|
| VST3 bundle | `~/.vst3/mw909.vst3` | no |
| VST3 CID | `ABCDEF019182FAEB6D7741756D773939` | no (§1.3) |
| CLAP file / id | `~/.clap/mw909.clap` / `com.mwaudio.mw909` | no |
| LV2 bundle / URI | `~/.lv2/mw909.lv2` (`libmw909.so`) / `https://github.com/mattWoolly/mw909` | no |
| REAPER caches | `~/.config/REAPER/reaper-vstplugins64.ini`, `reaper-clap-linux-x86_64.ini` | no `mw909` lines. Note there is **no mw808 line yet** either, so REAPER has never scanned mw808 (it lists mw140, MW160, mw303_Vinyl_Sim, mw303VinylSim, MW4000, mw90, mwStime, mwVnylSim, plus `mwStime.clap`). The only `mw99` grep hit under `~/.config/REAPER` is base64 text inside `Effects/Tukan/Spectrum (TUKAN).RPL`, a chance match. |
| User presets | `~/.config/mwAudio/mw909/Presets/{Kits,Patterns,Full}` | **already exists** (empty, created 13:13:xx by the scaffold's test run; see L23 in §10). Siblings: `~/.config/mwAudio/mw808/Presets/…` (08:29) and `~/.config/mwAudio/mw12F/ui.xml`. The legacy `~/.config/mw Audio/` dir also exists. |
| Standalone settings | `~/.config/mw909.settings` | no. JUCE's standalone stores `~/.config/<JucePlugin_Name>.settings` on Linux (`.deps/JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_Standalone.cpp:73-77`). `~/.config/mwStime.settings` exists; `mw808.settings` does not, because mw808's smoke test runs with a throwaway HOME. |

Nothing collides. CONFIRMED. One INFERRED caution: a host that caches by bundle name (REAPER) will treat mw909 as a new plugin, so no rescan games are needed. If the identity ever changes after a first install, delete the cache line or force a rescan (mw808 survey §12.8).

---

## 3. Build plumbing (mw808, to copy)

### 3.1 Top-level `CMakeLists.txt` (CONFIRMED, `mw808/CMakeLists.txt`)

- `cmake_minimum_required(VERSION 3.25)`, `CMAKE_EXPORT_COMPILE_COMMANDS ON` (`:4-6`).
- The version comes from `VERSION`, one line `MAJOR.MINOR.PATCH`, validated with a regex. A bad value is a `FATAL_ERROR`, and the file is added to `CMAKE_CONFIGURE_DEPENDS` (`:8-14`). mw808's `VERSION` is `0.1.0`.
- C++20, `CMAKE_CXX_EXTENSIONS OFF`. The default build type is Release (`:18-24`). No `-ffast-math` anywhere (`:26-30`).
- Options `MW808_BUILD_PLUGIN`, `MW808_BUILD_TESTS` and `MW808_BUILD_TOOLS`, all ON (`:32-34`).
- Order: `include(cmake/Deps.cmake)` → `enable_testing()` (before any `add_subdirectory`, so `catch_discover_tests` registers) → `libs/mw808-core` → `tools/mw808-render` (if present) → `mw808_fetch_juce()` + `mw808_fetch_clap_extensions()` + `plugin` (only when the plugin is built, so sanitizer presets configure without JUCE) → `mw808_fetch_catch2()` + `include(Catch)` + `tests` + `tools/ProjectChecks.cmake` (`:36-61`).
- The `install_user` custom target runs `tools/install-user.sh ${CMAKE_BINARY_DIR}` and never runs as part of a build (`:63-70`).

### 3.2 How `.deps` is consumed (CONFIRMED, `mw808/cmake/Deps.cmake`)

```cmake
set(MW808_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps")
function(mw808_use_local_dep name dir)            # :12-19
    string(TOUPPER "${name}" _upper)
    if(EXISTS "${MW808_DEPS_DIR}/${dir}/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_${_upper})
        set(FETCHCONTENT_SOURCE_DIR_${_upper} "${MW808_DEPS_DIR}/${dir}" CACHE PATH "Local checkout of ${name}")
    endif()
endfunction()
# macros mw808_fetch_catch2 (v3.7.1, shallow; appends ${catch2_SOURCE_DIR}/extras to CMAKE_MODULE_PATH),
# mw808_fetch_juce (8.0.13, shallow), mw808_fetch_clap_extensions (commit 51a9359…, not shallow)
```

- The cache variable names carry the hyphen: `FETCHCONTENT_SOURCE_DIR_CLAP-JUCE-EXTENSIONS`. mw808's `build/linux-release/CMakeCache.txt` holds exactly this, with **absolute** paths into `/home/mwoolly/projects/mw808/.deps/…`. CONFIRMED. The mw909 scaffold's cache points at `/home/mwoolly/projects/mw909/.deps/…` (scaffold snapshot). CONFIRMED.
- A missing `.deps/<dir>` falls back to the network fetch at the same pin. INFERRED from the code path; untested offline.
- `mw909/.deps/sources/` holds the PDFs and has no `CMakeLists.txt`, so Deps.cmake ignores it. The branding audit and the licence checker prune `.deps` (§6), and `.gitignore` ignores `.deps/` (`mw808/.gitignore:14`). It never ships. CONFIRMED.
- `.deps/ref-samples` (mw808: a clone of `https://github.com/tidalcycles/sounds-tr808-fischer.git` at `85fbecf`) reaches the unit tests as `MW808_REF_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/.deps/ref-samples"` (`tests/CMakeLists.txt:29-31`). **mw909 has no ref-samples yet.** Research topic 05 has to name a CC0 909 set. The tests must skip cleanly when it is absent (mw808 reads it from `BridgedTestUtil.h:427`, `MetalTestUtil.h:455`). CONFIRMED.

Dependency state in `mw909/.deps` (git, 2026-10-03), CONFIRMED:

| Dir | Tag/describe | Commit | Remote | Dirty |
|---|---|---|---|---|
| JUCE | `8.0.13` | `7c9d3783b127263d72bb65fe0a7e2dc8a02a7ac2` | juce-framework/JUCE | 0 |
| clap-juce-extensions | `0.26.0-108-g51a9359` | `51a9359315298de632cf44e9d7524940868441e6` | free-audio/clap-juce-extensions | 0; submodules `clap-libs/clap` 1.2.7 (`29ffcc2`) and `clap-libs/clap-helpers` (`a61bcdf`) populated |
| Catch2 | `v3.7.1` | `fa43b77429ba76c462b1898d6cd2f2d7a9416b14` | catchorg/Catch2 | 0 |

These match mw808's `.deps` byte for byte by commit. CONFIRMED.

### 3.3 `CMakePresets.json` (CONFIRMED, `mw808/CMakePresets.json`)

Schema 6, `cmakeMinimumRequired` 3.25. The hidden `base` sets Ninja, `binaryDir ${sourceDir}/build/${presetName}` and a Linux-host condition.

| Configure preset | Build type | Plugin | Flags |
|---|---|---|---|
| `linux-release` | Release (JUCE helper adds `-O3`; LTO on plugin targets) | ON | for installs, benchmarks, validators |
| `linux-default` | RelWithDebInfo | ON | day-to-day |
| `asan` | Debug | **OFF** | `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g` on C, CXX, exe and shared linker |
| `tsan` | Debug | **OFF** | `-fsanitize=thread -fno-omit-frame-pointer -g` |

Build presets mirror these four with no `jobs` field. Test presets: `base` has `outputOnFailure` and `noTestsAction: error`; `linux-release` and `linux-default`; `asan` (excludes label `validators`; `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`); `tsan` (excludes validators; `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`); `golden` (label `golden`, on linux-release); and `validators` (label `validators`, on linux-release).

For mw909: rename `MW808_BUILD_PLUGIN` → `MW909_BUILD_PLUGIN` in the asan and tsan presets, and change "mw808-render" in the linux-release description. **Gap:** both sanitizer presets build *without* the plugin, so the plugin shell tests (`mw808_plugin_tests`) and the UI stress tool never run under ASan through a preset. mw808 still found a use-after-free "by the stress test under ASan" (`plugin/src/PlaceholderEditor.h:9-16`), so someone built that ad hoc. The brief asks Phase-3 reviewers to run ASan+UBSan, so **add an `asan-plugin` preset**: plugin ON, Debug or RelWithDebInfo, the same flags, validators and `ui` excluded or run under xvfb. INFERRED recommendation.

### 3.4 Compile flags and layering (CONFIRMED)

- The core is three targets (`libs/mw808-core/CMakeLists.txt`): `mw808_dsp` (INTERFACE, header-only dsp + voices), `mw808_seq` (STATIC, `src/seq/*.cpp`) and `mw808_core` (STATIC, `src/*.cpp`, links both). Sources are globbed with `CONFIGURE_DEPENDS`. The static libs get `POSITION_INDEPENDENT_CODE ON`. Core warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion`. FP: `-ffp-contract=off`.
- Plugin: `juce::juce_recommended_{config,lto,warning}_flags`, plus `-flto=auto` on Release for GNU (`plugin/CMakeLists.txt:89-93`). The core include dir is added as `SYSTEM` so JUCE's `-Wfloat-equal`/`-Wswitch-enum`/`-Wsign-conversion` do not fire on core headers (`:63-67`).
- Tools: `mw808-render` links `mw808_core` and `mw808_tool_wav` with `-ffp-contract=off`. `mw808-wavcompare` links only `mw808_tool_wav` (`tools/mw808-render/CMakeLists.txt`).
- Unit tests: one executable per `tests/unit/<prefix>_<name>.cpp`. The prefix picks the library (`dsp_` → `mw808_dsp`, `seq_` → `mw808_seq`, `core_` → `mw808_core`); any other prefix is a `FATAL_ERROR`. `catch_discover_tests(... TEST_PREFIX "<name>: ")` (`tests/CMakeLists.txt`).
- `.clang-format`: LLVM base, 4-space indent, Allman for functions/classes/control (`AfterNamespace: false`), 100 columns, `PointerAlignment: Left` (`mw808/.clang-format`).

---

## 4. Format validators (CONFIRMED unless tagged)

### 4.1 Shared contract: `tools/validators/lib.sh`

- Exit 0 / 1 / 77, with `SKIP_EXIT=77` (`:5-16`).
- Build dir resolution order: `$MW808_VALIDATE_BUILD_DIR`, then argument 1, then `$MW808_BUILD_DIR`, then `build/linux-release` (`:34-42`).
- Tool cache: `${MW808_VALIDATOR_CACHE:-<repo>/build/validator-tools}` (`:20`).
- Identity constants: `MW808_PRODUCT="mw808"`, `MW808_LV2_URI="https://github.com/mattWoolly/mw808"` (`:22-24`).
- `find_artefacts` searches only under `*_artefacts/` and prunes `_deps`/`CMakeFiles`. It also skips nested build trees (any subdir with its own `CMakeCache.txt`) so a sanitizer sub-build inside a build dir is not validated by accident (`:44-64`).
- `setup_xvfb`: `xvfb-run -a` when `DISPLAY` is unset, or always when `MW808_FORCE_XVFB=1` (keeps editor windows off a developer desktop) (`:66-79`).
- `download url dest sha256` returns 0 ok, 1 offline, 2 checksum mismatch. It uses curl (`-fsSL --retry 2 --connect-timeout 20`) or wget, writes `.part` and renames only after verifying (`:83-106`).

### 4.2 `run_pluginval.sh`

| Setting | Value | Line |
|---|---|---|
| Version | `v1.0.4`, URL `https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Linux.zip` | 28-29 |
| Zip SHA-256 | `c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352` | 30 |
| Binary SHA-256 | `d3d342ea92293b0699a5c983937211e3faf2b0f38145b31cb8d78424a25e10ba` | 31 |
| Run | `--strictness-level 10 --validate-in-process --repeat 3 --randomise --timeout-ms 120000 [--disabled-tests <abs>] --validate <bundle>` | 32-36, 99-107 |
| Cache | `build/validator-tools/pluginval-v1.0.4/pluginval`; seeded from `${MW808_PLUGINVAL_SEED:-build/tools/pluginval}` if its digest matches | 51-61 |
| Waiver file | `tools/validators/pluginval-disabled-tests.txt`. **Does not exist in mw808: nothing is waived** | 19-21, 86-91 |
| Extra args | `MW808_PLUGINVAL_EXTRA` | 92-95 |

Seeds on disk with verified digests (`sha256sum`, 2026-10-03): `mw808/build/tools/pluginval` = `d3d342ea…`, `mw808/build/validator-tools/pluginval-v1.0.4/pluginval` = `d3d342ea…`. The same two files are in `mw909/build/` (scaffold copy). CONFIRMED.

### 4.3 `run_clap_validator.sh`

| Pin | URL | Archive SHA-256 | Binary SHA-256 |
|---|---|---|---|
| **0.4.1** (default) | `…/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-ubuntu-22.04.zip` (a zip holding a .tar.gz) | `49edadcfb407ea0dd946ce418300e853fbd2660fa4b0d00e4f19ff8eef24ad90` | `d9a78bfe781719de450bc5013141496eb13e1d9a538c67de1b8d325628670dbf` |
| 0.3.2 | `…/0.3.2/clap-validator-0.3.2-ubuntu-18.04.tar.gz` | `1476ed68f5657e76050e0c4f19790c02d819ecc62c35fd465059d21f05169cb1` | `d25b9f06cacfa06b1e2f6d4faf9511bd81e0c4676643c51639db7b291f017f4c` |

Select with `MW808_CLAP_VALIDATOR_VERSION`; pass extra args with `MW808_CLAP_VALIDATOR_EXTRA` (e.g. `--exclude param-fuzz`). The cache is `build/validator-tools/clap-validator-<ver>/clap-validator`. Linux x86_64 only, otherwise skip (`run_clap_validator.sh:12-48`). The cached archives `build/validator-tools/dl/cv032.tgz` and `cv041.zip` in both mw808 and mw909 hash to the pinned values. CONFIRMED.

Observed results, **UNVERIFIED for the final mw808 build**: the 06:51 scratch logs of the mw808 session (`/tmp/claude-1000/-home-mwoolly-projects-mw808/f399e79b-…/scratchpad/tools808/`) show, against an mw808 snapshot, clap-validator 0.4.1 "44 tests run, 33 passed, 0 failed, 1 warnings, 10 skipped". The warning is `state-invalid-random`: the plugin loaded random bytes successfully, because mw808 falls back to defaults rather than rejecting. 0.3.2: "21 tests run, 18 passed, 0 failed, 3 skipped". pluginval: `SUCCESS`. The README claims "pluginval at strictness 10 and clap-validator 0.4.1 (0 failures)" (`mw808/README.md:7`). The same scratch dir's `real/` logs are a run of the scripts against **mwStime** as a fixture. mwStime fails 7 clap tests (`param-fuzz-*` timeouts at 45 s and state reproducibility) and two pluginval state-restore checks plus a "Parameter thread safety" 30 s timeout. That is the failure mode mw808's shell was hardened against (§10).

### 4.4 `run_lv2_checks.sh` (no external tools needed)

It runs five stages per bundle (`:5-23`):

1. **Layout**: the manifest names `<LV2_URI>`, `lv2:Plugin` and the `lv2:binary`; every `rdfs:seeAlso` file exists.
2. **Turtle sanity** in awk: non-empty, `@prefix` present, last statement ends in `.`, and `<>`/`[]`/`()` balance outside strings and comments.
3. **Binary**: ELF shared object, `ldd` resolves, `nm -D` exports `lv2_descriptor`.
4. **dlopen** via python3 ctypes, walking `lv2_descriptor(0..63)` until the URI is found.
5. **Optional**: `lv2lint -s URI` and `sord_validate` (advisory only).

Exit 77 only when no bundle is built. The LV2 URI check reads `MW808_LV2_URI` from lib.sh, so it must become `MW909_LV2_URI`.

### 4.5 Registration: `tools/ProjectChecks.cmake`

- `license_headers` (LABEL `hygiene`, timeout 120 s) and `branding_audit --binaries ${CMAKE_BINARY_DIR}` (LABEL `hygiene`, 300 s) (`:21-27`).
- Three validator tests named `validators: pluginval VST3` (1800 s), `validators: clap-validator CLAP` (1800 s) and `validators: LV2 bundle checks` (300 s). All have `LABELS validators`, `SKIP_RETURN_CODE 77`, `RUN_SERIAL TRUE` and `ENVIRONMENT MW808_BUILD_DIR=${CMAKE_BINARY_DIR}` (`:29-45`).
- Not registered if `bash` is not found (`:13-17`).

mw808's CTest labels in use: `golden`, `host-smoke`, `hygiene`, `stress`, `ui`, `validators` (`ctest --print-labels`). 381 tests in total (`ctest -N`). There is no `tsan` or `ui-golden` label (contrast mwStime). CONFIRMED.

---

## 5. `tools/install-user.sh` behaviour (CONFIRMED, `mw808/tools/install-user.sh`)

- Usage: `tools/install-user.sh [--dry-run] [--config Release] [--formats vst3,clap,lv2] <build-dir>` or `--uninstall` (`:7-8`). `product="mw808"` is the only identity line (`:31`).
- Install root: `${MW808_INSTALL_ROOT:-$HOME}`. It refuses to run as root unless `MW808_INSTALL_ROOT` is set (`:52-61`).
- For each format it finds `*/mw808_artefacts/<config>/<VST3|CLAP|LV2>/<bundle>`, preferring the shallowest match so a nested sub-build never wins (`:130-133`). Then:
  1. `remove_installed` deletes **exactly** `<destdir>/<bundle>`. It asserts that the parent and basename are the expected ones and handles a symlink separately (`:86-102`).
  2. `cp -a`.
  3. `strip --strip-unneeded` on every `*.so` and the `.clap` in the destination copy (the build tree keeps symbols), printing sizes before and after (`:144-162`).
  4. `touch` the bundle, so mtime-caching hosts rescan (`:163`).
- Exit 0 done, 1 nothing to install or a failed step, 2 usage error. Default config is Release. Every action is echoed, and `--dry-run` prints them prefixed `[dry-run]`.
- Wired as the CMake target `install_user` (top-level `CMakeLists.txt:65-70`).

For mw909, change `product="mw909"` and the env var name. Nothing else in the script is mw808-specific. Because it removes only `~/.{vst3,clap,lv2}/mw909.*`, it cannot touch mw808. (Per the user's paranoid-deletion rule, the exact-path assertion is the right shape; keep it.) INFERRED.

---

## 6. Hygiene: licence headers and branding audit

### 6.1 Licence header checker (CONFIRMED, `mw808/tools/check_license_headers.sh`)

- It checks every `.h .hpp .cpp .cc .cmake CMakeLists.txt .sh` under `libs plugin tools tests scripts cmake`, plus the top-level `CMakeLists.txt`. Line 1 must equal `<prefix> SPDX-License-Identifier: AGPL-3.0-or-later` and line 2 `<prefix> mw808 — classic analog drum machine emulation. Copyright (C) 2026 mwAudio.`. The prefix is `//` for C/C++ and `#` otherwise; a `#!` line may come first in `.sh` files (`:5-18`, `:25-26`, `:41-52`).
- It uses **`find`, not `git`**, so it works in a non-repo tree (mw808 and mw909 are not git repos). It prunes `build*`, `.deps`, `_deps` and `node_modules` (`:15-17`, `:57`). It exits 2 if no files are found, so it cannot pass vacuously.
- For mw909 the notice becomes `mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.`. The scaffold already uses that exact string (`mw909/tools/check_license_headers.sh:26`, scaffold snapshot). CONFIRMED.
- It does **not** check `.md`, `.json`, `.txt` (patterns, tokens), `.gitignore`, `.clang-format` or `CMakePresets.json`. mw808 still puts `#` headers on `.gitignore`, `.clang-format`, `forbidden_tokens.txt` and the pattern `.txt` files by convention, but JSON cannot carry one (`cases.json` and `CMakePresets.json` have none). CONFIRMED.

### 6.2 Branding audit (CONFIRMED, `mw808/scripts/branding-audit.sh` + `scripts/forbidden_tokens.txt`)

- The text scan covers every file under the repo root except `research/`, `.deps/`, `.git/`, `build`, `build-*`, `cmake-build-*`, `node_modules`, `.cache` and the token file itself. Binary files are skipped by `grep -I`. Matching is `grep -IHnwiE`: case-insensitive, word boundaries, ERE (`:90-93`).
- `--binaries <dir>` runs `strings -a -n 4` over `*_artefacts/*/{VST3,CLAP,LV2,Standalone}/*` and over the files named **`mw808-render`** and **`mw808-wavcompare`** (hard-coded at `:111`, must be renamed). It skips `*_SharedCode.a` because random bytes produce chance matches (`:15-20`). Token lines prefixed `text:` apply to the text scan only (`:52-58`).
- Exit 0 clean, 1 hit, 2 usage or bad regex.
- The mw808 token list (`forbidden_tokens.txt:14-35`): `Roland`, `TR-[0-9]{1,3}[A-Z]?`, `text:TR[0-9]{1,3}[A-Z]?`, `CR-[0-9]{2,4}`, `text:CR[0-9]{2,4}`, `Rhythm[[:space:]_-]*Composer`, `text:ACB`, `Aira`, `Dr\.?[[:space:]_-]*Rhythm`, `Behringer`, `RD-[0-9]`, `text:RD[689]`, `Rhythm[[:space:]_-]*Designer`, `Nepheton`, `Drumazon`, `Audio[[:space:]_-]*Realism`, `Arturia`, `DrumBrute`, `Volca[[:space:]_-]*Beats`. "808" alone and "mw808" are deliberately allowed.

I tested the existing patterns against 909 spellings with the same `grep -wiE` semantics. The combined text and binary patterns were built from `mw808/scripts/forbidden_tokens.txt` exactly as `branding-audit.sh:53-58` builds them, then matched against `echo "x <spelling> x"`. CONFIRMED:

| Spelling | Caught by | Binary scan too? |
|---|---|---|
| `TR-909`, `TR-09`, `TR-8`, `TR-8S` | `TR-[0-9]{1,3}[A-Z]?` | yes |
| `TR909`, `tr909` | `text:TR[0-9]{1,3}[A-Z]?` | text only |
| `TR 909` (space) | **not caught** | |
| `RD-9`, `RD9` | `RD-[0-9]` / `text:RD[689]` | `RD9` text only |
| `Roland`, `Roland Cloud` | `Roland` | yes |
| `Drumazon`, `Nepheton`, `AudioRealism`/`Audio Realism`, `Behringer`, `Arturia` | existing | yes |
| `Softube` / `Heartbeat`, `Wave Alchemy`, `D16`, `Phoscyon`, `ReDominator`, `ADM` | **not caught** | |
| `909`, `mw909` | not caught (intended) | |

**Recommended additions for mw909** (keep all of mw808's lines):

```
text:TR[[:space:]]+[0-9]{1,3}[A-Z]?     # "TR 909"; text-only (short, binary noise risk)
Softube
Wave[[:space:]_-]*Alchemy
D16[[:space:]_-]*(Group|Audio)          # bare "D16" stays allowed: it is a diode designator on the schematic
Phoscyon                                 # D16's acid synth; same vendor family as Drumazon
ReDominator                              # AudioRealism's other drum machine
Heartbeat[[:space:]_-]*(VST|plugin|drum[[:space:]_-]*machine)   # bare "Heartbeat" is an ordinary word
# optional, text-only: text:ADM   (AudioRealism Drum Machine; review hits by hand)
```

Scaffold snapshot 13:13 (`mw909/scripts/forbidden_tokens.txt`) has already added the `TR` spaced form, `D16 (Group|Audio)`, `Wave Alchemy`, `Softube` and the qualified `Heartbeat` form. It also has a line `D16[[:space:]_-]*Heartbeat` whose comment ties Heartbeat to D16. **Heartbeat is Softube's product, not D16's**, so that line is harmless but mislabelled. Fix the comment. INFERRED from general product knowledge; the landscape research (04) should confirm it.

---

## 7. Golden renders and the test layout (CONFIRMED)

- `tests/golden/cases.json` is the single source of truth: a `_comment` array plus `cases[]`, each with `{id, args[], reference?, tol?, note}`. `args` is an `mw808-render` command line, and `@GOLDEN_DIR@` expands to the golden dir. The CMake checks: ids must match `^[a-z0-9_]+$` and be unique; args may not contain `|`, `;` or tabs. It registers one CTest `golden_<id>` (LABEL `golden`, 300 s) and writes `build/tests/golden/golden_cases.tsv` for the bless script (`tests/golden/CMakeLists.txt`). mw808 has 52 cases (51 rendered, one by `reference`), about 4.3 MB of 16-bit WAV (`blessed/MANIFEST.json`).
- `RunGoldenCase.cmake` renders into `build/.../renders/<id>.wav` and compares with `mw808-wavcompare --candidate --blessed [--tol]`. The default tolerance is 1 LSB; on failure it keeps the candidate and prints the re-render command line.
- `tools/bless_goldens.sh` refuses unless `BLESS_REASON` is set. It also honours `BLESS_ONLY` (regex subset), `BLESS_PRUNE=1`, `BLESS_NO_VERIFY=1`, `BLESS_NO_BUILD=1` (used by the `bless_goldens` target) and `BLESS_BLESSER`. It writes `blessed/MANIFEST.json`: reason, scope, date, blesser, `uname -srm`, CPU, renderer `--version`, cmake version, `VERSION`, a hash of `libs/mw808-core` sources, a hash of `tools/mw808-render` sources, counts and per-file sha256. It appends a line to `blessed/HISTORY.log`, then runs `ctest -L golden` to verify. If the TSV is older than `cases.json` it dies and asks for a re-configure. Hard-coded mw808 paths: `libs/mw808-core`, `tools/mw808-render`, `mw808-render`, `mw808-wavcompare`, `MW808_BUILD_DIR`.
- The reference platform is this box: Xeon E5-2699 v4, g++ 13.3.0, Linux 7.0.0-34 (MANIFEST). Patterns in `tests/golden/patterns/{basic,full,triplet}.txt` use the `mw808-render` pattern text format (`tools/mw808-render/PatternText.h`) with `--kit stock`, so goldens never depend on the factory library (`cases.json` `_comment`).
- mw808 blessing history (`blessed/HISTORY.log`): four blessings on 2026-10-03, the last at 12:48Z after the metal-voice review. mw909 must **not** copy `blessed/*`. Start a new corpus with a first `BLESS_REASON="initial blessing"`.
- Plugin tests (`tests/plugin/CMakeLists.txt`): `juce_add_console_app(mw808_plugin_tests)` compiles the shell **and** UI sources directly. It defines `JUCE_MODAL_LOOPS_PERMITTED=1`, `MW808_PRODUCT_NAME="mw808"` (hard-coded, `:44`), `MW808_TEST_SCRATCH_DIR`. It links with `--wrap=malloc,calloc,realloc,free,posix_memalign,aligned_alloc,memalign` and `MW808_TEST_WRAPS_MALLOC=1`, because JUCE's `HeapBlock` calls `malloc` directly (`:62-68`). Tests are discovered with prefix `plugin: `. The separate `stress: 30 s …` test (LABEL `stress`, `MW808_STRESS_SECONDS=30`) and `host-smoke: Standalone starts and quits cleanly` (`run_standalone_smoke.sh`, exit 77 skip) sit alongside.
- UI tool (`tests/ui/CMakeLists.txt`): `mw808_ui_tool`, with subcommands `screenshots`, `stress 200`, `idle 5`, `interact`, `wiring`, `lifetime 90` and `perf 3`. Each is a CTest `ui: …` (LABEL `ui`, 600 s, `RUN_SERIAL`) under `xvfb-run -a`. It also hard-codes `MW808_PRODUCT_NAME="mw808"` (`:56`). `tests/ui/DeferredInclude.cmake` is a bring-up shim, made obsolete once `tests/CMakeLists.txt:41-43` added `add_subdirectory(ui)`.

---

## 8. mw808-specific identifiers that must change (CONFIRMED by grep)

### 8.1 Strings and tags in code

| What | mw808 | mw909 | Where |
|---|---|---|---|
| State root ValueTree type | `"mw808"` | `"mw909"` | `plugin/src/state/StateCodec.h:40` (`kRootType`) |
| State version / pattern format | `kStateVersion = 1`, `kPatternFormat = 1` | keep 1 / 1 (fresh product) | `StateCodec.h:37-38` |
| Pattern blob guard | `patternSize="2196"` (= `sizeof(Pattern)`) | whatever the 909 `Pattern` is (fewer tracks, so a different size) | `StateCodec.h:10`. Blob size mismatch is refused, so mw808 sessions cannot load into mw909 (INFERRED, desirable) |
| Parse limits | `kMaxXmlDepth = 16`, `kMaxStateBytes = 16 MiB` | keep | `StateCodec.h:99-100` |
| Preset root tag | `mw808preset` | `mw909preset` | `PresetManager.cpp:22` |
| Preset extension | `.mw808` | `.mw909` | `PresetManager.h:82` |
| Preset root dir | `~/.config/mwAudio/mw808/Presets/{Kits,Patterns,Full}` (or `$MW808_PRESET_DIR`) | `…/mw909/…` (`$MW909_PRESET_DIR`) | `PresetManager.cpp:72-79` |
| Default unit seed | `kDefaultUnitSeed = 808` | `909` | `plugin/src/state/PatternModel.h:65` |
| Wordmark | `mw = "mw", num = "808"` | `"909"` | `plugin/ui/HeaderBar.cpp:268-271` |
| About title | `"mw808 " MW808_VERSION_STRING` | `mw909` | `HeaderBar.cpp:553` |
| Test RNG seeds | `juce::Random rng(808)`, `Rng rng(808U)` | any; 909 for symmetry | `tests/ui/UiTool.cpp:87`, `tests/plugin/test_parameters.cpp:227` |
| SIMD macro | `MW808_V2(...)` (35 uses) | `MW909_V2`, only if `MetalSimd.h`/`SwingVca.h` are carried over | `dsp/MetalSimd.h`, `dsp/SwingVca.h` |
| Namespaces | `mw808`, `mw808::dsp`, `::voices`, `::seq`, `::factory`, `::plugin`, `::plugin::state`, `::plugin::paramid`, `::ui`, `::tools`, `mw808Colours` | `mw909::…`, `mw909Colours` | everywhere (65× `namespace mw808…`) |
| Classes / files | `mw808Processor`, `mw808Editor`, `mw808LookAndFeel`, `mw808Colours.h`, `mw808EditorClass` | `mw909…` | `plugin/src`, `plugin/ui` |
| CMake targets | `mw808`, `mw808_dsp`, `mw808_seq`, `mw808_core`, `mw808-render`, `mw808-wavcompare`, `mw808_tool_wav`, `mw808_plugin_tests`, `mw808_ui_tool`, `mw808_test_<name>` | `mw909…` | all CMake files |
| Include root | `libs/mw808-core/include/mw808/…`, `#include "mw808/…"` | `libs/mw909-core/include/mw909/…` | |
| Licence notice text | "classic analog drum machine emulation" | "classic hybrid drum machine emulation" | every file's line 2 |

### 8.2 Environment variables and CMake/compile symbols (all `MW808_` → `MW909_`)

User-facing environment variables: `MW808_BUILD_DIR`, `MW808_VALIDATE_BUILD_DIR`, `MW808_VALIDATOR_CACHE`, `MW808_PLUGINVAL_SEED`, `MW808_PLUGINVAL_EXTRA`, `MW808_CLAP_VALIDATOR_VERSION`, `MW808_CLAP_VALIDATOR_EXTRA`, `MW808_FORCE_XVFB`, `MW808_INSTALL_ROOT`, `MW808_PRESET_DIR`, `MW808_STRESS_SECONDS`, `MW808_BENCH_SECONDS`, `MW808_SMOKE_SECONDS`, `MW808_SMOKE_INNER`, `MW808_RENDER_DIR`. `BLESS_*` stays unprefixed.

CMake options and cache: `MW808_BUILD_PLUGIN`, `MW808_BUILD_TESTS`, `MW808_BUILD_TOOLS`, `MW808_WITH_UI_SOURCES`, `MW808_BASH`, `MW808_XVFB_RUN`, `MW808_DEPS_DIR`, plus the internal globs.

Compile definitions: `MW808_PRODUCT_NAME`, `MW808_VERSION_STRING`, `MW808_USE_PLACEHOLDER_EDITOR`, `MW808_TEST_WRAPS_MALLOC`, `MW808_TEST_SCRATCH_DIR`, `MW808_REF_SAMPLES_DIR`, `MW808_SOURCE_DIR`, `MW808_VERSION`, `MW808_BUILD_TYPE`.

Shell constants: `MW808_PRODUCT`, `MW808_LV2_URI`, `MW808_REPO_ROOT`, `MW808_TOOLS_CACHE`, `MW808_TOOL_NAME`.

Counts are from `grep -rhoE 'MW808_[A-Z0-9_]+'` over the source dirs. CONFIRMED.

### 8.3 808-shaped data (not names, but must be redone)

- `Voices.h`: the 16-entry `VoiceId` enum, `kVoiceInfo`, `Hit` with 4 V normal / 4-14 V accent, and `kVoltsPerUnit = 5 V`. The brief replaces this calibration with Vpp against the 909 service-manual table.
- `MidiMap.h`: an 808 GM-ish map including congas, claves and maracas.
- `seq/Pattern.h`: `kNumTracks = 17`, 4 banks × 16 patterns, fill slots 13-16, `Scale` enum with pre-scale 1-4 plus Eighth.
- `Engine.h`: `kNumBuses = 12`, Main plus 11 individual outs.
- `ParameterIDs.h`: per-voice ids from `kVoiceInfo[v].id`.
- `Factory.cpp`, `FactoryPatterns.cpp`: 19 kits, 5 banks, 40 presets.
- `tests/golden/cases.json` and `patterns/*.txt`.

The 909 values belong to research topics 01-04. CONFIRMED that these are the 808 values.

---

## 9. File-by-file inventory of mw808, with the mw909 action

Legend:

- **as-is**: copy byte for byte.
- **copy+rename**: copy, then apply the identity renames of §8.1-8.2 and the header text. Logic is unchanged.
- **adapt**: same structure, with 909-specific edits to data or behaviour.
- **rewrite**: new content against the same contract.
- **drop**: do not carry over.

Every file except `LICENSE`, `VERSION`, `research/01-…` and `blessed/HISTORY.log` mentions `mw808`/`MW808`, so nothing else is strictly "as-is" (grep check). CONFIRMED.

### 9.1 Top level, cmake, scripts, docs

| Path | Action | mw808-specific things to change |
|---|---|---|
| `LICENSE` | as-is | AGPLv3 text |
| `VERSION` | as-is | `0.1.0` |
| `CMakeLists.txt` | copy+rename | `project(mw808 …)`, `MW808_*` vars, `libs/mw808-core`, `tools/mw808-render`, `mw808_fetch_*` |
| `CMakePresets.json` | copy+rename, plus the `asan-plugin` preset (§3.3) | `MW808_BUILD_PLUGIN` in asan/tsan; description text |
| `.gitignore` | copy+rename | header; the comment mentions `mw808-render` |
| `.clang-format` | copy+rename | header only |
| `cmake/Deps.cmake` | copy+rename | `MW808_DEPS_DIR`, `mw808_use_local_dep`, `mw808_fetch_*`. Keep the pins |
| `README.md` | rewrite | 909 content; the brief requires a note on which code was copied from mw808. Keep the section order: pitch, voices table vs manual, sequencer, MIDI, mixer, presets, interface, performance, building, layout, "What it doesn't do yet", licence |
| `docs/design/architecture.md` | adapt | The brief lists the contract fixes (noise by spectral density, Vpp calibration, `latencySamples()` slot, free-running oscillators, voice-specific weak hits, `PlayState` measure, host tempo 1..1000). Also fix the doc/code mismatch in §5.1 (L27) |
| `docs/design/plugin.md` | adapt | identity table (§1), parameter tables (909 voices), buses, preset paths, editor layout ("not a reskin") |
| `research/01..06` | rewrite | this round (mw909 topics) |
| `scripts/branding-audit.sh` | copy+rename | `-name mw808-render -o -name mw808-wavcompare` (`:111`), header |
| `scripts/forbidden_tokens.txt` | adapt | keep all mw808 lines and add the §6.2 tokens; the comment "808/909 and mw909 are allowed" |

### 9.2 `libs/mw808-core` → `libs/mw909-core`

Which 808 circuits do or do not exist in the 909 is INFERRED here from the brief's voice list (BD, SD, LT/MT/HT, RS, CP analog; CH/OH/crash/ride as 6-bit samples). Research 01/02 must confirm it.

| Path | Action | Notes |
|---|---|---|
| `CMakeLists.txt` | copy+rename, adapt | target names; consider a fourth layer for the sample/DAC chain, or keep it inside `mw909_dsp` |
| `include/mw808/Voices.h` | rewrite | 909 roster, `kVoiceInfo`, `Hit` model (909 accent: total plus per-instrument), Vpp calibration constant |
| `include/mw808/dsp/Math.h` | copy+rename | `prewarp`, `killDenormal`, `clamp`, saturator |
| `include/mw808/dsp/Filters.h` | copy+rename | TPT/ZDF filters; generic |
| `include/mw808/dsp/Noise.h` | rewrite | the brief asks for noise defined by spectral density (rate-independent) instead of unit RMS at host rate |
| `include/mw808/dsp/NoiseBusTap.h` | adapt or drop | exists to rate-compensate taps of the unit-RMS bus; probably obsolete under the new noise contract |
| `include/mw808/dsp/TriggerPulse.h` | adapt | the 909 trigger/accent path differs (research 03) |
| `include/mw808/dsp/BridgedT.h` | adapt or drop | 808 bridged-T resonator. The 909 BD is a VCO plus waveshaper. Keep it only if research 01 finds a resonator topology in the 909 SD/toms/RS |
| `include/mw808/dsp/Halfband.h` | copy+rename | polyphase IIR 2:1/4:1 decimators; reusable for oversampled analog voices and the sample chain |
| `include/mw808/dsp/MetalOscBank.h` | drop | 808 six-oscillator metal bank; the 909 metal is sampled |
| `include/mw808/dsp/MetalCommon.h` | drop (or lift single helpers) | 808 metal/noise helpers |
| `include/mw808/dsp/MetalSimd.h` | adapt or drop | v2d vector-extension filters, bit-identical to scalar; reuse if any 909 voice has parallel branches (`MW808_V2` → `MW909_V2`) |
| `include/mw808/dsp/SwingVca.h` | drop unless research shows the same VCA | 808 metal "swing-type" VCA |
| `include/mw808/voices/BassDrum.h` | rewrite | 909 BD: VCO, pitch envelope, Attack/Decay, triangle → near-sine shaper, noise click |
| `include/mw808/voices/SnareDrum.h` | rewrite | 909 SD: two oscillators plus noise, Tone/Snappy/Tune |
| `include/mw808/voices/TomConga.h` | rewrite as `Toms.h` | no congas on the 909; pitch sweep plus noise |
| `include/mw808/voices/RimShot.h` | rewrite | 909 RS |
| `include/mw808/voices/HandClap.h` | adapt | the noise-burst clap structure carries over; research 01 to state the differences from the 808 |
| `include/mw808/voices/HiHats.h` | rewrite | 6-bit sample playback, DAC, filter, VCA. Keep the choke-as-circuit idea and the API shape (`triggerOpen` / `triggerClosed`) |
| `include/mw808/voices/Cymbal.h` | rewrite as crash/ride sample voices | |
| `include/mw808/voices/{Claves,Maracas,Cowbell}.h` | drop | not on the 909 |
| `include/mw808/Engine.h`, `src/Engine.cpp` | adapt | voice set, bus count (909 individual outs per research 03), output stage, always-render rule for free-running voices |
| `include/mw808/Machine.h`, `src/Machine.cpp` | copy+rename, adapt | Machine/queues/`postBank` generation handoff carry over. `PlayState` already has `measure` (`Machine.h:107`); keep it |
| `include/mw808/MidiMap.h` | adapt | 909 GM map (BD 36, RS 37, SD 38, CP 39, LT 41/43, CH 42, MT 45/47, OH 46, HT 48/50, crash 49, ride 51), per research 04 |
| `include/mw808/Factory.h` | copy+rename | data types |
| `src/Factory.cpp` | rewrite | kits/presets for 909 |
| `src/FactoryPatterns.cpp` | adapt | keep the step-string notation and its compile-time checker; rewrite the data (house, techno, acid, garage, gabber, electro, breaks…) |
| `include/mw808/seq/Pattern.h` | adapt | tracks = 909 voices + accent; add last-step and per-instrument accent; scale per the 909 |
| `include/mw808/seq/Sequencer.h`, `src/seq/Sequencer.cpp` | adapt | add 909 shuffle/flam semantics, tap write, track/song mode. Keep the host-PPQ derivation, relocation and pending-queue machinery |
| `include/mw808/seq/PatternOps.h`, `src/seq/PatternOps.cpp` | copy+rename | voice-agnostic; check the OH/CH exclusivity rule in `randomizePattern` |
| `include/mw808/seq/MidiFile.h`, `src/seq/MidiFile.cpp` | copy+rename | voice-agnostic SMF I/O |
| `include/mw808/util/SpscQueue.h` | copy+rename | |

### 9.3 `plugin/`

| Path | Action | Notes |
|---|---|---|
| `CMakeLists.txt` | copy+rename | identity lines (§1.1), `MW808_*`, description |
| `src/mw808Processor.{h,cpp}` | copy+rename, adapt | bus layout; everything in §10 L4-L13 carries over unchanged |
| `src/ParameterIDs.h` | adapt | 909 ids; frozen from mw909's first release |
| `src/Parameters.{h,cpp}` | adapt | 909 parameter set (per-instrument accent, total accent, shuffle/flam, hat decay…) |
| `src/MidiLearn.{h,cpp}` | copy+rename | |
| `src/PresetManager.{h,cpp}` | copy+rename | tag, extension, dir, env var |
| `src/PlaceholderEditor.h` | copy+rename | keep: it is the safe stand-in (L4) |
| `src/state/PatternModel.{h,cpp}` | copy+rename | `kDefaultUnitSeed` |
| `src/state/StateCodec.{h,cpp}` | copy+rename | `kRootType` |
| `ui/mw808Colours.h` | rewrite | a 909 palette; the brief says it must not feel like a reskin |
| `ui/mw808LookAndFeel.{h,cpp}` | adapt | drawing vocabulary reusable; new look |
| `ui/mw808Editor.{h,cpp}` | adapt | 1280×720 scale-transform shell reusable |
| `ui/HeaderBar.{h,cpp}` | adapt | wordmark `"909"`, about box |
| `ui/VoicePanel.{h,cpp}` | rewrite | 909 panel layout |
| `ui/SequencerStrip.{h,cpp}`, `ui/StepEdit.h`, `ui/GridView.{h,cpp}` | adapt | 909 sequencer controls (last step, scale, shuffle/flam, tap write, per-track playhead from `PlayState::measure`, which mw808's GRID does not use) |
| `ui/MixerView.{h,cpp}` | adapt | voice count, outs |
| `ui/UiCommon.{h,cpp}` | copy+rename | shared plumbing; 2 voice-specific mentions |

### 9.4 `tools/`

| Path | Action | Notes |
|---|---|---|
| `ProjectChecks.cmake` | copy+rename | |
| `check_license_headers.sh` | copy+rename | notice text |
| `install-user.sh` | copy+rename | `product=`, `MW909_INSTALL_ROOT` |
| `bless_goldens.sh` | copy+rename | paths, binaries, `MW909_BUILD_DIR` |
| `validators/lib.sh` | copy+rename | `MW909_PRODUCT`, `MW909_LV2_URI`, env vars |
| `validators/run_pluginval.sh`, `run_clap_validator.sh`, `run_lv2_checks.sh` | copy+rename | keep pins and checksums identical |
| `mw808-render/CMakeLists.txt` | copy+rename | |
| `mw808-render/{Wav.h,Wav.cpp,WavCompare.cpp,Common.h,Common.cpp}` | copy+rename | generic |
| `mw808-render/main.cpp` | copy+rename, adapt | help text, voice list |
| `mw808-render/Render.cpp` | adapt | voice subcommand over the 909 roster |
| `mw808-render/Bench.cpp` | adapt | "all 16 voices" worst case becomes the 909 set |
| `mw808-render/PatternText.{h,cpp}` | adapt | voice codes in the text format |

### 9.5 `tests/`

| Path | Action | Notes |
|---|---|---|
| `CMakeLists.txt` | copy+rename | `MW909_REF_SAMPLES_DIR` → `.deps/ref-samples` (909 set TBD) |
| `golden/CMakeLists.txt`, `golden/RunGoldenCase.cmake` | copy+rename | |
| `golden/cases.json` | rewrite | 909 voices: stock/44k/48k, accent ladders, the sample voices at several tunings, patterns incl. shuffle/flam |
| `golden/patterns/{basic,full,triplet}.txt` | rewrite | 909 voice codes |
| `golden/blessed/*.wav` (51 files), `MANIFEST.json`, `HISTORY.log` | drop | bless afresh |
| `plugin/CMakeLists.txt` | copy+rename | hard-coded `MW808_PRODUCT_NAME="mw808"` (`:44`); **add `MW909_PRESET_DIR` to the test ENVIRONMENT** (L23) |
| `plugin/{AllocSpy.h,AllocSpy.cpp,TestSupport.h,TestSupport.cpp}` | copy+rename | |
| `plugin/run_standalone_smoke.sh` | copy+rename | |
| `plugin/test_{midilearn,stress,bench}.cpp` | copy+rename | |
| `plugin/test_{parameters,patterns,presets,processing,state}.cpp` | adapt | voice ids, parameter list, tag names (`mw909preset`), seed literals |
| `ui/CMakeLists.txt` | copy+rename | hard-coded product name (`:56`); add `MW909_PRESET_DIR` |
| `ui/DeferredInclude.cmake` | drop | bring-up shim, superseded (§7) |
| `ui/UiTool.cpp` | adapt | views and controls of the new editor |
| `unit/CoreTestUtil.h`, `unit/SeqTestUtil.h` | copy+rename | |
| `unit/dsp_filters.cpp` | copy+rename | |
| `unit/dsp_metal_halfband.cpp` | copy+rename if `Halfband.h` kept | |
| `unit/BridgedTestUtil.h` | adapt | render rig, decay/pitch measurement and reference-sample loader are reusable. Rename it (e.g. `AnalogTestUtil.h`) |
| `unit/MetalTestUtil.h` | adapt | measurement helpers reusable for sample voices |
| `unit/dsp_bridged_prims.cpp`, `unit/dsp_metal_{chain,oscbank}.cpp` | drop (unless their primitives are kept) | |
| `unit/dsp_voice_{bd,sd,tom,rs,cp,hats,cy}.cpp` | rewrite | same test *shapes* (pitch/decay/level across 44.1-192 kHz, retrigger never clicks, sleep/wake, determinism, NaN-safe `setParams`) against 909 numbers |
| `unit/dsp_voice_{cb,cl,ma}.cpp` | drop | |
| `unit/seq_{timing,transport,robust,midifile,patternops}.cpp` | copy+rename | |
| `unit/seq_{steps,oracle,reference}.cpp` | adapt | the oracle and second reference are the model for verifying 909 shuffle/flam |
| `unit/core_{engine,machine,factory}.cpp` | adapt | |

### 9.6 Not in mw808, needed in mw909

- `.deps/ref-samples` (a 909 CC0 set; research 05).
- A sample/DAC chain module.
- An `asan-plugin` preset.
- A `MW909_PRESET_DIR` env entry on the plugin and UI tests.
- Optionally `tools/validators/pluginval-disabled-tests.txt`. Create it only with a tracked reason; mw808 needed none.

---

## 10. Lessons visible in mw808's code, comments and logs

All CONFIRMED from the cited lines unless tagged.

| # | Lesson | Where |
|---|---|---|
| L1 | Do **not** set `CLAP_PROCESS_EVENTS_RESOLUTION_SAMPLES`. With it, the CLAP wrapper splits host blocks at parameter events, and every sub-call reports the block-start transport position, so Host Sync re-anchors at each split. | `plugin/CMakeLists.txt:95-99` |
| L2 | JUCE's bare `-flto` links partitions serially; add `-flto=auto` on Release for GNU. | `plugin/CMakeLists.txt:89-93` |
| L3 | Include the core as `SYSTEM` in JUCE-linked targets. JUCE's warning flags otherwise flag deliberate exact float compares in core headers. | `plugin/CMakeLists.txt:63-67` |
| L4 | Never use `juce::GenericAudioProcessorEditor`. Its rows are `AudioProcessorListener`s that JUCE's `ParameterChangeForwarder` walks without a lock, so opening or closing it during audio-thread automation is a use-after-free, found under ASan. Attach controls through APVTS attachments. | `plugin/src/PlaceholderEditor.h:9-16` |
| L5 | The VST3 wrapper delivers program changes on the audio thread. Record them in an atomic and apply them on the 30 Hz timer. | `mw808Processor.h` thread notes; `.cpp:359-360` |
| L6 | `prepareToPlay` takes a "process gate" that the audio thread only *tries*. A colliding block outputs silence. `reset()` just raises a flag. | `mw808Processor.h` header comment; `.cpp:43-56,198-199` |
| L7 | CLAP: after `setStateInformation`, call `updateHostDisplay(ChangeDetails().withProgramChanged(true))` (deferred to the message thread if needed). The wrapper turns it into a values rescan. Without it clap-validator's `state-reproducibility-*` fails, as it does for mwStime. | `.cpp:516-525`; mwStime failure in mw808 scratch `real/cv.log` (UNVERIFIED as cause) |
| L8 | APVTS ignores a value it considers "approximately equal". An exact restore nudges such a parameter to the *nearest* non-ignored value and back, never to a range end, because a block may run in between. | `.cpp:450-457` |
| L9 | State blobs: check JUCE's declared XML length against the buffer (the parser accepts truncated docs). Require valid UTF-8 (`getXmlFromBinary` asserts otherwise). Refuse a DOCTYPE (entity expansion, file reads). Bound the nesting depth to 16 (recursive parser stack) and the size to 16 MiB. Unreadable or future versions fall back to a known-good default state, never half-applied. | `.cpp:491-514`; `StateCodec.h:99-100` and comments |
| L10 | MIDI out: cap note-ons per block. Pass on a note-off only for notes actually sent. `ensureSize` allocates only when the host reserved too little: VST3/CLAP reserve enough, LV2 more, the Standalone nothing (one allocation on its first MIDI block). | `.cpp:272-280` |
| L11 | Bypass must send note-offs for notes MIDI Out still holds. | `.cpp:316-319` |
| L12 | Hosts probe programs with bad indices and wrappers re-send the current one; both must be no-ops. | `.cpp:351` |
| L13 | Without a host transport (Standalone, some offline renders) Host Sync behaves as Internal, so Run always works. | `.cpp:259-260`; `plugin.md` §2 |
| L14 | State restores arrive on foreign threads. `PatternModel` hands the bank to the audio thread at once (so an offline bounce right after load plays it) and stages a copy for the message thread to adopt. `capture()` serves the staged copy so save-after-load is exact. | `plugin/src/state/PatternModel.h:16-31` |
| L15 | The allocation spy must wrap `malloc`/`calloc`/`realloc`/`free`/`posix_memalign`/`aligned_alloc`/`memalign` at link time (`-Wl,--wrap`), not only `operator new`, because JUCE's `HeapBlock` calls malloc. The test checks block sizes 1-8192 with zero allocations. | `tests/plugin/CMakeLists.txt:62-68`; `README.md` Performance |
| L16 | Under Xvfb with no window manager, use plain titled windows. "Temporary" windows need WM atoms that may not exist. | `tests/ui/UiTool.cpp:39-41` |
| L17 | pluginval's default 30 s per-test timeout trips on this loaded shared box during editor tests, so use 120 s. `--disabled-tests` needs an absolute path. mw808 ships no waiver. | `tools/validators/run_pluginval.sh:19-21,34-36,86-91` |
| L18 | Validators run `RUN_SERIAL`, skip nested build trees, and only download a tool after finding an artefact to test. | `ProjectChecks.cmake:43`; `lib.sh:44-64`; `run_pluginval.sh:44-48` |
| L19 | Branding binary scans produce chance hits on short codes in random bytes. Keep short tokens `text:`-only and do not scan `*_SharedCode.a`. | `forbidden_tokens.txt:10-12`; `branding-audit.sh:15-20` |
| L20 | Licence checking with `git ls-files` passes vacuously in a non-repo (the lesson from mwStime). mw808 uses `find` and exits 2 when nothing is found. | `check_license_headers.sh:15-18,64-67` |
| L21 | The audio thread runs with FTZ/DAZ (`juce::ScopedNoDenormals`, `.cpp:193`). `killDenormal` remains as a guard for tests and tools that run without it. | `dsp/Math.h:45-47` |
| L22 | `std::atomic<T>` only for T ≤ 8 bytes: 16-byte atomics are not lock-free here. | `docs/design/architecture.md` §1 |
| L23 | The Standalone smoke runs under a throwaway `HOME`/`XDG_CONFIG_HOME` so it never writes `~/.config`. **But the plugin tests and UI tool do not set `MW808_PRESET_DIR`.** `~/.config/mwAudio/mw808/Presets/{Kits,Patterns,Full}` was created 2026-10-03 08:29:08, and `…/mw909/Presets/…` at 13:13 while the scaffold's tests ran. INFERRED cause: a processor or editor in a test calls `PresetManager::refresh()` → `createDirectory()` on the default root. Fix in mw909: set `MW909_PRESET_DIR=${CMAKE_BINARY_DIR}/test-presets` in those tests' `ENVIRONMENT`. | `run_standalone_smoke.sh:5-7,35-39`; `PresetManager.cpp:72-79,93-98`; directory mtimes |
| L24 | Golden blessing is gated (`BLESS_REASON`) and recorded (MANIFEST + HISTORY). The CMake-generated TSV means the bless script needs no JSON parser. A stale TSV is refused. | `tools/bless_goldens.sh` |
| L25 | `FETCHCONTENT_SOURCE_DIR_*` are cached as absolute paths into the project's own `.deps`. Never copy a configured build dir from mw808 to mw909; configure fresh. (The scaffold did configure fresh: its cache points into `mw909/.deps`.) | `mw808/build/linux-release/CMakeCache.txt`; INFERRED risk |
| L26 | `ctest -N` (listing only) still rewrites `<build>/Testing/Temporary/LastTest.log`. This survey ran it once in `mw808/build/linux-release` at 13:14 and overwrote that 121-byte log. No source was touched. To inspect mw808 without writing to it, read `CTestTestfile.cmake` instead. | observed |
| L27 | Docs lag the code. `architecture.md` §5.1 says "Clamp to 20..400", but `Sequencer.cpp:34-35,54-55` already converts Host Sync at the host tempo over 1..1000 BPM and clamps only `tempo()` and the internal clock to 20..400, which is what the brief asks for. Write mw909's architecture.md from the code. | cited lines |
| L28 | Engine "now always renders the metal voices" (free-running oscillators keep moving while asleep) was a late review fix that forced a full re-bless. Bake it into the contract from day one; the brief says so too. | `tests/golden/blessed/HISTORY.log` (12:48Z) |
| L29 | The editor is deliberately vector-only (no images, OpenGL or WebView), with static layers cached per scale. With the sequencer running it redraws at 30 fps but repaints about 1 % of the window per frame. | `plugin.md` §7; `README.md` Interface |
| L30 | The last full ctest in mw808 (13:02) recorded `branding_audit` as failed (`Testing/Temporary/LastTestsFailed.log`). Re-running `scripts/branding-audit.sh --binaries build/linux-release` at 13:15 passes (209 text files, 10 binaries clean). Most likely a doc was mid-edit; `docs/` mtime is 13:02. Re-run hygiene last, after docs settle. | UNVERIFIED cause |
| L31 | `MW_PRODUCT_NAME` lives in `plugin/CMakeLists.txt` directory scope, so the test CMake files cannot see it and hard-code the product name. Hoisting it to the top-level `CMakeLists.txt`, as mw808's own research §13.1 suggested, would make the rename one line. | `plugin/CMakeLists.txt:8-12`, `tests/plugin/CMakeLists.txt:44`, `tests/ui/CMakeLists.txt:56` |

---

## 11. Brief contract fixes, mapped to where mw808 has the weakness

| Brief item | mw808 location today | Status in mw808 |
|---|---|---|
| Noise by spectral density | `dsp/Noise.h:13` "unit RMS"; `NoiseBusTap.h` compensates per voice | weakness present. CONFIRMED |
| Vpp output calibration | `Voices.h` `kVoltsPerUnit = 5.0f` (5 V peak) | weakness present. CONFIRMED |
| `latencySamples()` slot | `architecture.md` §3 "oversampled voices expose `latencySamples()`… engine does not compensate"; `Cymbal.h:64` | ad hoc per voice, not in the generic contract. CONFIRMED |
| Free-running oscillators rendered every block | late fix (L28) | fixed in mw808, so put it in the contract |
| Voice-specific weak hits | `Voices.h` `Hit` comment: "monotonic… down to ~0.5 V" | weakness present. CONFIRMED |
| Measure count in `PlayState` | `Machine.h:107` `measure` exists; `SequencerStrip.cpp:741` uses it, `GridView` does not (README "doesn't do yet") | half done. CONFIRMED |
| Host tempo 1..1000 | `Sequencer.cpp:34-35` already does it; the doc is stale (L27) | code fine, doc wrong. CONFIRMED |

---

## 12. Open questions

1. **Sample-voice licensing** (brief Phase 1 §2) decides whether `.deps` needs more than the CC0 references, and whether any generated sample data is committed. Committed data must pass the branding audit (filenames!). It is not a conventions question, but it affects the layout.
2. **Bus count.** mw808 has Main plus 11 outs. The number of 909 individual outputs (research 03) sets `kNumBuses` and the `Out 2..N` names.
3. **Fourth core layer.** Should the sample/DAC chain be its own CMake target (e.g. `mw909_pcm`) with its own `pcm_` test prefix? The `tests/CMakeLists.txt` prefix rule makes that a two-line change.
4. **`asan-plugin` preset.** Should it exist officially (§3.3), and should the `ui` label run under it?
5. **Branding tokens.** Should `ADM` (text-only) and `Heartbeat` (bare) be forbidden despite false-positive risk? The current recommendation is no to bare `Heartbeat` and optional for `text:ADM`.
6. **Header text in Markdown.** mw808 code uses an em dash, Markdown a colon. The checker ignores `.md`, so both pass. Pick one for mw909's docs. This research file follows the task's colon form.
7. **mw808 REAPER visibility.** REAPER has never scanned mw808 (no cache line). If the user expects the pair side by side in REAPER, a rescan is needed after mw909 installs. This is the user's action and not automated: scripts must never launch the REAPER GUI (mw808 survey §8.5).

Scratch files for this survey: `/tmp/claude-1000/mw909-research-conventions/` (`codes_raw.txt`, `bytescan.py`, `bytescan.txt`, `strings_report.txt`).
