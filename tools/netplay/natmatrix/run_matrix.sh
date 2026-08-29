#!/usr/bin/env bash
# run_matrix.sh -- run the real netplay connection cascade across a NAT-type
# matrix inside Linux network namespaces, and emit one JSON line per attempt.
#
# Cascade under test: STUN -> direct punch -> rendezvous signaling -> bilateral
# punch (all raced concurrently by p2p_race, src/netplay/direct_p2p.c:1359).
# There is no relay; the S5 relay was removed and is deliberately not modelled.
#
# For every cell we record the MEASURED NAT type on each side (rig/nat_classify.py)
# alongside the declared one. A cell whose emulation did not produce the intended
# NAT behaviour is reported as such rather than being silently counted -- a
# harness that only proves the easy cells work would be worthless here.
#
# Usage:
#   run_matrix.sh --probe /path/to/p2p_probe [options]
#     --types "a b c"     NAT types on both axes
#                         (default: fullcone addr-restricted port-restricted symmetric)
#     --reps N            attempts per cell (default 3)
#     --owd-a MS          one-way delay added on side A's WAN egress (default 0)
#     --owd-b MS          one-way delay added on side B's WAN egress (default 0)
#     --jitter MS         netem jitter (default 0)
#     --loss PCT          uniform packet loss on both WAN egresses (default 0)
#     --deliver-loss PCT  drop this % of rendezvous DELIVER frames toward the
#                         HOST -- the mechanism that makes the residual
#                         split-brain band reachable
#     --out FILE          JSONL results (default /tmp/s8matrix/results.jsonl)
#     --label NAME        free-text tag recorded on every row
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
NATNS="$HERE/natns.sh"
NODE="${NODE_BIN:-node}"

PROBE=""
TYPES="fullcone addr-restricted port-restricted symmetric"
REPS=3
OWD_A=0; OWD_B=0; JITTER=0; LOSS=0; DELIVER_LOSS=0
OUT="/tmp/s8matrix/results.jsonl"
LABEL="default"

while [ $# -gt 0 ]; do
    case "$1" in
        --probe) PROBE="$2"; shift 2 ;;
        --types) TYPES="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --owd-a) OWD_A="$2"; shift 2 ;;
        --owd-b) OWD_B="$2"; shift 2 ;;
        --jitter) JITTER="$2"; shift 2 ;;
        --loss) LOSS="$2"; shift 2 ;;
        --deliver-loss) DELIVER_LOSS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --label) LABEL="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -x "$PROBE" ] || { echo "run_matrix.sh: --probe must be an executable p2p_probe" >&2; exit 2; }

WORK="$(dirname "$OUT")"
mkdir -p "$WORK"
: > "$OUT"

SRV_IP=203.0.113.100
SRV_IP2=203.0.113.101
STUN_SPEC="${SRV_IP}:19302,${SRV_IP2}:19302,${SRV_IP}:19303"
SIGNAL="udp://${SRV_IP}:3478"

ns() { sudo -n ip netns exec "$1" "${@:2}"; }

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

STUN_PID=""; RDV_PID=""
# Killing a backgrounded `sudo ip netns exec ...` reaps only the sudo wrapper --
# the python/node child survives, keeps its UDP port bound, and poisons every
# later repetition. So also match the child by its FULL unique path. These
# patterns are lane-private absolute paths; never run an unscoped pkill here,
# other lanes are building in this VM.
stop_servers() {
    [ -n "$STUN_PID" ] && kill "$STUN_PID" 2>/dev/null
    [ -n "$RDV_PID" ]  && kill "$RDV_PID"  2>/dev/null
    sudo -n pkill -f "$HERE/rig/stun_mock.py" 2>/dev/null
    sudo -n pkill -f "$REPO/tools/rendezvous-server/rendezvous-server.js" 2>/dev/null
    STUN_PID=""; RDV_PID=""
    sleep 0.3
}
kill_probes() { sudo -n pkill -x p2p_probe 2>/dev/null; }
cleanup_all() { stop_servers; "$NATNS" down >/dev/null 2>&1; }
trap cleanup_all EXIT

total=0; connected=0

for A in $TYPES; do
for B in $TYPES; do
    echo "=== cell A=$A B=$B ===" >&2

    if ! "$NATNS" up "$A" "$B" >"$WORK/up.log" 2>&1; then
        echo "  TOPOLOGY_UP_FAILED" >&2
        printf '{"label":"%s","natA":"%s","natB":"%s","rep":0,"cell_status":"TOPOLOGY_UP_FAILED"}\n' \
            "$LABEL" "$A" "$B" >> "$OUT"
        continue
    fi

    # --- measure what the rig ACTUALLY emulates, before adding impairments ---
    ns s8-srv python3 "$HERE/rig/nat_classify.py" observer \
        --ip1 "$SRV_IP" --ip2 "$SRV_IP2" --p1 19401 --p2 19402 \
        > "$WORK/classify_obs.log" 2>&1 &
    COBS=$!
    sleep 1
    OBS_A=$(ns s8-hA timeout 40 python3 "$HERE/rig/nat_classify.py" prober --json \
            --ip1 "$SRV_IP" --ip2 "$SRV_IP2" --p1 19401 --p2 19402 2>/dev/null | tail -1)
    OBS_B=$(ns s8-hB timeout 40 python3 "$HERE/rig/nat_classify.py" prober --json \
            --ip1 "$SRV_IP" --ip2 "$SRV_IP2" --p1 19401 --p2 19402 2>/dev/null | tail -1)
    kill $COBS 2>/dev/null; wait $COBS 2>/dev/null
    MEAS_A=$(printf '%s' "$OBS_A" | sed -n 's/.*"nat_type": *"\([^"]*\)".*/\1/p')
    MEAS_B=$(printf '%s' "$OBS_B" | sed -n 's/.*"nat_type": *"\([^"]*\)".*/\1/p')
    MEAS_A="${MEAS_A:-unmeasured}"; MEAS_B="${MEAS_B:-unmeasured}"
    echo "  declared A=$A B=$B | measured A=$MEAS_A B=$MEAS_B" >&2

    # --- impairments ---
    "$NATNS" netem A "$OWD_A" "$JITTER" "$LOSS" >/dev/null 2>&1
    "$NATNS" netem B "$OWD_B" "$JITTER" "$LOSS" >/dev/null 2>&1
    "$NATNS" deliverloss "$DELIVER_LOSS" A       >/dev/null 2>&1

    for rep in $(seq 1 "$REPS"); do
        stop_servers
        ns s8-srv python3 "$HERE/rig/stun_mock.py" \
            --bind "${SRV_IP}:19302" --bind "${SRV_IP2}:19302" --bind "${SRV_IP}:19303" \
            > "$WORK/stun_${A}_${B}_${rep}.log" 2>&1 &
        STUN_PID=$!
        ns s8-srv "$NODE" "$REPO/tools/rendezvous-server/rendezvous-server.js" 3478 \
            > "$WORK/rdv_${A}_${B}_${rep}.log" 2>&1 &
        RDV_PID=$!
        sleep 1

        CODEF="$WORK/code_${A}_${B}_${rep}.txt"
        rm -f "$CODEF"
        sudo -n rm -rf "$WORK/hA" "$WORK/hB"; mkdir -p "$WORK/hA" "$WORK/hB"

        # Per-cell/rep filenames: the stderr traces are the ONLY record of what the
        # cascade actually did (which endpoint each side punched, whether a leg
        # retargeted, why it gave up). Overwriting them per rep would discard the
        # evidence needed to explain any cell that fails.
        TAG="${A}_${B}_${rep}"
        ns s8-hA env HOME="$WORK/hA" XDG_DATA_HOME="$WORK/hA" \
            "$PROBE" --role host --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
            --code-file "$CODEF" --timeout-ms 45000 \
            > "$WORK/host_$TAG.json" 2> "$WORK/host_$TAG.err" &
        HPID=$!

        ns s8-hB env HOME="$WORK/hB" XDG_DATA_HOME="$WORK/hB" \
            "$PROBE" --role join --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
            --code-file "$CODEF" --timeout-ms 45000 \
            > "$WORK/join_$TAG.json" 2> "$WORK/join_$TAG.err"
        JRC=$?

        # The HOST has no wall-clock budget by design -- it waits in HOST_WAITING
        # as long as the room code is on screen (direct_p2p.c:2516-2522). So on a
        # failing cell the host never terminates on its own. Give it a grace
        # period to land a late handoff, then stop that ONE pid (never pkill).
        for _ in $(seq 1 30); do kill -0 "$HPID" 2>/dev/null || break; sleep 0.1; done
        if kill -0 "$HPID" 2>/dev/null; then
            kill "$HPID" 2>/dev/null; kill_probes; HRC=99
        else
            wait "$HPID"; HRC=$?
        fi
        kill_probes

        HJSON=$(tail -1 "$WORK/host_$TAG.json" 2>/dev/null)
        JJSON=$(tail -1 "$WORK/join_$TAG.json" 2>/dev/null)

        # A cell is only scored if the JOINER actually ran. The joiner is the side
        # that carries the race budget and therefore the side that can conclude.
        case "$JJSON" in *'"ran":true'*) JRAN=true ;; *) JRAN=false ;; esac

        if [ "$JRAN" != true ]; then
            STATUS="DID_NOT_RUN"
        elif [ "$JRC" = 0 ]; then
            STATUS="CONNECTED"
        elif [ "$JRC" = 10 ]; then
            STATUS="NOT_CONNECTED"
        elif [ "$JRC" = 30 ]; then
            STATUS="TIMEOUT"
        else
            STATUS="RIG_ERROR"
        fi

        [ "$STATUS" = CONNECTED ] && connected=$((connected+1))
        total=$((total+1))
        echo "  rep $rep: $STATUS (join rc=$JRC host rc=$HRC)" >&2

        printf '{"label":"%s","natA":"%s","natB":"%s","measA":"%s","measB":"%s","rep":%d,' \
            "$LABEL" "$A" "$B" "$MEAS_A" "$MEAS_B" "$rep" >> "$OUT"
        printf '"owd_a":%s,"owd_b":%s,"loss":%s,"deliver_loss":%s,' \
            "$OWD_A" "$OWD_B" "$LOSS" "$DELIVER_LOSS" >> "$OUT"
        printf '"cell_status":"%s","join_rc":%s,"host_rc":%s,' \
            "$STATUS" "$JRC" "$HRC" >> "$OUT"
        printf '"join":%s,"host":%s}\n' \
            "${JJSON:-null}" "${HJSON:-null}" >> "$OUT"
    done

    stop_servers
    "$NATNS" down >/dev/null 2>&1
done
done

echo "matrix done: $connected/$total attempts connected -> $OUT" >&2
