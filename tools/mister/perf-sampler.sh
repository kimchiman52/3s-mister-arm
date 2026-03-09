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
  --tag <name>           Output tag; writes artifacts/mister-port/perf/<tag>.json.
  --gameplay-idle        Use the built-in scripted idle-versus path and wait for gameplay before capture.
  --gameplay-warmup <n>  Warmup frames to skip after gameplay or the selected wait condition becomes active
                         (default: 120 for gameplay waits, 0 for test-phase waits).
  --perf-basic          Capture low-overhead frame/update/render/present timings; lightweight
                        test-state metadata may still be exported when available.
  --perf-wait-test-phase <name>
                         Delay capture until the test runner reaches the named phase
                         (title, menu, character-select-transition, character-select, game-transition, game, game-input-active, wipe-transition-type1).
  --test-scene-preset <name>
                         Named scripted gameplay preset (stage-heavy, effect-heavy, super-heavy).
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
  --software-frame-mode <off|on>
                         Optional config override for `software-frame-mode` during capture.
                         Perf captures always force `show-fps = false` temporarily so the
                         player-facing HUD does not skew optimization measurements.
  --host <ip-or-host>    MiSTer host (default: $MISTER_HOST or 192.168.1.171).
  --user <name>          SSH user (default: $MISTER_USER or root).
  --password <value>     SSH password (default: $MISTER_PASSWORD).
  --remote-root <path>   Remote 3SX root (default: $MISTER_ROOT or /media/fat/games/3sx).
  --copy-afs <path>      Optional local SF33RD.AFS path to stage before capture.
  --help                 Show this message.

Environment:
  MISTER_HOST, MISTER_USER, MISTER_PASSWORD, MISTER_ROOT
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
    [ "$value" = "stage-heavy" ] || [ "$value" = "effect-heavy" ] || [ "$value" = "super-heavy" ]
}

is_supported_software_frame_mode() {
    local value="$1"
    [ "$value" = "off" ] || [ "$value" = "on" ]
}

is_supported_perf_wait_test_phase() {
    local value="$1"
    [ "$value" = "title" ] || [ "$value" = "menu" ] ||
        [ "$value" = "character-select-transition" ] || [ "$value" = "character-select" ] ||
        [ "$value" = "game-transition" ] || [ "$value" = "game" ] || [ "$value" = "game-input-active" ] ||
        [ "$value" = "wipe-transition-type1" ]
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

extract_marker_block() {
    local begin_marker="$1"
    local end_marker="$2"
    local src_path="$3"
    local dst_path="$4"

    awk -v begin="$begin_marker" -v end="$end_marker" '
        {
            line = $0
            sub(/\r$/, "", line)
        }
        line == begin { capture=1; next }
        line == end { capture=0; exit }
        capture { print line }
    ' "$src_path" >"$dst_path"
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
test_scene_preset=""
test_p1_character=""
test_p2_character=""
test_p1_super_art=""
test_p2_super_art=""
test_p1_super_full=0
test_preserve_game_transition=0
test_delay_gameplay_inputs_until_active=0
test_stage=""
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

if [ -n "$test_scene_preset" ] && ! is_supported_test_scene_preset "$test_scene_preset"; then
    echo "error: --test-scene-preset must be one of stage-heavy, effect-heavy, or super-heavy." >&2
    exit 2
fi

if [ "$test_preserve_game_transition" -eq 1 ] &&
    [ "$perf_wait_test_phase" != "game-transition" ] &&
    [ "$perf_wait_test_phase" != "wipe-transition-type1" ]; then
    echo "error: --test-preserve-game-transition requires --perf-wait-test-phase game-transition or wipe-transition-type1." >&2
    exit 2
fi

if [ "$gameplay_idle" -eq 1 ] && [ -n "$perf_wait_test_phase" ]; then
    echo "error: --gameplay-idle cannot be combined with --perf-wait-test-phase." >&2
    exit 2
fi

if [ -n "$perf_wait_test_phase" ] && [ "$gameplay_warmup_explicit" -eq 0 ]; then
    gameplay_warmup=0
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

if [ -n "$password" ]; then
    require_cmd expect
fi

local_perf_dir="artifacts/mister-port/perf"
local_output_path="${local_perf_dir}/${tag}.json"
remote_output_path="${remote_root}/logs/perf-${tag}.json"
remote_log_path="/tmp/3sx-perf-${tag}.log"
remote_config_path="${remote_root}/config"
remote_resources_afs="${remote_root}/resources/SF33RD.AFS"
extra_app_args=""

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

if [ -n "$copy_afs_path" ]; then
    if [ ! -f "$copy_afs_path" ]; then
        echo "error: --copy-afs path not found: $copy_afs_path" >&2
        exit 2
    fi

    mister_ssh_exec "$host" "$user" "$password" "mkdir -p '${remote_root}/resources'"
    mister_scp_upload "$copy_afs_path" "$host" "$user" "$password" "$remote_resources_afs"
fi

json_begin_marker="__3SX_JSON_BEGIN__"
json_end_marker="__3SX_JSON_END__"
log_begin_marker="__3SX_LOG_BEGIN__"
log_end_marker="__3SX_LOG_END__"

remote_run_cmd=$(cat <<EOF
set -e
remote_config_backup='/tmp/3sx-config-${tag}.bak'
remote_config_missing_marker='/tmp/3sx-config-${tag}.missing'
cleanup() {
  if [ -f "\$remote_config_backup" ]; then
    cp "\$remote_config_backup" '${remote_config_path}'
    rm -f "\$remote_config_backup"
  elif [ -f "\$remote_config_missing_marker" ]; then
    rm -f '${remote_config_path}' "\$remote_config_missing_marker"
  fi
  rm -f '${remote_output_path}' '${remote_log_path}'
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
  '${remote_root}/run-3sx.sh' --perf-capture '${frames}' --scene '${scene}' --perf-output '${remote_output_path}' \
  ${extra_app_args} \
  >'${remote_log_path}' 2>&1
printf '%s\n' '${json_begin_marker}'
base64 '${remote_output_path}'
printf '%s\n' '${json_end_marker}'
printf '%s\n' '${log_begin_marker}'
base64 '${remote_log_path}'
printf '%s\n' '${log_end_marker}'
EOF
)

echo "Running perf sample on ${user}@${host} (scene=${scene}, frames=${frames}, tag=${tag})"

remote_stdout_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-stdout.XXXXXX")"
mister_ssh_exec "$host" "$user" "$password" "$remote_run_cmd" >"${remote_stdout_path}"

remote_json_b64_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-json-b64.XXXXXX")"
remote_log_b64_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-log-b64.XXXXXX")"
extract_marker_block "${json_begin_marker}" "${json_end_marker}" "${remote_stdout_path}" "${remote_json_b64_path}"
extract_marker_block "${log_begin_marker}" "${log_end_marker}" "${remote_stdout_path}" "${remote_log_b64_path}"

if [ ! -s "${remote_json_b64_path}" ] || [ ! -s "${remote_log_b64_path}" ]; then
    echo "error: remote perf capture did not return a structured payload" >&2
    cat "${remote_stdout_path}" >&2
    exit 1
fi

mister_base64_decode_file "${remote_json_b64_path}" "${local_output_path}"
local_remote_log_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-log.XXXXXX")"
mister_base64_decode_file "${remote_log_b64_path}" "${local_remote_log_path}"
tail -n 40 "${local_remote_log_path}" || true

capture_log_line="$(grep 'PERF capture start:' "${local_remote_log_path}" | tail -n 1 || true)"
mode_log_line="$(grep 'PERF capture enabled:' "${local_remote_log_path}" | tail -n 1 || true)"
captured_stage_id=""
captured_test_stage_override=""
captured_p1_character=""
captured_p2_character=""
captured_p1_super_art=""
captured_p2_super_art=""
captured_software_frame_mode=""
captured_test_phase=""
captured_wait_test_phase=""
if [ -n "$capture_log_line" ]; then
    captured_stage_id="$(extract_perf_log_field "stage_id" "$capture_log_line")"
    captured_test_stage_override="$(extract_perf_log_field "test_stage_override" "$capture_log_line")"
    captured_p1_character="$(extract_perf_log_field "p1_character" "$capture_log_line")"
    captured_p2_character="$(extract_perf_log_field "p2_character" "$capture_log_line")"
    captured_p1_super_art="$(extract_perf_log_field "p1_super_art" "$capture_log_line")"
    captured_p2_super_art="$(extract_perf_log_field "p2_super_art" "$capture_log_line")"
    captured_test_phase="$(extract_perf_log_string_field "test_phase" "$capture_log_line")"
    captured_wait_test_phase="$(extract_perf_log_string_field "wait_test_phase" "$capture_log_line")"
fi
if [ -n "$mode_log_line" ]; then
    captured_software_frame_mode="$(extract_perf_log_string_field "software_frame_mode" "$mode_log_line")"
fi

if command -v jq >/dev/null 2>&1; then
    metadata_stage_id="null"
    metadata_test_stage_override="null"
    metadata_p1_character="null"
    metadata_p2_character="null"
    metadata_p1_super_art="null"
    metadata_p2_super_art="null"
    metadata_software_frame_mode="$captured_software_frame_mode"
    metadata_capture_start_test_phase="$captured_test_phase"
    metadata_perf_wait_test_phase="$captured_wait_test_phase"
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
    if [ -n "$software_frame_mode" ]; then
        metadata_software_frame_mode="$software_frame_mode"
    fi
    if [ -z "$metadata_perf_wait_test_phase" ] && [ -n "$perf_wait_test_phase" ]; then
        metadata_perf_wait_test_phase="$perf_wait_test_phase"
    fi
    if [ "$metadata_perf_wait_test_phase" = "game-input-active" ] &&
        { [ -z "$metadata_capture_start_test_phase" ] || [ "$metadata_capture_start_test_phase" = "game" ]; }; then
        metadata_capture_start_test_phase="game-input-active"
    fi

    temp_output_path="$(mktemp "${TMPDIR:-/tmp}/3sx-perf-json.XXXXXX")"
    jq \
        --arg scene "$scene" \
        --arg test_scene_preset "$test_scene_preset" \
        --arg software_frame_mode "$metadata_software_frame_mode" \
        --arg capture_start_test_phase "$metadata_capture_start_test_phase" \
        --arg perf_wait_test_phase "$metadata_perf_wait_test_phase" \
        --argjson stage_id "$metadata_stage_id" \
        --argjson test_stage_override "$metadata_test_stage_override" \
        --argjson p1_character "$metadata_p1_character" \
        --argjson p2_character "$metadata_p2_character" \
        --argjson p1_super_art "$metadata_p1_super_art" \
        --argjson p2_super_art "$metadata_p2_super_art" \
        --argjson p1_super_full "$(if [ "$test_p1_super_full" -eq 1 ]; then printf 'true'; else printf 'false'; fi)" \
        --argjson preserve_game_transition "$(if [ "$test_preserve_game_transition" -eq 1 ]; then printf 'true'; else printf 'false'; fi)" \
        --argjson delay_gameplay_inputs_until_active "$(if [ "$test_delay_gameplay_inputs_until_active" -eq 1 ]; then printf 'true'; else printf 'false'; fi)" \
        '.metadata = ((.metadata // {}) + {scene: $scene, test_scene_preset: (if ($test_scene_preset | length) > 0 then $test_scene_preset else null end), software_frame_mode: (if ($software_frame_mode | length) > 0 then $software_frame_mode else null end), capture_start_test_phase: (if ($capture_start_test_phase | length) > 0 and $capture_start_test_phase != "(none)" then $capture_start_test_phase else null end), perf_wait_test_phase: (if ($perf_wait_test_phase | length) > 0 and $perf_wait_test_phase != "(none)" then $perf_wait_test_phase else null end), stage_id: $stage_id, test_stage_override: $test_stage_override, p1_character: $p1_character, p2_character: $p2_character, p1_super_art: $p1_super_art, p2_super_art: $p2_super_art, test_p1_super_full: $p1_super_full, test_preserve_game_transition: $preserve_game_transition, test_delay_gameplay_inputs_until_active: $delay_gameplay_inputs_until_active})' \
        "${local_output_path}" >"${temp_output_path}"
    mv "${temp_output_path}" "${local_output_path}"
fi

rm -f "${local_remote_log_path}"
rm -f "${remote_stdout_path}" "${remote_json_b64_path}" "${remote_log_b64_path}"

echo "Saved perf JSON to ${local_output_path}"

if command -v jq >/dev/null 2>&1; then
    fps_mean="$(jq -r '.metrics.fps.mean // empty' "${local_output_path}")"
    frame_mean_ms="$(jq -r '.metrics.frame_time.mean_ms // empty' "${local_output_path}")"
    frame_max_ms="$(jq -r '.metrics.frame_time.max_ms // empty' "${local_output_path}")"
    update_mean_ms="$(jq -r '.metrics.update.mean_ms // empty' "${local_output_path}")"
    render_mean_ms="$(jq -r '.metrics.render.mean_ms // empty' "${local_output_path}")"
    present_mean_ms="$(jq -r '.metrics.present.mean_ms // empty' "${local_output_path}")"
    metadata_stage_id="$(jq -r '.metadata.stage_id // empty' "${local_output_path}")"
    metadata_preset="$(jq -r '.metadata.test_scene_preset // empty' "${local_output_path}")"
    metadata_mode="$(jq -r '.metadata.software_frame_mode // empty' "${local_output_path}")"
    metadata_test_phase="$(jq -r '.metadata.capture_start_test_phase // empty' "${local_output_path}")"

    printf 'Perf summary: tag=%s scene=%s fps=%s frame_mean_ms=%s frame_max_ms=%s update_mean_ms=%s render_mean_ms=%s present_mean_ms=%s stage_id=%s preset=%s software_frame_mode=%s test_phase=%s\n' \
        "$tag" "$scene" "${fps_mean:-unknown}" "${frame_mean_ms:-unknown}" "${frame_max_ms:-unknown}" \
        "${update_mean_ms:-unknown}" "${render_mean_ms:-unknown}" "${present_mean_ms:-unknown}" \
        "${metadata_stage_id:-unknown}" "${metadata_preset:-none}" "${metadata_mode:-unknown}" \
        "${metadata_test_phase:-unknown}"

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
fi
