#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mister/mister-common.sh
source "${SCRIPT_DIR}/mister-common.sh"

usage() {
    cat <<'EOF'
Usage:
  tools/mister/misterctl.sh [global options] <command> [command options]

Global options:
  --host <ip-or-host>    MiSTer host (default: $MISTER_HOST or 192.168.1.171)
  --user <name>          SSH user (default: $MISTER_USER or root)
  --password <value>     SSH password (default: $MISTER_PASSWORD)
  --remote-root <path>   Remote 3SX root (default: $MISTER_ROOT or /media/fat/games/3sx)

Commands:
  exec --command <sh>        Run one remote shell command
  exec --script-file <path>  Run a local shell script remotely
  deploy --src <path>        Rsync a package tree to the remote root and refresh /media/fat/Scripts/3SX.sh
  probe                      Run scripts/run-3sx.sh --probe-renderer-only
  smoke                      Run a bounded scripts/launch-osd.sh smoke and tail logs
  health                     Run a short remote health command and verify SF33RD.AFS
EOF
}

host="${MISTER_HOST:-192.168.1.171}"
user="${MISTER_USER:-root}"
password="${MISTER_PASSWORD:-}"
remote_root="${MISTER_ROOT:-/media/fat/games/3sx}"

while [ "$#" -gt 0 ]; do
    case "$1" in
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
    --help|-h)
        usage
        exit 0
        ;;
    *)
        break
        ;;
    esac
done

if [ "$#" -eq 0 ]; then
    usage
    exit 2
fi

command_name="$1"
shift

mister_require_cmd ssh

case "${command_name}" in
exec)
    remote_cmd=""
    script_file=""
    while [ "$#" -gt 0 ]; do
        case "$1" in
        --command)
            remote_cmd="$2"
            shift 2
            ;;
        --script-file)
            script_file="$2"
            shift 2
            ;;
        *)
            echo "unknown exec option: $1" >&2
            exit 2
            ;;
        esac
    done

    if [ -n "${remote_cmd}" ] && [ -n "${script_file}" ]; then
        echo "exec requires exactly one of --command or --script-file" >&2
        exit 2
    fi

    if [ -n "${script_file}" ]; then
        if [ ! -f "${script_file}" ]; then
            echo "script file not found: ${script_file}" >&2
            exit 2
        fi
        remote_cmd="$(cat "${script_file}")"
    fi

    if [ -z "${remote_cmd}" ]; then
        echo "exec requires --command or --script-file" >&2
        exit 2
    fi

    mister_lock_acquire
    mister_ssh_exec "${host}" "${user}" "${password}" "${remote_cmd}"
    ;;
deploy)
    src_path=""
    while [ "$#" -gt 0 ]; do
        case "$1" in
        --src)
            src_path="$2"
            shift 2
            ;;
        *)
            echo "unknown deploy option: $1" >&2
            exit 2
            ;;
        esac
    done

    if [ -z "${src_path}" ]; then
        echo "deploy requires --src" >&2
        exit 2
    fi
    if [ ! -d "${src_path}" ]; then
        echo "deploy source directory not found: ${src_path}" >&2
        exit 2
    fi

    mister_require_cmd rsync
    mister_lock_acquire
    mister_rsync_deploy "${src_path%/}/" "${host}" "${user}" "${password}" "${remote_root}/"
    wrapper_cmd=$(cat <<EOF
set -e
mkdir -p /media/fat/Scripts '${remote_root}/logs'
cat > /media/fat/Scripts/3SX.sh <<'SCRIPT_WRAPPER'
#!/bin/sh
set -eu

LOG_DIR="/media/fat/games/3sx/logs"
LOG_PATH="\${LOG_DIR}/osd-wrapper.log"

mkdir -p "\${LOG_DIR}"

{
    echo "==== 3SX OSD wrapper ===="
    date 2>/dev/null || true
    echo "pid=\$\$ ppid=\$PPID"
    echo "tty=\$(tty 2>&1 || true)"
    echo "args=\$*"
    echo "-------------------------"
} >"\${LOG_PATH}"

exec /media/fat/games/3sx/scripts/launch-osd.sh "\$@" >>"\${LOG_PATH}" 2>&1
SCRIPT_WRAPPER
chmod +x /media/fat/Scripts/3SX.sh
EOF
)
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_cmd}"
    ;;
probe)
    mister_lock_acquire
    mister_ssh_exec "${host}" "${user}" "${password}" "'${remote_root}/scripts/run-3sx.sh' --probe-renderer-only"
    ;;
smoke)
    smoke_cmd=$(cat <<EOF
set -e
cd '${remote_root}'
rm -f logs/last-run.log
timeout 20 ./scripts/launch-osd.sh || rc=\$?
printf '__RUNTIME_RC__=%s\n' "\${rc:-0}"
tail -n 40 logs/backend.log || true
tail -n 40 logs/last-run.log || true
EOF
)
    mister_lock_acquire
    mister_ssh_exec "${host}" "${user}" "${password}" "${smoke_cmd}"
    ;;
health)
    health_cmd=$(cat <<EOF
set -e
echo __MISTER_HEALTH_OK__
uname -a
if [ -f '${remote_root}/resources/SF33RD.AFS' ]; then
  echo __AFS_OK__
else
  echo __AFS_MISSING__
fi
EOF
)
    mister_lock_acquire
    mister_ssh_exec "${host}" "${user}" "${password}" "${health_cmd}"
    ;;
*)
    echo "unknown command: ${command_name}" >&2
    usage
    exit 2
    ;;
esac
