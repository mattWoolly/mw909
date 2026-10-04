#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# install-user.sh — install the built mw909 plugins and standalone for the current user.
#
#   tools/install-user.sh [--dry-run] [--config Release] [--formats vst3,clap,lv2,standalone] <build-dir>
#   tools/install-user.sh [--dry-run] [--formats ...] --uninstall
#
# Install, per format:
#   <build>/.../mw909_artefacts/<config>/VST3/mw909.vst3     ->  ~/.vst3/mw909.vst3
#   <build>/.../mw909_artefacts/<config>/CLAP/mw909.clap     ->  ~/.clap/mw909.clap
#   <build>/.../mw909_artefacts/<config>/LV2/mw909.lv2       ->  ~/.lv2/mw909.lv2
#   <build>/.../mw909_artefacts/<config>/Standalone/mw909    ->  ~/.local/bin/mw909
# 1. remove the previously installed copy (exactly that path, nothing else),
# 2. copy the new one,
# 3. strip --strip-unneeded every ELF object in the destination copy
#    (the build tree keeps its symbols),
# 4. touch it so hosts that cache by mtime rescan it.
# Every action is printed. Only Release artefacts are installed unless
# --config names another configuration.
#
# --dry-run     print what would happen, change nothing
# --uninstall   remove the installed copies
# --formats     comma list, any of vst3 clap lv2 standalone (default: all)
# MW909_INSTALL_ROOT  install under this directory instead of $HOME (for tests)
#
# Exit: 0 done, 1 nothing to install or a step failed, 2 usage error.

set -euo pipefail

product="mw909"
dry=0
uninstall=0
config="Release"
formats="vst3,clap,lv2,standalone"
build_dir=""

usage() { sed -n '4,28p' "$0" | sed 's/^# \{0,1\}//'; }
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1; shift ;;
        --uninstall) uninstall=1; shift ;;
        --config) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; config="$2"; shift 2 ;;
        --formats) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; formats="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "install-user: unknown option $1" >&2; usage >&2; exit 2 ;;
        *) [ -z "${build_dir}" ] || { echo "install-user: more than one build directory" >&2; exit 2; }
           build_dir="$1"; shift ;;
    esac
done

root="${MW909_INSTALL_ROOT:-${HOME:-}}"
if [ -z "${root}" ] || [ ! -d "${root}" ]; then
    echo "install-user: install root '${root}' is not a directory (HOME unset?)" >&2
    exit 2
fi
root="$(cd "${root}" && pwd)"
if [ "$(id -u)" -eq 0 ] && [ -z "${MW909_INSTALL_ROOT:-}" ]; then
    echo "install-user: refusing to run as root; this installs for one user" >&2
    exit 2
fi

say() { printf 'install-user: %s\n' "$*"; }
run() {
    if [ "${dry}" -eq 1 ]; then
        printf '  [dry-run] %s\n' "$*"
    else
        printf '  + %s\n' "$*"
        "$@"
    fi
}

# format -> artefact subdir, bundle name, destination dir
declare -A subdir=([vst3]=VST3 [clap]=CLAP [lv2]=LV2 [standalone]=Standalone)
declare -A bundle=([vst3]="${product}.vst3" [clap]="${product}.clap" [lv2]="${product}.lv2" [standalone]="${product}")
declare -A destdir=([vst3]="${root}/.vst3" [clap]="${root}/.clap" [lv2]="${root}/.lv2" [standalone]="${root}/.local/bin")

IFS=',' read -r -a wanted <<< "${formats}"
for f in "${wanted[@]}"; do
    [ -n "${subdir[$f]:-}" ] || { echo "install-user: unknown format '$f' (vst3 clap lv2 standalone)" >&2; exit 2; }
done

[ "${dry}" -eq 1 ] && say "DRY RUN: nothing will be changed"
say "install root: ${root}"

# Removes exactly <destdir>/<bundle> after checking the path is what we expect.
remove_installed() {
    local dest="$1" parent="$2" name="$3"
    if [ "$(dirname "${dest}")" != "${parent}" ] || [ "$(basename "${dest}")" != "${name}" ]; then
        echo "install-user: internal error: refusing to remove unexpected path ${dest}" >&2
        exit 1
    fi
    if [ -L "${dest}" ]; then
        say "removing symlink ${dest} -> $(readlink "${dest}")"
        run rm -f -- "${dest}"
    elif [ -e "${dest}" ]; then
        local count
        count="$(find "${dest}" | wc -l)"
        say "removing old ${dest} (${count} filesystem entries)"
        run rm -rf -- "${dest}"
    fi
}

# --- uninstall ------------------------------------------------------------------------
if [ "${uninstall}" -eq 1 ]; then
    [ -z "${build_dir}" ] || say "note: --uninstall ignores the build directory"
    for f in "${wanted[@]}"; do
        dest="${destdir[$f]}/${bundle[$f]}"
        if [ -e "${dest}" ] || [ -L "${dest}" ]; then
            remove_installed "${dest}" "${destdir[$f]}" "${bundle[$f]}"
        else
            say "${f}: ${dest} is not installed"
        fi
    done
    say "uninstall done"
    exit 0
fi

# --- install --------------------------------------------------------------------------
[ -n "${build_dir}" ] || { echo "install-user: give the build directory (e.g. build/linux-release)" >&2; usage >&2; exit 2; }
[ -d "${build_dir}" ] || { echo "install-user: ${build_dir} is not a directory" >&2; exit 1; }
build_dir="$(cd "${build_dir}" && pwd)"
say "build dir:    ${build_dir} (config ${config})"

have_strip=1
command -v strip >/dev/null 2>&1 || { have_strip=0; say "WARNING: strip not found; installing unstripped binaries"; }

installed=0
for f in "${wanted[@]}"; do
    src="$(find "${build_dir}" \( -name _deps -o -name CMakeFiles \) -prune -o \
        -path "*/${product}_artefacts/${config}/${subdir[$f]}/${bundle[$f]}" -print 2>/dev/null \
        | awk -F/ '{ print NF "\t" $0 }' | sort -n -k1,1 -s | sed -n 1p | cut -f2-)"
    # (the shallowest match: a sub-build nested in <build-dir> never wins over its own artefacts)
    if [ -z "${src}" ]; then
        say "${f}: no ${config} artefact ${subdir[$f]}/${bundle[$f]} in ${build_dir}, skipped"
        continue
    fi
    dest="${destdir[$f]}/${bundle[$f]}"
    say "${f}: ${src} -> ${dest}"
    remove_installed "${dest}" "${destdir[$f]}" "${bundle[$f]}"
    [ -d "${destdir[$f]}" ] || run mkdir -p -- "${destdir[$f]}"
    run cp -a -- "${src}" "${dest}"

    # ELF objects in the destination copy: every *.so, the .clap itself and
    # the standalone executable. (In a dry run the destination does not exist
    # yet; list them from the source.)
    probe="${dest}"
    [ "${dry}" -eq 1 ] && probe="${src}"
    if [ "${f}" = standalone ]; then
        sos=("${probe}")
    else
        mapfile -t sos < <(find "${probe}" -type f \( -name '*.so' -o -name '*.clap' \) | sort)
    fi
    for so in ${sos[@]:+"${sos[@]}"}; do
        target="${dest}${so#"${probe}"}"
        if ! file -L "${so}" | grep -qE 'ELF.*(shared object|executable)'; then
            say "  ${target} is not an ELF object, not stripped"
            continue
        fi
        if [ "${have_strip}" -eq 1 ]; then
            before="$(stat -c %s "${so}")"
            run strip --strip-unneeded -- "${target}"
            if [ "${dry}" -eq 0 ]; then
                say "  stripped ${target}: ${before} -> $(stat -c %s "${target}") bytes"
            fi
        fi
    done
    run touch -- "${dest}"
    installed=$((installed + 1))
done

if [ "${installed}" -eq 0 ]; then
    say "nothing installed: no ${config} artefacts found (build with --preset linux-release first)"
    exit 1
fi
say "done: ${installed} bundle(s) $([ "${dry}" -eq 1 ] && echo 'would be installed' || echo installed)"
