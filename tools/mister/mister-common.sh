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
    printf '%s\n' "${MISTER_LOCK_DIR:-${TMPDIR:-/tmp}/3sx-mister-remote.lock}"
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

MISTER_LOCK_HELD=0
MISTER_LOCK_OWNER_FILE=""
MISTER_LOCK_DIR_VALUE=""

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

mister_base64_decode_file() {
    local src_path="$1"
    local dst_path="$2"

    if base64 --decode <"${src_path}" >"${dst_path}" 2>/dev/null; then
        return 0
    fi

    if base64 -D <"${src_path}" >"${dst_path}" 2>/dev/null; then
        return 0
    fi

    if command -v openssl >/dev/null 2>&1; then
        openssl base64 -d -A -in "${src_path}" -out "${dst_path}"
        return $?
    fi

    echo "unable to decode base64 payload: no supported decoder found" >&2
    return 1
}

mister_ssh_expect() {
    local host="$1"
    local user="$2"
    local password="$3"
    local remote_cmd="$4"
    local encoded_cmd
    local timeout_seconds
    encoded_cmd="$(mister_base64_encode_inline "${remote_cmd}")"
    timeout_seconds="$(mister_cmd_timeout)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_CMD="$encoded_cmd" EXPECT_TIMEOUT="$timeout_seconds" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set cmd $env(EXPECT_CMD)
spawn ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 ${user}@${host} "echo '${cmd}' | base64 -d | sh"
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

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_ssh_expect "${host}" "${user}" "${password}" "${remote_cmd}"
    else
        ssh -o StrictHostKeyChecking=no "${user}@${host}" "${remote_cmd}"
    fi
}

mister_scp_expect() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local timeout_seconds
    timeout_seconds="$(mister_transfer_timeout)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
spawn scp -o StrictHostKeyChecking=no -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 "$src_path" ${user}@${host}:${dst_path}
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

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_scp_expect "${src_path}" "${host}" "${user}" "${password}" "${dst_path}"
    else
        scp -o StrictHostKeyChecking=no "${src_path}" "${user}@${host}:${dst_path}"
    fi
}

mister_rsync_expect() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local timeout_seconds
    timeout_seconds="$(mister_transfer_timeout)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
spawn rsync -av --delete --omit-dir-times --no-perms --no-owner --no-group --exclude resources/SF33RD.AFS --filter {P resources/SF33RD.AFS} -e {ssh -o StrictHostKeyChecking=no -o PubkeyAuthentication=no -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1} "$src_path" ${user}@${host}:${dst_path}
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

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_rsync_expect "${src_path}" "${host}" "${user}" "${password}" "${dst_path}"
    else
        rsync -av --delete --omit-dir-times --no-perms --no-owner --no-group \
            --exclude 'resources/SF33RD.AFS' \
            --filter 'P resources/SF33RD.AFS' \
            "${src_path}" "${user}@${host}:${dst_path}"
    fi
}
