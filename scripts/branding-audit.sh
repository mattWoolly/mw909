#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# branding-audit.sh — fails when a forbidden brand token (scripts/forbidden_tokens.txt)
# appears in the project's text files, in the strings of built binaries, in
# the registered test names, or in the factory content names.
#
# Usage:
#   scripts/branding-audit.sh [--binaries <build-dir>]... [--all-builds] [repo-root]
#
#   tree scan    every file under the repo root except research/ (sources
#                may be cited there), .deps/, build*/, .git/ and the token
#                file's contents:
#                * file and directory names (relative paths), all files;
#                * text files (source, UI strings, presets, pattern files,
#                  scripts, CMake, docs, README, tests, tools): every line;
#                * binary files (blessed WAVs, ROM sources, images): their
#                  `strings`, 8-bit and UTF-16LE (metadata chunks such as a
#                  WAV INFO/ICOP), against the full tokens only.
#   --binaries   for a build directory, also scan:
#                * `strings` (8-bit and UTF-16LE) of the shipped plugin artefacts (everything under
#                  *_artefacts/<config>/{VST3,CLAP,LV2,Standalone}/: .so,
#                  .clap, the standalone, generated .ttl / .json), the
#                  command-line tools and every other mw909* executable
#                  (unit, plugin and UI test programs). Intermediate static
#                  archives (*_SharedCode.a) are not shipped and not scanned:
#                  their compressed/random byte runs produce chance matches.
#                  For the same reason an ELF file is scanned without its
#                  binary DWARF sections (.debug_info, .debug_line, ... of the
#                  Debug and sanitizer trees: offsets and LEB128 operands,
#                  where a clone code turned up by chance); every loaded
#                  section, the symbol tables and the debug string tables
#                  (.debug_str, .debug_line_str: real identifiers and paths)
#                  are still scanned. Release artefacts carry no DWARF, so
#                  what ships is scanned byte for byte.
#                * the CTest registrations (CTestTestfile.cmake and the
#                  Catch2-discovered *_tests*.cmake lists): every test name.
#                * the factory content as the renderer lists it (kits,
#                  presets, memories, every pattern name), when mw909-render
#                  is built there.
#   --all-builds the same for every directory under <repo>/build/.
#
# Matching: case-insensitive, each token an extended regex, at word
# boundaries. Full tokens treat '_' as a boundary too, so a brand inside a
# snake_case identifier or test name is caught (grep -w would see the
# underscore as part of the word). The short "text:" codes keep grep -w
# semantics, so identifiers such as a schematic node "rd9_" stay legal.
# Every line is matched twice: as written, and normalised (normalise()
# below): typographic dashes (U+2010..U+2015, U+2212, U+FE58, U+FE63,
# U+FF0D) become '-', no-break and other Unicode spaces become ' ',
# zero-width characters and soft hyphens are dropped, and camelCase humps
# are split (makerShuffle -> maker Shuffle, kitXY909 -> kit XY909,
# applyXy909Swing -> apply Xy909 Swing, MakerXY9 -> Maker XY9), so a brand
# inside a camelCase identifier or with a typographic dash is caught. The
# self-test below holds the real spellings, split with "" so this file passes.
# Exit codes: 0 clean, 1 forbidden token found, 2 usage or configuration error.

set -euo pipefail

repo_root=""
declare -a build_dirs=()
all_builds=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --binaries)
            [ "$#" -ge 2 ] || { echo "branding-audit: --binaries needs a build directory" >&2; exit 2; }
            build_dirs+=("$2")
            shift 2
            ;;
        --all-builds) all_builds=1; shift ;;
        -h|--help) sed -n '4,57p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "branding-audit: unknown option $1" >&2; exit 2 ;;
        *) repo_root="$1"; shift ;;
    esac
done

if [ -z "${repo_root}" ]; then
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
repo_root="$(cd "${repo_root}" && pwd)"
token_file="${repo_root}/scripts/forbidden_tokens.txt"
[ -f "${token_file}" ] || { echo "branding-audit: ${token_file} not found" >&2; exit 2; }

# Full tokens (text and binaries) and the "text:" short codes (text only).
mapfile -t full_tokens < <(grep -vE '^[[:space:]]*(#|$)|^text:' "${token_file}")
mapfile -t short_tokens < <(grep -E '^text:' "${token_file}" | sed 's/^text://')
[ "${#full_tokens[@]}" -gt 0 ] || { echo "branding-audit: no tokens in ${token_file}" >&2; exit 2; }
[ "${#short_tokens[@]}" -gt 0 ] || short_tokens=('x^')
join() { printf '%s\n' "$@" | paste -sd '|'; }
# Full tokens: boundaries by hand, '_' counts as one (a token must not touch a
# letter or digit). Short codes: grep -w (letters, digits and '_' are word).
full_re="(^|[^[:alnum:]])($(join "${full_tokens[@]}"))([^[:alnum:]]|$)"
short_re="$(join "${short_tokens[@]}")"
# Validate the regexes once (grep exits 2 on a bad expression).
for p in "${full_re}" "${short_re}"; do
    rc=0
    grep -qiE "${p}" <<< "x" || rc=$?
    if [ "${rc}" -eq 2 ]; then
        echo "branding-audit: invalid regular expression in ${token_file}" >&2
        exit 2
    fi
done
# normalise: stdin -> stdout, one output line per input line (line numbers
# stay valid). Byte-level, so invalid UTF-8 passes through untouched.
normalise() {
    LC_ALL=C perl -pe '
        s/\xE2\x80[\x90-\x95]|\xE2\x88\x92|\xEF\xB9[\x98\xA3]|\xEF\xBC\x8D/-/g;
        s/\xC2\xA0|\xE2\x80[\x80-\x8A\xA8\xA9\xAF]|\xE2\x81\x9F|\xE3\x80\x80/ /g;
        s/\xE2\x80[\x8B-\x8D]|\xE2\x81\xA0|\xEF\xBB\xBF|\xC2\xAD//g;
        s/([a-z])(?=[A-Z])/$1 /g;
        s/([A-Z])(?=[A-Z][a-z])/$1 /g;
        s/([0-9])(?=[A-Z][a-z])/$1 /g;
    '
}
# matches <text>: 0 if <text> holds a forbidden token (text-scan rules).
matches() {
    local t n
    t="$1"
    n="$(printf '%s\n' "$1" | normalise)"
    grep -qiE "${full_re}" <<< "${t}" || grep -qwiE "${short_re}" <<< "${t}" \
        || grep -qiE "${full_re}" <<< "${n}" || grep -qwiE "${short_re}" <<< "${n}"
}
# scan_text <label> <full|both>: stdin -> "label:line:text" hits, matched as
# written and normalised (a line is reported once). "full" applies the full
# tokens only (binaries), "both" the short text codes too.
scan_text() {
    local label="$1" mode="$2" in
    in="$(mktemp -p "${scratch}")"
    cat > "${in}"
    {
        grep -HniE --label="${label}" "${full_re}" < "${in}" || true
        normalise < "${in}" | grep -HniE --label="${label}" "${full_re}" || true
        if [ "${mode}" = both ]; then
            grep -HnwiE --label="${label}" "${short_re}" < "${in}" || true
            normalise < "${in}" | grep -HnwiE --label="${label}" "${short_re}" || true
        fi
    } | awk -v l="${label}" '{ k = substr($0, length(l) + 2); sub(/:.*/, "", k); if (!seen[k]++) print }'
    rm -f "${in}"
}
# binary_strings <file>: printable 8-bit and UTF-16LE runs of >= 4 characters.
binary_strings() { strings -a -n 4 "$1" 2>/dev/null || true; strings -a -n 4 -e l "$1" 2>/dev/null || true; }
# built_strings <file>: binary_strings of a built file, minus the binary
# DWARF sections of an ELF file that has them (see --binaries above). A file
# that is not ELF, has no such section or cannot be rewritten is scanned whole.
built_strings() {
    local f="$1" stripped sections=""
    if [ "$(head -c 4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 7f454c46 ]; then
        sections="$(LC_ALL=C readelf -SW "$f" 2>/dev/null || true)"
    fi
    if grep -qE '[[:space:]]\.debug_' <<< "${sections}"; then
        stripped="${scratch}/dwarf-stripped"
        if objcopy -R '.debug_*' -R '!.debug_str' -R '!.debug_line_str' "$f" "${stripped}" 2>/dev/null; then
            binary_strings "${stripped}"
            rm -f "${stripped}"
            return
        fi
        rm -f "${stripped}"
    fi
    binary_strings "$f"
}
# Self-test: the matcher must catch these spellings (guards against a broken
# token file or regex quoting) and must not catch the product's own names.
for probe in "x Ro""land x" "test_tr""909_flam" "seq_ro""land_shuffle" "TR-""909" "TR ""909" "TR""909" "RD-""9" \
             "static int Ro""landShuffle = 1;" "void applyTr""909Swing();" "auto kitTR""909 = 0;" "Behr""ingerR""D9 clone" \
             "the TR"$'\u2013'"909 sound" "the TR"$'\u2011'"909 sound" "the TR"$'\u2014'"909 sound" "TR"$'\u2212'"909" \
             "TR/""909" "TR.""909" "Ro"$'\u00ad'"land" "Ro"$'\u200b'"land" "TR"$'\u00a0'"909" "XMLRo""landKit" \
             "drum""Brute" "x2Ro""landKit"; do
    matches "${probe}" || { echo "branding-audit: self-test failed to match '${probe}'" >&2; exit 2; }
done
for probe in "mw909" "909" "808" "mw909_test_seq_steps" "D16" "heartbeat timer" "rd9_ = 1.0" \
             "mw909Voices" "kNumVoices909" "the 909"$'\u2019'"s sound" "voice_cr_stock_44k.wav" "trackTrigger"; do
    if matches "${probe}"; then
        echo "branding-audit: self-test matched the allowed '${probe}'" >&2
        exit 2
    fi
done

if [ "${all_builds}" -eq 1 ] && [ -d "${repo_root}/build" ]; then
    while IFS= read -r d; do build_dirs+=("$d"); done \
        < <(find "${repo_root}/build" -mindepth 1 -maxdepth 1 -type d | sort)
fi

echo "branding audit: $((${#full_tokens[@]} + ${#short_tokens[@]})) token pattern(s) (${#full_tokens[@]} for binaries) from ${token_file#"${repo_root}"/}"
hits=0
scratch="$(mktemp -d)"
trap 'rm -rf "${scratch}"' EXIT

# --- the tree: names, text files, binary files ---------------------------------------
text_hits="${scratch}/text"
name_hits="${scratch}/names_tree"
bin_hits="${scratch}/bin_tree"
: > "${text_hits}"
: > "${bin_hits}"
scanned=0
binaries_scanned=0
cd "${repo_root}"
mapfile -d '' tree_files < <(find . \
    \( -path ./research -o -path ./.deps -o -path ./.git -o -path './build' -o -path './build-*' \
       -o -path './cmake-build-*' -o -name node_modules -o -path ./.cache \) -prune -o \
    \( -type f -o -type d -o -type l \) -print0)
# Names: every path below the root (files, directories, links).
for f in "${tree_files[@]}"; do
    [ "$f" = "." ] || printf '%s\n' "${f#./}"
done | scan_text "file name" both > "${name_hits}"
for f in "${tree_files[@]}"; do
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    [ "$f" != ./scripts/forbidden_tokens.txt ] || continue
    [ -r "$f" ] || { echo "branding-audit: cannot read ${f}" >&2; exit 2; }
    if [ ! -s "$f" ] || grep -Iq . "$f"; then
        scanned=$((scanned + 1))
        scan_text "${f#./}" both < "$f" >> "${text_hits}"
    else
        # Binary: its printable runs, full tokens only (the short codes occur
        # by chance in sample data).
        binaries_scanned=$((binaries_scanned + 1))
        binary_strings "$f" | scan_text "${f#./} (strings)" full >> "${bin_hits}"
    fi
done
for kind in name text bin; do
    case "${kind}" in
        name) file="${name_hits}"; what="file name(s)" ;;
        text) file="${text_hits}"; what="line(s) in text files" ;;
        bin) file="${bin_hits}"; what="string(s) in binary files" ;;
    esac
    n="$(wc -l < "${file}")"
    if [ "${n}" -gt 0 ]; then
        echo "FAIL: ${n} ${what}:"
        sed 's/^/  /' "${file}"
        hits=$((hits + n))
    fi
done
if [ "${hits}" -eq 0 ]; then
    echo "tree: ${#tree_files[@]} path name(s), ${scanned} text file(s), ${binaries_scanned} binary file(s) scanned, clean"
fi

# --- build directories -----------------------------------------------------------------
report() { # label, file with hits
    local count
    count="$(wc -l < "$2")"
    if [ "${count}" -gt 0 ]; then
        echo "FAIL: $1"
        head -n 20 "$2" | sed 's/^/  /'
        hits=$((hits + count))
    fi
}
for bd in ${build_dirs[@]:+"${build_dirs[@]}"}; do
    if [ ! -d "${bd}" ]; then
        echo "build dir: ${bd} does not exist, skipped"
        continue
    fi
    before="${hits}"

    # Binaries: shipped artefacts, the tools, every mw909* executable.
    mapfile -d '' bins < <(find "${bd}" \( -name _deps -o -name CMakeFiles -o -name validator-tools \) -prune -o -type f \
        \( -path '*_artefacts/*/VST3/*' -o -path '*_artefacts/*/CLAP/*' -o -path '*_artefacts/*/LV2/*' \
           -o -path '*_artefacts/*/Standalone/*' -o \( -name 'mw909*' -perm -u+x ! -name '*.a' ! -name '*.so' \) \) \
        ! -name '*_SharedCode.a' -print0 2>/dev/null)
    for b in ${bins[@]:+"${bins[@]}"}; do
        out="${scratch}/bin"
        built_strings "$b" | scan_text "strings" full > "${out}"
        report "${b}" "${out}"
    done

    # Test names: CTest registrations, including Catch2-discovered lists.
    mapfile -d '' ctest_files < <(find "${bd}" \( -name _deps -o -name CMakeFiles \) -prune -o -type f \
        \( -name CTestTestfile.cmake -o -name '*_tests*.cmake' -o -name '*_include*.cmake' \) -print0 2>/dev/null)
    for c in ${ctest_files[@]:+"${ctest_files[@]}"}; do
        out="${scratch}/ctest"
        scan_text "$(basename "$c")" both < "$c" > "${out}"
        report "${c} (test names)" "${out}"
    done

    # Factory content names as the renderer lists them.
    render="$(find "${bd}" -path '*/tools/mw909-render/mw909-render' -type f -perm -u+x 2>/dev/null | head -n1)"
    names=0
    if [ -n "${render}" ]; then
        out="${scratch}/names"
        {
            "${render}" list
            n_mem="$("${render}" list memories | grep -cE '^ +[0-9]+ ')"
            for ((m = 0; m < n_mem; m++)); do "${render}" list patterns "${m}"; done
        } > "${scratch}/list" 2>&1 || true
        names="$(wc -l < "${scratch}/list")"
        scan_text "list" both < "${scratch}/list" > "${out}"
        report "${render} list (factory names)" "${out}"
    fi

    if [ "${hits}" -eq "${before}" ]; then
        echo "build dir ${bd}: ${#bins[@]} binar(ies), ${#ctest_files[@]} CTest file(s), ${names} factory-name line(s), clean"
    fi
done

if [ "${hits}" -gt 0 ]; then
    echo "RESULT: FAIL (${hits} forbidden-token hit(s))"
    exit 1
fi
echo "RESULT: PASS"
