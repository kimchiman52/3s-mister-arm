#!/usr/bin/env bash
set -euo pipefail

# Push a built Miyoo runtime tree onto the device over SSH (dropbear).
#
# Default source is build/miyoo-package/ — the SD-card-rooted layout
# produced by tools/miyoo/package.sh (and re-emitted by build-game.sh,
# build-runtime-package.sh, build-release.sh).
#
# Default destination is /mnt/SDCARD/ on the OnionOS device. The
# package tree begins at Roms/PORTS/Games/3s-arm/ etc. so a single
# rsync of build/miyoo-package/ onto /mnt/SDCARD/ lands every file at
# the expected SD path.
#
# SAFETY: this script never passes --delete to rsync. The destination
# is a shared SD root that holds save data, screenshots, other ports,
# and the user's SF33RD.AFS placement. Use the --dry-run flag if you
# want to inspect the file list before mutating the device.
#
# Usage:
#   tools/miyoo/deploy.sh                    # full package, defaults
#   tools/miyoo/deploy.sh --dry-run          # show what would change
#   tools/miyoo/deploy.sh --app-only         # only Roms/PORTS/Games/3s-arm/
#   tools/miyoo/deploy.sh --shortcut-only    # only the .port shortcut
#   tools/miyoo/deploy.sh --src <other>      # use a different source tree
#
# Requires: rsync, ssh, sshpass.
#
# Environment overrides:
#   MIYOO_HOST       (default 192.168.1.190)
#   MIYOO_USER       (default root)
#   MIYOO_PASSWORD   (default onion)
#   MIYOO_REMOTE_ROOT (default /mnt/SDCARD)
#   MIYOO_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1 + MIYOO_UNSAFE_CONFIRM_REMOTE_ROOT=<path>
#       — both required to target a remote root other than /mnt/SDCARD.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

host="${MIYOO_HOST:-192.168.1.190}"
user="${MIYOO_USER:-root}"
password="${MIYOO_PASSWORD:-onion}"
remote_root="${MIYOO_REMOTE_ROOT:-/mnt/SDCARD}"
src_dir="${ROOT_DIR}/build/miyoo-package"
mode="full"
dry_run=0
check_only=0
connect_timeout="${MIYOO_CONNECT_TIMEOUT:-10}"

usage() {
    cat <<EOF
Usage:
  tools/miyoo/deploy.sh [options]

Options:
  --src <dir>           Source package tree (default: ${src_dir})
  --host <ip-or-host>   Miyoo host (default: \$MIYOO_HOST or 192.168.1.190)
  --user <name>         SSH user (default: \$MIYOO_USER or root)
  --password <value>    SSH password (default: \$MIYOO_PASSWORD or 'onion')
  --remote-root <path>  Remote SD root (default: \$MIYOO_REMOTE_ROOT or /mnt/SDCARD)
  --app-only            Sync only Roms/PORTS/Games/3s-arm/
  --shortcut-only       Sync only Roms/PORTS/Shortcuts/Action/3s-arm.port
  --dry-run             Pass --dry-run to rsync; no files mutated on device
  --check               Print resolved settings and exit
  --help                Show this message

Defaults assume the OnionOS dev unit. SSH only works while WiFi + the
Onion SSH toggle are both ON; if you get "connection refused", flip
the SSH toggle in OnionOS' network menu.

The script never deletes files on the device. SF33RD.AFS, launch.log,
config, keymap edits, savestates, etc. are preserved automatically
because they're not in the package source tree.
EOF
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 2
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --src)
        src_dir="$2"
        shift 2
        ;;
    --host)
        host="$2"
        shift 2
        ;;
    --user)
        user="$2"
        shift 2
        ;;
    --password)
        password="$2"
        shift 2
        ;;
    --remote-root)
        remote_root="$2"
        shift 2
        ;;
    --app-only)
        mode="app-only"
        shift
        ;;
    --shortcut-only)
        mode="shortcut-only"
        shift
        ;;
    --dry-run)
        dry_run=1
        shift
        ;;
    --check)
        check_only=1
        shift
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        echo "unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

# Reject nonstandard remote roots unless both unsafe overrides are set.
# Mirrors the MiSTer convention: a single "unsafe mode" boolean is not
# enough — the operator must also retype the exact target path.
if [ "${remote_root}" != "/mnt/SDCARD" ]; then
    if [ "${MIYOO_UNSAFE_ALLOW_ANY_REMOTE_ROOT:-0}" != "1" ] || \
       [ "${MIYOO_UNSAFE_CONFIRM_REMOTE_ROOT:-}" != "${remote_root}" ]; then
        echo "refusing nonstandard --remote-root: ${remote_root}" >&2
        echo "set MIYOO_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1 and" >&2
        echo "MIYOO_UNSAFE_CONFIRM_REMOTE_ROOT=${remote_root} to override" >&2
        exit 2
    fi
fi

if [ ! -d "${src_dir}" ]; then
    echo "source tree not found: ${src_dir}" >&2
    echo "build it first: tools/miyoo/build-game.sh" >&2
    exit 2
fi

case "${mode}" in
full)
    rsync_src="${src_dir%/}/"
    rsync_dst="${remote_root%/}/"
    ;;
app-only)
    app_src="${src_dir%/}/Roms/PORTS/Games/3s-arm"
    if [ ! -d "${app_src}" ]; then
        echo "--app-only source missing: ${app_src}" >&2
        exit 2
    fi
    rsync_src="${app_src}/"
    rsync_dst="${remote_root%/}/Roms/PORTS/Games/3s-arm/"
    ;;
shortcut-only)
    shortcut_src="${src_dir%/}/Roms/PORTS/Shortcuts/Action/3s-arm.port"
    if [ ! -f "${shortcut_src}" ]; then
        echo "--shortcut-only source missing: ${shortcut_src}" >&2
        exit 2
    fi
    rsync_src="${shortcut_src}"
    rsync_dst="${remote_root%/}/Roms/PORTS/Shortcuts/Action/3s-arm.port"
    ;;
*)
    echo "internal error: unhandled mode ${mode}" >&2
    exit 2
    ;;
esac

if [ "${check_only}" -eq 1 ]; then
    echo "host=${host}"
    echo "user=${user}"
    echo "remote_root=${remote_root}"
    echo "mode=${mode}"
    echo "src=${rsync_src}"
    echo "dst=${rsync_dst}"
    echo "dry_run=${dry_run}"
    exit 0
fi

require_cmd rsync
require_cmd ssh
require_cmd sshpass

ssh_cmd="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
-o LogLevel=ERROR -o ConnectTimeout=${connect_timeout} -o ConnectionAttempts=1 \
-o PubkeyAuthentication=no -o PreferredAuthentications=password \
-o NumberOfPasswordPrompts=1"

# --copy-links: exFAT cannot follow POSIX symlinks. The package tree
#   already inlines libSDL3.so* as real files, but be explicit so a
#   future symlink in the source tree doesn't ship as an opaque blob.
# --no-owner / --no-group: target FS doesn't preserve uid/gid anyway.
# --omit-dir-times: avoid spurious dir-mtime churn on the SD card.
# --exclude='._*': drop AppleDouble metadata if the source was ever
#   touched on macOS via Finder.
# NO --delete: the SD root is shared. SF33RD.AFS, save data, log files,
#   and other apps live there too and must be preserved.
rsync_args=(-av --copy-links --no-owner --no-group --omit-dir-times \
    --exclude='._*')

if [ "${dry_run}" -eq 1 ]; then
    rsync_args+=(--dry-run)
fi

echo ":: deploying ${mode} → ${user}@${host}:${rsync_dst}"
echo ":: source: ${rsync_src}"
if [ "${dry_run}" -eq 1 ]; then
    echo ":: DRY RUN — no files will be written on device"
fi

SSHPASS="${password}" sshpass -e rsync "${rsync_args[@]}" \
    -e "${ssh_cmd}" \
    "${rsync_src}" \
    "${user}@${host}:${rsync_dst}"

echo ":: done"
