#!/usr/bin/env bash

if [ -n "${__MISTER_COMMON_SH:-}" ]; then
    return 0
fi
__MISTER_COMMON_SH=1

mister_require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        return 2
    fi
}

mister_lock_dir() {
    printf '%s\n' "${MISTER_LOCK_DIR:-${TMPDIR:-/tmp}/3s-mister-arm-remote.lock}"
}

mister_lock_timeout() {
    printf '%s\n' "${MISTER_LOCK_TIMEOUT:-900}"
}

mister_cmd_timeout() {
    printf '%s\n' "${MISTER_CMD_TIMEOUT:-300}"
}

mister_transfer_timeout() {
    printf '%s\n' "${MISTER_TRANSFER_TIMEOUT:-1200}"
}

mister_ssh_connect_timeout() {
    printf '%s\n' "${MISTER_SSH_CONNECT_TIMEOUT:-10}"
}

mister_ssh_password_args() {
    local timeout_seconds
    timeout_seconds="$(mister_ssh_connect_timeout)"

    printf '%s\n' \
        -o StrictHostKeyChecking=no \
        -o ConnectTimeout="${timeout_seconds}" \
        -o ConnectionAttempts=1 \
        -o PubkeyAuthentication=no \
        -o PreferredAuthentications=password \
        -o NumberOfPasswordPrompts=1
}

mister_ssh_key_only_args() {
    local timeout_seconds
    timeout_seconds="$(mister_ssh_connect_timeout)"

    printf '%s\n' \
        -o StrictHostKeyChecking=no \
        -o ConnectTimeout="${timeout_seconds}" \
        -o ConnectionAttempts=1 \
        -o BatchMode=yes \
        -o IdentitiesOnly=yes \
        -o IdentityAgent=none \
        -o PreferredAuthentications=publickey \
        -o NumberOfPasswordPrompts=0
}

mister_rsync_ssh_password_command() {
    local timeout_seconds
    timeout_seconds="$(mister_ssh_connect_timeout)"

    printf '%s\n' \
        "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=${timeout_seconds} -o ConnectionAttempts=1 -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1"
}

mister_rsync_ssh_key_only_command() {
    local timeout_seconds
    timeout_seconds="$(mister_ssh_connect_timeout)"

    printf '%s\n' \
        "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=${timeout_seconds} -o ConnectionAttempts=1 -o BatchMode=yes -o IdentitiesOnly=yes -o IdentityAgent=none -o PreferredAuthentications=publickey -o NumberOfPasswordPrompts=0"
}

mister_normalize_remote_path() {
    local path="${1:-}"

    if [ -z "${path}" ]; then
        printf '\n'
        return 0
    fi

    while [ "${path}" != "/" ] && [ "${path%/}" != "${path}" ]; do
        path="${path%/}"
    done

    printf '%s\n' "${path}"
}

mister_allow_unsafe_remote_paths() {
    [ "${MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT:-0}" = "1" ]
}

mister_remote_path_has_unsafe_segments() {
    local path="$1"

    case "${path}" in
    *'//'*) return 0 ;;
    *'/./'*|*'/../'*|*/.|*/..) return 0 ;;
    esac

    return 1
}

mister_require_confirmed_remote_override() {
    local path="$1"
    local label="$2"

    if ! mister_allow_unsafe_remote_paths; then
        echo "refusing nonstandard ${label}: ${path}" >&2
        echo "set MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1 and MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=${path} to override deliberately" >&2
        return 2
    fi

    if [ "${MISTER_UNSAFE_CONFIRM_REMOTE_ROOT:-}" != "${path}" ]; then
        echo "refusing nonstandard ${label}: ${path}" >&2
        echo "typed confirmation mismatch; set MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=${path} to override deliberately" >&2
        return 2
    fi

    return 0
}

mister_require_safe_runtime_root() {
    local path normalized
    path="${1:-}"
    normalized="$(mister_normalize_remote_path "${path}")"

    case "${normalized}" in
    "")
        echo "refusing empty remote runtime root" >&2
        return 2
        ;;
    /*)
        ;;
    *)
        echo "refusing non-absolute remote runtime root: ${path}" >&2
        return 2
        ;;
    esac

    if mister_remote_path_has_unsafe_segments "${normalized}"; then
        echo "refusing remote runtime root with ambiguous path segments: ${normalized}" >&2
        return 2
    fi

    if [ "${normalized}" = "/media/fat/games/3s-arm" ]; then
        return 0
    fi

    case "${normalized}" in
    /|/media|/media/fat|/media/fat/games)
        echo "refusing dangerous remote runtime root: ${normalized}" >&2
        echo "expected the exact runtime root /media/fat/games/3s-arm" >&2
        return 2
        ;;
    *)
        case "${normalized}" in
        */games/3s-arm)
            mister_require_confirmed_remote_override "${normalized}" "remote runtime root"
            ;;
        *)
            echo "refusing nonstandard remote runtime root: ${normalized}" >&2
            echo "expected the exact runtime root /media/fat/games/3s-arm" >&2
            return 2
            ;;
        esac
        ;;
    esac
}

mister_require_safe_fat_root() {
    local path normalized
    path="${1:-}"
    normalized="$(mister_normalize_remote_path "${path}")"

    case "${normalized}" in
    "")
        echo "refusing empty remote /media/fat root" >&2
        return 2
        ;;
    /*)
        ;;
    *)
        echo "refusing non-absolute remote /media/fat root: ${path}" >&2
        return 2
        ;;
    esac

    if mister_remote_path_has_unsafe_segments "${normalized}"; then
        echo "refusing remote /media/fat root with ambiguous path segments: ${normalized}" >&2
        return 2
    fi

    if [ "${normalized}" = "/media/fat" ]; then
        return 0
    fi

    case "${normalized}" in
    /|/media)
        echo "refusing dangerous remote /media/fat root: ${normalized}" >&2
        echo "expected the exact wrapper root /media/fat" >&2
        return 2
        ;;
    *)
        mister_require_confirmed_remote_override "${normalized}" "remote /media/fat root"
        ;;
    esac
}

mister_require_safe_ini_path() {
    local path normalized
    path="${1:-}"
    normalized="$(mister_normalize_remote_path "${path}")"

    case "${normalized}" in
    "")
        echo "refusing empty remote MiSTer.ini path" >&2
        return 2
        ;;
    /*)
        ;;
    *)
        echo "refusing non-absolute remote MiSTer.ini path: ${path}" >&2
        return 2
        ;;
    esac

    if mister_remote_path_has_unsafe_segments "${normalized}"; then
        echo "refusing remote MiSTer.ini path with ambiguous path segments: ${normalized}" >&2
        return 2
    fi

    case "${normalized}" in
    /media/fat/MiSTer.ini|/media/fat/MiSTer_*.ini)
        return 0
        ;;
    /|/media|/media/fat)
        echo "refusing dangerous remote MiSTer.ini path: ${normalized}" >&2
        echo "expected /media/fat/MiSTer.ini or /media/fat/MiSTer_*.ini" >&2
        return 2
        ;;
    *)
        mister_require_confirmed_remote_override "${normalized}" "remote MiSTer.ini path"
        ;;
    esac
}

mister_require_remote_exec_opt_in() {
    if [ "${MISTER_ALLOW_REMOTE_EXEC:-0}" = "1" ]; then
        return 0
    fi

    echo "remote exec is disabled by default; set MISTER_ALLOW_REMOTE_EXEC=1 to opt in" >&2
    return 2
}

mister_shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\"'\"'/g")"
}

mister_allow_busy_target() {
    [ "${MISTER_ALLOW_BUSY_TARGET:-0}" = "1" ]
}

mister_remote_busy_regex() {
    printf '%s\n' "${MISTER_REMOTE_BUSY_REGEX:-(^|/)(3s-arm|launch-osd\\.sh|run-3s-arm\\.sh|perf-sampler\\.sh)( |$)|MiSTer_3S-ARM|(^| )rsync( |$)|(^| )scp( |$)|sftp-server}"
}

mister_target_busy_status_script() {
    local regex
    regex="$(mister_remote_busy_regex)"

    cat <<EOF
set -e
busy_output=\$(ps | grep -E $(mister_shell_quote "${regex}") | grep -v grep || true)
if [ -n "\${busy_output}" ]; then
  echo __MISTER_TARGET_BUSY__
  printf '%s\n' "\${busy_output}"
  exit 24
else
  echo __MISTER_TARGET_IDLE__
fi
EOF
}

mister_require_target_idle() {
    local host="$1"
    local user="$2"
    local password="$3"
    local command_label="$4"
    local status_script
    local rc=0

    if mister_allow_busy_target; then
        return 0
    fi

    status_script="$(mister_target_busy_status_script)"
    mister_ssh_exec "${host}" "${user}" "${password}" "${status_script}"
    rc=$?

    if [ "${rc}" -eq 0 ]; then
        return 0
    fi

    if [ "${rc}" -eq 24 ]; then
        echo "refusing ${command_label}: MiSTer target appears busy" >&2
        echo "rerun with MISTER_ALLOW_BUSY_TARGET=1 only if you intentionally accept the collision risk" >&2
        return 2
    fi

    return "${rc}"
}

MISTER_LOCK_HELD=0
MISTER_LOCK_OWNER_FILE=""
MISTER_LOCK_DIR_VALUE=""

mister_lock_status() {
    local lock_dir owner_file owner_pid owner_live
    lock_dir="$(mister_lock_dir)"
    owner_file="${lock_dir}/owner"
    owner_pid=""
    owner_live=0

    if [ ! -d "${lock_dir}" ]; then
        printf 'lock_state=free\n'
        printf 'lock_dir=%s\n' "${lock_dir}"
        return 0
    fi

    if [ -f "${lock_dir}/pid" ]; then
        owner_pid="$(cat "${lock_dir}/pid" 2>/dev/null || true)"
        if [ -n "${owner_pid}" ] && kill -0 "${owner_pid}" 2>/dev/null; then
            owner_live=1
        fi
    fi

    printf 'lock_state=held\n'
    printf 'lock_dir=%s\n' "${lock_dir}"
    printf 'owner_pid=%s\n' "${owner_pid}"
    printf 'owner_live=%s\n' "${owner_live}"

    if [ -f "${owner_file}" ]; then
        cat "${owner_file}"
    fi
}

mister_lock_release() {
    if [ "${MISTER_LOCK_HELD}" -ne 1 ]; then
        return 0
    fi

    if [ -n "${MISTER_LOCK_DIR_VALUE}" ] && [ -d "${MISTER_LOCK_DIR_VALUE}" ]; then
        if [ -n "${MISTER_LOCK_OWNER_FILE}" ] && [ -f "${MISTER_LOCK_OWNER_FILE}" ]; then
            owner_pid="$(cat "${MISTER_LOCK_OWNER_FILE}" 2>/dev/null || true)"
            if [ "${owner_pid}" = "$$" ]; then
                rm -rf "${MISTER_LOCK_DIR_VALUE}"
            fi
        else
            rm -rf "${MISTER_LOCK_DIR_VALUE}"
        fi
    fi

    MISTER_LOCK_HELD=0
    MISTER_LOCK_OWNER_FILE=""
    MISTER_LOCK_DIR_VALUE=""
}

mister_lock_acquire() {
    if [ "${MISTER_LOCK_HELD}" -eq 1 ]; then
        return 0
    fi

    local lock_dir timeout_seconds start_ts now_ts owner_file owner_pid owner_age
    lock_dir="$(mister_lock_dir)"
    timeout_seconds="$(mister_lock_timeout)"
    start_ts="$(date +%s)"

    while ! mkdir "${lock_dir}" 2>/dev/null; do
        owner_file="${lock_dir}/pid"
        owner_pid=""
        if [ -f "${owner_file}" ]; then
            owner_pid="$(cat "${owner_file}" 2>/dev/null || true)"
        fi

        if [ -n "${owner_pid}" ] && ! kill -0 "${owner_pid}" 2>/dev/null; then
            rm -rf "${lock_dir}"
            continue
        fi

        now_ts="$(date +%s)"
        owner_age=$((now_ts - start_ts))
        if [ "${owner_age}" -ge "${timeout_seconds}" ]; then
            echo "timed out waiting for MiSTer lock at ${lock_dir}" >&2
            return 1
        fi

        sleep 1
    done

    MISTER_LOCK_DIR_VALUE="${lock_dir}"
    MISTER_LOCK_OWNER_FILE="${lock_dir}/pid"
    printf '%s\n' "$$" >"${MISTER_LOCK_OWNER_FILE}"
    {
        printf 'pid=%s\n' "$$"
        printf 'cwd=%s\n' "$(pwd)"
        printf 'time=%s\n' "$(date)"
    } >"${lock_dir}/owner"

    MISTER_LOCK_HELD=1
    trap 'mister_lock_release' EXIT INT TERM HUP
}

mister_base64_encode_inline() {
    printf '%s' "$1" | base64 | tr -d '\n'
}

mister_ssh_expect() {
    local host="$1"
    local user="$2"
    local password="$3"
    local remote_cmd="$4"
    local encoded_cmd
    local timeout_seconds
    local ssh_args
    encoded_cmd="$(mister_base64_encode_inline "${remote_cmd}")"
    timeout_seconds="$(mister_cmd_timeout)"
    ssh_args="$(mister_ssh_password_args)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_CMD="$encoded_cmd" EXPECT_TIMEOUT="$timeout_seconds" EXPECT_SSH_ARGS="$ssh_args" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set cmd $env(EXPECT_CMD)
set ssh_args [split $env(EXPECT_SSH_ARGS) "\n"]
spawn ssh {*}$ssh_args ${user}@${host} "echo '${cmd}' | base64 -d | sh"
expect {
  -re {[Pp]assword:} { send -- "$pw\r"; exp_continue }
  eof
}
set status [lindex [wait] 3]
exit $status
EOF
}

mister_ssh_exec() {
    local host="$1"
    local user="$2"
    local password="$3"
    local remote_cmd="$4"
    local -a ssh_args

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_ssh_expect "${host}" "${user}" "${password}" "${remote_cmd}"
    else
        while IFS= read -r arg; do
            ssh_args+=("${arg}")
        done < <(mister_ssh_key_only_args)
        ssh "${ssh_args[@]}" "${user}@${host}" "${remote_cmd}" || {
            echo "MiSTer key-only SSH failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
            return 1
        }
    fi
}

mister_scp_expect() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local timeout_seconds
    local scp_args
    timeout_seconds="$(mister_transfer_timeout)"
    scp_args="$(mister_ssh_password_args)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" EXPECT_SCP_ARGS="$scp_args" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
set scp_args [split $env(EXPECT_SCP_ARGS) "\n"]
spawn scp {*}$scp_args "$src_path" ${user}@${host}:${dst_path}
expect {
  -re {[Pp]assword:} { send -- "$pw\r"; exp_continue }
  eof
}
set status [lindex [wait] 3]
exit $status
EOF
}

mister_scp_upload() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"
    local -a scp_args

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_scp_expect "${src_path}" "${host}" "${user}" "${password}" "${dst_path}"
    else
        while IFS= read -r arg; do
            scp_args+=("${arg}")
        done < <(mister_ssh_key_only_args)
        scp "${scp_args[@]}" "${src_path}" "${user}@${host}:${dst_path}" || {
            echo "MiSTer key-only SCP upload failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
            return 1
        }
    fi
}

mister_scp_download_expect() {
    local host="$1"
    local user="$2"
    local password="$3"
    local src_path="$4"
    local dst_path="$5"

    local timeout_seconds
    local scp_args
    timeout_seconds="$(mister_transfer_timeout)"
    scp_args="$(mister_ssh_password_args)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" EXPECT_SCP_ARGS="$scp_args" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
set scp_args [split $env(EXPECT_SCP_ARGS) "\n"]
spawn scp {*}$scp_args ${user}@${host}:${src_path} "$dst_path"
expect {
  -re {[Pp]assword:} { send -- "$pw\r"; exp_continue }
  eof
}
set status [lindex [wait] 3]
exit $status
EOF
}

mister_scp_download() {
    local host="$1"
    local user="$2"
    local password="$3"
    local src_path="$4"
    local dst_path="$5"
    local -a scp_args

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_scp_download_expect "${host}" "${user}" "${password}" "${src_path}" "${dst_path}"
    else
        while IFS= read -r arg; do
            scp_args+=("${arg}")
        done < <(mister_ssh_key_only_args)
        scp "${scp_args[@]}" "${user}@${host}:${src_path}" "${dst_path}" || {
            echo "MiSTer key-only SCP download failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
            return 1
        }
    fi
}

mister_rsync_expect() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local timeout_seconds
    local rsync_shell
    timeout_seconds="$(mister_transfer_timeout)"
    rsync_shell="$(mister_rsync_ssh_password_command)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" EXPECT_RSYNC_SHELL="$rsync_shell" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
set rsync_shell $env(EXPECT_RSYNC_SHELL)
spawn rsync -av --delete --omit-dir-times --no-perms --no-owner --no-group --exclude resources/SF33RD.AFS --filter {P resources/SF33RD.AFS} --exclude config --filter {P config} --exclude keymap --filter {P keymap} -e $rsync_shell "$src_path" ${user}@${host}:${dst_path}
expect {
  -re {[Pp]assword:} { send -- "$pw\r"; exp_continue }
  eof
}
set status [lindex [wait] 3]
exit $status
EOF
}

mister_rsync_expect_copy() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local timeout_seconds
    local rsync_shell
    timeout_seconds="$(mister_transfer_timeout)"
    rsync_shell="$(mister_rsync_ssh_password_command)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" EXPECT_RSYNC_SHELL="$rsync_shell" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
set rsync_shell $env(EXPECT_RSYNC_SHELL)
spawn rsync -av --omit-dir-times --no-perms --no-owner --no-group -e $rsync_shell "$src_path" ${user}@${host}:${dst_path}
expect {
  -re {[Pp]assword:} { send -- "$pw\r"; exp_continue }
  eof
}
set status [lindex [wait] 3]
exit $status
EOF
}

mister_rsync_deploy() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    mister_require_safe_runtime_root "${dst_path}" || return $?

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_rsync_expect "${src_path}" "${host}" "${user}" "${password}" "${dst_path}"
    else
        local rsync_shell
        rsync_shell="$(mister_rsync_ssh_key_only_command)"
        rsync -av --delete --omit-dir-times --no-perms --no-owner --no-group \
            --exclude 'resources/SF33RD.AFS' \
            --filter 'P resources/SF33RD.AFS' \
            --exclude 'config' \
            --filter 'P config' \
            --exclude 'keymap' \
            --filter 'P keymap' \
            -e "${rsync_shell}" \
            "${src_path}" "${user}@${host}:${dst_path}" || {
            echo "MiSTer key-only rsync deploy failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
            return 1
        }
    fi
}

mister_rsync_deploy_wrapper() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"
    local deploy_mode="${6:-full}"

    local wrapper_root="${src_path%/}"
    local runtime_src="${wrapper_root}/games/3s-arm/"
    local core_src="${wrapper_root}/_Other/3S-ARM.rbf"
    local hps_src="${wrapper_root}/MiSTer_3S-ARM"

    case "${deploy_mode}" in
    full|artifacts-only|wrapper-only|core-only)
        ;;
    *)
        echo "unknown wrapper deploy mode: ${deploy_mode}" >&2
        return 1
        ;;
    esac

    if [ "${deploy_mode}" != "core-only" ]; then
        [ -f "${hps_src}" ] || { echo "wrapper HPS binary not found in package: ${hps_src}" >&2; return 1; }
    fi

    local have_core=0
    if [ "${deploy_mode}" != "wrapper-only" ]; then
        if [ -f "${core_src}" ]; then
            have_core=1
        elif [ "${deploy_mode}" = "core-only" ]; then
            echo "wrapper core RBF not found in package: ${core_src}" >&2
            return 1
        else
            echo "note: wrapper core RBF not found in package: ${core_src} (skipping RBF deploy)" >&2
        fi
    fi

    if [ "${deploy_mode}" = "full" ]; then
        [ -d "${runtime_src}" ] || { echo "wrapper runtime tree not found in package: ${runtime_src}" >&2; return 1; }
    fi

    mister_require_safe_fat_root "${dst_path}" || return $?
    mister_require_safe_runtime_root "${dst_path%/}/games/3s-arm/" || return $?

    local mkdir_dirs="'${dst_path%/}/'"
    if [ "${have_core}" -eq 1 ]; then
        mkdir_dirs="${mkdir_dirs} '${dst_path%/}/_Other'"
    fi
    if [ "${deploy_mode}" = "full" ]; then
        mkdir_dirs="${mkdir_dirs} '${dst_path%/}/games/3s-arm'"
    fi
    mister_ssh_exec "${host}" "${user}" "${password}" "mkdir -p ${mkdir_dirs}"

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        if [ "${deploy_mode}" != "core-only" ]; then
            mister_rsync_expect_copy "${hps_src}" "${host}" "${user}" "${password}" "${dst_path%/}/"
        fi
        if [ "${have_core}" -eq 1 ]; then
            mister_rsync_expect_copy "${core_src}" "${host}" "${user}" "${password}" "${dst_path%/}/_Other/"
        fi
        if [ "${deploy_mode}" = "full" ]; then
            mister_rsync_expect "${runtime_src}" "${host}" "${user}" "${password}" "${dst_path%/}/games/3s-arm/"
        fi
    else
        local rsync_shell
        rsync_shell="$(mister_rsync_ssh_key_only_command)"
        if [ "${deploy_mode}" != "core-only" ]; then
            rsync -av --omit-dir-times --no-perms --no-owner --no-group \
                -e "${rsync_shell}" \
                "${hps_src}" "${user}@${host}:${dst_path%/}/" || {
                echo "MiSTer key-only wrapper upload failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
                return 1
            }
        fi
        if [ "${have_core}" -eq 1 ]; then
            rsync -av --omit-dir-times --no-perms --no-owner --no-group \
                -e "${rsync_shell}" \
                "${core_src}" "${user}@${host}:${dst_path%/}/_Other/" || {
                echo "MiSTer key-only wrapper-core upload failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
                return 1
            }
        fi
        if [ "${deploy_mode}" = "full" ]; then
            rsync -av --delete --omit-dir-times --no-perms --no-owner --no-group \
                --exclude 'resources/SF33RD.AFS' \
                --filter 'P resources/SF33RD.AFS' \
                --exclude 'config' \
                --filter 'P config' \
                --exclude 'keymap' \
                --filter 'P keymap' \
                -e "${rsync_shell}" \
                "${runtime_src}" "${user}@${host}:${dst_path%/}/games/3s-arm/" || {
                echo "MiSTer key-only wrapper runtime deploy failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
                return 1
            }
        fi
    fi
}
