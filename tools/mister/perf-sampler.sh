#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/mister/mister-common.sh
source "${SCRIPT_DIR}/mister-common.sh"

usage() {
    cat <<'EOF'
Usage: tools/mister/perf-sampler.sh --scene <name> --frames <count> --tag <name> [options]

Options:
  --scene <name>         Scene label for perf metadata.
  --frames <count>       Number of frames to capture.
  --tag <name>           Output tag; safe basename only (`[A-Za-z0-9][A-Za-z0-9._-]*`).
  --gameplay-idle        Use the built-in scripted idle-versus path and wait for gameplay before capture.
  --gameplay-warmup <n>  Warmup frames to skip after gameplay or the selected wait condition becomes active
                         (default: 120 for gameplay waits, 0 for test-phase waits).
  --perf-basic          Capture low-overhead frame/update/render/present timings; lightweight
                        test-state metadata may still be exported when available.
  --perf-wait-test-phase <name>
                         Delay capture until the test runner reaches the named phase
                         (title, menu, character-select-transition, character-select, game-transition, game, game-input-active, wipe-transition-type1).
  --perf-wait-runtime-state <name>
                         Delay capture until the runtime reaches the named state
                         (attract-demo-logo).
  --test-scene-preset <name>
                         Named scripted gameplay preset (stage-heavy, effect-heavy, super-heavy, basic-exchange).
  --test-p1-character <name-or-id>
                         Optional test-runner player 1 character override for scripted gameplay capture; requires --gameplay-idle or --test-scene-preset.
  --test-p2-character <name-or-id>
                         Optional test-runner player 2 character override for scripted gameplay capture; requires --gameplay-idle or --test-scene-preset.
  --test-p1-super-art <id>
                         Optional test-runner player 1 super art override (0-2); requires --gameplay-idle or --test-scene-preset.
  --test-p2-super-art <id>
                         Optional test-runner player 2 super art override (0-2); requires --gameplay-idle or --test-scene-preset.
  --test-p1-super-full   Optional test-runner player 1 full-super bootstrap on the first gameplay frame;
                         requires --gameplay-idle or --test-scene-preset.
  --test-preserve-game-transition
                         Optional test-runner flag that keeps the pre-game transition unskipped;
                         requires --perf-wait-test-phase game-transition.
  --test-delay-gameplay-inputs-until-active
                         Optional test-runner flag that delays scripted gameplay inputs and
                         first-frame super bootstrap until both players reach gameplay/input-active state.
  --test-stage <id>      Optional test-runner stage override (0-19, excluding 17); requires --gameplay-idle or --test-scene-preset.
  --scale-mode <mode>    Optional config override for `scale-mode` during capture
                         (nearest, native, linear, soft-linear, square-pixels, integer).
  --software-frame-mode <off|on>
                         Optional config override for `software-frame-mode` during capture.
                         Perf captures always force `show-fps = false` temporarily so the
                         player-facing HUD does not skew optimization measurements.
  --host <ip-or-host>    MiSTer host (default: $MISTER_HOST or 192.168.1.171).
  --user <name>          SSH user (default: $MISTER_USER or root).
  --password <value>     SSH password (default: $MISTER_PASSWORD).
  --remote-root <path>   Remote 3SX root (default: $MISTER_ROOT or /media/fat/games/3sx).
  --copy-afs <path>      Optional local SF33RD.AFS path to stage temporarily before capture.
  --help                 Show this message.

Environment:
  MISTER_HOST, MISTER_USER, MISTER_PASSWORD, MISTER_ROOT
  MISTER_UNSAFE_ALLOW_ANY_REMOTE_ROOT=1 and MISTER_UNSAFE_CONFIRM_REMOTE_ROOT=<exact-path>
    only for deliberate nonstandard remote test trees
  MISTER_LOCK_DIR, MISTER_LOCK_TIMEOUT, MISTER_CMD_TIMEOUT, MISTER_TRANSFER_TIMEOUT
EOF
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 2
    fi
}

resolve_character_id() {
    local value="$1"
    local normalized

    if [[ "$value" =~ ^[0-9]+$ ]]; then
        printf '%d\n' "$((10#$value))"
        return 0
    fi

    normalized="$(printf '%s' "$value" | tr '[:upper:]' '[:lower:]')"
    case "$normalized" in
    gill) printf '0\n' ;;
    alex) printf '1\n' ;;
    ryu) printf '2\n' ;;
    yun) printf '3\n' ;;
    dudley) printf '4\n' ;;
    necro) printf '5\n' ;;
    hugo) printf '6\n' ;;
    ibuki) printf '7\n' ;;
    elena) printf '8\n' ;;
    oro) printf '9\n' ;;
    yang) printf '10\n' ;;
    ken) printf '11\n' ;;
    sean) printf '12\n' ;;
    urien) printf '13\n' ;;
    akuma) printf '14\n' ;;
    chunli|chun-li|chun_li) printf '15\n' ;;
    makoto) printf '16\n' ;;
    q) printf '17\n' ;;
    twelve) printf '18\n' ;;
    remy) printf '19\n' ;;
    *) return 1 ;;
    esac
}

is_supported_stage_id() {
    local value="$1"
    [ "$value" -ge 0 ] && [ "$value" -le 19 ] && [ "$value" -ne 17 ]
}

is_supported_test_scene_preset() {
    local value="$1"
    [ "$value" = "stage-heavy" ] || [ "$value" = "effect-heavy" ] || [ "$value" = "super-heavy" ] ||
        [ "$value" = "basic-exchange" ]
}

is_supported_software_frame_mode() {
    local value="$1"
    [ "$value" = "off" ] || [ "$value" = "on" ]
}

is_supported_scale_mode() {
    local value="$1"
    [ "$value" = "nearest" ] || [ "$value" = "native" ] || [ "$value" = "linear" ] ||
        [ "$value" = "soft-linear" ] || [ "$value" = "square-pixels" ] || [ "$value" = "integer" ]
}

is_supported_perf_wait_test_phase() {
    local value="$1"
    [ "$value" = "title" ] || [ "$value" = "menu" ] ||
        [ "$value" = "character-select-transition" ] || [ "$value" = "character-select" ] ||
        [ "$value" = "game-transition" ] || [ "$value" = "game" ] || [ "$value" = "game-input-active" ] ||
        [ "$value" = "wipe-transition-type1" ]
}

is_supported_perf_wait_runtime_state() {
    local value="$1"
    [ "$value" = "attract-demo-logo" ]
}

is_safe_perf_tag() {
    local value="$1"
    [[ "$value" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]
}

apply_test_scene_preset_defaults() {
    local preset="$1"

    case "$preset" in
    stage-heavy)
        if [ -z "$test_stage" ]; then
            test_stage="19"
        fi
        ;;
    effect-heavy)
        if [ -z "$test_p1_character" ]; then
            test_p1_character="2"
        fi
        if [ -z "$test_p2_character" ]; then
            test_p2_character="11"
        fi
        if [ -z "$test_p1_super_art" ]; then
            test_p1_super_art="0"
        fi
        if [ -z "$test_p2_super_art" ]; then
            test_p2_super_art="0"
        fi
        if [ -z "$test_stage" ]; then
            test_stage="19"
        fi
        ;;
    super-heavy)
        if [ -z "$test_p1_character" ]; then
            test_p1_character="2"
        fi
        if [ -z "$test_p2_character" ]; then
            test_p2_character="2"
        fi
        if [ -z "$test_p1_super_art" ]; then
            test_p1_super_art="0"
        fi
        if [ -z "$test_p2_super_art" ]; then
            test_p2_super_art="0"
        fi
        if [ -z "$test_stage" ]; then
            test_stage="19"
        fi
        ;;
    basic-exchange)
        if [ -z "$test_p1_character" ]; then
            test_p1_character="2"
        fi
        if [ -z "$test_p2_character" ]; then
            test_p2_character="11"
        fi
        if [ -z "$test_p1_super_art" ]; then
            test_p1_super_art="0"
        fi
        if [ -z "$test_p2_super_art" ]; then
            test_p2_super_art="0"
        fi
        if [ -z "$test_stage" ]; then
            test_stage="11"
        fi
        ;;
    esac
}

extract_perf_log_token() {
    local field_name="$1"
    local log_line="$2"
    local token=""

    for token in $log_line; do
        case "$token" in
        "${field_name}="*)
            printf '%s\n' "${token#*=}"
            return 0
            ;;
        esac
    done
}

extract_perf_log_field() {
    local field_name="$1"
    local log_line="$2"
    local token=""

    token="$(extract_perf_log_token "$field_name" "$log_line")"
    if [[ "$token" =~ ^-?[0-9]+$ ]]; then
        printf '%s\n' "$token"
    fi
}

extract_perf_log_string_field() {
    local field_name="$1"
    local log_line="$2"

    extract_perf_log_token "$field_name" "$log_line"
}

if ! command -v jq >/dev/null 2>&1; then
    echo "warning: jq not found locally; summary output will be minimal." >&2
fi

require_cmd ssh
require_cmd scp

scene=""
frames=""
tag=""
host="${MISTER_HOST:-192.168.1.171}"
user="${MISTER_USER:-root}"
password="${MISTER_PASSWORD:-}"
remote_root="${MISTER_ROOT:-/media/fat/games/3sx}"
copy_afs_path=""
gameplay_idle=0
gameplay_warmup=120
gameplay_warmup_explicit=0
perf_basic=0
perf_wait_test_phase=""
perf_wait_runtime_state=""
test_scene_preset=""
test_p1_character=""
test_p2_character=""
test_p1_super_art=""
test_p2_super_art=""
test_p1_super_full=0
test_preserve_game_transition=0
test_delay_gameplay_inputs_until_active=0
test_stage=""
scale_mode=""
software_frame_mode=""
have_test_overrides=0

while [ "$#" -gt 0 ]; do
    case "$1" in
    --scene)
        scene="$2"
        shift 2
        ;;
    --frames)
        frames="$2"
        shift 2
        ;;
    --tag)
        tag="$2"
        shift 2
        ;;
    --gameplay-idle)
        gameplay_idle=1
        shift
        ;;
    --gameplay-warmup)
        gameplay_warmup="$2"
        gameplay_warmup_explicit=1
        shift 2
        ;;
    --perf-basic)
        perf_basic=1
        shift
        ;;
    --perf-wait-test-phase)
        perf_wait_test_phase="$2"
        shift 2
        ;;
    --perf-wait-runtime-state)
        perf_wait_runtime_state="$2"
        shift 2
        ;;
    --test-scene-preset)
        test_scene_preset="$2"
        shift 2
        ;;
    --test-p1-character)
        test_p1_character="$2"
        have_test_overrides=1
        shift 2
        ;;
    --test-p2-character)
        test_p2_character="$2"
        have_test_overrides=1
        shift 2
        ;;
    --test-p1-super-art)
        test_p1_super_art="$2"
        have_test_overrides=1
        shift 2
        ;;
    --test-p2-super-art)
        test_p2_super_art="$2"
        have_test_overrides=1
        shift 2
        ;;
    --test-p1-super-full)
        test_p1_super_full=1
        have_test_overrides=1
        shift
        ;;
    --test-preserve-game-transition)
        test_preserve_game_transition=1
        shift
        ;;
    --test-delay-gameplay-inputs-until-active)
        test_delay_gameplay_inputs_until_active=1
        have_test_overrides=1
        shift
        ;;
    --test-stage)
        test_stage="$2"
        have_test_overrides=1
        shift 2
        ;;
    --scale-mode)
        scale_mode="$2"
        shift 2
        ;;
    --software-frame-mode)
        software_frame_mode="$2"
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
    --copy-afs)
        copy_afs_path="$2"
        shift 2
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        echo "unknown argument: $1" >&2
        usage
        exit 2
        ;;
    esac
done

if [ -z "$scene" ] || [ -z "$frames" ] || [ -z "$tag" ]; then
    echo "error: --scene, --frames, and --tag are required." >&2
    usage
    exit 2
fi

if ! is_safe_perf_tag "$tag"; then
    echo "error: --tag must match [A-Za-z0-9][A-Za-z0-9._-]*." >&2
    exit 2
fi

if ! [[ "$frames" =~ ^[0-9]+$ ]] || [ "$frames" -le 0 ]; then
    echo "error: --frames must be a positive integer." >&2
    exit 2
fi

if ! [[ "$gameplay_warmup" =~ ^[0-9]+$ ]]; then
    echo "error: --gameplay-warmup must be a non-negative integer." >&2
    exit 2
fi

if [ -n "$perf_wait_test_phase" ] && ! is_supported_perf_wait_test_phase "$perf_wait_test_phase"; then
    echo "error: --perf-wait-test-phase must be one of title, menu, character-select-transition, character-select, game-transition, game, game-input-active, or wipe-transition-type1." >&2
    exit 2
fi

if [ -n "$perf_wait_runtime_state" ] && ! is_supported_perf_wait_runtime_state "$perf_wait_runtime_state"; then
    echo "error: --perf-wait-runtime-state must be attract-demo-logo." >&2
    exit 2
fi

if [ -n "$test_scene_preset" ] && ! is_supported_test_scene_preset "$test_scene_preset"; then
    echo "error: --test-scene-preset must be one of stage-heavy, effect-heavy, super-heavy, or basic-exchange." >&2
    exit 2
fi

if [ -n "$perf_wait_test_phase" ] && [ -n "$perf_wait_runtime_state" ]; then
    echo "error: --perf-wait-test-phase cannot be combined with --perf-wait-runtime-state." >&2
    exit 2
fi

if [ "$test_preserve_game_transition" -eq 1 ] &&
    [ "$perf_wait_test_phase" != "game-transition" ] &&
    [ "$perf_wait_test_phase" != "wipe-transition-type1" ]; then
    echo "error: --test-preserve-game-transition requires --perf-wait-test-phase game-transition or wipe-transition-type1." >&2
    exit 2
fi

if [ "$gameplay_idle" -eq 1 ] && { [ -n "$perf_wait_test_phase" ] || [ -n "$perf_wait_runtime_state" ]; }; then
    echo "error: --gameplay-idle cannot be combined with --perf-wait-test-phase or --perf-wait-runtime-state." >&2
    exit 2
fi

if { [ -n "$perf_wait_test_phase" ] || [ -n "$perf_wait_runtime_state" ]; } && [ "$gameplay_warmup_explicit" -eq 0 ]; then
    gameplay_warmup=0
fi

if [ -n "$perf_wait_runtime_state" ] &&
    { [ "$have_test_overrides" -eq 1 ] || [ -n "$test_scene_preset" ] || [ "$test_preserve_game_transition" -eq 1 ]; }; then
    echo "error: --perf-wait-runtime-state cannot be combined with test-runner scene overrides or preserved transitions." >&2
    exit 2
fi

if [ "$have_test_overrides" -eq 1 ] && [ "$gameplay_idle" -ne 1 ] && [ -z "$test_scene_preset" ]; then
    echo "error: --test-* overrides require --gameplay-idle or --test-scene-preset." >&2
    exit 2
fi

if [ -n "$test_scene_preset" ]; then
    apply_test_scene_preset_defaults "$test_scene_preset"
fi

if [ -n "$test_p1_character" ]; then
    resolved_character=""
    if ! resolved_character="$(resolve_character_id "$test_p1_character")"; then
        echo "error: unsupported --test-p1-character value: $test_p1_character" >&2
        exit 2
    fi
    if [ "$resolved_character" -lt 0 ] || [ "$resolved_character" -gt 19 ]; then
        echo "error: --test-p1-character must resolve to 0-19." >&2
        exit 2
    fi
    test_p1_character="$resolved_character"
fi

if [ -n "$test_p2_character" ]; then
    resolved_character=""
    if ! resolved_character="$(resolve_character_id "$test_p2_character")"; then
        echo "error: unsupported --test-p2-character value: $test_p2_character" >&2
        exit 2
    fi
    if [ "$resolved_character" -lt 0 ] || [ "$resolved_character" -gt 19 ]; then
        echo "error: --test-p2-character must resolve to 0-19." >&2
        exit 2
    fi
    test_p2_character="$resolved_character"
fi

if [ -n "$test_p1_super_art" ] && ! [[ "$test_p1_super_art" =~ ^[0-2]$ ]]; then
    echo "error: --test-p1-super-art must be 0, 1, or 2." >&2
    exit 2
fi

if [ -n "$test_p2_super_art" ] && ! [[ "$test_p2_super_art" =~ ^[0-2]$ ]]; then
    echo "error: --test-p2-super-art must be 0, 1, or 2." >&2
    exit 2
fi

if [ -n "$test_stage" ]; then
    if ! [[ "$test_stage" =~ ^[0-9]+$ ]]; then
        echo "error: --test-stage must be an integer." >&2
        exit 2
    fi

    test_stage="$((10#$test_stage))"
    if ! is_supported_stage_id "$test_stage"; then
        echo "error: --test-stage must be one of 0-19 excluding 17." >&2
        exit 2
    fi
fi

if [ -n "$software_frame_mode" ] && ! is_supported_software_frame_mode "$software_frame_mode"; then
    echo "error: --software-frame-mode must be 'off' or 'on'." >&2
    exit 2
fi

if [ -n "$scale_mode" ] && ! is_supported_scale_mode "$scale_mode"; then
    echo "error: --scale-mode must be one of nearest, native, linear, soft-linear, square-pixels, or integer." >&2
    exit 2
fi

if [ -n "$password" ]; then
    require_cmd expect
fi

mister_require_safe_runtime_root "$remote_root" || exit 1

local_perf_dir="artifacts/mister-port/perf"
local_output_path="${local_perf_dir}/${tag}.json"
remote_output_path="${remote_root}/logs/perf-${tag}.json"
remote_log_path="/tmp/3sx-perf-${tag}.log"
remote_config_path="${remote_root}/config"
remote_resources_afs="${remote_root}/resources/SF33RD.AFS"
remote_afs_backup_path="/tmp/3sx-afs-${tag}.bak"
remote_afs_missing_marker="/tmp/3sx-afs-${tag}.missing"
remote_afs_restore_needed=0
extra_app_args=""
remote_stdout_path=""
local_remote_log_path=""
local_downloaded_output_path=""

cleanup_local() {
    local status="${1:-$?}"
    local cleanup_path=""
    trap - EXIT INT TERM HUP

    if [ "${remote_afs_restore_needed}" -eq 1 ]; then
        mister_ssh_exec "$host" "$user" "$password" "
set -e
if [ -f '${remote_afs_backup_path}' ]; then
  cp '${remote_afs_backup_path}' '${remote_resources_afs}'
  rm -f '${remote_afs_backup_path}'
elif [ -f '${remote_afs_missing_marker}' ]; then
  rm -f '${remote_resources_afs}' '${remote_afs_missing_marker}'
fi
        " || echo "warning: failed to restore remote SF33RD.AFS after perf capture" >&2
        remote_afs_restore_needed=0
    fi

    for cleanup_path in \
        "${local_downloaded_output_path}" \
        "${local_remote_log_path}" \
        "${remote_stdout_path}"; do
        if [ -n "${cleanup_path}" ]; then
            rm -f "${cleanup_path}"
        fi
    done

    mister_lock_release || true
    exit "${status}"
}

if [ "$gameplay_idle" -eq 1 ] || [ "$have_test_overrides" -eq 1 ] || [ -n "$test_scene_preset" ] ||
    [ "$test_preserve_game_transition" -eq 1 ] ||
    [ -n "$perf_wait_test_phase" ]; then
    extra_app_args="--test-enable"
fi

if [ "$perf_basic" -eq 1 ]; then
    extra_app_args="${extra_app_args} --perf-basic"
fi

if { [ "$gameplay_idle" -eq 1 ] || [ -n "$test_scene_preset" ]; } && [ -z "$perf_wait_test_phase" ]; then
    extra_app_args="${extra_app_args} --perf-wait-in-game --perf-warmup '${gameplay_warmup}'"
fi

if [ -n "$perf_wait_test_phase" ]; then
    extra_app_args="${extra_app_args} --perf-wait-test-phase '${perf_wait_test_phase}' --perf-warmup '${gameplay_warmup}'"
fi

if [ -n "$perf_wait_runtime_state" ]; then
    extra_app_args="${extra_app_args} --perf-wait-runtime-state '${perf_wait_runtime_state}' --perf-warmup '${gameplay_warmup}'"
fi

if [ -n "$test_scene_preset" ]; then
    extra_app_args="${extra_app_args} --test-scene-preset '${test_scene_preset}'"
fi

if [ -n "$test_p1_character" ]; then
    extra_app_args="${extra_app_args} --test-p1-character '${test_p1_character}'"
fi

if [ -n "$test_p2_character" ]; then
    extra_app_args="${extra_app_args} --test-p2-character '${test_p2_character}'"
fi

if [ -n "$test_p1_super_art" ]; then
    extra_app_args="${extra_app_args} --test-p1-super-art '${test_p1_super_art}'"
fi

if [ -n "$test_p2_super_art" ]; then
    extra_app_args="${extra_app_args} --test-p2-super-art '${test_p2_super_art}'"
fi

if [ "$test_p1_super_full" -eq 1 ]; then
    extra_app_args="${extra_app_args} --test-p1-super-full"
fi

if [ "$test_preserve_game_transition" -eq 1 ]; then
    extra_app_args="${extra_app_args} --test-preserve-game-transition"
fi

if [ "$test_delay_gameplay_inputs_until_active" -eq 1 ]; then
    extra_app_args="${extra_app_args} --test-delay-gameplay-inputs-until-active"
fi

if [ -n "$test_stage" ]; then
    extra_app_args="${extra_app_args} --test-stage '${test_stage}'"
fi

mkdir -p "$local_perf_dir"
mister_lock_acquire
trap 'cleanup_local $?' EXIT
trap 'cleanup_local 130' INT
trap 'cleanup_local 129' HUP
trap 'cleanup_local 143' TERM

if [ -n "$copy_afs_path" ]; then
    if [ ! -f "$copy_afs_path" ]; then
        echo "error: --copy-afs path not found: $copy_afs_path" >&2
        exit 2
    fi

    mister_ssh_exec "$host" "$user" "$password" "
set -e
mkdir -p '${remote_root}/resources'
rm -f '${remote_afs_backup_path}' '${remote_afs_missing_marker}'
if [ -f '${remote_resources_afs}' ]; then
  cp '${remote_resources_afs}' '${remote_afs_backup_path}'
else
  : >'${remote_afs_missing_marker}'
fi
"
    remote_afs_restore_needed=1
    mister_scp_upload "$copy_afs_path" "$host" "$user" "$password" "$remote_resources_afs"
fi

remote_run_cmd=$(cat <<EOF
set -e
remote_config_backup='/tmp/3sx-config-${tag}.bak'
remote_config_missing_marker='/tmp/3sx-config-${tag}.missing'
cleanup() {
  status=\$?
  if [ -f "\$remote_config_backup" ]; then
    cp "\$remote_config_backup" '${remote_config_path}'
    rm -f "\$remote_config_backup"
  elif [ -f "\$remote_config_missing_marker" ]; then
    rm -f '${remote_config_path}' "\$remote_config_missing_marker"
  fi
  if [ "\$status" -ne 0 ]; then
    rm -f '${remote_output_path}' '${remote_log_path}'
  fi
  exit "\$status"
}
trap cleanup EXIT
if [ ! -f '${remote_resources_afs}' ]; then
  echo 'Missing required resource file at ${remote_resources_afs}' >&2
  exit 20
fi
rm -f '${remote_output_path}' '${remote_log_path}'
rm -f "\$remote_config_backup" "\$remote_config_missing_marker"
if [ -f '${remote_config_path}' ]; then
  cp '${remote_config_path}' "\$remote_config_backup"
  cp "\$remote_config_backup" '${remote_config_path}.tmp'
else
  : >"\$remote_config_missing_marker"
  : >'${remote_config_path}.tmp'
fi
if [ -n '${scale_mode}' ]; then
  grep -v '^[[:space:]]*scale-mode[[:space:]]*=' '${remote_config_path}.tmp' >'${remote_config_path}.tmp.scale' || true
  mv '${remote_config_path}.tmp.scale' '${remote_config_path}.tmp'
  printf '%s\n' 'scale-mode = ${scale_mode}' >>'${remote_config_path}.tmp'
fi
if [ -n '${software_frame_mode}' ]; then
  grep -v '^[[:space:]]*software-frame-mode[[:space:]]*=' '${remote_config_path}.tmp' >'${remote_config_path}.tmp.mode' || true
  mv '${remote_config_path}.tmp.mode' '${remote_config_path}.tmp'
  printf '%s\n' 'software-frame-mode = ${software_frame_mode}' >>'${remote_config_path}.tmp'
fi
grep -v '^[[:space:]]*show-fps[[:space:]]*=' '${remote_config_path}.tmp' >'${remote_config_path}.tmp.showfps' || true
mv '${remote_config_path}.tmp.showfps' '${remote_config_path}.tmp'
printf '%s\n' 'show-fps = false' >>'${remote_config_path}.tmp'
mv '${remote_config_path}.tmp' '${remote_config_path}'
SDL_VIDEODRIVER=dummy SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy \
  '${remote_root}/scripts/run-3sx.sh' --perf-capture '${frames}' --scene '${scene}' --perf-output '${remote_output_path}' \
  ${extra_app_args} \
  >'${remote_log_path}' 2>&1
EOF
)

echo "Running perf sample on ${user}@${host} (scene=${scene}, frames=${frames}, tag=${tag})"

remote_stdout_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-stdout.XXXXXX")"
mister_ssh_exec "$host" "$user" "$password" "$remote_run_cmd" >"${remote_stdout_path}"
if [ ! -f "${remote_stdout_path}" ]; then
    echo "error: remote perf capture did not produce a host-side command log" >&2
    cat "${remote_stdout_path}" >&2
    exit 1
fi

local_downloaded_output_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-json.XXXXXX")"
local_remote_log_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-log.XXXXXX")"

mister_scp_download "$host" "$user" "$password" "${remote_output_path}" "${local_downloaded_output_path}" || {
    echo "error: failed to download remote perf JSON from ${remote_output_path}" >&2
    cat "${remote_stdout_path}" >&2
    exit 1
}

mister_scp_download "$host" "$user" "$password" "${remote_log_path}" "${local_remote_log_path}" || {
    echo "error: failed to download remote perf log from ${remote_log_path}" >&2
    cat "${remote_stdout_path}" >&2
    exit 1
}

mv "${local_downloaded_output_path}" "${local_output_path}"
local_downloaded_output_path=""

mister_ssh_exec "$host" "$user" "$password" "rm -f '${remote_output_path}' '${remote_log_path}'" >/dev/null || \
    echo "warning: failed to remove remote perf artifacts ${remote_output_path} and ${remote_log_path}" >&2

tail -n 40 "${local_remote_log_path}" || true

capture_log_line="$(grep 'PERF capture start:' "${local_remote_log_path}" | tail -n 1 || true)"
mode_log_line="$(grep 'PERF capture enabled:' "${local_remote_log_path}" | tail -n 1 || true)"
captured_stage_id=""
captured_test_stage_override=""
captured_p1_character=""
captured_p2_character=""
captured_p1_super_art=""
captured_p2_super_art=""
captured_scale_mode=""
captured_software_frame_mode=""
captured_test_phase=""
captured_wait_test_phase=""
captured_wait_runtime_state=""
if [ -n "$capture_log_line" ]; then
    captured_stage_id="$(extract_perf_log_field "stage_id" "$capture_log_line")"
    captured_test_stage_override="$(extract_perf_log_field "test_stage_override" "$capture_log_line")"
    captured_p1_character="$(extract_perf_log_field "p1_character" "$capture_log_line")"
    captured_p2_character="$(extract_perf_log_field "p2_character" "$capture_log_line")"
    captured_p1_super_art="$(extract_perf_log_field "p1_super_art" "$capture_log_line")"
    captured_p2_super_art="$(extract_perf_log_field "p2_super_art" "$capture_log_line")"
    captured_test_phase="$(extract_perf_log_string_field "test_phase" "$capture_log_line")"
    captured_wait_test_phase="$(extract_perf_log_string_field "wait_test_phase" "$capture_log_line")"
    captured_wait_runtime_state="$(extract_perf_log_string_field "wait_runtime_state" "$capture_log_line")"
fi
if [ -n "$mode_log_line" ]; then
    captured_scale_mode="$(extract_perf_log_string_field "scale_mode" "$mode_log_line")"
    captured_software_frame_mode="$(extract_perf_log_string_field "software_frame_mode" "$mode_log_line")"
fi

if command -v jq >/dev/null 2>&1; then
    metadata_stage_id="null"
    metadata_test_stage_override="null"
    metadata_p1_character="null"
    metadata_p2_character="null"
    metadata_p1_super_art="null"
    metadata_p2_super_art="null"
    metadata_scale_mode="$captured_scale_mode"
    metadata_software_frame_mode="$captured_software_frame_mode"
    metadata_capture_start_test_phase="$captured_test_phase"
    metadata_perf_wait_test_phase="$captured_wait_test_phase"
    metadata_perf_wait_runtime_state="$captured_wait_runtime_state"
    if [ -n "$captured_stage_id" ] && [ "$captured_stage_id" -ge 0 ]; then
        metadata_stage_id="$captured_stage_id"
    fi
    if [ -n "$captured_test_stage_override" ] && [ "$captured_test_stage_override" -ge 0 ]; then
        metadata_test_stage_override="$captured_test_stage_override"
    elif [ -n "$test_stage" ]; then
        metadata_test_stage_override="$test_stage"
    fi
    if [ -n "$captured_p1_character" ] && [ "$captured_p1_character" -ge 0 ]; then
        metadata_p1_character="$captured_p1_character"
    elif [ -n "$test_p1_character" ]; then
        metadata_p1_character="$test_p1_character"
    fi
    if [ -n "$captured_p2_character" ] && [ "$captured_p2_character" -ge 0 ]; then
        metadata_p2_character="$captured_p2_character"
    elif [ -n "$test_p2_character" ]; then
        metadata_p2_character="$test_p2_character"
    fi
    if [ -n "$captured_p1_super_art" ] && [ "$captured_p1_super_art" -ge 0 ]; then
        metadata_p1_super_art="$captured_p1_super_art"
    elif [ -n "$test_p1_super_art" ]; then
        metadata_p1_super_art="$test_p1_super_art"
    fi
    if [ -n "$captured_p2_super_art" ] && [ "$captured_p2_super_art" -ge 0 ]; then
        metadata_p2_super_art="$captured_p2_super_art"
    elif [ -n "$test_p2_super_art" ]; then
        metadata_p2_super_art="$test_p2_super_art"
    fi
    if [ -z "$metadata_scale_mode" ] && [ -n "$scale_mode" ]; then
        metadata_scale_mode="$scale_mode"
    fi
    if [ -n "$software_frame_mode" ]; then
        metadata_software_frame_mode="$software_frame_mode"
    fi
    if [ -z "$metadata_perf_wait_test_phase" ] && [ -n "$perf_wait_test_phase" ]; then
        metadata_perf_wait_test_phase="$perf_wait_test_phase"
    fi
    if [ -z "$metadata_perf_wait_runtime_state" ] && [ -n "$perf_wait_runtime_state" ]; then
        metadata_perf_wait_runtime_state="$perf_wait_runtime_state"
    fi
    if [ "$metadata_perf_wait_test_phase" = "game-input-active" ] &&
        { [ -z "$metadata_capture_start_test_phase" ] || [ "$metadata_capture_start_test_phase" = "game" ]; }; then
        metadata_capture_start_test_phase="game-input-active"
    fi

    temp_output_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-json.XXXXXX")"
    jq \
        --arg scene "$scene" \
        --arg test_scene_preset "$test_scene_preset" \
        --arg scale_mode "$metadata_scale_mode" \
        --arg software_frame_mode "$metadata_software_frame_mode" \
        --arg capture_start_test_phase "$metadata_capture_start_test_phase" \
        --arg perf_wait_test_phase "$metadata_perf_wait_test_phase" \
        --arg perf_wait_runtime_state "$metadata_perf_wait_runtime_state" \
        --argjson stage_id "$metadata_stage_id" \
        --argjson test_stage_override "$metadata_test_stage_override" \
        --argjson p1_character "$metadata_p1_character" \
        --argjson p2_character "$metadata_p2_character" \
        --argjson p1_super_art "$metadata_p1_super_art" \
        --argjson p2_super_art "$metadata_p2_super_art" \
        --argjson p1_super_full "$(if [ "$test_p1_super_full" -eq 1 ]; then printf 'true'; else printf 'false'; fi)" \
        --argjson preserve_game_transition "$(if [ "$test_preserve_game_transition" -eq 1 ]; then printf 'true'; else printf 'false'; fi)" \
        --argjson delay_gameplay_inputs_until_active "$(if [ "$test_delay_gameplay_inputs_until_active" -eq 1 ]; then printf 'true'; else printf 'false'; fi)" \
        '.metadata = ((.metadata // {}) + {scene: $scene, test_scene_preset: (if ($test_scene_preset | length) > 0 then $test_scene_preset else null end), scale_mode: (if ($scale_mode | length) > 0 then $scale_mode else null end), software_frame_mode: (if ($software_frame_mode | length) > 0 then $software_frame_mode else null end), capture_start_test_phase: (if ($capture_start_test_phase | length) > 0 and $capture_start_test_phase != "(none)" then $capture_start_test_phase else null end), perf_wait_test_phase: (if ($perf_wait_test_phase | length) > 0 and $perf_wait_test_phase != "(none)" then $perf_wait_test_phase else null end), perf_wait_runtime_state: (if ($perf_wait_runtime_state | length) > 0 and $perf_wait_runtime_state != "(none)" then $perf_wait_runtime_state else null end), stage_id: $stage_id, test_stage_override: $test_stage_override, p1_character: $p1_character, p2_character: $p2_character, p1_super_art: $p1_super_art, p2_super_art: $p2_super_art, test_p1_super_full: $p1_super_full, test_preserve_game_transition: $preserve_game_transition, test_delay_gameplay_inputs_until_active: $delay_gameplay_inputs_until_active})' \
        "${local_output_path}" >"${temp_output_path}"
    mv "${temp_output_path}" "${local_output_path}"
fi

echo "Saved perf JSON to ${local_output_path}"

if command -v jq >/dev/null 2>&1; then
    fps_mean="$(jq -r '.metrics.fps.mean // empty' "${local_output_path}")"
    frame_mean_ms="$(jq -r '.metrics.frame_time.mean_ms // empty' "${local_output_path}")"
    frame_max_ms="$(jq -r '.metrics.frame_time.max_ms // empty' "${local_output_path}")"
    update_mean_ms="$(jq -r '.metrics.update.mean_ms // empty' "${local_output_path}")"
    render_mean_ms="$(jq -r '.metrics.render.mean_ms // empty' "${local_output_path}")"
    present_mean_ms="$(jq -r '.metrics.present.mean_ms // empty' "${local_output_path}")"
    dominant_present_path="$(jq -r '
        (.metrics.fbdev_present_path // {})
        | to_entries
        | map(select((.value.ratio // null) != null))
        | if length == 0 then empty else max_by(.value.ratio).key end
    ' "${local_output_path}")"
    metadata_stage_id="$(jq -r '.metadata.stage_id // empty' "${local_output_path}")"
    metadata_preset="$(jq -r '.metadata.test_scene_preset // empty' "${local_output_path}")"
    metadata_scale_mode="$(jq -r '.metadata.scale_mode // empty' "${local_output_path}")"
    metadata_software_frame_mode="$(jq -r '.metadata.software_frame_mode // empty' "${local_output_path}")"
    metadata_test_phase="$(jq -r '.metadata.capture_start_test_phase // empty' "${local_output_path}")"
    metadata_runtime_state="$(jq -r '.metadata.perf_wait_runtime_state // empty' "${local_output_path}")"

    printf 'Perf summary: tag=%s scene=%s fps=%s frame_mean_ms=%s frame_max_ms=%s update_mean_ms=%s render_mean_ms=%s present_mean_ms=%s dominant_present_path=%s stage_id=%s preset=%s scale_mode=%s software_frame_mode=%s test_phase=%s runtime_state=%s\n' \
        "$tag" "$scene" "${fps_mean:-unknown}" "${frame_mean_ms:-unknown}" "${frame_max_ms:-unknown}" \
        "${update_mean_ms:-unknown}" "${render_mean_ms:-unknown}" "${present_mean_ms:-unknown}" \
        "${dominant_present_path:-unknown}" "${metadata_stage_id:-unknown}" "${metadata_preset:-none}" \
        "${metadata_scale_mode:-unknown}" "${metadata_software_frame_mode:-unknown}" \
        "${metadata_test_phase:-unknown}" "${metadata_runtime_state:-none}"

    software_frame_modulation_summary="$(jq -r '
        if (.metrics.software_frame_fast_non_integer_alpha_only_pixels.mean // null) == null then
            empty
        else
            "Software-frame modulation: " +
            "fast_non_integer_alpha_only_tasks=" + ((.metrics.software_frame_fast_non_integer_alpha_only_tasks.mean // 0) | tostring) + " " +
            "fast_non_integer_alpha_only_pixels=" + ((.metrics.software_frame_fast_non_integer_alpha_only_pixels.mean // 0) | tostring) + " " +
            "fast_non_integer_rgb_mod_tasks=" + ((.metrics.software_frame_fast_non_integer_rgb_mod_tasks.mean // 0) | tostring) + " " +
            "fast_non_integer_rgb_mod_pixels=" + ((.metrics.software_frame_fast_non_integer_rgb_mod_pixels.mean // 0) | tostring) + " " +
            "generic_textured_alpha_only_tasks=" + ((.metrics.software_frame_generic_textured_alpha_only_tasks.mean // 0) | tostring) + " " +
            "generic_textured_alpha_only_pixels=" + ((.metrics.software_frame_generic_textured_alpha_only_pixels.mean // 0) | tostring) + " " +
            "generic_textured_rgb_mod_tasks=" + ((.metrics.software_frame_generic_textured_rgb_mod_tasks.mean // 0) | tostring) + " " +
            "generic_textured_rgb_mod_pixels=" + ((.metrics.software_frame_generic_textured_rgb_mod_pixels.mean // 0) | tostring)
        end
    ' "${local_output_path}")"
    if [ -n "$software_frame_modulation_summary" ]; then
        printf '%s\n' "$software_frame_modulation_summary"
    fi

    textured_geometry_fallback_summary="$(jq -r '
        (.metrics.software_frame_textured_geometry_fallback_families // [])
        | .[:3]
        | map(
            "Geometry fallback family: " +
            "kind=\(.family_kind) " +
            "texture_handle=\(.texture_handle) " +
            "palette_handle=\(.palette_handle) " +
            "logical=\(.logical_source_kind):\(.logical_ix_num) " +
            "tasks_total=\(.task_count_total) " +
            "task_ratio=\(.task_ratio) " +
            "pixels_total=\(.submitted_pixels_total) " +
            "pixel_ratio=\(.submitted_pixel_ratio) " +
            "source=\(.source_width)x\(.source_height) " +
            "src_rect_x=\(.source_rect_x_min)-\(.source_rect_x_max) " +
            "src_rect_y=\(.source_rect_y_min)-\(.source_rect_y_max) " +
            "src_rect_w=\(.source_rect_w_min)-\(.source_rect_w_max) " +
            "src_rect_h=\(.source_rect_h_min)-\(.source_rect_h_max) " +
            "dst_height=\(.dst_height_min)-\(.dst_height_max) " +
            "dst_top_width=\(.dst_top_width_min)-\(.dst_top_width_max) " +
            "dst_bottom_width=\(.dst_bottom_width_min)-\(.dst_bottom_width_max) " +
            "dst_left_dx=\(.dst_left_dx_min)-\(.dst_left_dx_max) " +
            "dst_right_dx=\(.dst_right_dx_min)-\(.dst_right_dx_max)"
          )
        | join("\n")
    ' "${local_output_path}")"
    if [ -n "$textured_geometry_fallback_summary" ]; then
        printf '%s\n' "$textured_geometry_fallback_summary"
    fi

    stock_state_summary="$(jq -r '
        if (.test_state // null) == null then
            empty
        else
            "Stock state: p1_stock_frames=\(.test_state.p1_super_art_stock_available_frames_total // 0) " +
            "p1_stock_first=\((.test_state.p1_super_art_stock_available_first_frame // "none") | tostring) " +
            "p1_stock_max=\(.test_state.p1_super_art_max_store // 0)/\(.test_state.p1_super_art_store_max // 0) " +
            "p1_gauge_max=\(.test_state.p1_super_art_gauge_max_value // 0)/\(.test_state.p1_super_art_gauge_max_capacity // 0) " +
            "p2_stock_frames=\(.test_state.p2_super_art_stock_available_frames_total // 0) " +
            "p2_stock_first=\((.test_state.p2_super_art_stock_available_first_frame // "none") | tostring) " +
            "p2_stock_max=\(.test_state.p2_super_art_max_store // 0)/\(.test_state.p2_super_art_store_max // 0) " +
            "p2_gauge_max=\(.test_state.p2_super_art_gauge_max_value // 0)/\(.test_state.p2_super_art_gauge_max_capacity // 0)"
        end
    ' "${local_output_path}")"
    if [ -n "$stock_state_summary" ]; then
        printf '%s\n' "$stock_state_summary"
    fi

    ready_state_summary="$(jq -r '
        if (.test_state // null) == null then
            empty
        else
            "Ready state: p1_ready_frames=\(.test_state.p1_super_art_ready_frames_total // 0) " +
            "p1_ready_first=\((.test_state.p1_super_art_ready_first_frame // "none") | tostring) " +
            "p1_ready_r1_hist=\((.test_state.p1_super_art_ready_routine1_frames // []) | map(tostring) | join("/")) " +
            "p1_ready_rno_first=\(if (.test_state.p1_super_art_ready_first_routine // null) == null then "none" else (.test_state.p1_super_art_ready_first_routine | map(tostring) | join("/")) end) " +
            "p1_ready_rno_last=\(if (.test_state.p1_super_art_ready_last_routine // null) == null then "none" else (.test_state.p1_super_art_ready_last_routine | map(tostring) | join("/")) end) " +
            "p2_ready_frames=\(.test_state.p2_super_art_ready_frames_total // 0) " +
            "p2_ready_first=\((.test_state.p2_super_art_ready_first_frame // "none") | tostring) " +
            "p2_ready_r1_hist=\((.test_state.p2_super_art_ready_routine1_frames // []) | map(tostring) | join("/")) " +
            "p2_ready_rno_first=\(if (.test_state.p2_super_art_ready_first_routine // null) == null then "none" else (.test_state.p2_super_art_ready_first_routine | map(tostring) | join("/")) end) " +
            "p2_ready_rno_last=\(if (.test_state.p2_super_art_ready_last_routine // null) == null then "none" else (.test_state.p2_super_art_ready_last_routine | map(tostring) | join("/")) end)"
        end
    ' "${local_output_path}")"
    if [ -n "$ready_state_summary" ]; then
        printf '%s\n' "$ready_state_summary"
    fi

    super_reachability_summary="$(jq -r '
        if (.test_state // null) == null then
            empty
        else
            "Super reachability: " +
            "p1_entries=\(.test_state.p1_super_art_entry_calls_total // 0) " +
            "p1_cmd_sel_entries=\(.test_state.p1_super_art_entry_cmd_sel_calls_total // 0) " +
            "p1_cmd_sel_not_ready=\(.test_state.p1_super_art_entry_cmd_sel_not_ready_total // 0) " +
            "p1_direct_entries=\(.test_state.p1_super_art_entry_direct_calls_total // 0) " +
            "p2_entries=\(.test_state.p2_super_art_entry_calls_total // 0) " +
            "p2_cmd_sel_entries=\(.test_state.p2_super_art_entry_cmd_sel_calls_total // 0) " +
            "p2_cmd_sel_not_ready=\(.test_state.p2_super_art_entry_cmd_sel_not_ready_total // 0) " +
            "p2_direct_entries=\(.test_state.p2_super_art_entry_direct_calls_total // 0)"
        end
    ' "${local_output_path}")"
    if [ -n "$super_reachability_summary" ]; then
        printf '%s\n' "$super_reachability_summary"
    fi

    test_state_summary="$(jq -r '
        if (.test_state // null) == null then
            empty
        else
            "Test state: p1_super_active_frames=\(.test_state.p1_super_art_active_frames_total // 0) " +
            "p1_super_first=\((.test_state.p1_super_art_active_first_frame // "none") | tostring) " +
            "p1_metamorphose_frames=\(.test_state.p1_metamorphose_frames_total // 0) " +
            "p1_metamorphose_first=\((.test_state.p1_metamorphose_first_frame // "none") | tostring) " +
            "p2_super_active_frames=\(.test_state.p2_super_art_active_frames_total // 0) " +
            "p2_super_first=\((.test_state.p2_super_art_active_first_frame // "none") | tostring) " +
            "p2_metamorphose_frames=\(.test_state.p2_metamorphose_frames_total // 0) " +
            "p2_metamorphose_first=\((.test_state.p2_metamorphose_first_frame // "none") | tostring)"
        end
    ' "${local_output_path}")"
    if [ -n "$test_state_summary" ]; then
        printf '%s\n' "$test_state_summary"
    fi

    super_command_summary="$(jq -r '
        if (.test_state // null) == null then
            empty
        else
            "Super command: " +
            "p1_checks=\(.test_state.p1_super_art_command_check_calls_total // 0) " +
            "p1_ready_checks=\(.test_state.p1_super_art_command_ready_checks_total // 0) " +
            "p1_pcon_blocks=\(.test_state.p1_super_art_command_blocked_pcon_dp_total // 0) " +
            "p1_ground_candidates=\(.test_state.p1_super_art_command_ground_candidate_checks_total // 0) " +
            "p1_ground_preblocked=\(.test_state.p1_super_art_command_ground_precondition_blocked_total // 0) " +
            "p1_ground_no_match=\(.test_state.p1_super_art_command_ground_no_match_total // 0) " +
            "p1_air_candidates=\(.test_state.p1_super_art_command_air_candidate_checks_total // 0) " +
            "p1_air_preblocked=\(.test_state.p1_super_art_command_air_precondition_blocked_total // 0) " +
            "p1_air_no_match=\(.test_state.p1_super_art_command_air_no_match_total // 0) " +
            "p1_matches=\(.test_state.p1_super_art_command_matches_total // 0) " +
            "p2_checks=\(.test_state.p2_super_art_command_check_calls_total // 0) " +
            "p2_ready_checks=\(.test_state.p2_super_art_command_ready_checks_total // 0) " +
            "p2_pcon_blocks=\(.test_state.p2_super_art_command_blocked_pcon_dp_total // 0) " +
            "p2_ground_candidates=\(.test_state.p2_super_art_command_ground_candidate_checks_total // 0) " +
            "p2_ground_preblocked=\(.test_state.p2_super_art_command_ground_precondition_blocked_total // 0) " +
            "p2_ground_no_match=\(.test_state.p2_super_art_command_ground_no_match_total // 0) " +
            "p2_air_candidates=\(.test_state.p2_super_art_command_air_candidate_checks_total // 0) " +
            "p2_air_preblocked=\(.test_state.p2_super_art_command_air_precondition_blocked_total // 0) " +
            "p2_air_no_match=\(.test_state.p2_super_art_command_air_no_match_total // 0) " +
            "p2_matches=\(.test_state.p2_super_art_command_matches_total // 0)"
        end
    ' "${local_output_path}")"
    if [ -n "$super_command_summary" ]; then
        printf '%s\n' "$super_command_summary"
    fi

    transition_state_summary="$(jq -r '
        if (.transition_state // null) == null then
            empty
        else
            "Transition state: start_g_no=\((.transition_state.capture_start_g_no // []) | map(tostring) | join("/")) " +
            "start_e_no=\((.transition_state.capture_start_e_no // []) | map(tostring) | join("/")) " +
            "menu_task=\(.transition_state.capture_start_menu_task_condition // 0):" +
            "\((.transition_state.capture_start_menu_task_r_no // []) | map(tostring) | join("/")) " +
            "start_break_into=\(.transition_state.capture_start_break_into // 0) " +
            "start_hnc=\(.transition_state.capture_start_hnc_num // 0) " +
            "start_exec_wipe=\(.transition_state.capture_start_exec_wipe // 0) " +
            "start_wipe_type=\(.transition_state.capture_start_active_wipe_type // -1) " +
            "start_wipe_limit=\(.transition_state.capture_start_wipe_limit // 0) " +
            "break_into_frames=\(.transition_state.break_into_frames_total // 0) " +
            "break_into_first=\((.transition_state.break_into_first_frame // "none") | tostring) " +
            "hnc_frames=\(.transition_state.hnc_active_frames_total // 0) " +
            "hnc_first=\((.transition_state.hnc_active_first_frame // "none") | tostring) " +
            "hnc_max=\(.transition_state.hnc_max_num // 0) " +
            "wipe_type1_frames=\(.transition_state.wipe_type1_active_frames_total // 0) " +
            "wipe_type1_first=\((.transition_state.wipe_type1_active_first_frame // "none") | tostring) " +
            "wipe_type1_max_limit=\(.transition_state.wipe_type1_max_limit // 0)"
        end
    ' "${local_output_path}")"
    if [ -n "$transition_state_summary" ]; then
        printf '%s\n' "$transition_state_summary"
    fi

    title_state_summary="$(jq -r '
        if (.title_state // null) == null then
            empty
        else
            "Title state: start_d_no=\((.title_state.capture_start_d_no // []) | map(tostring) | join("/")) " +
            "start_title_tex=\(.title_state.capture_start_title_tex_flag // 0) " +
            "start_opening_r_no=\(.title_state.capture_start_opening_r_no_0 // 0)/" +
            "\(.title_state.capture_start_opening_r_no_1 // 0)/" +
            "\(.title_state.capture_start_opening_r_no_2 // 0) " +
            "start_opening_free_work=\(.title_state.capture_start_opening_free_work // 0) " +
            "title_logo_frames=\(.title_state.title_logo_active_frames_total // 0) " +
            "title_logo_first=\((.title_state.title_logo_active_first_frame // "none") | tostring)"
        end
    ' "${local_output_path}")"
    if [ -n "$title_state_summary" ]; then
        printf '%s\n' "$title_state_summary"
    fi

    attract_demo_logo_summary="$(jq -r '
        if (.attract_demo_logo_state // null) == null then
            empty
        else
            "Attract demo logo: " +
            "start_demo_flag=\(.attract_demo_logo_state.capture_start_demo_flag // 0) " +
            "start_effect_index=\((.attract_demo_logo_state.capture_start_effect_index // "none") | tostring) " +
            "start_routine2=\((.attract_demo_logo_state.capture_start_routine2 // "none") | tostring) " +
            "start_direction=\((.attract_demo_logo_state.capture_start_direction // "none") | tostring) " +
            "start_dir_timer=\((.attract_demo_logo_state.capture_start_dir_timer // "none") | tostring) " +
            "active_frames=\(.attract_demo_logo_state.active_frames_total // 0) " +
            "active_first=\((.attract_demo_logo_state.active_first_frame // "none") | tostring) " +
            "max_direction=\((.attract_demo_logo_state.max_direction // "none") | tostring)"
        end
    ' "${local_output_path}")"
    if [ -n "$attract_demo_logo_summary" ]; then
        printf '%s\n' "$attract_demo_logo_summary"
    fi
fi
