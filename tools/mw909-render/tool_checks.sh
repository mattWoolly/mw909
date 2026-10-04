#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# tool_checks.sh <mw909-render> <mw909-wavcompare> <scratch-dir> <golden-dir>
#
# Behavioural checks of the command-line tools (CTest "tools: ...", LABEL
# tools). Every check prints one line; the script stops at the first failure.
#   * help for every topic, exit codes for a bad command / option / value
#   * list output (memories, kits, presets, voices, patterns)
#   * every voice renders, twice, byte for byte the same
#   * knobs by id, accent codes, revision, seed/variance, Synth source, a
#     user ROM import: each changes the render
#   * block-partition independence (voice and pattern, 1 / 7 / 512 samples)
#   * --print-text round trip renders identically (polymeter and flams)
#   * pattern sources: memory/pattern by name, preset, text file, chain, song
#   * stems: one file per voice; a pattern's played voices non-silent; a
#     voice render's own stem non-silent and every other stem silent
#   * --keep-latency: the file is the latency (32 samples at 44.1 kHz)
#     longer and is the plain render delayed by it
#   * MIDI export with its read-back check, notes-only export, import
#   * compare: identical -> 0, different -> 1, NaN/Inf -> 1, via both front ends
#   * --seed is decimal (010 = 10), --tempo limited to the text format's range
#   * bench runs (short, no gate)

set -euo pipefail

render="$1"
compare="$2"
work="$3"
golden="$4"
rm -rf "${work}"
mkdir -p "${work}/stems"
cd "${work}"

n=0
pass() { n=$((n + 1)); printf 'ok %2d  %s\n' "${n}" "$*"; }
die() { printf 'FAIL   %s\n' "$*" >&2; exit 1; }
expect_rc() { # <rc> <command...>
    local want="$1" rc=0
    shift
    "$@" >/dev/null 2>&1 || rc=$?
    [ "${rc}" -eq "${want}" ] || die "expected exit ${want}, got ${rc}: $*"
}
same() { cmp -s "$1" "$2" || die "$3: $1 and $2 differ"; }
differ() { ! cmp -s "$1" "$2" || die "$3: $1 and $2 are identical"; }
R() { "${render}" "$@"; }
# peak_db <file.wav>: the file's peak in dBFS (-999.00 = digital silence), as
# the comparer reports it.
peak_db() {
    "${compare}" --quiet --candidate "$1" --blessed "$1" > peak.txt || die "cannot read $1"
    sed -n 's/^  blessed  : peak \([-0-9.]*\) dBFS.*/\1/p' peak.txt
}
silent() { [ "$(peak_db "$1")" = "-999.00" ]; }
audible() { # <file> : peak above -60 dBFS
    local p
    p="$(peak_db "$1")"
    [ -n "${p}" ] && awk -v p="${p}" 'BEGIN { exit !(p > -60) }'
}
V() { R voice --quiet --format 32f --rate 44100 "$@"; }

# --- command line ---------------------------------------------------------------
for t in "" voice pattern midi bench compare list set pattern-format; do
    R help ${t:+"${t}"} > help.txt
    [ -s help.txt ] || die "help ${t} printed nothing"
done
R --version > version.txt
grep -q '^mw909-render ' version.txt || die "--version"
expect_rc 2 R
expect_rc 2 R frobnicate
expect_rc 2 R voice --voice xx -o x.wav
expect_rc 2 R voice --voice bd --nosuchknob 1 -o x.wav
expect_rc 2 R voice --voice bd --decay 2 -o x.wav
expect_rc 2 R pattern --memory 0 --pattern nosuchpattern -o x.wav
expect_rc 2 R pattern --memory 99 -o x.wav
expect_rc 1 R pattern --text does-not-exist.txt -o x.wav
expect_rc 2 R pattern --memory 0 --pattern I-1-1 --tempo 500 --print-text
expect_rc 2 R voice --voice bd --rate 384000 --length 1500 --format 32f --channels 2 -o x.wav
pass "help topics and exit codes"

# (Output goes to a file first: `grep -q` on a pipe would cut the writer off.)
R list > list.txt
grep -q 'factory memories' list.txt || die "list"
R list voices > voices.txt
[ "$(grep -cE '^  [a-z]{2} ' voices.txt)" -eq 11 ] || die "list voices: expected 11 voices"
R list patterns 0 > patterns.txt
grep -q 'I-1-1' patterns.txt || die "list patterns"
pass "list"

# --- voices -----------------------------------------------------------------------------
for v in bd sd lt mt ht rs cp ch oh cr rd; do
    V --voice "${v}" --hits normal,full --spacing 0.2 -o "v_${v}.wav"
    [ "$(stat -c %s "v_${v}.wav")" -gt 1000 ] || die "voice ${v}: empty file"
    audible "v_${v}.wav" || die "voice ${v}: silent render"
    V --voice "${v}" --hits normal,full --spacing 0.2 -o "v_${v}_again.wav"
    same "v_${v}.wav" "v_${v}_again.wav" "determinism of ${v}"
done
pass "all 11 voices render audibly, twice, byte-identical"

V --voice bd --hits normal,full --spacing 0.2 --decay 0.9 --attack 1 -o k1.wav
differ v_bd.wav k1.wav "BD knobs by id"
V --voice bd --hits normal,full --spacing 0.2 --knob decay=0.9 --knob attack=1 -o k2.wav
same k1.wav k2.wav "--knob id=value is --<id> value"
V --voice bd --hits normal,full --spacing 0.2 --set bd.decay=0.9 --set bd.attack=1 -o k3.wav
same k1.wav k3.wav "--set bd.<knob>"
V --voice bd --hits c0,c24,c63 --spacing 0.2 -o codes.wav
V --voice bd --hits 0,1.9047619,5 --spacing 0.2 -o volts.wav
same codes.wav volts.wav "accent codes == their volts"
V --voice bd --hits normal,full --spacing 0.2 --revision early -o early.wav
differ v_bd.wav early.wav "BD Early revision"
V --voice sd --hits normal,full --spacing 0.2 --variance 1 --seed 7 -o var7.wav
V --voice sd --hits normal,full --spacing 0.2 --variance 1 --seed 8 -o var8.wav
V --voice sd --hits normal,full --spacing 0.2 --variance 1 --seed 010 -o var010.wav
V --voice sd --hits normal,full --spacing 0.2 --variance 1 --seed 10 -o var10.wav
V --voice sd --hits normal,full --spacing 0.2 --variance 1 --seed 0xa -o var0xa.wav
same var010.wav var10.wav "--seed 010 is decimal ten"
same var0xa.wav var10.wav "--seed 0xa is ten"
differ var8.wav var10.wav "SD unit seed 8 vs 10"
expect_rc 2 R voice --voice sd --seed 1e3 -o x.wav
differ v_sd.wav var7.wav "SD variance"
differ var7.wav var8.wav "SD unit seed"
V --voice cr --hits normal --spacing 0.2 --source synth -o synth.wav
V --voice cr --hits normal --spacing 0.2 -o rom.wav
differ rom.wav synth.wav "CR Synth source"
V --voice rd --hits normal --spacing 0.2 --user-rom rom.wav -o user.wav
V --voice rd --hits normal --spacing 0.2 -o rd_rom.wav
differ rd_rom.wav user.wav "RD user ROM"
pass "knobs, accent codes, revision, variance/seed, Synth and User sources"

# --keep-latency: 32f stereo, header 58 bytes; the latency at 44.1 kHz is 32
# samples = 256 bytes. The kept file is the plain render delayed by it.
V --voice bd --hits normal,full --spacing 0.2 --keep-latency -o keep.wav
plain_bytes="$(stat -c %s v_bd.wav)"
keep_bytes="$(stat -c %s keep.wav)"
[ "$((keep_bytes - plain_bytes))" -eq 256 ] || die "--keep-latency: ${keep_bytes} vs ${plain_bytes} bytes (want +256)"
data_bytes=$((plain_bytes - 58))
cmp -s -n "${data_bytes}" keep.wav v_bd.wav $((58 + 256)) 58 || die "--keep-latency: not the plain render delayed by 32 samples"
[ "$(od -A n -v -t x1 -j 58 -N 256 keep.wav | tr -d ' \n' | tr -d 0)" = "" ] || die "--keep-latency: the 32-sample pre-roll is not silent"
pass "--keep-latency: +32 samples, the plain render delayed"

V --voice oh --hits normal,full --spacing 0.2 --block 1 -o b1.wav
V --voice oh --hits normal,full --spacing 0.2 --block 512 -o b512.wav
same b1.wav b512.wav "voice block partition"
R pattern --quiet --format 32f --rate 44100 --memory 0 --pattern I-1-1 --measures 1 --tempo 200 --tail 0 --block 7 -o p7.wav
R pattern --quiet --format 32f --rate 44100 --memory 0 --pattern I-1-1 --measures 1 --tempo 200 --tail 0 --block 512 -o p512.wav
same p7.wav p512.wav "pattern block partition"
pass "block-partition independence (1/512, 7/512)"

# --- patterns ------------------------------------------------------------------------------
for spec in "7|Three Over Four" "0|Wide Flams" "0|Triplet Gallop"; do
    mem="${spec%%|*}"
    pat="${spec#*|}"
    R pattern --memory "${mem}" --pattern "${pat}" --print-text > rt.txt
    R pattern --quiet --format 32f --rate 44100 --memory "${mem}" --pattern "${pat}" --measures 2 --tempo 200 --tail 0 -o a.wav
    R pattern --quiet --format 32f --rate 44100 --text rt.txt --measures 2 --tempo 200 --tail 0 -o b.wav
    same a.wav b.wav "text round trip of ${pat}"
done
pass "--print-text round trips (polymeter, flams, triplets)"

R pattern --quiet --format 16 --rate 44100 --preset "Classic Four" --measures 1 -o preset.wav
R pattern --quiet --format 16 --rate 44100 --memory 0 --chain --measures 2 --tempo 240 -o chain.wav
R pattern --quiet --format 16 --rate 44100 --text "${golden}/patterns/song.txt" --song-track 1 --measures 4 --tempo 240 -o song.wav
R pattern --quiet --format 16 --rate 44100 --text "${golden}/patterns/full.txt" --variation AB --measures 2 --jitter-seed 3 -o full.wav
R pattern --quiet --format 16 --rate 44100 --memory 0 --pattern I-1-1 --measures 1 --mix typical -o typical.wav
pass "pattern sources: preset, chain, song track, text file, typical mix"

R pattern --quiet --format 32f --rate 44100 --memory 0 --pattern "Voice Check" --measures 1 --tempo 200 -o vc.wav --stems stems
[ "$(ls stems/*.wav | wc -l)" -eq 11 ] || die "stems: expected 11 files"
for v in bd sd lt mt ht rs cp ch oh cr rd; do
    audible "stems/${v}.wav" || die "pattern stem ${v} is silent (Voice Check plays every voice)"
done
mkdir -p vstems
V --voice sd --hits normal,full --spacing 0.2 --stems vstems -o vs.wav
[ "$(ls vstems/*.wav | wc -l)" -eq 11 ] || die "voice stems: expected 11 files"
for v in bd sd lt mt ht rs cp ch oh cr rd; do
    if [ "${v}" = sd ]; then
        audible "vstems/${v}.wav" || die "voice stem sd is silent"
    else
        silent "vstems/${v}.wav" || die "voice stem ${v} is not silent (only sd was played)"
    fi
done
pass "stems: 11 files; pattern stems audible; a voice render's other stems silent"

# --- MIDI ------------------------------------------------------------------------------------
R midi --quiet --memory 0 --pattern "Wide Flams" -o wf.mid
R midi --quiet --memory 7 --pattern 0 --variation B --no-embed -o pv.mid
R midi --read pv.mid > imported.txt
grep -q '^BD ' imported.txt || die "midi --read"
[ "$(head -c 4 wf.mid)" = "MThd" ] || die "not a MIDI file"
pass "MIDI export (exact read-back), notes-only export, import"

# --- compare ---------------------------------------------------------------------------------
expect_rc 0 "${compare}" --candidate v_bd.wav --blessed v_bd_again.wav
expect_rc 1 "${compare}" --candidate v_bd.wav --blessed k1.wav
expect_rc 0 R compare --candidate v_bd.wav --blessed v_bd_again.wav
expect_rc 1 R compare --candidate v_bd.wav --blessed k1.wav
expect_rc 2 "${compare}" --candidate nope.wav --blessed v_bd.wav
# NaN / infinity never match, in either direction (32f WAV: header 58 bytes).
cp v_bd.wav nan.wav
printf '\x00\x00\xc0\x7f' | dd of=nan.wav bs=1 seek=$((58 + 4000)) conv=notrunc status=none
cp v_bd.wav inf.wav
printf '\x00\x00\x80\x7f' | dd of=inf.wav bs=1 seek=$((58 + 4000)) conv=notrunc status=none
expect_rc 1 "${compare}" --candidate nan.wav --blessed v_bd.wav
expect_rc 1 "${compare}" --candidate v_bd.wav --blessed nan.wav
expect_rc 1 "${compare}" --candidate nan.wav --blessed nan.wav
expect_rc 1 "${compare}" --candidate inf.wav --blessed v_bd.wav --tol 1
expect_rc 1 R compare --candidate nan.wav --blessed v_bd.wav --tol 1
pass "compare: identical, different, unreadable, NaN/Inf never pass"

R bench --seconds 0.2 --repeats 1 --no-gate > bench.txt
grep -q 'dense, no strips/FX' bench.txt && grep -q 'silent engine' bench.txt || die "bench output"
pass "bench (short run, no gate)"

echo "tool checks: all ${n} passed"
