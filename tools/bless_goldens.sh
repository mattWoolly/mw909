#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# bless_goldens.sh — re-render the golden corpus into tests/golden/blessed/ and
# write tests/golden/blessed/MANIFEST.json. Gated: refuses unless BLESS_REASON
# says why the reference sound is changing.
#
# Usage (one command; configures and builds the tools if needed):
#   BLESS_REASON="why" tools/bless_goldens.sh [build-dir]
#     build-dir  default: $MW909_BUILD_DIR, else build/linux-release
#
# Environment:
#   BLESS_REASON   required, free text (recorded in MANIFEST.json)
#   BLESS_ONLY     optional extended regex: re-bless only matching case ids,
#                  e.g. BLESS_ONLY='^voice_(ch|oh|cr|rd)_' for the sample
#                  voices. The manifest records the scope.
#   BLESS_PRUNE=1  delete blessed WAVs whose case no longer exists
#   BLESS_NO_VERIFY=1  skip the closing `ctest -L golden` run
#   BLESS_NO_BUILD=1   do not (re)build the tools (used by the CMake target)
#   BLESS_BLESSER  name recorded as the blesser (default: $USER)
#   BLESS_ANY_BUILD_TYPE=1  allow blessing from a non-Release build (the
#                  reference renders come from the Release tools by default)
#
# The case list comes from <build-dir>/tests/golden/golden_cases.tsv, which
# CMake generates from tests/golden/cases.json, so this script has no JSON
# parser of its own. Exit: 0 blessed and verified, 1 refused or failed.

set -euo pipefail

log() { printf 'bless_goldens: %s\n' "$*"; }
die() { printf 'bless_goldens: %s\n' "$*" >&2; exit 1; }

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
golden_dir="${repo_root}/tests/golden"
blessed_dir="${golden_dir}/blessed"

# --- the gate ---------------------------------------------------------------------
if [ -z "${BLESS_REASON:-}" ]; then
    cat >&2 <<'EOF'
bless_goldens: REFUSED - BLESS_REASON is not set.
  Blessing replaces the reference renders every golden test compares against.
  Say why the sound is meant to change, e.g.:
    BLESS_REASON="ride: refitted ROM clock after review" tools/bless_goldens.sh build/linux-release
EOF
    exit 1
fi

build_dir="${1:-${MW909_BUILD_DIR:-${repo_root}/build/linux-release}}"
case "${build_dir}" in /*) ;; *) build_dir="$(pwd)/${build_dir}" ;; esac

# --- tools --------------------------------------------------------------------------
if [ -z "${BLESS_NO_BUILD:-}" ]; then
    if [ ! -f "${build_dir}/CMakeCache.txt" ]; then
        log "configuring ${build_dir} (Release, plugin off)"
        cmake -S "${repo_root}" -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMW909_BUILD_PLUGIN=OFF >/dev/null
    fi
    log "building mw909-render and mw909-wavcompare in ${build_dir}"
    cmake --build "${build_dir}" --target mw909-render mw909-wavcompare >/dev/null
    # Refresh the case list if cases.json changed since the last configure.
    cmake "${build_dir}" >/dev/null
fi

render_bin="${build_dir}/tools/mw909-render/mw909-render"
compare_bin="${build_dir}/tools/mw909-render/mw909-wavcompare"
case_list="${build_dir}/tests/golden/golden_cases.tsv"
[ -x "${render_bin}" ] || die "renderer not found: ${render_bin}"
[ -x "${compare_bin}" ] || die "comparer not found: ${compare_bin}"
[ -f "${case_list}" ] || die "case list not found: ${case_list} (configure with MW909_BUILD_TESTS=ON)"
build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "${build_dir}/CMakeCache.txt" 2>/dev/null | head -n1)"
if [ "${build_type}" != "Release" ] && [ -z "${BLESS_ANY_BUILD_TYPE:-}" ]; then
    die "${build_dir} is a '${build_type:-unknown}' build; bless from a Release build (or set BLESS_ANY_BUILD_TYPE=1)"
fi
if [ "${case_list}" -ot "${golden_dir}/cases.json" ]; then
    die "${case_list} is older than cases.json; re-run cmake on ${build_dir}"
fi

mkdir -p "${blessed_dir}"
only="${BLESS_ONLY:-}"
log "reason : ${BLESS_REASON}"
log "scope  : ${only:-all cases}"
log "render : $("${render_bin}" --version)"

# --- render -------------------------------------------------------------------------
declare -a ids=()
rendered=0
tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT
while IFS=$'\t' read -r -a fields; do
    [ "${#fields[@]}" -ge 4 ] || continue
    id="${fields[0]}"
    ref="${fields[1]}"
    args=("${fields[@]:3}")
    ids+=("${id}")
    [ "${ref}" = "-" ] || continue             # checked against another case's file
    if [ -n "${only}" ] && ! [[ "${id}" =~ ${only} ]]; then
        continue
    fi
    "${render_bin}" "${args[@]}" --quiet --out "${tmp_dir}/${id}.wav"
    mv -f "${tmp_dir}/${id}.wav" "${blessed_dir}/${id}.wav"
    rendered=$((rendered + 1))
    printf '  blessed %-34s %8d bytes\n' "${id}" "$(stat -c %s "${blessed_dir}/${id}.wav")"
done < "${case_list}"
[ "${#ids[@]}" -gt 0 ] || die "no cases in ${case_list}"
[ "${rendered}" -gt 0 ] || die "BLESS_ONLY='${only}' matched no case"
log "rendered ${rendered} of ${#ids[@]} case(s)"

# --- orphans ------------------------------------------------------------------------
for f in "${blessed_dir}"/*.wav; do
    [ -e "$f" ] || continue
    stem="$(basename "$f" .wav)"
    found=0
    for id in "${ids[@]}"; do [ "${id}" = "${stem}" ] && { found=1; break; }; done
    if [ "${found}" -eq 0 ]; then
        if [ -n "${BLESS_PRUNE:-}" ]; then
            log "pruning orphan ${f}"
            rm -f -- "$f"
        else
            log "WARNING: ${f} belongs to no case (BLESS_PRUNE=1 deletes it)"
        fi
    fi
done

# --- MANIFEST.json ------------------------------------------------------------------
json_escape() {
    printf '%s' "$1" | tr '\n\t\r' '   ' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}
core_hash="$(cd "${repo_root}" && find libs/mw909-core -type f \( -name '*.h' -o -name '*.cpp' \) -print0 \
    | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"
tool_hash="$(cd "${repo_root}" && find tools/mw909-render -type f \( -name '*.h' -o -name '*.cpp' \) -print0 \
    | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-16)"
total_bytes="$(du -cb "${blessed_dir}"/*.wav | tail -n1 | cut -f1)"

manifest="${blessed_dir}/MANIFEST.json"
{
    printf '{\n'
    printf '  "reason": "%s",\n' "$(json_escape "${BLESS_REASON}")"
    printf '  "scope": "%s",\n' "$(json_escape "${only:-all}")"
    printf '  "date": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "blesser": "%s",\n' "$(json_escape "${BLESS_BLESSER:-${USER:-unknown}}")"
    printf '  "platform": "%s",\n' "$(json_escape "$(uname -srm)")"
    printf '  "cpu": "%s",\n' "$(json_escape "$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')")"
    printf '  "renderer": "%s",\n' "$(json_escape "$("${render_bin}" --version)")"
    printf '  "buildType": "%s",\n' "$(json_escape "${build_type:-unknown}")"
    printf '  "cmake": "%s",\n' "$(json_escape "$(cmake --version | head -n1)")"
    printf '  "projectVersion": "%s",\n' "$(json_escape "$(head -n1 "${repo_root}/VERSION")")"
    printf '  "coreSourceHash": "%s",\n' "${core_hash}"
    printf '  "renderToolSourceHash": "%s",\n' "${tool_hash}"
    printf '  "caseCount": %d,\n' "${#ids[@]}"
    printf '  "renderedThisTime": %d,\n' "${rendered}"
    printf '  "totalBytes": %d,\n' "${total_bytes}"
    printf '  "files": {\n'
    first=1
    for f in "${blessed_dir}"/*.wav; do
        [ -e "$f" ] || continue
        [ "${first}" -eq 1 ] || printf ',\n'
        first=0
        printf '    "%s": "%s"' "$(basename "$f")" "$(sha256sum "$f" | cut -d' ' -f1)"
    done
    printf '\n  }\n}\n'
} > "${manifest}"
log "wrote ${manifest} (${total_bytes} bytes of WAV)"
# MANIFEST.json describes the latest blessing; HISTORY.log keeps every one.
printf '%s\t%s\t%s\t%d case(s)\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${BLESS_BLESSER:-${USER:-unknown}}" \
    "${only:-all}" "${rendered}" "$(printf '%s' "${BLESS_REASON}" | tr '\n\t' '  ')" >> "${blessed_dir}/HISTORY.log"

# --- verify -------------------------------------------------------------------------
if [ -z "${BLESS_NO_VERIFY:-}" ]; then
    log "verifying: ctest -L golden"
    if ! ctest --test-dir "${build_dir}" -L golden --output-on-failure -j 8 >"${tmp_dir}/ctest.log" 2>&1; then
        cat "${tmp_dir}/ctest.log" >&2
        die "the freshly blessed corpus does not pass its own tests"
    fi
    grep -E 'tests passed|tests failed' "${tmp_dir}/ctest.log" | sed 's/^/  /'
    # A SKIPPED golden test has no blessed file to compare against. After a
    # full blessing every case must have one; a skip means a case points at a
    # file that was never written (e.g. a mistyped `reference`).
    if [ -z "${only}" ] && grep -qE '\*\*\*Skipped|MW909_GOLDEN_SKIPPED' "${tmp_dir}/ctest.log"; then
        grep -E '\*\*\*Skipped' "${tmp_dir}/ctest.log" | sed 's/^/  /' >&2 || true
        die "golden test(s) skipped after a full blessing: a case has no blessed reference"
    fi
fi
log "done"
