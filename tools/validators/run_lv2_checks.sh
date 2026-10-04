#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# run_lv2_checks.sh [build-dir] — basic checks of every built LV2 bundle that
# need nothing beyond coreutils/binutils (and python3 for the load test):
#
#   1. layout     manifest.ttl present; the lv2:binary and every rdfs:seeAlso
#                 file it names exist; the plugin URI is the one in
#                 docs/design/plugin.md
#   2. Turtle     every .ttl is non-empty text with @prefix lines, its last
#                 statement ends in '.', and its <...> / [...] / (...) pairs
#                 balance outside string literals
#   3. binary     the .so is an ELF shared object, all its libraries resolve
#                 (ldd), and it exports lv2_descriptor (nm -D)
#   4. load       dlopen the .so, walk lv2_descriptor(0..) and find the
#                 plugin URI among the descriptors (python3 ctypes; skipped
#                 with a warning when python3 is absent)
#   5. optional   lv2lint and sord_validate when installed. sord_validate
#                 without the LV2 schemata reports false positives, so its
#                 result is printed but does not fail the run.
#
#   exit 0 all bundles pass, 1 a check failed, 2 the build directory named
#   does not exist (usage error), 77 no LV2 bundle built (SKIP)
#
# Each bundle is checked as a private copy (lib.sh stage_artefact); the load
# test and the optional tools run under run_guarded (hard timeout, process
# group killed afterwards).

set -euo pipefail
MW909_TOOL_NAME="run_lv2_checks"
# shellcheck source=lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

resolve_build_dir "${1:-}"

mapfile -t bundles < <(find_artefacts d '*.lv2')
if [ "${#bundles[@]}" -eq 0 ]; then
    skip "no LV2 bundle under ${BUILD_DIR} (build the plugin first)"
fi
setup_xvfb

failures=0
fail() { warn "FAIL: $*"; failures=$((failures + 1)); }
ok()   { log "ok: $*"; }

# Prints the IRIs that follow `predicate` in a Turtle file (relative ones as written).
iris_after() {
    local predicate="$1" file="$2"
    grep -oE "${predicate}[[:space:]]+<[^>]*>" "${file}" 2>/dev/null | sed -E 's/.*<([^>]*)>/\1/'
}

check_turtle() {
    local ttl="$1" name
    name="$(basename "${ttl}")"
    if [ ! -s "${ttl}" ]; then
        fail "${name} is empty"
        return
    fi
    if ! grep -Iq . "${ttl}"; then
        fail "${name} is not a text file"
        return
    fi
    grep -qE '^[[:space:]]*@prefix' "${ttl}" || fail "${name} has no @prefix declarations"
    local last
    last="$(grep -vE '^[[:space:]]*(#.*)?$' "${ttl}" | tail -n1 | sed -E 's/[[:space:]]+$//')"
    [[ "${last}" == *. ]] || fail "${name}: last statement does not end in '.' (${last:0:60})"
    # Bracket balance outside "..." literals, <IRIs> and comments (awk, one pass).
    # An IRI must close on its own line; '#' inside an IRI is not a comment.
    local balance
    balance="$(awk '
        BEGIN { inq = 0; a = 0; b = 0; p = 0 }
        { line = $0
          for (i = 1; i <= length(line); i++) {
              c = substr(line, i, 1)
              if (inq) { if (c == "\\") { i++; continue } if (c == "\"") inq = 0; continue }
              if (c == "\"") { inq = 1; continue }
              if (c == "#") break
              if (c == "<") { j = index(substr(line, i + 1), ">"); if (j == 0) { a++; break } i += j; continue }
              if (c == ">") a--
              else if (c == "[") b++; else if (c == "]") b--
              else if (c == "(") p++; else if (c == ")") p--
          } }
        END { printf "%d %d %d", a, b, p }' "${ttl}")"
    [ "${balance}" = "0 0 0" ] || fail "${name}: unbalanced <> [] () (net ${balance})"
}

load_test() {
    local so="$1"
    if ! command -v python3 >/dev/null 2>&1; then
        warn "python3 not installed: skipping the dlopen/lv2_descriptor load test"
        return
    fi
    local out rc=0
    out="$(run_guarded 60 ${XVFB[@]:+"${XVFB[@]}"} python3 - "${so}" <<'PY' 2>&1
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1], mode=ctypes.RTLD_LOCAL)
fn = lib.lv2_descriptor
fn.restype = ctypes.c_void_p
fn.argtypes = [ctypes.c_uint32]
for i in range(64):
    d = fn(i)
    if not d:
        break
    uri = ctypes.cast(d, ctypes.POINTER(ctypes.c_char_p))[0]
    print("descriptor", i, uri.decode("utf-8", "replace") if uri else "<null>")
PY
)" || rc=$?
    printf '%s\n' "${out}" | sed 's/^/    /'
    if [ "${rc}" -ne 0 ]; then
        fail "$(basename "${so}"): load test failed (rc ${rc})"
    elif printf '%s\n' "${out}" | grep -qxF "descriptor 0 ${MW909_LV2_URI}" ||
         printf '%s\n' "${out}" | grep -qE "^descriptor [0-9]+ ${MW909_LV2_URI}\$"; then
        ok "$(basename "${so}") loads and lv2_descriptor returns ${MW909_LV2_URI}"
    else
        fail "$(basename "${so}"): no descriptor with URI ${MW909_LV2_URI}"
    fi
}

for original in "${bundles[@]}"; do
    stage_artefact "${original}" bundle
    log "checking ${bundle}"
    manifest="${bundle}/manifest.ttl"
    if [ ! -f "${manifest}" ]; then
        fail "no manifest.ttl in ${bundle}"
        continue
    fi

    # 1. layout
    if grep -qF "<${MW909_LV2_URI}>" "${manifest}"; then
        ok "manifest declares <${MW909_LV2_URI}>"
    else
        fail "manifest.ttl does not mention <${MW909_LV2_URI}>"
    fi
    grep -qE 'lv2:Plugin' "${manifest}" || fail "manifest.ttl declares no lv2:Plugin"
    mapfile -t binaries < <(iris_after 'lv2:binary' "${manifest}" | sort -u)
    if [ "${#binaries[@]}" -eq 0 ]; then
        fail "manifest.ttl names no lv2:binary"
    fi
    for b in "${binaries[@]}"; do
        [ -f "${bundle}/${b}" ] && ok "binary ${b} present" || fail "lv2:binary <${b}> missing from the bundle"
    done
    while IFS= read -r f; do
        [ -n "${f}" ] || continue
        case "${f}" in *://*) continue ;; esac
        [ -f "${bundle}/${f}" ] && ok "seeAlso ${f} present" || fail "rdfs:seeAlso <${f}> missing from the bundle"
    done < <(iris_after 'rdfs:seeAlso' "${manifest}" | sort -u)

    # 2. Turtle sanity
    mapfile -t ttls < <(find "${bundle}" -maxdepth 1 -name '*.ttl' | sort)
    for ttl in "${ttls[@]}"; do
        before="${failures}"
        check_turtle "${ttl}"
        [ "${failures}" -eq "${before}" ] && ok "$(basename "${ttl}") looks like well-formed Turtle"
    done
    if [ "${#ttls[@]}" -eq 0 ] || ! grep -qsE 'doap:name' "${ttls[@]}"; then
        fail "no doap:name in any .ttl"
    fi

    # 3 + 4. shared objects
    mapfile -t sos < <(find "${bundle}" -maxdepth 1 -name '*.so' | sort)
    [ "${#sos[@]}" -gt 0 ] || fail "no .so in ${bundle}"
    for so in "${sos[@]}"; do
        name="$(basename "${so}")"
        if file -L "${so}" | grep -q 'ELF 64-bit.*shared object'; then
            ok "${name} is an ELF shared object"
        else
            fail "${name} is not an ELF shared object: $(file -bL "${so}")"
        fi
        missing="$(ldd "${so}" 2>/dev/null | grep 'not found' || true)"
        [ -z "${missing}" ] && ok "${name}: all libraries resolve" || fail "${name}: unresolved libraries: ${missing}"
        if nm -D --defined-only "${so}" 2>/dev/null | grep -qE '[[:space:]]T[[:space:]]+lv2_descriptor$'; then
            ok "${name} exports lv2_descriptor"
        else
            fail "${name} does not export lv2_descriptor"
        fi
        load_test "${so}"
    done

    # 5. optional external validators
    export LV2_PATH="$(dirname "${bundle}")${LV2_PATH:+:${LV2_PATH}}"
    if command -v lv2lint >/dev/null 2>&1; then
        log "running lv2lint -s ${MW909_LV2_URI}"
        if run_guarded 300 ${XVFB[@]:+"${XVFB[@]}"} lv2lint -s "${MW909_LV2_URI}"; then ok "lv2lint"; else fail "lv2lint"; fi
    else
        log "lv2lint not installed (optional)"
    fi
    if command -v sord_validate >/dev/null 2>&1; then
        log "running sord_validate (advisory: schema-less runs report false positives)"
        run_guarded 120 sord_validate "${ttls[@]}" || warn "sord_validate reported problems (advisory, not counted)"
    else
        log "sord_validate not installed (optional)"
    fi
done

if [ "${failures}" -ne 0 ]; then
    warn "${failures} LV2 check(s) failed"
    exit 1
fi
log "all ${#bundles[@]} LV2 bundle(s) passed the basic checks"
