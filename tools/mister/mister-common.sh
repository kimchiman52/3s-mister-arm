#!/usr/bin/env bash

if [ -n "${__MISTER_COMMON_SH:-}" ]; then
    return 0
fi
__MISTER_COMMON_SH=1

# Directory this file lives in, so sibling data files (runtime-owned-paths.txt)
# resolve no matter what the caller's cwd is.
MISTER_COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# ===========================================================================
# Runtime deploy -- manifest-scoped, task #93
# ===========================================================================
#
# The deploy used to be `rsync -av --delete` scoped to /media/fat/games/3s-arm/
# with a fixed `--exclude`/`--filter P` allowlist naming the on-device files
# that were permitted to survive. Everything else the device held and the
# package did not was deleted.
#
# That policy destroyed real user data twice. 2026-07-25: libminiupnpc.so,
# replays/ and the user's ROM. 2026-08-29: `training` -- the persisted
# training-mode settings written by src/port/config/training_config.c:183 --
# with no device backup, plus balance.status (src/arcade/arcade_balance.c:91).
# Each time the response was to extend the allowlist. That is the third patch
# to a list that structurally cannot keep up: it enumerates the runtime files
# that exist today while the game keeps adding writers, so every new persistent
# file is destroyed by default until somebody remembers it. At the time of
# writing the list still omitted `saves/` -- the game's actual save data,
# `settings` and `sysdir` (src/sf33rd/Source/PS2/mc/savesub.c:42,335,340) --
# and `states/`, so the next loss was already queued up.
#
# The policy is now inverted. The deploy deletes only paths that a previous
# deploy recorded as its own, in a manifest it writes to the device. Anything
# unrecognised -- not in the package, not in the previous manifest -- survives
# by construction, whether or not anyone thought to add it to a list. On a
# device with no manifest yet, nothing is deleted at all.
#
# `--delete` is gone rather than narrowed. Its purpose was to stop stale
# artifacts (a dropped .so, a renamed script) accumulating, and the manifest
# diff does exactly that job with the blast radius bounded to paths we shipped.
# rsync's `--delete` cannot express "delete only what I previously installed";
# its semantics are "make the destination match the source", which is simply
# the wrong policy for a directory that is both a package install root and the
# application's writable home. There is also no `rm -rf` anywhere in this path:
# stale files are removed with `rm -f <exact path>` and stale directories with
# `rmdir` (which refuses a non-empty directory), so even a corrupted manifest
# cannot take out a subtree that still holds runtime data.
#
# The preserve list survives as `mister_deploy_preserve_paths`, demoted from a
# delete-shield to what it now genuinely is: a list of device-owned paths the
# package must never overwrite. The `--filter 'P ...'` protect rules that
# accompanied it are gone, because protect rules only ever mattered against
# `--delete`.

mister_deploy_manifest_name() {
    printf '%s\n' '.deploy-manifest'
}

mister_deploy_inventory_path() {
    printf '%s\n' "${MISTER_RUNTIME_OWNED_PATHS:-${MISTER_COMMON_DIR}/runtime-owned-paths.txt}"
}

# Device-owned paths a deploy must never overwrite and never delete.
# Patterns are matched with shell globbing against a path and its prefixes.
mister_deploy_preserve_paths() {
    printf '%s\n' \
        'resources/SF33RD.AFS' \
        'resources/*.zip' \
        'config' \
        'keymap' \
        'state' \
        'replays' \
        'training' \
        'balance.status' \
        'saves' \
        'states' \
        'logs'
}

# Top-level names the game persists at runtime, read from the inventory that
# tools/mister/derive-runtime-paths.sh derives from the source.
mister_deploy_runtime_owned_names() {
    local inventory
    inventory="$(mister_deploy_inventory_path)"

    if [ ! -f "${inventory}" ]; then
        echo "runtime-owned inventory not found: ${inventory}" >&2
        return 2
    fi

    sed -E 's/[[:space:]]*#.*$//; s/^[[:space:]]+//; s/[[:space:]]+$//' "${inventory}" |
        grep -v '^$' || true
}

mister_deploy_path_is_preserved() {
    local path="$1"
    local pattern

    while IFS= read -r pattern; do
        [ -n "${pattern}" ] || continue
        # Unquoted on purpose: these are glob patterns ('resources/*.zip').
        # shellcheck disable=SC2254
        case "${path}" in
        ${pattern} | ${pattern}/*) return 0 ;;
        esac
    done < <(mister_deploy_preserve_paths)

    return 1
}

# The rsync argument vector for a runtime deploy. Single definition so the
# password (expect) path and the key-only path cannot drift apart -- they had,
# and mister_rsync_deploy_wrapper had drifted from both, which is why the
# wrapper deploy's list was still missing `training` and `balance.status` after
# the 2026-08-29 loss was "fixed".
mister_deploy_rsync_args() {
    local pattern

    printf '%s\n' -av --omit-dir-times --no-perms --no-owner --no-group

    while IFS= read -r pattern; do
        [ -n "${pattern}" ] || continue
        printf '%s\n' "--exclude=${pattern}"
    done < <(mister_deploy_preserve_paths)
}

# Manifest of what a package tree installs: one "<type> <relative path>" line
# per entry, `d` for directories and `f` for everything else, sorted. Preserved
# paths are omitted, so a package that accidentally contains one can never make
# it into the set of paths a later deploy considers its own to delete.
mister_deploy_local_manifest() {
    local src_dir="${1%/}"
    local path

    if [ ! -d "${src_dir}" ]; then
        echo "package directory not found: ${src_dir}" >&2
        return 2
    fi

    {
        while IFS= read -r path; do
            path="${path#./}"
            [ -n "${path}" ] || continue
            mister_deploy_path_is_preserved "${path}" && continue
            printf 'd %s\n' "${path}"
        done < <(cd "${src_dir}" && find . -mindepth 1 -type d)

        while IFS= read -r path; do
            path="${path#./}"
            [ -n "${path}" ] || continue
            mister_deploy_path_is_preserved "${path}" && continue
            printf 'f %s\n' "${path}"
        done < <(cd "${src_dir}" && find . -mindepth 1 ! -type d)
    } | LC_ALL=C sort
}

# A relative path we are willing to name in a remote `rm`/`rmdir`.
mister_deploy_path_is_sane() {
    local path="$1"

    [ -n "${path}" ] || return 1
    case "${path}" in
    /* | -* | *'..'* | *'*'* | *'?'* | *'$'* | *'`'* | *'"'* | *"'"* | *'
'*)
        return 1
        ;;
    esac

    return 0
}

# Plan what a deploy of ${package_dir} may remove, given the manifest the
# previous deploy left on the device.
#
# Emits one line per candidate:
#   prune <d|f> <path>   safe to remove: a previous deploy installed it and
#                        this package does not
#   veto  <reason> <path>  must not be removed; the caller aborts the deploy
#
# Nothing that is absent from ${old_manifest} is ever a candidate. That is the
# whole point: an on-device file this tooling never installed is invisible to
# the planner and therefore survives, no list membership required.
mister_deploy_plan_prune() {
    local package_dir="$1"
    local old_manifest="$2"
    local new_paths owned_names package_top
    local line type path top

    [ -f "${old_manifest}" ] || return 0

    new_paths="$(mister_deploy_local_manifest "${package_dir}" | cut -d' ' -f2-)"
    owned_names="$(mister_deploy_runtime_owned_names)" || return $?
    # Top-level names the package itself installs are package-owned; the deploy
    # is the authority on their contents. Without this, `lib` and `bin` -- which
    # the inventory lists because the wrapper hardcodes those literals -- would
    # veto exactly the stale-artifact cleanup the prune exists to do (a dropped
    # lib/libminiupnpc.so, say).
    package_top="$(cd "${package_dir%/}" && find . -mindepth 1 -maxdepth 1 | sed 's|^\./||')"

    while IFS= read -r line; do
        [ -n "${line}" ] || continue
        type="${line%% *}"
        path="${line#* }"

        if ! mister_deploy_path_is_sane "${path}"; then
            printf 'veto unsafe-path %s\n' "${path}"
            continue
        fi

        case "${type}" in
        d | f) ;;
        *)
            printf 'veto unknown-type %s\n' "${path}"
            continue
            ;;
        esac

        # Still shipped by this package: not stale, nothing to do.
        if printf '%s\n' "${new_paths}" | grep -qxF "${path}"; then
            continue
        fi

        if mister_deploy_path_is_preserved "${path}"; then
            printf 'veto preserved %s\n' "${path}"
            continue
        fi

        top="${path%%/*}"
        if printf '%s\n' "${owned_names}" | grep -qxF "${top}" &&
            ! printf '%s\n' "${package_top}" | grep -qxF "${top}"; then
            printf 'veto runtime-owned %s\n' "${path}"
            continue
        fi

        printf 'prune %s %s\n' "${type}" "${path}"
    done <"${old_manifest}"
}

# Remote script that removes a planned prune set. Files go first, then
# directories deepest-first via `rmdir`, which fails on a non-empty directory
# and so cannot take runtime data with it.
mister_deploy_prune_script() {
    local remote_root="$1"
    local plan_file="$2"
    local line type path
    local -a files=()
    local -a dirs=()

    while IFS= read -r line; do
        case "${line}" in
        'prune '*) ;;
        *) continue ;;
        esac
        line="${line#prune }"
        type="${line%% *}"
        path="${line#* }"
        if [ "${type}" = "d" ]; then
            dirs+=("${path}")
        else
            files+=("${path}")
        fi
    done <"${plan_file}"

    printf 'set -e\n'
    printf 'cd %s\n' "$(mister_shell_quote "${remote_root}")"

    if [ "${#files[@]}" -gt 0 ]; then
        printf 'rm -f --'
        for path in "${files[@]}"; do
            printf ' %s' "$(mister_shell_quote "${path}")"
        done
        printf '\n'
    fi

    if [ "${#dirs[@]}" -gt 0 ]; then
        # Deepest first so a directory is empty by the time rmdir reaches it.
        while IFS= read -r path; do
            [ -n "${path}" ] || continue
            printf 'rmdir -- %s 2>/dev/null || true\n' "$(mister_shell_quote "${path}")"
        done < <(printf '%s\n' "${dirs[@]}" | awk '{print gsub(/\//,"/"), $0}' | sort -rn | cut -d' ' -f2-)
    fi

    printf 'echo __MISTER_PRUNE_DONE__\n'
}

mister_deploy_fetch_manifest() {
    local host="$1"
    local user="$2"
    local password="$3"
    local remote_root="$4"
    local out_file="$5"
    local manifest_path raw

    manifest_path="${remote_root%/}/$(mister_deploy_manifest_name)"
    : >"${out_file}"

    raw="$(mister_ssh_exec "${host}" "${user}" "${password}" "$(
        printf 'echo __MISTER_MANIFEST_BEGIN__\n'
        printf '[ -f %s ] && cat %s\n' \
            "$(mister_shell_quote "${manifest_path}")" \
            "$(mister_shell_quote "${manifest_path}")"
        printf 'echo __MISTER_MANIFEST_END__\n'
    )" 2>/dev/null)" || true

    printf '%s\n' "${raw}" |
        tr -d '\r' |
        sed -n '/^__MISTER_MANIFEST_BEGIN__$/,/^__MISTER_MANIFEST_END__$/p' |
        sed '1d;$d' >"${out_file}"
}

mister_deploy_write_manifest() {
    local host="$1"
    local user="$2"
    local password="$3"
    local remote_root="$4"
    local manifest_file="$5"
    local manifest_path encoded

    manifest_path="${remote_root%/}/$(mister_deploy_manifest_name)"
    encoded="$(base64 <"${manifest_file}" | tr -d '\n')"

    mister_ssh_exec "${host}" "${user}" "${password}" "$(
        printf 'set -e\n'
        printf 'printf %%s %s | base64 -d > %s\n' \
            "$(mister_shell_quote "${encoded}")" \
            "$(mister_shell_quote "${manifest_path}.tmp")"
        printf 'mv %s %s\n' \
            "$(mister_shell_quote "${manifest_path}.tmp")" \
            "$(mister_shell_quote "${manifest_path}")"
        printf 'echo __MISTER_MANIFEST_WRITTEN__\n'
    )"
}

# Upload the package, then remove only what a previous deploy owned and this
# one does not. Shared by `deploy` and by the runtime half of `deploy-wrapper`.
mister_deploy_runtime_tree() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local tmp_dir old_manifest new_manifest plan_file prune_script
    local vetoes prunes rc=0

    mister_require_safe_runtime_root "${dst_path}" || return $?

    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/mister-deploy.XXXXXX")" || return 1
    old_manifest="${tmp_dir}/old-manifest"
    new_manifest="${tmp_dir}/new-manifest"
    plan_file="${tmp_dir}/plan"
    prune_script="${tmp_dir}/prune.sh"

    mister_deploy_local_manifest "${src_path}" >"${new_manifest}" || {
        rm -rf "${tmp_dir}"
        return 2
    }

    # rsync creates only the final component of a destination path, so a root
    # whose parents do not exist yet fails the transfer with a bare
    # "unexpected end of file". The path has already been through
    # mister_require_safe_runtime_root above.
    mister_ssh_exec "${host}" "${user}" "${password}" \
        "mkdir -p $(mister_shell_quote "${dst_path%/}")" >/dev/null 2>&1 || true

    mister_deploy_fetch_manifest "${host}" "${user}" "${password}" "${dst_path}" "${old_manifest}"

    if [ ! -s "${old_manifest}" ]; then
        echo "deploy: no previous manifest on the device; nothing is owned, so nothing will be removed."
        : >"${plan_file}"
    else
        mister_deploy_plan_prune "${src_path}" "${old_manifest}" >"${plan_file}" || {
            echo "deploy: could not plan the prune set; refusing to deploy." >&2
            rm -rf "${tmp_dir}"
            return 2
        }
    fi

    # Pre-flight: say what would be removed before anything is removed, and
    # refuse outright if the plan names something unrecognised.
    vetoes="$(grep '^veto ' "${plan_file}" || true)"
    prunes="$(grep '^prune ' "${plan_file}" || true)"

    if [ -n "${prunes}" ]; then
        echo "deploy: stale paths recorded by the previous deploy and absent from this package:"
        printf '%s\n' "${prunes}" | sed 's/^prune [df] /  - /'
    else
        echo "deploy: no stale paths to remove."
    fi

    if [ -n "${vetoes}" ]; then
        echo "ERROR: the previous deploy's manifest names paths this deploy must not remove:" >&2
        printf '%s\n' "${vetoes}" | sed 's/^veto /  - /' >&2
        echo "       'runtime-owned' means the game persists that path (see" >&2
        echo "       tools/mister/runtime-owned-paths.txt); a package should never ship it." >&2
        echo "       'unsafe-path'/'unknown-type' means the manifest is malformed." >&2
        echo "       Refusing to deploy rather than delete it." >&2
        echo "       To reset ownership -- after which the deploy installs but removes" >&2
        echo "       nothing until it has written a manifest of its own -- delete" >&2
        echo "       ${dst_path%/}/$(mister_deploy_manifest_name) on the device." >&2
        rm -rf "${tmp_dir}"
        return 2
    fi

    if [ "${MISTER_DEPLOY_PLAN_ONLY:-0}" = "1" ]; then
        echo "deploy: MISTER_DEPLOY_PLAN_ONLY=1, stopping before any transfer."
        rm -rf "${tmp_dir}"
        return 0
    fi

    mister_rsync_runtime_upload "${src_path}" "${host}" "${user}" "${password}" "${dst_path}" || rc=$?
    if [ "${rc}" -ne 0 ]; then
        rm -rf "${tmp_dir}"
        return "${rc}"
    fi

    if [ -n "${prunes}" ]; then
        if [ "${MISTER_DEPLOY_NO_PRUNE:-0}" = "1" ]; then
            echo "deploy: MISTER_DEPLOY_NO_PRUNE=1, leaving the stale paths in place."
            echo "        The manifest is not updated, so the next deploy re-plans the same removals."
            rm -rf "${tmp_dir}"
            return 0
        fi

        mister_deploy_prune_script "${dst_path}" "${plan_file}" >"${prune_script}"
        mister_ssh_exec "${host}" "${user}" "${password}" "$(cat "${prune_script}")" || rc=$?
        if [ "${rc}" -ne 0 ]; then
            echo "deploy: prune step failed; leaving the previous manifest in place so it can be retried." >&2
            rm -rf "${tmp_dir}"
            return "${rc}"
        fi
    fi

    mister_deploy_write_manifest "${host}" "${user}" "${password}" "${dst_path}" "${new_manifest}" || rc=$?
    rm -rf "${tmp_dir}"
    return "${rc}"
}

mister_rsync_expect() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"

    local timeout_seconds
    local rsync_shell
    local rsync_args
    timeout_seconds="$(mister_transfer_timeout)"
    rsync_shell="$(mister_rsync_ssh_password_command)"
    # Newline-separated and split inside expect, matching how EXPECT_SCP_ARGS
    # is already handled above, so the Tcl spawn cannot carry a hand-copied
    # duplicate of the argument list.
    rsync_args="$(mister_deploy_rsync_args)"

    EXPECT_HOST="$host" EXPECT_USER="$user" EXPECT_PASSWORD="$password" EXPECT_SRC="$src_path" EXPECT_DST="$dst_path" EXPECT_TIMEOUT="$timeout_seconds" EXPECT_RSYNC_SHELL="$rsync_shell" EXPECT_RSYNC_ARGS="$rsync_args" expect <<'EOF'
set timeout $env(EXPECT_TIMEOUT)
set host $env(EXPECT_HOST)
set user $env(EXPECT_USER)
set pw $env(EXPECT_PASSWORD)
set src_path $env(EXPECT_SRC)
set dst_path $env(EXPECT_DST)
set rsync_shell $env(EXPECT_RSYNC_SHELL)
set rsync_args [split $env(EXPECT_RSYNC_ARGS) "\n"]
spawn rsync {*}$rsync_args -e $rsync_shell "$src_path" ${user}@${host}:${dst_path}
expect {
  -re {[Pp]assword:} { send -- "$pw\r"; exp_continue }
  eof
}
set status [lindex [wait] 3]
exit $status
EOF
}

# Transfer only. Deletion is the caller's business (mister_deploy_runtime_tree).
mister_rsync_runtime_upload() {
    local src_path="$1"
    local host="$2"
    local user="$3"
    local password="$4"
    local dst_path="$5"
    local arg
    local -a rsync_args

    if [ -n "${password}" ]; then
        mister_require_cmd expect || return $?
        mister_rsync_expect "${src_path}" "${host}" "${user}" "${password}" "${dst_path}"
    else
        local rsync_shell
        rsync_shell="$(mister_rsync_ssh_key_only_command)"
        while IFS= read -r arg; do
            rsync_args+=("${arg}")
        done < <(mister_deploy_rsync_args)
        rsync "${rsync_args[@]}" -e "${rsync_shell}" \
            "${src_path}" "${user}@${host}:${dst_path}" || {
            echo "MiSTer key-only rsync deploy failed; set MISTER_PASSWORD to use password auth or configure a working SSH key." >&2
            return 1
        }
    fi
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
    mister_deploy_runtime_tree "$@"
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
    local core_src
    core_src=$(find "${wrapper_root}/_Other" -maxdepth 1 -type f -name '3S-ARM*.rbf' 2>/dev/null | sort | tail -1)
    : "${core_src:=${wrapper_root}/_Other/3S-ARM.rbf}"
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
            # Task #93: same manifest-scoped path as `deploy`. This branch used
            # to call mister_rsync_expect directly, which meant it carried
            # --delete with an even staler preserve list than the one in
            # mister_rsync_deploy -- it never gained `training` or
            # `balance.status` at all, so a wrapper deploy destroyed them even
            # after the 2026-08-29 fix.
            mister_deploy_runtime_tree "${runtime_src}" "${host}" "${user}" "${password}" "${dst_path%/}/games/3s-arm/" || return $?
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
            # Task #93: see the note on the password branch above.
            mister_deploy_runtime_tree "${runtime_src}" "${host}" "${user}" "${password}" "${dst_path%/}/games/3s-arm/" || return $?
        fi
    fi
}
