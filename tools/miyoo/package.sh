#!/usr/bin/env bash
set -euo pipefail

# Package the cmake install prefix into a tree that mirrors the SD card
# root so a single `cp -R` or `rsync -a` merges cleanly into
# /mnt/SDCARD/. Produces:
#
#   <OUTPUT_DIR>/Roms/PORTS/Games/3s-arm/           # the app dir
#       3s-arm                                       # binary at root
#       launch.sh                                    # standalone fallback
#       lib/libSDL3.so*                              # bundled SDL3 only
#       licenses/                                    # license texts
#       resources/PUT_SF33RD_AFS_HERE.txt
#   <OUTPUT_DIR>/Roms/PORTS/Shortcuts/Action/3s-arm.port   # OSD entry
#   <OUTPUT_DIR>/Roms/PORTS/Imgs/PUT_3S_ARM_PNG_HERE.txt   # icon placeholder
#
# Deploy via:
#   tools/miyoo/deploy.sh                          # SSH/rsync to device
#   rsync -a build/miyoo-package/ /Volumes/<SD>/   # SD-card fallback
#
# IMPORTANT: libmi_*.so are NOT bundled. Runtime resolves
# libmi_sys.so / libmi_gfx.so / libmi_common.so from the device's
# /config/lib/, populated by the OnionOS firmware BSP. This matches
# the universal community deployment pattern.
#
# OnionOS surfaces the app via the .port shortcut under Shortcuts/Action/
# (3rd Strike is a fighting game; Action is the closest stock genre).
# The .port script invokes /mnt/SDCARD/Emu/PORTS/launch_standalone.sh
# which handles cwd, LD_LIBRARY_PATH=GameDir/lib prepend, performance
# governor, and the SF33RD.AFS existence check. The shortcut also
# pre-pends /config/lib to LD_LIBRARY_PATH so libmi_*.so resolves.

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <cmake-install-prefix> <output-dir>" >&2
    exit 1
fi

INSTALL_PREFIX="$1"
OUTPUT_DIR="$2"

if [ ! -d "$INSTALL_PREFIX" ]; then
    echo "Install prefix not found: $INSTALL_PREFIX" >&2
    exit 1
fi

APP_DIR="$OUTPUT_DIR/Roms/PORTS/Games/3s-arm"
SHORTCUTS_DIR="$OUTPUT_DIR/Roms/PORTS/Shortcuts/Action"
IMGS_DIR="$OUTPUT_DIR/Roms/PORTS/Imgs"

rm -rf "$OUTPUT_DIR"
mkdir -p "$APP_DIR/lib" "$APP_DIR/licenses" "$APP_DIR/resources" \
    "$SHORTCUTS_DIR" "$IMGS_DIR"

# Binary at root of the app dir (OnionOS app-dir convention).
if [ -f "$INSTALL_PREFIX/bin/3s-arm" ]; then
    cp "$INSTALL_PREFIX/bin/3s-arm" "$APP_DIR/3s-arm"
elif [ -f "$INSTALL_PREFIX/3S-ARM.app/Contents/MacOS/3S-ARM" ]; then
    cp "$INSTALL_PREFIX/3S-ARM.app/Contents/MacOS/3S-ARM" "$APP_DIR/3s-arm"
else
    echo "Missing binary: $INSTALL_PREFIX/bin/3s-arm" >&2
    exit 1
fi

# Bundle SDL3 only. The cmake install rule writes libSDL3.so* into
# $INSTALL_PREFIX/lib; copy that subset and explicitly exclude any
# libmi_*.so that might leak in (they should not — vendored .so files
# are link-time only and never staged into the install prefix's lib).
#
# --copy-links / -L dereferences symlinks during rsync. Required because
# the deployed package lands on an exFAT SD card that does not preserve
# POSIX symlinks. Without -L, libSDL3.so.0 ships as an opaque 16-byte
# AppleDouble blob the Linux loader can't follow, producing "invalid
# ELF header" at first-light. Dereferencing produces real-file copies
# under each symlink name (libSDL3.so, libSDL3.so.0, libSDL3.so.0.4.4
# all become full 7MB files — wasteful but correct).
if [ -d "$INSTALL_PREFIX/lib" ]; then
    rsync -a --copy-links --no-owner --no-group \
        --exclude='libmi_*.so*' \
        --exclude='libmi_*.so' \
        "$INSTALL_PREFIX/lib/" "$APP_DIR/lib/"
fi

if [ -d "$INSTALL_PREFIX/share/3s-arm/licenses" ]; then
    rsync -a --no-owner --no-group \
        "$INSTALL_PREFIX/share/3s-arm/licenses/" "$APP_DIR/licenses/"
fi

# Standalone-fallback launcher inside the app dir. Bypassed when the
# user reaches the app via the OnionOS Ports menu (which goes through
# launch_standalone.sh via the .port shortcut below). Useful for SSH
# testing: `ssh root@miyoo "cd /mnt/SDCARD/Roms/PORTS/Games/3s-arm && ./launch.sh"`.
cat > "$APP_DIR/launch.sh" <<'LAUNCHER'
#!/bin/sh
# 3s-arm fallback launcher (SSH / direct exec). Mirrors the .port
# shortcut env, minus the governor toggle, for manual testing. Audio
# routes through the direct MI_AO backend baked into the binary —
# no SDL3 audio, no /dev/dsp, no libpadsp shim.

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SELF_DIR"

# audioserver holds /dev/MI_AO exclusively; if it's running when 3s-arm
# starts, MI_AO_Enable() inside the direct MI_AO backend hits EBUSY and
# audio falls back to silence. The .port shortcut handles this via
# KillAudioserver=1 (consumed by /mnt/SDCARD/Emu/PORTS/launch_standalone.sh)
# but launch.sh runs outside that script, so we kill audioserver here.
# Idempotent — safe if it's already gone.
killall audioserver 2>/dev/null || true

# /config/lib stays first so libmi_*.so (firmware BSP) — including
# libmi_ao.so used by the direct audio backend — can never be
# shadowed by an accidentally-bundled libmi_*.so under $SELF_DIR/lib.
# /mnt/SDCARD/miyoo/lib remains in the path for SDL3's transitive
# deps (libasound etc.) but we no longer LD_PRELOAD libpadsp from
# there.
export LD_LIBRARY_PATH="/config/lib:/mnt/SDCARD/miyoo/lib:$SELF_DIR/lib:${LD_LIBRARY_PATH:-}"

export SDL_VIDEODRIVER=evdev
# SDL3 was built without libudev. Without that, SDL3's evdev video driver
# only enumerates devices listed in SDL_EVDEV_DEVICES (see
# release-3.4.4 src/core/linux/SDL_evdev.c). Onion exposes a single
# event node combining the gamepad+keymon stream at /dev/input/event0.
export SDL_EVDEV_DEVICES=2:/dev/input/event0

# Audio: direct MI_AO backend baked into the 3s-arm binary
# (src/port/sound/mi_ao_backend.c). Tell SDL3 explicitly to use the
# dummy audio driver so its SDL_InitSubSystem(AUDIO) call doesn't
# open /dev/dsp behind our back and fight us for MI_AO. The 3s-arm
# binary never calls SDL_OpenAudioDeviceStream on this profile, but
# SDL3 may still probe a device during init if a real driver is
# auto-selected.
export SDL_AUDIODRIVER=dummy
# To A/B against the legacy SDL3+OSS path, set THIRDSARM_AUDIO_SDL=1
# AND override SDL_AUDIODRIVER=dsp here (and put libpadsp back in
# LD_PRELOAD too).
export LD_PRELOAD="$SELF_DIR/lib/libSDL3.so.0"

# Stock clock is sufficient: measured 6.87ms work / 16.75ms frame at
# 1600 MHz; at 1200 MHz extrapolates to ~9.2ms — still well under the
# 16.67ms 60Hz budget. Onion's PerformanceMode=1 already pins the
# governor; no overclock helper needed.

exec ./3s-arm "$@"
LAUNCHER
chmod +x "$APP_DIR/launch.sh"

# Ship a Miyoo-tuned keymap so existing on-device keymap files (auto-
# written from old defaults the very first time the binary booted) get
# overwritten via rsync. Mapping mirrors the C-level Miyoo defaults in
# src/port/config/keymap.c. Capcom-PS2-style: punches Y/X/L1, kicks B/A/R1.
cat > "$APP_DIR/keymap" <<'KEYMAP'
up = Up
down = Down
left = Left
right = Right
north = Left Shift
west = Left Alt
south = Left Ctrl
east = Space
left-shoulder = E
right-shoulder = Backspace
left-trigger = Tab
right-trigger = T
back = Right Ctrl
start = Return
KEYMAP

cat > "$APP_DIR/resources/PUT_SF33RD_AFS_HERE.txt" <<'SENTINEL'
SF33RD.AFS placement
====================

3s-arm needs SF33RD.AFS to run. Drop the file alongside this README
(at <APP_DIR>/resources/SF33RD.AFS).

Asset placement is the user's responsibility per legal constraints
(consistent with the MiSTer port). launch_standalone.sh checks that
this file exists before launching the binary; if missing, OnionOS
shows an error popup pointing at the expected path.
SENTINEL

# OnionOS Ports shortcut. Lives under Shortcuts/Action/ — fighting
# games slot under "Action" since OnionOS doesn't ship a stock
# "Fighting" genre.
cat > "$SHORTCUTS_DIR/3s-arm.port" <<'PORTFILE'
#!/bin/sh
# Standalone Ports Script — 3rd Strike (Cut 1)

# main configuration :
GameName="3rd Strike"
GameDir="3s-arm"
GameExecutable="3s-arm"
GameDataFile="SF33RD.AFS"

# additional configuration
# KillAudioserver=1: the direct MI_AO backend in src/port/sound/mi_ao_backend.c
# opens MI_AO exclusively. audioserver holds MI_AO too; both can't coexist.
# launch_standalone.sh kills audioserver before exec when this is 1.
KillAudioserver=1
PerformanceMode=1

# specific to this port :
Arguments=""
export SDL_VIDEODRIVER=evdev
# SDL3 was built without libudev. Without it, SDL3's evdev video driver
# only enumerates devices listed in SDL_EVDEV_DEVICES (see
# release-3.4.4 src/core/linux/SDL_evdev.c). Onion exposes a single
# event node combining the gamepad+keymon stream at /dev/input/event0.
export SDL_EVDEV_DEVICES=2:/dev/input/event0
# Audio: direct MI_AO backend baked into the binary
# (src/port/sound/mi_ao_backend.c). Tell SDL3 explicitly to use the
# dummy audio driver so its SDL_InitSubSystem(AUDIO) call doesn't
# open /dev/dsp behind our back and fight us for MI_AO. The 3s-arm
# binary never calls SDL_OpenAudioDeviceStream on this profile, but
# SDL3 may still probe a device during init if a real driver is
# auto-selected.
export SDL_AUDIODRIVER=dummy
# To A/B compare against the legacy SDL3+OSS path, set
#   export THIRDSARM_AUDIO_SDL=1
# AND override SDL_AUDIODRIVER=dsp here (and add libpadsp back to
# LD_PRELOAD if you want the broken rate-translation layer too).
export LD_PRELOAD="/mnt/SDCARD/Roms/PORTS/Games/3s-arm/lib/libSDL3.so.0"
# /config/lib stays first so libmi_*.so (including libmi_ao.so used
# by the direct audio backend) is resolved from the firmware BSP.
# /mnt/SDCARD/miyoo/lib remains in the path for SDL3's transitive
# deps (libasound, etc.).
export LD_LIBRARY_PATH="/config/lib:/mnt/SDCARD/miyoo/lib:$LD_LIBRARY_PATH"
# Pin Paths_GetPrefPath() lookups to the on-SD game dir so resources/
# config/ keymap/ etc. resolve to the launch_standalone.sh cwd rather
# than $HOME/.local/share/CrowdedStreet/3S-ARM/. Mirrors MiSTer wrapper's
# THIRDSARM_HOME=/media/fat/games/3s-arm.
export THIRDSARM_HOME=/mnt/SDCARD/Roms/PORTS/Games/3s-arm
# Vsync ON: with the SDL render path skipped + use_native_render_path
# forced, frame work is ~7ms / 16.67ms budget. FBIOPAN_DISPLAY's vblank
# wait fits cleanly in the idle slot, eliminates tearing.
export THIRDSARM_NO_VSYNC=0

# Stock clock is sufficient: measured 6.87ms work at 1600 MHz; at
# stock 1200 MHz extrapolates to ~9.2ms — still under the 16.67ms
# 60Hz budget. PerformanceMode=1 already pins the governor.

# Capture launch + binary stdout/stderr to the SD card so a failed boot
# leaves a forensic trail readable after remounting on a host. OnionOS
# does not capture .port-script output by default; without this the
# binary's panic messages / missing-symbol errors are lost. Cut 1 keeps
# this on permanently; Cut 2 audio polish can revisit if log noise
# becomes a perf hit.
LOG="/mnt/SDCARD/Roms/PORTS/Games/3s-arm/launch.log"

{
    echo "===================================================================="
    echo ":: 3s-arm .port script — $(date '+%Y-%m-%d %H:%M:%S')"
    echo "===================================================================="
    echo "GameName=$GameName  GameDir=$GameDir  GameExecutable=$GameExecutable"
    echo "GameDataFile=$GameDataFile"
    echo "PerformanceMode=$PerformanceMode  KillAudioserver=$KillAudioserver"
    echo "SDL_VIDEODRIVER=$SDL_VIDEODRIVER  SDL_AUDIODRIVER=$SDL_AUDIODRIVER"
    echo "Pre-launcher LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
    echo "/config/lib SigmaStar BSP libs:"
    ls -la /config/lib/libmi_*.so 2>&1 | head -20 || echo "  (could not list /config/lib)"
    echo "===================================================================="

    /mnt/SDCARD/Emu/PORTS/launch_standalone.sh "$GameName" "$GameDir" "$GameExecutable" "$Arguments" "$GameDataFile" "$KillAudioserver" "$PerformanceMode"
    rc=$?

    echo "===================================================================="
    echo ":: launch_standalone.sh exited with rc=$rc"
    echo "===================================================================="
} > "$LOG" 2>&1
PORTFILE
chmod +x "$SHORTCUTS_DIR/3s-arm.port"

# Icon placeholder. Drop an actual PNG (~250x250 recommended; OnionOS
# scales to the menu tile) at Imgs/3s-arm.png and the OnionOS Ports menu
# will use it. Without an image, the entry shows a default tile — ugly
# but functional.
cat > "$IMGS_DIR/PUT_3S_ARM_PNG_HERE.txt" <<'IMG_SENTINEL'
Optional icon
=============

Drop a PNG named "3s-arm.png" alongside this file. OnionOS will pick it
up automatically as the Ports-menu tile for 3rd Strike. ~250x250 px is
typical; the menu scales to fit. Without an icon the entry shows a
default tile.
IMG_SENTINEL

# Make the binary executable.
chmod +x "$APP_DIR/3s-arm"

echo "Miyoo package created at: $OUTPUT_DIR"
echo "Layout (relative to SD card root):"
( cd "$OUTPUT_DIR" && find Roms -maxdepth 5 -mindepth 1 \( -type f -o -type l \) | sort )
echo ""
echo "Deploy (SSH, default):"
echo "  tools/miyoo/deploy.sh --src $OUTPUT_DIR"
echo "Deploy (SD-card fallback):"
echo "  rsync -a $OUTPUT_DIR/ /Volumes/<SD-CARD>/"
