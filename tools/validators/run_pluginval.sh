#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# run_pluginval.sh [build-dir] — validates every built VST3 bundle with a
# pinned pluginval at strictness 10 (in-process, 3 randomised repeats).
#
#   exit 0   all VST3 bundles passed
#   exit 1   a bundle failed, or the pluginval download failed its checksum
#   exit 77  pluginval unavailable (offline, no seed) or no VST3 built: SKIP
#   exit 2   the build directory named does not exist (usage error)
#
# pluginval is pinned to v1.0.4. The Linux release zip and the binary inside
# it are both checked against SHA-256 digests recorded here (verified on
# 2026-10-02 and re-verified 2026-10-03 against the GitHub release download). The binary is cached in
# build/validator-tools/pluginval-v1.0.4/; a copy already at
# build/tools/pluginval (or $MW909_PLUGINVAL_SEED) seeds the cache when its
# digest matches.
#
# Each bundle is validated as a private copy (lib.sh stage_artefact) under a
# hard timeout, and no pluginval or Xvfb process survives the script.
#
# Optional: tools/validators/pluginval-disabled-tests.txt (one pluginval test
# name per line) is passed as --disabled-tests; it does not exist by default,
# so nothing is waived. MW909_PLUGINVAL_EXTRA adds further arguments.

set -euo pipefail
MW909_TOOL_NAME="run_pluginval"
# shellcheck source=lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

PLUGINVAL_VERSION="v1.0.4"
PLUGINVAL_URL="https://github.com/Tracktion/pluginval/releases/download/${PLUGINVAL_VERSION}/pluginval_Linux.zip"
PLUGINVAL_ZIP_SHA256="c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352"
PLUGINVAL_BIN_SHA256="d3d342ea92293b0699a5c983937211e3faf2b0f38145b31cb8d78424a25e10ba"
STRICTNESS=10
REPEAT=3
# Per-test timeout. pluginval's default (30 s) trips on a loaded shared build
# machine during the editor tests; two minutes keeps a genuine hang visible.
TIMEOUT_MS=120000
# Hard limit for one whole pluginval run (CTest's TIMEOUT is 1800 s).
RUN_TIMEOUT_S="${MW909_PLUGINVAL_RUN_TIMEOUT:-1500}"

if [ "$(uname -s)" != "Linux" ]; then
    skip "this script pins the Linux build of pluginval"
fi

resolve_build_dir "${1:-}"

# --- locate the artefacts first: no point fetching a tool with nothing to test ---
mapfile -t bundles < <(find_artefacts d '*.vst3')
if [ "${#bundles[@]}" -eq 0 ]; then
    skip "no VST3 bundle under ${BUILD_DIR} (build the plugin first)"
fi

# --- get pluginval ------------------------------------------------------------------
dest_dir="${MW909_TOOLS_CACHE}/pluginval-${PLUGINVAL_VERSION}"
pluginval="${dest_dir}/pluginval"
seed="${MW909_PLUGINVAL_SEED:-${MW909_REPO_ROOT}/build/tools/pluginval}"

if [ -x "${pluginval}" ] && [ "$(sha256_of "${pluginval}")" = "${PLUGINVAL_BIN_SHA256}" ]; then
    log "using cached pluginval ${PLUGINVAL_VERSION}: ${pluginval}"
elif [ -f "${seed}" ] && [ "$(sha256_of "${seed}")" = "${PLUGINVAL_BIN_SHA256}" ]; then
    mkdir -p "${dest_dir}"
    cp -f "${seed}" "${pluginval}"
    chmod +x "${pluginval}"
    log "seeded pluginval ${PLUGINVAL_VERSION} from ${seed} (binary digest verified)"
else
    zip="${MW909_TOOLS_CACHE}/pluginval-${PLUGINVAL_VERSION}-Linux.zip"
    rc=0
    download "${PLUGINVAL_URL}" "${zip}" "${PLUGINVAL_ZIP_SHA256}" || rc=$?
    if [ "${rc}" -eq 2 ]; then
        exit 1
    elif [ "${rc}" -ne 0 ]; then
        skip "could not download pluginval ${PLUGINVAL_VERSION}"
    fi
    command -v unzip >/dev/null 2>&1 || skip "unzip is not installed"
    rm -rf "${dest_dir}"
    mkdir -p "${dest_dir}"
    unzip -q -o "${zip}" -d "${dest_dir}"
    rm -f "${zip}"
    chmod +x "${pluginval}" 2>/dev/null || true
    if [ ! -x "${pluginval}" ] || [ "$(sha256_of "${pluginval}")" != "${PLUGINVAL_BIN_SHA256}" ]; then
        warn "pluginval binary inside the verified zip has an unexpected digest"
        exit 1
    fi
fi

setup_xvfb

extra=()
disabled="${_mw909_lib_dir}/pluginval-disabled-tests.txt"
if [ -f "${disabled}" ]; then
    warn "WAIVER: disabling the pluginval tests listed in ${disabled}:"
    sed 's/^/    /' "${disabled}" >&2
    extra+=(--disabled-tests "${disabled}")   # must be an absolute path
fi
if [ -n "${MW909_PLUGINVAL_EXTRA:-}" ]; then
    read -r -a more <<< "${MW909_PLUGINVAL_EXTRA}"
    extra+=("${more[@]}")
fi

failed=0
for original in "${bundles[@]}"; do
    stage_artefact "${original}" bundle
    log "validating ${bundle} (strictness ${STRICTNESS}, in-process, repeat ${REPEAT}, randomised)"
    if run_guarded "${RUN_TIMEOUT_S}" ${XVFB[@]:+"${XVFB[@]}"} "${pluginval}" \
        --strictness-level "${STRICTNESS}" \
        --validate-in-process \
        --repeat "${REPEAT}" \
        --randomise \
        --timeout-ms "${TIMEOUT_MS}" \
        ${extra[@]:+"${extra[@]}"} \
        --validate "${bundle}"; then
        log "PASS: ${original}"
    else
        warn "FAIL: ${original}"
        failed=$((failed + 1))
    fi
done

if [ "${failed}" -ne 0 ]; then
    warn "${failed} of ${#bundles[@]} VST3 bundle(s) failed pluginval"
    exit 1
fi
log "all ${#bundles[@]} VST3 bundle(s) passed pluginval ${PLUGINVAL_VERSION} at strictness ${STRICTNESS}"
