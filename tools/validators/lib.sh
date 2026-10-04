#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# Shared helpers for the format-validator scripts in this directory. Sourced,
# not run. Every validator follows the same CTest contract:
#   exit 0   the artefact passed
#   exit 1   the artefact failed, or a downloaded tool failed its checksum
#   exit 2   usage error: an explicitly named build directory does not exist
#   exit 77  skip: the tool cannot be fetched, or no artefact is built
#
# Build directory: $MW909_VALIDATE_BUILD_DIR when set (lets `ctest -L validators`
# in one tree check the plugin of another), else the first argument, else
# $MW909_BUILD_DIR, else build/linux-release.
# Tool cache: <repo>/build/validator-tools (override with $MW909_VALIDATOR_CACHE).
#
# Process hygiene (a validator must never outlive its run):
#   * every artefact is copied to a private temp dir first (stage_artefact)
#     and validated there, never in a build tree someone may be rebuilding;
#   * every validator runs through run_guarded: its own session (setsid), a
#     hard timeout (timeout --kill-after), and the whole process group killed
#     when it returns;
#   * on exit (normal, skip, failure, INT or TERM) the trap kills anything
#     left in those groups or still holding the staged copy, reports it, and
#     removes the temp dir.

SKIP_EXIT=77

_mw909_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MW909_REPO_ROOT="$(cd "${_mw909_lib_dir}/../.." && pwd)"
MW909_TOOLS_CACHE="${MW909_VALIDATOR_CACHE:-${MW909_REPO_ROOT}/build/validator-tools}"

# Plugin identity (docs/design/plugin.md §1).
MW909_PRODUCT="mw909"
MW909_LV2_URI="https://github.com/mattWoolly/mw909"

log()  { printf '%s: %s\n' "${MW909_TOOL_NAME:-validator}" "$*"; }
warn() { printf '%s: %s\n' "${MW909_TOOL_NAME:-validator}" "$*" >&2; }

skip() {
    warn "$* - SKIPPING (exit ${SKIP_EXIT})"
    exit "${SKIP_EXIT}"
}

# resolve_build_dir [arg]: sets BUILD_DIR (absolute). A directory named
# explicitly ($MW909_VALIDATE_BUILD_DIR, the argument or $MW909_BUILD_DIR)
# that does not exist is a usage error (exit 2: a mistyped path must fail,
# not show as skipped); only the implicit default build/linux-release
# missing is a skip (nothing built yet).
resolve_build_dir() {
    local explicit="${MW909_VALIDATE_BUILD_DIR:-${1:-${MW909_BUILD_DIR:-}}}"
    BUILD_DIR="${explicit:-${MW909_REPO_ROOT}/build/linux-release}"
    case "${BUILD_DIR}" in /*) ;; *) BUILD_DIR="$(pwd)/${BUILD_DIR}" ;; esac
    if [ ! -d "${BUILD_DIR}" ]; then
        if [ -n "${explicit}" ]; then
            warn "build directory ${BUILD_DIR} does not exist (usage error, exit 2)"
            exit 2
        fi
        skip "default build directory ${BUILD_DIR} does not exist"
    fi
    log "build directory: ${BUILD_DIR}"
}

# find_artefacts <type d|f> <glob>: prints matching plugin artefacts under the
# JUCE artefact tree of BUILD_DIR (never inside _deps or another bundle).
# Build trees nested inside BUILD_DIR (a directory holding its own
# CMakeCache.txt, e.g. sanitizer sub-builds) belong to another configuration
# and are skipped; validate them by passing their directory explicitly.
find_artefacts() {
    local type="$1" glob="$2" a n skip
    local -a nested=()
    while IFS= read -r n; do
        nested+=("$(dirname "${n}")")
    done < <(find "${BUILD_DIR}" -mindepth 2 \( -name _deps -o -name CMakeFiles -o -name '*_artefacts' \) -prune -o \
                -name CMakeCache.txt -type f -print 2>/dev/null)
    while IFS= read -r a; do
        skip=0
        for n in ${nested[@]:+"${nested[@]}"}; do
            case "${a}" in "${n}"/*) skip=1; break ;; esac
        done
        [ "${skip}" -eq 1 ] || printf '%s\n' "${a}"
    done < <(find "${BUILD_DIR}" \( -name _deps -o -name CMakeFiles \) -prune -o \
                -path '*_artefacts/*' -type "${type}" -name "${glob}" -print 2>/dev/null | sort)
}

# xvfb wrapper for anything that may open a window on a headless box.
# MW909_FORCE_XVFB=1 uses it even when DISPLAY is set (keeps editor windows off
# the desktop of a developer machine).
XVFB=()
setup_xvfb() {
    if [ -z "${DISPLAY:-}" ] || [ -n "${MW909_FORCE_XVFB:-}" ]; then
        if command -v xvfb-run >/dev/null 2>&1; then
            log "wrapping in xvfb-run -a (DISPLAY='${DISPLAY:-}', MW909_FORCE_XVFB='${MW909_FORCE_XVFB:-}')"
            XVFB=(xvfb-run -a)
        else
            warn "no DISPLAY and no xvfb-run: GUI tests may fail"
        fi
    fi
}

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

# --- staging and process hygiene ------------------------------------------------------
STAGE_DIR=""
GUARD_PGIDS=()

# kill_group <pgid>: TERM, then KILL, whatever is left in a process group.
kill_group() {
    local g="$1" i
    kill -0 -- "-${g}" 2>/dev/null || return 0
    kill -TERM -- "-${g}" 2>/dev/null || true
    for i in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 -- "-${g}" 2>/dev/null || return 0
        sleep 0.5
    done
    warn "process group ${g} ignored TERM; sending KILL"
    kill -KILL -- "-${g}" 2>/dev/null || true
}

# kill_leftovers: every guarded group, then anything still using the stage dir.
kill_leftovers() {
    local g left
    for g in ${GUARD_PGIDS[@]:+"${GUARD_PGIDS[@]}"}; do
        kill_group "${g}"
    done
    if [ -n "${STAGE_DIR}" ]; then
        left="$(pgrep -f -- "${STAGE_DIR}" 2>/dev/null || true)"
        if [ -n "${left}" ]; then
            warn "killing leftover processes using ${STAGE_DIR}: $(printf '%s ' ${left})"
            # shellcheck disable=SC2086
            kill -KILL ${left} 2>/dev/null || true
        fi
    fi
}

_mw909_on_exit() {
    local rc=$?
    trap - EXIT
    kill_leftovers
    if [ -n "${STAGE_DIR}" ] && [ -d "${STAGE_DIR}" ]; then
        rm -rf -- "${STAGE_DIR}"
    fi
    exit "${rc}"
}
trap _mw909_on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# stage_artefact <artefact> <var>: copies a bundle or file into a private temp
# dir (created on first use) and stores the copy's path in <var>.
stage_artefact() {
    local src="$1" __var="$2" dest
    if [ -z "${STAGE_DIR}" ]; then
        STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mw909-validate.XXXXXX")"
    fi
    dest="$(mktemp -d "${STAGE_DIR}/a.XXXX")/$(basename "${src}")"
    cp -a -- "${src}" "${dest}"
    log "staged a copy of ${src} at ${dest}"
    printf -v "${__var}" '%s' "${dest}"
}

# run_guarded <seconds> <command...>: runs the command in its own session with
# a hard timeout, then kills whatever is left in its process group. Returns
# the command's status (124 = timed out).
run_guarded() {
    local secs="$1" pid rc=0
    shift
    # A background job of a non-interactive shell is not a group leader, so
    # setsid execs in place and $! is the new session's (and group's) id.
    setsid timeout --kill-after=30 "${secs}" "$@" <&0 &
    pid=$!
    GUARD_PGIDS+=("${pid}")
    wait "${pid}" || rc=$?
    kill_group "${pid}"
    if [ "${rc}" -eq 124 ] || [ "${rc}" -eq 137 ]; then
        warn "timed out after ${secs} s: $1"
    fi
    return "${rc}"
}

# download <url> <dest> <sha256>: returns 0 ok, 1 offline/unavailable, 2 checksum mismatch.
download() {
    local url="$1" dest="$2" want="$3" got
    mkdir -p "$(dirname "${dest}")"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 2 --connect-timeout 20 -o "${dest}.part" "${url}" || { rm -f "${dest}.part"; return 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "${dest}.part" "${url}" || { rm -f "${dest}.part"; return 1; }
    else
        warn "neither curl nor wget is installed"
        return 1
    fi
    got="$(sha256_of "${dest}.part")"
    if [ "${got}" != "${want}" ]; then
        warn "CHECKSUM MISMATCH for ${url}"
        warn "  expected ${want}"
        warn "  got      ${got}"
        rm -f "${dest}.part"
        return 2
    fi
    mv -f "${dest}.part" "${dest}"
    log "downloaded ${url} (sha256 ${got:0:16}... verified)"
    return 0
}
