#!/usr/bin/env bash
set -euo pipefail
if [[ -z ${DBUS_SESSION_BUS_ADDRESS:-} ]]; then
    exec dbus-run-session -- bash "$0" "$@"
fi
test_program=$(realpath "${1:?Isolated zc_tests executable required}")
mkdir -p "${2:?Artifact directory required}"
artifacts=$(realpath "$2")
export LIBGL_ALWAYS_SOFTWARE=1
# Ensure openbox has a valid menu file so it does not exit on startup.
# CI runners may lack /var/lib/openbox/debian-menu.xml, and Docker runtime
# containers run as an unprivileged user without sudo.  A user-level config
# takes precedence and requires no elevated permissions.
_ob_menu='<?xml version="1.0" encoding="utf-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu"><menu id="root-menu" label="Openbox"></menu></openbox_menu>'
if [[ ! -f /var/lib/openbox/debian-menu.xml ]]; then
    mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/openbox"
    printf '%s\n' "$_ob_menu" > "${XDG_CONFIG_HOME:-$HOME/.config}/openbox/menu.xml"
fi
QT_QPA_PLATFORM=xcb ZCVERSIONBOX_EXPECTED_QPA=xcb ZCVERSIONBOX_SMOKE_OUTPUT="$artifacts/x11.png" \
    xvfb-run -a -s '-screen 0 1280x900x24' bash -c '
        openbox >"$2/openbox.log" 2>&1 & wm_pid=$!
        trap "kill $wm_pid 2>/dev/null || true" EXIT
        for ((i=0; i<50; ++i)); do
            xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q "window id" && break
            kill -0 "$wm_pid" 2>/dev/null || { cat "$2/openbox.log"; exit 1; }
            sleep 0.1
        done
        xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q "window id" || {
            cat "$2/openbox.log"
            echo "Openbox did not become ready" >&2
            exit 1
        }
        "$1" platformRuntimeSmoke closingWithoutTrayExitsWindow -o "$2/x11.txt,txt"
    ' _ "$test_program" "$artifacts"

runtime=$(mktemp -d /tmp/zc-wayland.XXXXXXXX)
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=zc-wayland
weston --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" --idle-time=0 \
    --width=1280 --height=900 > "$artifacts/weston.log" 2>&1 & weston_pid=$!
cleanup()
{
    kill "$weston_pid" 2>/dev/null || true
    wait "$weston_pid" 2>/dev/null || true
    [[ "$runtime" == /tmp/zc-wayland.* ]] && rm -rf -- "$runtime"
}
trap cleanup EXIT
for ((i=0; i<100; ++i)); do
    [[ -S "$runtime/$WAYLAND_DISPLAY" ]] && break
    kill -0 "$weston_pid" 2>/dev/null || { cat "$artifacts/weston.log"; exit 1; }
    sleep 0.1
done
[[ -S "$runtime/$WAYLAND_DISPLAY" ]] || { cat "$artifacts/weston.log"; exit 1; }
QT_QPA_PLATFORM=wayland ZCVERSIONBOX_EXPECTED_QPA=wayland ZCVERSIONBOX_SMOKE_OUTPUT="$artifacts/wayland.png" \
    "$test_program" platformRuntimeSmoke closingWithoutTrayExitsWindow -o "$artifacts/wayland.txt,txt"
