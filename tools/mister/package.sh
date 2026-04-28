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

if [ -f "$INSTALL_PREFIX/bin/3s-arm" ]; then
    cp "$INSTALL_PREFIX/bin/3s-arm" "$OUTPUT_DIR/bin/3s-arm"
elif [ -f "$INSTALL_PREFIX/3S-ARM.app/Contents/MacOS/3S-ARM" ]; then
    cp "$INSTALL_PREFIX/3S-ARM.app/Contents/MacOS/3S-ARM" "$OUTPUT_DIR/bin/3s-arm"
else
    echo "Missing binary: $INSTALL_PREFIX/bin/3s-arm"
    echo "Also checked: $INSTALL_PREFIX/3S-ARM.app/Contents/MacOS/3S-ARM"
    exit 1
fi

if [ -d "$INSTALL_PREFIX/lib" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/lib/" "$OUTPUT_DIR/lib/"
elif [ -d "$INSTALL_PREFIX/3S-ARM.app/Contents/Frameworks" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/3S-ARM.app/Contents/Frameworks/" "$OUTPUT_DIR/lib/"
fi

if [ -d "$INSTALL_PREFIX/share/3s-arm/licenses" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/share/3s-arm/licenses/" "$OUTPUT_DIR/licenses/"
elif [ -d "$INSTALL_PREFIX/3S-ARM.app/Contents/Resources/licenses" ]; then
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/3S-ARM.app/Contents/Resources/licenses/" "$OUTPUT_DIR/licenses/"
fi

if [ -d "$INSTALL_PREFIX/share/3s-arm/assets" ]; then
    mkdir -p "$OUTPUT_DIR/assets"
    rsync -a --no-owner --no-group "$INSTALL_PREFIX/share/3s-arm/assets/" "$OUTPUT_DIR/assets/"
fi

cat > "$OUTPUT_DIR/scripts/run-3s-arm.sh" <<'LAUNCHER'
#!/bin/sh
set -eu

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(CDPATH= cd -- "${SELF_DIR}/.." && pwd)"

# One-shot: ensure net.core.rmem_max is bumped at boot. Uses user-startup.sh
# so it survives MiSTer OTA updates. Idempotent via the magic marker.
if [ -w /media/fat/linux/user-startup.sh ] && ! grep -q '# 3S-ARM-NETPLAY-SYSCTL' /media/fat/linux/user-startup.sh 2>/dev/null; then
    cat >> /media/fat/linux/user-startup.sh <<'SYSCTL_EOF'

# 3S-ARM-NETPLAY-SYSCTL (added by 3s-arm first-run)
sysctl -w net.core.rmem_max=524288 2>/dev/null || true
SYSCTL_EOF
fi
# Best-effort apply to current boot (no-op if user-startup already ran).
sysctl -w net.core.rmem_max=524288 2>/dev/null || true

export THIRDSARM_HOME="${THIRDSARM_HOME:-$APP_DIR}"
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-alsa}"

exec "$APP_DIR/bin/3s-arm" "$@"
LAUNCHER

cat > "$OUTPUT_DIR/scripts/launch-osd.sh" <<'OSD_LAUNCHER'
#!/bin/sh
set -eu

SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
APP_DIR="$(CDPATH= cd -- "${SELF_DIR}/.." && pwd)"
LOG_DIR="$APP_DIR/logs"
LOG_PATH="$LOG_DIR/last-run.log"

mkdir -p "$LOG_DIR"

# Rotate the previous session's logs so the next launch preserves them
# instead of clobbering. This matters for post-mortem on netplay desyncs:
# when a desync kicks one peer back to title and the wrapper then restarts
# the runtime, the post-restart truncate would otherwise overwrite the
# actual netplay-match log we need to root-cause the divergence.
[ -f "$LOG_PATH" ] && mv -f "$LOG_PATH" "$LOG_DIR/last-run-prev.log" 2>/dev/null || true
[ -f "$LOG_DIR/backend.log" ] && mv -f "$LOG_DIR/backend.log" "$LOG_DIR/backend-prev.log" 2>/dev/null || true

# Keep OSD launches quiet on the text console; inspect logs/last-run.log for diagnostics.
{
    echo "==== 3S-ARM OSD launch ===="
    date 2>/dev/null || true
    echo "pid=$$ ppid=$PPID"
    echo "tty=$(tty 2>&1 || true)"
    echo "active_vt=$(cat /sys/class/tty/tty0/active 2>/dev/null || true)"
    echo "args=$*"
    echo "------------------------"
} >"$LOG_PATH"

# Native video: bypass the MiSTer scaler for analog CRT output.
# Set THIRDSARM_NATIVE_VIDEO=0 to disable and use the scaler path instead.
export THIRDSARM_NATIVE_VIDEO="${THIRDSARM_NATIVE_VIDEO:-1}"

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

if "$SELF_DIR/run-3s-arm.sh" "$@" >>"$LOG_PATH" 2>&1; then
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

chmod +x "$OUTPUT_DIR/scripts/run-3s-arm.sh" "$OUTPUT_DIR/scripts/launch-osd.sh" \
    "$OUTPUT_DIR/bin/3s-arm"

echo "MiSTer package created at: $OUTPUT_DIR"
