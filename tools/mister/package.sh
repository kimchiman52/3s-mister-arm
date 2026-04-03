#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <cmake-install-prefix> <output-dir>"
    exit 1
fi

INSTALL_PREFIX="$1"
OUTPUT_DIR="$2"

if [ ! -d "$INSTALL_PREFIX" ]; then
    echo "Install prefix not found: $INSTALL_PREFIX"
    exit 1
fi

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/bin" "$OUTPUT_DIR/lib" "$OUTPUT_DIR/licenses" "$OUTPUT_DIR/scripts"

if [ -f "$INSTALL_PREFIX/bin/3sx" ]; then
    cp "$INSTALL_PREFIX/bin/3sx" "$OUTPUT_DIR/bin/3sx"
elif [ -f "$INSTALL_PREFIX/3SX.app/Contents/MacOS/3SX" ]; then
    cp "$INSTALL_PREFIX/3SX.app/Contents/MacOS/3SX" "$OUTPUT_DIR/bin/3sx"
else
    echo "Missing binary: $INSTALL_PREFIX/bin/3sx"
    echo "Also checked: $INSTALL_PREFIX/3SX.app/Contents/MacOS/3SX"
    exit 1
fi

if [ -d "$INSTALL_PREFIX/lib" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/lib/" "$OUTPUT_DIR/lib/"
elif [ -d "$INSTALL_PREFIX/3SX.app/Contents/Frameworks" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/3SX.app/Contents/Frameworks/" "$OUTPUT_DIR/lib/"
fi

if [ -d "$INSTALL_PREFIX/share/3sx/licenses" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/share/3sx/licenses/" "$OUTPUT_DIR/licenses/"
elif [ -d "$INSTALL_PREFIX/3SX.app/Contents/Resources/licenses" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/3SX.app/Contents/Resources/licenses/" "$OUTPUT_DIR/licenses/"
fi

cat > "$OUTPUT_DIR/scripts/run-3sx.sh" <<'LAUNCHER'
#!/bin/sh
set -eu

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(CDPATH= cd -- "${SELF_DIR}/.." && pwd)"

export THREESX_HOME="${THREESX_HOME:-$APP_DIR}"
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-alsa}"

exec "$APP_DIR/bin/3sx" "$@"
LAUNCHER

cat > "$OUTPUT_DIR/scripts/launch-osd.sh" <<'OSD_LAUNCHER'
#!/bin/sh
set -eu

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(CDPATH= cd -- "${SELF_DIR}/.." && pwd)"
LOG_DIR="$APP_DIR/logs"
LOG_PATH="$LOG_DIR/last-run.log"

mkdir -p "$LOG_DIR"

# Keep OSD launches quiet on the text console; inspect logs/last-run.log for diagnostics.
{
    echo "==== 3SX OSD launch ===="
    date 2>/dev/null || true
    echo "pid=$$ ppid=$PPID"
    echo "tty=$(tty 2>&1 || true)"
    echo "active_vt=$(cat /sys/class/tty/tty0/active 2>/dev/null || true)"
    echo "args=$*"
    echo "------------------------"
} >"$LOG_PATH"

# Native video: bypass the MiSTer scaler for analog CRT output.
# Set THREESX_NATIVE_VIDEO=0 to disable and use the scaler path instead.
export THREESX_NATIVE_VIDEO="${THREESX_NATIVE_VIDEO:-1}"

# On stock MiSTer OSD launches, force SDL onto dummy video and software renderer.
# Final on-screen output is handled by the fbdev presenter path in-app.
export SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER:-dummy}"
export SDL_RENDER_DRIVER="${SDL_RENDER_DRIVER:-software}"
{
    echo "SDL_VIDEO_DRIVER=$SDL_VIDEO_DRIVER"
    echo "SDL_RENDER_DRIVER=$SDL_RENDER_DRIVER"
    echo "SDL_AUDIO_DRIVER=${SDL_AUDIO_DRIVER:-}"
} >>"$LOG_PATH"

restore_console() {
    active_vt="$(cat /sys/class/tty/tty0/active 2>/dev/null || true)"
    console_path="/dev/tty1"

    case "$active_vt" in
        tty[0-9]*)
            console_path="/dev/$active_vt"
            ;;
    esac

    if [ ! -c "$console_path" ]; then
        return 0
    fi

    kbd_mode -a <"$console_path" >/dev/null 2>&1 || true
    TERM=linux setterm -reset -default -blank poke -cursor on -clear all >"$console_path" 2>/dev/null || true
    printf '\033c' >"$console_path" 2>/dev/null || true
    chvt "${console_path#/dev/tty}" >/dev/null 2>&1 || true
}

terminate_child() {
    status="$1"
    restore_console
    echo "exit=$status" >>"$LOG_PATH"
    exit "$status"
}

trap 'terminate_child 130' INT
trap 'terminate_child 129' HUP
trap 'terminate_child 143' TERM

if "$SELF_DIR/run-3sx.sh" "$@" >>"$LOG_PATH" 2>&1; then
    trap - INT HUP TERM
    restore_console
    echo "exit=0" >>"$LOG_PATH"
    exit 0
else
    status=$?
    trap - INT HUP TERM
    restore_console
    echo "exit=$status" >>"$LOG_PATH"
    exit "$status"
fi
OSD_LAUNCHER

chmod +x "$OUTPUT_DIR/scripts/run-3sx.sh" "$OUTPUT_DIR/scripts/launch-osd.sh" \
    "$OUTPUT_DIR/bin/3sx"

echo "MiSTer package created at: $OUTPUT_DIR"
