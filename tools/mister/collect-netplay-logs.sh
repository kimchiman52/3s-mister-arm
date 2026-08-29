#!/usr/bin/env bash
#
# collect-netplay-logs.sh — pull a tester's netplay evidence off a MiSTer.
#
# READ-ONLY ON THE DEVICE. This script never writes, renames, deletes or
# executes anything in the remote tree. It runs exactly two kinds of remote
# command — `ls` and `test` — and downloads with scp. There is deliberately
# no rm, no rsync (and therefore no --delete), no mkdir, no redirect into a
# remote path, and it never touches menu.rbf or any core file. If you are
# editing this and reach for a remote write: don't. Collection must be safe
# to run against a device mid-session without perturbing the thing being
# diagnosed.
#
# !! DEPLOY DEPENDENCY — THE ORDER MATTERS !!
# On this branch a deploy DELETES the remote logs/ directory. All three
# rsync preserve lists in tools/mister/mister-common.sh omit it — `grep -n
# logs tools/mister/mister-common.sh` returns nothing — so the
# `rsync -av --delete` in mister_rsync_deploy takes logs/ with it, the
# netplay report included. The fix that adds "logs" to the preserved set is
# commit f8b29ded on the unmerged branch fix/tools-safety-93-90 (there:
# mister-common.sh:648); that file belongs to that lane and is deliberately
# NOT edited from here. Until it merges: COLLECT BEFORE YOU DEPLOY.
#
# Usage: tools/mister/collect-netplay-logs.sh [--help] [--host <ip>] ...
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=tools/mister/mister-common.sh
source "${SCRIPT_DIR}/mister-common.sh"

SESSION_LOG_COUNT="${NETPLAY_COLLECT_SESSION_LOGS:-5}"

usage() {
    cat <<'EOF'
Usage:
  tools/mister/collect-netplay-logs.sh [options]

Copies the netplay evidence off a MiSTer into a local
artifacts/netplay-logs-<UTC>/ directory and tars it. Read-only on the
device: nothing is written, moved or deleted there.

Options:
  --host <ip-or-host>    MiSTer host (default: $MISTER_HOST or 192.168.1.171)
  --user <name>          SSH user (default: $MISTER_USER or root)
  --password <value>     SSH password (default: $MISTER_PASSWORD)
  --remote-root <path>   Remote 3S-ARM root (default: $MISTER_ROOT or
                         /media/fat/games/3s-arm)
  --out <dir>            Local output parent dir (default: <repo>/artifacts)
  -h, --help             This text

What it collects, in order of importance:
  logs/netplay-report.txt     THE file. Every attributed connection result,
                              across launches, one line each.
  logs/netplay-report.1.txt   The previous generation, if the live one has
                              already rotated.
  logs/netplay-*.log          The newest 5 per-session logs, for the full
                              detail behind those one-liners.

Exit status:
  0  the report file was collected (missing session logs are a warning)
  1  nothing at all was collected

IF THE TESTER HAS NO SSH — THE ONE FILE, BY HAND
  You do not need this script, a network, or a terminal. There is a single
  file, and it is plain text:

      /media/fat/games/3s-arm/logs/netplay-report.txt

  1. Power the MiSTer off and take the SD card out.
  2. Put the card in a PC or Mac.
  3. Open the card, go to  games/3s-arm/logs .
  4. Copy  netplay-report.txt  off the card and send it. If there is also a
     netplay-report.1.txt, send that too — it is the older half.
  5. Put the card back and power on.

  It contains no personal information: no room codes (they are redacted at
  the source), no tokens, no keys, no passwords. Just times, which stage of
  connecting failed, and why.

  COLLECT IT BEFORE YOU UPDATE THE BUILD. Installing a new build currently
  erases the logs folder on the card, this file with it. Copy it off first,
  then update.
EOF
}

host="${MISTER_HOST:-192.168.1.171}"
user="${MISTER_USER:-root}"
password="${MISTER_PASSWORD:-}"
remote_root="${MISTER_ROOT:-/media/fat/games/3s-arm}"
out_parent="${REPO_ROOT}/artifacts"

while [ "$#" -gt 0 ]; do
    case "$1" in
    -h | --help)
        usage
        exit 0
        ;;
    --host)
        host="${2:-}"
        shift 2
        ;;
    --user)
        user="${2:-}"
        shift 2
        ;;
    --password)
        password="${2:-}"
        shift 2
        ;;
    --remote-root)
        remote_root="${2:-}"
        shift 2
        ;;
    --out)
        out_parent="${2:-}"
        shift 2
        ;;
    *)
        echo "unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if [ -z "${host}" ]; then
    echo "empty --host" >&2
    exit 2
fi

# Same guard every other script in this directory applies to the remote
# root. It is a read-only run, but a typo'd root would silently collect
# nothing and look like "the tester has no logs", which is a worse failure
# than an explicit refusal.
remote_root="$(mister_normalize_remote_path "${remote_root}")"
mister_require_safe_runtime_root "${remote_root}"

remote_logs="${remote_root}/logs"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
out_dir="${out_parent}/netplay-logs-${stamp}"
mkdir -p "${out_dir}"

echo "collecting netplay logs from ${user}@${host}:${remote_logs}"
echo "  -> ${out_dir}"

collected=0
report_collected=0

# Download one remote file. A miss is a warning: a tester who never got as
# far as a failed connection legitimately has no report, and a tester who
# has a report but only two session logs is still a complete report.
fetch_one() {
    local rel="$1"
    local label="$2"
    if mister_scp_download "${host}" "${user}" "${password}" \
        "${remote_logs}/${rel}" "${out_dir}/${rel}" >/dev/null 2>&1; then
        echo "  ok      ${rel}"
        collected=$((collected + 1))
        return 0
    fi
    echo "  missing ${rel} (${label})"
    return 1
}

if fetch_one "netplay-report.txt" "no connection has been attempted on this build yet, or a deploy wiped logs/"; then
    report_collected=1
fi
fetch_one "netplay-report.1.txt" "the live report has not rotated yet — normal" || true

# Newest N session logs. `ls -1t` only reads the directory. The remote side
# is busybox ash on a MiSTer, so keep it to one pipeline of core utilities.
session_list=""
if session_list="$(mister_ssh_exec "${host}" "${user}" "${password}" \
    "ls -1t '${remote_logs}'/netplay-*.log 2>/dev/null | head -n ${SESSION_LOG_COUNT}" 2>/dev/null)"; then
    :
else
    session_list=""
fi

if [ -z "${session_list}" ]; then
    echo "  missing netplay-*.log (no per-session logs on the device)"
else
    while IFS= read -r remote_path; do
        # mister_ssh_exec goes through expect when a password is set, so its
        # stdout also carries the `spawn ssh ...` banner and any password
        # prompt. Accept only lines that are literally an absolute path to a
        # session log; that also drops an unexpanded glob when the directory
        # is empty.
        remote_path="${remote_path%$'\r'}"
        case "${remote_path}" in
        /*/netplay-*.log) ;;
        *) continue ;;
        esac
        base="$(basename "${remote_path}")"
        if mister_scp_download "${host}" "${user}" "${password}" \
            "${remote_path}" "${out_dir}/${base}" >/dev/null 2>&1; then
            echo "  ok      ${base}"
            collected=$((collected + 1))
        else
            echo "  missing ${base} (download failed)"
        fi
    done <<<"${session_list}"
fi

if [ "${collected}" -eq 0 ]; then
    echo "collected nothing from ${host}:${remote_logs}" >&2
    echo "if the tester recently updated their build, logs/ was erased by the deploy — see --help" >&2
    rmdir "${out_dir}" 2>/dev/null || true
    exit 1
fi

tarball="${out_dir}.tar.gz"
tar -czf "${tarball}" -C "${out_parent}" "$(basename "${out_dir}")"
echo "collected ${collected} file(s)"
echo "tarball: ${tarball}"

if [ "${report_collected}" -eq 0 ]; then
    echo "WARNING: netplay-report.txt was not present — you have session logs but not the attributed summary" >&2
fi

exit 0
