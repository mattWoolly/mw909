#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# check_license_headers.sh [repo-root] — every C/C++, CMake, shell and Python
# file in the tree must open with the two-line licence header:
#
#   // SPDX-License-Identifier: AGPL-3.0-or-later   (.h .hh .hpp .hxx .inl .ipp .c .cc .cpp .cxx .m .mm)
#   // mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
#   # SPDX-License-Identifier: AGPL-3.0-or-later    (.cmake CMakeLists.txt .sh .bash .py, *.in
#                                                    templates, extensionless scripts)
#   # mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# A script may have its #! line first. A *.in template takes the comment
# style of the file it generates (Foo.h.in: //, anything else: #). Every
# executable or #! file under tools/ and scripts/ is checked whatever its
# name. Files are found with `find` from the repository root, not git, so the
# check works in a tree that is not a repository. Skipped: research/
# (third-party citations), .deps/ (third-party checkouts), the top-level build
# trees (./build, ./build-*, ./cmake-build-*), _deps/, .git/, node_modules/
# and .cache/.
# Exit: 0 all present, 1 a header is missing or wrong, 2 nothing found.

set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "${root}"

spdx="SPDX-License-Identifier: AGPL-3.0-or-later"
notice="mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio."

checked=0
bad=0
# C-family sources by extension; CMake, shell, Python and configure-time
# templates by name; and every executable or #! file under tools/ and
# scripts/ whatever its name (an extensionless script is still a script).
c_like=(-name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' -o -name '*.inl' -o -name '*.ipp'
        -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.mm' -o -name '*.m')
hash_like=(-name '*.cmake' -o -name CMakeLists.txt -o -name '*.sh' -o -name '*.bash' -o -name '*.py')
is_c_like() {
    case "$1" in
        *.h|*.hh|*.hpp|*.hxx|*.inl|*.ipp|*.c|*.cc|*.cpp|*.cxx|*.mm|*.m) return 0 ;;
        *.in) is_c_like "${1%.in}" ;;
        *) return 1 ;;
    esac
}
list_files() {
    # Build trees are pruned at the top level only (./build, ./build-*,
    # ./cmake-build-*) plus CMake's _deps anywhere: a source directory that
    # happens to be called "build" deeper down is still checked.
    find . \( -path ./research -o -path ./.deps -o -path ./.git -o -path ./.cache \
              -o -path ./build -o -path './build-*' -o -path './cmake-build-*' -o -name _deps \
              -o -name node_modules \) -prune -o \
        -type f \( "${c_like[@]}" -o "${hash_like[@]}" -o -name '*.in' \) -print0
    # Scripts without an extension: executable, or starting with "#!".
    find ./tools ./scripts -type f ! -name '*.*' -print0 2>/dev/null \
        | while IFS= read -r -d '' f; do
            if [ -x "$f" ] || [ "$(head -c 2 "$f")" = '#!' ]; then printf '%s\0' "$f"; fi
        done
}
while IFS= read -r -d '' f; do
    checked=$((checked + 1))
    if is_c_like "$f"; then prefix="//"; else prefix="#"; fi
    first="$(sed -n '1p' "$f")"
    second="$(sed -n '2p' "$f")"
    if [[ "${first}" == '#!'* ]]; then
        first="${second}"
        second="$(sed -n '3p' "$f")"
    fi
    if [ "${first}" != "${prefix} ${spdx}" ] || [ "${second}" != "${prefix} ${notice}" ]; then
        echo "MISSING OR WRONG HEADER: ${f#./}"
        echo "    line 1: ${first:0:100}"
        echo "    line 2: ${second:0:100}"
        bad=$((bad + 1))
    fi
done < <(list_files | sort -zu)

if [ "${checked}" -eq 0 ]; then
    echo "license headers: no files found under ${root}" >&2
    exit 2
fi
if [ "${bad}" -ne 0 ]; then
    echo "license headers: ${bad} of ${checked} file(s) lack the two-line header:"
    echo "    // ${spdx}      (C/C++; '#' instead of '//' in CMake, shell and Python files)"
    echo "    // ${notice}"
    exit 1
fi
echo "license headers: all ${checked} file(s) carry the two-line header"
