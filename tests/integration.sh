#!/bin/sh
set -eu
if ! command -v Xvfb >/dev/null 2>&1; then
    echo "SKIP GUI launch checks: Xvfb is unavailable"
    exit 0
fi
display=:$((90 + ($$ % 100)))
Xvfb "$display" -screen 0 1024x768x24 >/tmp/terminator-xvfb.log 2>&1 &
xvfb_pid=$!
app_pid=
cleanup() {
    if [ -n "$app_pid" ]; then kill "$app_pid" 2>/dev/null || true; wait "$app_pid" 2>/dev/null || true; fi
    kill "$xvfb_pid" 2>/dev/null || true
    wait "$xvfb_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
export DISPLAY="$display"
i=0
socket="/tmp/.X11-unix/X${display#:}"
while [ ! -S "$socket" ] && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
cc -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L tests/gui_driver.c -lX11 -o build/gui_driver
./terminator >/tmp/terminator-thread.log 2>&1 &
app_pid=$!
timeout 10s ./build/gui_driver /tmp/terminator-gui-thread
kill -0 "$app_pid"
kill "$app_pid"
wait "$app_pid" 2>/dev/null || true
app_pid=
./terminator useProcess=True >/tmp/terminator-process.log 2>&1 &
app_pid=$!
timeout 10s ./build/gui_driver /tmp/terminator-gui-process
kill -0 "$app_pid"
kill "$app_pid"
wait "$app_pid" 2>/dev/null || true
app_pid=
echo "X11 GUI interaction checks passed (thread and process modes)"
