#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
#
# Start the Standalone, let it run for a few seconds, ask it to quit and check
# that it does so by itself with exit code 0. Runs with a throw-away HOME so
# the application's settings file lands in the build tree, not in ~/.config.
# Exit 0 = pass, 1 = fail, 77 = skipped (no binary, or no display and no xvfb-run).
#
#   MW909_BUILD_DIR   build tree to look in (default: the directory CTest runs in)
#   MW909_SMOKE_SECONDS  how long to let it run (default 5)
set -u

build_dir="${MW909_BUILD_DIR:-$PWD}"
seconds="${MW909_SMOKE_SECONDS:-5}"

app="$(find "$build_dir" -type f -path '*_artefacts*/Standalone/*' -perm -u+x 2>/dev/null | head -n 1)"
if [ -z "$app" ]; then
    echo "run_standalone_smoke: no Standalone binary under $build_dir; skipping"
    exit 77
fi

# Re-enter under a virtual display whenever xvfb-run exists, so no window ever
# appears on a real desktop; without it, an existing display is used.
if [ -z "${MW909_SMOKE_INNER:-}" ]; then
    if command -v xvfb-run >/dev/null 2>&1; then
        MW909_SMOKE_INNER=1 exec xvfb-run -a "$0" "$@"
    fi
    if [ -z "${DISPLAY:-}" ]; then
        echo "run_standalone_smoke: no display and no xvfb-run; skipping"
        exit 77
    fi
fi

home="$build_dir/standalone-smoke-home"
mkdir -p "$home/.config"

echo "run_standalone_smoke: starting $app"
HOME="$home" XDG_CONFIG_HOME="$home/.config" "$app" &
pid=$!
sleep "$seconds"
if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid"
    echo "run_standalone_smoke: FAILED, the application exited early (code $?)"
    exit 1
fi
# SIGINT is the application's clean quit path.
kill -INT "$pid"
for _ in $(seq 1 100); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid"
    wait "$pid" 2>/dev/null
    echo "run_standalone_smoke: FAILED, the application did not quit within 10 s"
    exit 1
fi
wait "$pid"
status=$?
if [ $status -ne 0 ]; then
    echo "run_standalone_smoke: FAILED, exit code $status"
    exit 1
fi
echo "run_standalone_smoke: ran ${seconds} s and quit cleanly (exit 0)"
exit 0
