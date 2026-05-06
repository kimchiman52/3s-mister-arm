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
  --remote-root <path>   Remote 3S-ARM root (default: $MISTER_ROOT or /media/fat/games/3s-arm)
  --remote-fat-root <p>  Remote /media/fat root (default: $MISTER_FAT_ROOT or /media/fat)

Commands:
  lock-status                Show the shared local MiSTer lock owner, if any
  busy-status                Show whether the MiSTer target appears busy
  configure-3s-arm-ini         Update a MiSTer INI with the 3S-ARM core section
  exec --command <sh>        Run one remote shell command (requires MISTER_ALLOW_REMOTE_EXEC=1)
  exec --script-file <path>  Run a local shell script remotely (requires MISTER_ALLOW_REMOTE_EXEC=1)
  deploy --src <path>        Rsync a runtime package tree into the remote 3S-ARM root
  deploy-wrapper --src <p>   Sync wrapper-owned files into the remote /media/fat tree
    --artifacts-only         Copy only MiSTer_3S-ARM + 3S-ARM.rbf; leave games/3s-arm untouched
    --wrapper-only           Copy only MiSTer_3S-ARM; leave 3S-ARM.rbf and games/3s-arm untouched
    --core-only              Copy only 3S-ARM.rbf; leave MiSTer_3S-ARM and games/3s-arm untouched
  probe                      Run run-3s-arm.sh --probe-renderer-only
  smoke                      Run a bounded launch-osd.sh smoke and tail logs
  probe-wrapper              Run MiSTer_3S-ARM with --probe-renderer-only and tail wrapper logs
  smoke-wrapper              Run a bounded MiSTer_3S-ARM launch and tail wrapper logs
  run-wrapper                Run MiSTer_3S-ARM with explicit runtime args and tail wrapper logs
  wrapper-status             Show installed wrapper artifacts, matching processes, and wrapper logs
  capture-wrapper            Capture detailed live wrapper/runtime state without mutating the target
  health                     Run a short remote health command and verify SF33RD.AFS

Safety:
  Nonstandard --remote-root / --remote-fat-root values are rejected unless
  MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1 and
  MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=<exact-path> are both set deliberately.
  Busy targets are rejected for deploy/probe/smoke-style commands unless
  MISTER_ALLOW_BUSY_TARGET=1 is set deliberately.
EOF
}

host="${MISTER_HOST:-192.168.1.171}"
user="${MISTER_USER:-root}"
password="${MISTER_PASSWORD:-}"
remote_root="${MISTER_ROOT:-/media/fat/games/3s-arm}"
remote_fat_root="${MISTER_FAT_ROOT:-/media/fat}"
# Override with an explicit relative path (e.g. "_Other/3S-ARM_20260429.rbf")
# when reproducing a specific build. Empty (default) → preflight resolves the
# latest dated rbf at runtime via glob, matching the MiSTer cores convention.
wrapper_core_relpath="${MISTER_WRAPPER_CORE_RELPATH:-}"

wrapper_preflight_script() {
    local explicit="${wrapper_core_relpath}"
    cat <<EOF
if [ ! -x './MiSTer_3S-ARM' ]; then
  echo __WRAPPER_HPS_MISSING__
  exit 10
fi
if [ -n '${explicit}' ]; then
  WRAPPER_RBF='./${explicit}'
else
  WRAPPER_RBF=\$(ls -1t ./_Other/3S-ARM_*.rbf 2>/dev/null | head -1)
fi
if [ -z "\${WRAPPER_RBF}" ] || [ ! -f "\${WRAPPER_RBF}" ]; then
  echo __WRAPPER_RBF_MISSING__
  exit 11
fi
if [ ! -f './games/3s-arm/bin/3s-arm' ]; then
  echo __WRAPPER_RUNTIME_MISSING__
  exit 12
fi
mkdir -p ./games/3s-arm/logs
rm -f ./games/3s-arm/logs/osd-wrapper.log ./games/3s-arm/logs/last-run.log
EOF
}

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
    --remote-fat-root)
        remote_fat_root="$2"
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
lock-status)
    mister_lock_status
    ;;
busy-status)
    status_cmd="$(mister_target_busy_status_script)"
    mister_ssh_exec "${host}" "${user}" "${password}" "${status_cmd}"
    ;;
configure-3s-arm-ini)
    ini_path="/media/fat/MiSTer.ini"
    main_value="MiSTer_3S-ARM"
    # video_mode: not set by default — core controls native video timing.
    # vga_scaler: only written when explicitly passed (e.g. --vga-scaler 0
    # to override a global vga_scaler=1 that would break YC color output).
    video_mode_value=""
    vga_scaler_value=""

    while [ "$#" -gt 0 ]; do
        case "$1" in
        --ini)
            ini_path="$2"
            shift 2
            ;;
        --main)
            main_value="$2"
            shift 2
            ;;
        --video-mode)
            video_mode_value="$2"
            shift 2
            ;;
        --vga-scaler)
            vga_scaler_value="$2"
            shift 2
            ;;
        *)
            echo "unknown configure-3s-arm-ini option: $1" >&2
            exit 2
            ;;
        esac
    done

    if [ -z "${ini_path}" ] || [ -z "${main_value}" ]; then
        echo "configure-3s-arm-ini requires non-empty --ini and --main values" >&2
        exit 2
    fi

    mister_require_safe_ini_path "${ini_path}"
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "configure-3s-arm-ini"

    ini_path_q="$(mister_shell_quote "${ini_path}")"
    main_value_q="$(mister_shell_quote "${main_value}")"
    video_mode_value_q="$(mister_shell_quote "${video_mode_value}")"
    vga_scaler_value_q="$(mister_shell_quote "${vga_scaler_value}")"

    configure_ini_cmd=$(cat <<EOF
set -e
ini_path=${ini_path_q}
main_value=${main_value_q}
video_mode_value=${video_mode_value_q}
vga_scaler_value=${vga_scaler_value_q}
backup="\${ini_path}.pre-3s-arm-\$(date +%Y%m%d-%H%M%S).bak"
tmp=\$(mktemp)
cp -p "\${ini_path}" "\${backup}"
awk 'BEGIN{skip=0} /^\[3S-ARM\]\$/{skip=1; next} /^\[/{if(skip){skip=0}} !skip {print}' "\${ini_path}" > "\${tmp}"
printf '\n[3S-ARM]\n' >> "\${tmp}"
printf 'main=%s\n' "\${main_value}" >> "\${tmp}"
if [ -n "\${video_mode_value}" ]; then
    printf 'video_mode=%s\n' "\${video_mode_value}" >> "\${tmp}"
fi
if [ -n "\${vga_scaler_value}" ]; then
    printf 'vga_scaler=%s\n' "\${vga_scaler_value}" >> "\${tmp}"
fi
cp "\${tmp}" "\${ini_path}"
rm -f "\${tmp}"
echo __MISTER_3S_ARM_INI_UPDATED__
echo "backup=\${backup}"
grep -n -A4 '^\[3S-ARM\]' "\${ini_path}"
EOF
)
    mister_ssh_exec "${host}" "${user}" "${password}" "${configure_ini_cmd}"
    ;;
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

    mister_require_remote_exec_opt_in
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

    mister_require_safe_runtime_root "${remote_root}"
    mister_require_cmd rsync
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "deploy"
    mister_rsync_deploy "${src_path%/}/" "${host}" "${user}" "${password}" "${remote_root}/"
    wrapper_cmd=$(cat <<EOF
set -e
mkdir -p '${remote_root}/logs'
rm -f /media/fat/Scripts/3S-ARM.sh '/media/fat/Scripts/3S-ARM_Training_Yun_Ryu_Ryu_Stage.sh' '/media/fat/Scripts/3S-ARM Training Yun Ryu Ryu Stage.sh'
EOF
)
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_cmd}"
    ;;
deploy-wrapper)
    src_path=""
    wrapper_deploy_mode="full"
    while [ "$#" -gt 0 ]; do
        case "$1" in
        --src)
            src_path="$2"
            shift 2
            ;;
        --artifacts-only)
            wrapper_deploy_mode="artifacts-only"
            shift
            ;;
        --wrapper-only)
            wrapper_deploy_mode="wrapper-only"
            shift
            ;;
        --core-only)
            wrapper_deploy_mode="core-only"
            shift
            ;;
        *)
            echo "unknown deploy-wrapper option: $1" >&2
            exit 2
            ;;
        esac
    done

    if [ -z "${src_path}" ]; then
        echo "deploy-wrapper requires --src" >&2
        exit 2
    fi
    if [ ! -d "${src_path}" ]; then
        echo "deploy-wrapper source directory not found: ${src_path}" >&2
        exit 2
    fi

    mister_require_safe_fat_root "${remote_fat_root}"
    mister_require_cmd rsync
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "deploy-wrapper"
    mister_rsync_deploy_wrapper "${src_path%/}/" "${host}" "${user}" "${password}" "${remote_fat_root}/" "${wrapper_deploy_mode}"
    ;;
probe)
    mister_require_safe_runtime_root "${remote_root}"
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "probe"
    mister_ssh_exec "${host}" "${user}" "${password}" "'${remote_root}/run-3s-arm.sh' --probe-renderer-only"
    ;;
smoke)
    mister_require_safe_runtime_root "${remote_root}"
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
    mister_require_target_idle "${host}" "${user}" "${password}" "smoke"
    mister_ssh_exec "${host}" "${user}" "${password}" "${smoke_cmd}"
    ;;
probe-wrapper)
    mister_require_safe_fat_root "${remote_fat_root}"
    wrapper_probe_cmd=$(cat <<EOF
set -e
cd '${remote_fat_root}'
$(wrapper_preflight_script)
timeout 20 env THIRDSARM_WRAPPER_FORCE=1 ./MiSTer_3S-ARM "\${WRAPPER_RBF}" '' --probe-renderer-only || rc=\$?
printf '__WRAPPER_PROBE_RC__=%s\n' "\${rc:-0}"
tail -n 80 ./games/3s-arm/logs/osd-wrapper.log || true
tail -n 80 ./games/3s-arm/logs/last-run.log || true
EOF
)
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "probe-wrapper"
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_probe_cmd}"
    ;;
smoke-wrapper)
    mister_require_safe_fat_root "${remote_fat_root}"
    wrapper_smoke_cmd=$(cat <<EOF
set -e
cd '${remote_fat_root}'
$(wrapper_preflight_script)
timeout 20 env THIRDSARM_WRAPPER_FORCE=1 ./MiSTer_3S-ARM "\${WRAPPER_RBF}" || rc=\$?
printf '__WRAPPER_RC__=%s\n' "\${rc:-0}"
tail -n 80 ./games/3s-arm/logs/osd-wrapper.log || true
tail -n 80 ./games/3s-arm/logs/last-run.log || true
EOF
)
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "smoke-wrapper"
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_smoke_cmd}"
    ;;
run-wrapper)
    mister_require_safe_fat_root "${remote_fat_root}"
    wrapper_timeout="20"
    runtime_args_invocation=""
    runtime_arg_count=0

    while [ "$#" -gt 0 ]; do
        case "$1" in
        --runtime-arg)
            runtime_args_invocation="${runtime_args_invocation} $(mister_shell_quote "$2")"
            runtime_arg_count=$((runtime_arg_count + 1))
            shift 2
            ;;
        --timeout-seconds)
            wrapper_timeout="$2"
            shift 2
            ;;
        *)
            echo "unknown run-wrapper option: $1" >&2
            exit 2
            ;;
        esac
    done

    if [ "${runtime_arg_count}" -eq 0 ]; then
        echo "run-wrapper requires at least one --runtime-arg" >&2
        exit 2
    fi

    if ! [[ "${wrapper_timeout}" =~ ^[0-9]+$ ]]; then
        echo "run-wrapper --timeout-seconds must be a non-negative integer" >&2
        exit 2
    fi

    wrapper_timeout_prefix=""
    if [ "${wrapper_timeout}" -gt 0 ]; then
        wrapper_timeout_prefix="timeout ${wrapper_timeout} "
    fi

    wrapper_run_cmd=$(cat <<EOF
set -e
cd '${remote_fat_root}'
$(wrapper_preflight_script)
${wrapper_timeout_prefix}env THIRDSARM_WRAPPER_FORCE=1 ./MiSTer_3S-ARM "\${WRAPPER_RBF}" ''${runtime_args_invocation} || rc=\$?
printf '__WRAPPER_RUN_RC__=%s\n' "\${rc:-0}"
tail -n 80 ./games/3s-arm/logs/osd-wrapper.log || true
tail -n 80 ./games/3s-arm/logs/last-run.log || true
EOF
)
    mister_lock_acquire
    mister_require_target_idle "${host}" "${user}" "${password}" "run-wrapper"
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_run_cmd}"
    ;;
wrapper-status)
    mister_require_safe_fat_root "${remote_fat_root}"
    wrapper_status_cmd=$(cat <<EOF
set -e
cd '${remote_fat_root}'
if [ -x './MiSTer_3S-ARM' ]; then
  echo __WRAPPER_HPS_OK__
else
  echo __WRAPPER_HPS_MISSING__
fi
if [ -n '${wrapper_core_relpath}' ]; then
  WRAPPER_RBF='./${wrapper_core_relpath}'
else
  WRAPPER_RBF=\$(ls -1t ./_Other/3S-ARM_*.rbf 2>/dev/null | head -1)
fi
if [ -n "\${WRAPPER_RBF}" ] && [ -f "\${WRAPPER_RBF}" ]; then
  printf '__WRAPPER_RBF_OK__=%s\n' "\${WRAPPER_RBF}"
else
  echo __WRAPPER_RBF_MISSING__
fi
if [ -f './games/3s-arm/bin/3s-arm' ]; then
  echo __WRAPPER_RUNTIME_OK__
else
  echo __WRAPPER_RUNTIME_MISSING__
fi
if [ -f './games/3s-arm/resources/SF33RD.AFS' ]; then
  echo __WRAPPER_AFS_OK__
else
  echo __WRAPPER_AFS_MISSING__
fi
wrapper_ps=\$(ps | grep -E 'MiSTer_3S-ARM|launch-osd\\.sh|(^|/)3s-arm( |$)' | grep -v grep || true)
if printf '%s\n' "\${wrapper_ps}" | grep -q 'MiSTer_3S-ARM'; then
  if printf '%s\n' "\${wrapper_ps}" | grep -q 'launch-osd\\.sh'; then
    echo __WRAPPER_LAUNCH_MODE__=mixed
  else
    echo __WRAPPER_LAUNCH_MODE__=native-wrapper
  fi
else
  if printf '%s\n' "\${wrapper_ps}" | grep -q 'launch-osd\\.sh'; then
    echo __WRAPPER_LAUNCH_MODE__=legacy-launcher
  else
    if [ -n "\${wrapper_ps}" ]; then
      echo __WRAPPER_LAUNCH_MODE__=runtime-only
    else
      echo __WRAPPER_LAUNCH_MODE__=idle
    fi
  fi
fi
echo __WRAPPER_PS_BEGIN__
printf '%s\n' "\${wrapper_ps}"
echo __WRAPPER_PS_END__
echo __WRAPPER_LOGS_BEGIN__
tail -n 80 ./games/3s-arm/logs/osd-wrapper.log 2>/dev/null || echo __WRAPPER_LOG_MISSING__
tail -n 80 ./games/3s-arm/logs/last-run.log 2>/dev/null || echo __WRAPPER_LASTRUN_MISSING__
echo __WRAPPER_LOGS_END__
EOF
)
    mister_lock_acquire
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_status_cmd}"
    ;;
capture-wrapper)
    mister_require_safe_fat_root "${remote_fat_root}"
    wrapper_capture_cmd=$(cat <<EOF
set -e
cd '${remote_fat_root}'
echo __WRAPPER_CAPTURE_BEGIN__
echo "active_vt=\$(cat /sys/class/tty/tty0/active 2>/dev/null || true)"
echo "fb_mode=\$(cat /sys/module/MiSTer_fb/parameters/mode 2>/dev/null || true)"
echo "__PS_TABLE_BEGIN__"
ps -o pid=,ppid=,pgid=,sid=,tty=,stat=,wchan=,args= 2>/dev/null | grep -E 'MiSTer_3S-ARM|launch-osd\\.sh|run-3s-arm\\.sh|(^|/)3s-arm( |$)' | grep -v grep || true
echo "__PS_TABLE_END__"
echo "__WRAPPER_LOG_BEGIN__"
tail -n 120 ./games/3s-arm/logs/osd-wrapper.log 2>/dev/null || echo __WRAPPER_LOG_MISSING__
echo "__WRAPPER_LOG_END__"
echo "__LAST_RUN_BEGIN__"
tail -n 120 ./games/3s-arm/logs/last-run.log 2>/dev/null || echo __WRAPPER_LASTRUN_MISSING__
echo "__LAST_RUN_END__"
echo __WRAPPER_CAPTURE_END__
EOF
)
    mister_lock_acquire
    mister_ssh_exec "${host}" "${user}" "${password}" "${wrapper_capture_cmd}"
    ;;
health)
    mister_require_safe_runtime_root "${remote_root}"
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
