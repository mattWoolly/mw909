#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# run_clap_validator.sh [build-dir] — validates every built .clap with a
# pinned clap-validator release from github.com/free-audio/clap-validator.
#
#   exit 0   all .clap files passed
#   exit 1   a plugin failed, or the download failed its checksum
#   exit 77  clap-validator unavailable (offline) or no .clap built: SKIP
#   exit 2   the build directory named does not exist (usage error)
#
# Pins (Linux builds; archive and binary SHA-256 verified on 2026-10-02):
#   0.4.1 (default)  clap-validator-0.4.1-127-g152b982-ubuntu-22.04.zip, which
#                    holds a .tar.gz holding the binary. Stricter: state
#                    reproducibility and parameter fuzzing.
#   0.3.2            clap-validator-0.3.2-ubuntu-18.04.tar.gz
# Select with MW909_CLAP_VALIDATOR_VERSION. Cached in
# build/validator-tools/clap-validator-<version>/. MW909_CLAP_VALIDATOR_EXTRA
# adds arguments to `clap-validator validate` (e.g. "--exclude param-fuzz").
# Each .clap is validated as a private copy (lib.sh stage_artefact) under a
# hard timeout (MW909_CLAP_VALIDATOR_RUN_TIMEOUT, default 1500 s), and no
# clap-validator process survives the script.

set -euo pipefail
MW909_TOOL_NAME="run_clap_validator"
# shellcheck source=lib.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

CV_VERSION="${MW909_CLAP_VALIDATOR_VERSION:-0.4.1}"
case "${CV_VERSION}" in
    0.4.1)
        CV_URL="https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-ubuntu-22.04.zip"
        CV_ARCHIVE_SHA256="49edadcfb407ea0dd946ce418300e853fbd2660fa4b0d00e4f19ff8eef24ad90"
        CV_BIN_SHA256="d9a78bfe781719de450bc5013141496eb13e1d9a538c67de1b8d325628670dbf"
        CV_ARCHIVE="zip-of-tgz"
        ;;
    0.3.2)
        CV_URL="https://github.com/free-audio/clap-validator/releases/download/0.3.2/clap-validator-0.3.2-ubuntu-18.04.tar.gz"
        CV_ARCHIVE_SHA256="1476ed68f5657e76050e0c4f19790c02d819ecc62c35fd465059d21f05169cb1"
        CV_BIN_SHA256="d25b9f06cacfa06b1e2f6d4faf9511bd81e0c4676643c51639db7b291f017f4c"
        CV_ARCHIVE="tgz"
        ;;
    *)
        warn "unknown pinned version '${CV_VERSION}' (pins: 0.4.1, 0.3.2)"
        exit 1
        ;;
esac

if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    skip "the pinned clap-validator build is Linux x86_64 only"
fi

resolve_build_dir "${1:-}"

mapfile -t plugins < <(find_artefacts f '*.clap')
if [ "${#plugins[@]}" -eq 0 ]; then
    skip "no .clap under ${BUILD_DIR} (build the plugin first)"
fi

# --- get clap-validator -------------------------------------------------------------
dest_dir="${MW909_TOOLS_CACHE}/clap-validator-${CV_VERSION}"
validator="${dest_dir}/clap-validator"

if [ -x "${validator}" ] && [ "$(sha256_of "${validator}")" = "${CV_BIN_SHA256}" ]; then
    log "using cached clap-validator ${CV_VERSION}: ${validator}"
else
    archive="${MW909_TOOLS_CACHE}/clap-validator-${CV_VERSION}.download"
    rc=0
    download "${CV_URL}" "${archive}" "${CV_ARCHIVE_SHA256}" || rc=$?
    if [ "${rc}" -eq 2 ]; then
        exit 1
    elif [ "${rc}" -ne 0 ]; then
        skip "could not download clap-validator ${CV_VERSION}"
    fi
    rm -rf "${dest_dir}"
    mkdir -p "${dest_dir}"
    if [ "${CV_ARCHIVE}" = "zip-of-tgz" ]; then
        command -v unzip >/dev/null 2>&1 || skip "unzip is not installed"
        unzip -q -o "${archive}" -d "${dest_dir}/unpack"
        inner="$(find "${dest_dir}/unpack" -name '*.tar.gz' | head -n1)"
        [ -n "${inner}" ] || { warn "no .tar.gz inside the clap-validator zip"; exit 1; }
        tar -xzf "${inner}" -C "${dest_dir}"
        rm -rf "${dest_dir}/unpack"
    else
        tar -xzf "${archive}" -C "${dest_dir}"
    fi
    rm -f "${archive}"
    found="$(find "${dest_dir}" -type f -name clap-validator | head -n1)"
    [ -n "${found}" ] && [ "${found}" != "${validator}" ] && mv -f "${found}" "${validator}"
    chmod +x "${validator}" 2>/dev/null || true
    if [ ! -x "${validator}" ] || [ "$(sha256_of "${validator}")" != "${CV_BIN_SHA256}" ]; then
        warn "clap-validator binary inside the verified archive has an unexpected digest"
        exit 1
    fi
fi
log "$(timeout 30 "${validator}" --version 2>/dev/null || echo "clap-validator ${CV_VERSION}")"

setup_xvfb

extra=()
if [ -n "${MW909_CLAP_VALIDATOR_EXTRA:-}" ]; then
    read -r -a extra <<< "${MW909_CLAP_VALIDATOR_EXTRA}"
fi

failed=0
for original in "${plugins[@]}"; do
    stage_artefact "${original}" plugin
    log "validating ${plugin}"
    if run_guarded "${MW909_CLAP_VALIDATOR_RUN_TIMEOUT:-1500}" ${XVFB[@]:+"${XVFB[@]}"} \
        "${validator}" validate ${extra[@]:+"${extra[@]}"} "${plugin}"; then
        log "PASS: ${original}"
    else
        warn "FAIL: ${original}"
        failed=$((failed + 1))
    fi
done

if [ "${failed}" -ne 0 ]; then
    warn "${failed} of ${#plugins[@]} CLAP plugin(s) failed clap-validator ${CV_VERSION}"
    exit 1
fi
log "all ${#plugins[@]} CLAP plugin(s) passed clap-validator ${CV_VERSION}"
