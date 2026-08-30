#!/usr/bin/env bash
# run_matrix.sh -- run the real netplay connection cascade across a NAT-type
# matrix inside Linux network namespaces, and emit one JSON line per attempt.
#
# Cascade under test: STUN -> direct punch -> rendezvous signaling -> bilateral
# punch (all raced concurrently by p2p_race, src/netplay/direct_p2p.c:1724).
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
#     --natpmp-joiner     run rig/natpmp_mock.py in the JOINER's NAT namespace
#                         and point the joiner probe's production NAT-PMP client
#                         at it (task #121 joiner port-map path). Off by default.
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
# --natpmp-joiner: start rig/natpmp_mock.py inside the JOINER's NAT namespace and
# point the joiner probe's production NAT-PMP client at it
# (Natpmp_TestHook_SetGateway via p2p_probe --natpmp-gateway). This is the only
# way the task #121 joiner port-map path can be exercised in the rig: natpmp.c
# refuses to consult the real default route in a test build. Off by default, so
# the flagless invocation is byte-identical to the pre-#121 harness.
NATPMP_JOINER=0
JOINER_LAN_GW="10.2.0.1"   # s8-nB's LAN-side address (natns.sh LAN_B.1)

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
        --natpmp-joiner) NATPMP_JOINER=1; shift ;;
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

# The NAT classifier observer is started the same way as the mocks above --
# backgrounded `sudo ip netns exec python3 ...` -- and it has the same hazard,
# plus a worse one. `kill $PID; wait $PID` reaps the sudo wrapper but leaves the
# python child holding UDP 19401/19402 inside s8-srv, and `wait` then blocks
# forever on a pipeline whose grandchild still owns the redirected fd. Measured:
# the whole matrix hung on this line and every run died at the 4th cell (rc=124
# under `timeout`). So: kill the wrapper, pkill the child by its FULL
# lane-private path exactly as stop_servers does, and never `wait`.
COBS=""
stop_classifier() {
    [ -n "$COBS" ] && kill "$COBS" 2>/dev/null
    sudo -n pkill -f "$HERE/rig/nat_classify.py observer" 2>/dev/null
    COBS=""
    sleep 0.3
}
cleanup_all() {
    stop_servers; stop_classifier
    [ "$NATPMP_JOINER" = 1 ] && "$NATNS" portmap B down >/dev/null 2>&1
    "$NATNS" down >/dev/null 2>&1
    return 0
}
trap cleanup_all EXIT

total=0; connected=0
rig_errors=0; not_run=0; scored=0

for A in $TYPES; do
for B in $TYPES; do
    echo "=== cell A=$A B=$B ===" >&2

    if ! "$NATNS" up "$A" "$B" >"$WORK/up.log" 2>&1; then
        echo "  TOPOLOGY_UP_FAILED" >&2
        sed -n '1,20p' "$WORK/up.log" >&2
        printf '{"label":"%s","natA":"%s","natB":"%s","rep":0,"cell_status":"TOPOLOGY_UP_FAILED"}\n' \
            "$LABEL" "$A" "$B" >> "$OUT"
        rig_errors=$((rig_errors+1))
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
    stop_classifier
    MEAS_A=$(printf '%s' "$OBS_A" | sed -n 's/.*"nat_type": *"\([^"]*\)".*/\1/p')
    MEAS_B=$(printf '%s' "$OBS_B" | sed -n 's/.*"nat_type": *"\([^"]*\)".*/\1/p')
    MEAS_A="${MEAS_A:-unmeasured}"; MEAS_B="${MEAS_B:-unmeasured}"
    echo "  declared A=$A B=$B | measured A=$MEAS_A B=$MEAS_B" >&2

    PROBE_JOIN_EXTRA=""

    for rep in $(seq 1 "$REPS"); do
        stop_servers

        # REPETITIONS MUST BE INDEPENDENT (task-102, re-landed in task #126).
        # The classification pass above, and every earlier rep, leave live
        # conntrack entries and live xt_recent entries in the NAT namespaces --
        # both outlive a rep by design (120 s, set in natns.sh apply_nat). A warm
        # NAT is a DIFFERENT NAT: measured on this rig, addr-restricted x
        # fullcone answered the host's punch from external port 22733 on rep 1
        # and from 7000 on reps 2 and 3, because rep 1 had already put the peer's
        # address into the s8sent list. Reps 2+ were then not replications of rep
        # 1 at all, and averaging them hid a 1-in-3 failure. Tearing the topology
        # down and back up is the only way to clear per-namespace netfilter state
        # without a conntrack(8) binary on the rig host.
        "$NATNS" portmap B down >/dev/null 2>&1
        "$NATNS" up "$A" "$B" >>"$WORK/up.log" 2>&1

        # --- impairments (re-applied: the re-up above cleared them) ---
        "$NATNS" netem A "$OWD_A" "$JITTER" "$LOSS" >/dev/null 2>&1
        "$NATNS" netem B "$OWD_B" "$JITTER" "$LOSS" >/dev/null 2>&1
        "$NATNS" deliverloss "$DELIVER_LOSS" A       >/dev/null 2>&1

        # --- joiner port-map gateway (#121) -------------------------------
        # Started AFTER classification on purpose: measA/measB must describe
        # the NAT as the rig configured it, not as a granted mapping
        # temporarily makes it look. Re-started per rep because the re-up
        # above deleted the namespace the daemon was living in. If the mock
        # refuses to come up the cell is a RIG error, not a NAT finding, so
        # say so and abandon the cell rather than quietly running without it.
        if [ "$NATPMP_JOINER" = 1 ]; then
            if "$NATNS" portmap B up >"$WORK/portmapB_${A}_${B}_${rep}.log" 2>&1; then
                PROBE_JOIN_EXTRA="--natpmp-gateway $JOINER_LAN_GW"
            else
                echo "  PORTMAP_UP_FAILED (side B, rep $rep)" >&2
                sed -n '1,20p' "$WORK/portmapB_${A}_${B}_${rep}.log" >&2
                printf '{"label":"%s","natA":"%s","natB":"%s","rep":%d,"cell_status":"RIG_ERROR","reason":"portmap B up failed"}\n' \
                    "$LABEL" "$A" "$B" "$rep" >> "$OUT"
                rig_errors=$((rig_errors+1))
                # Abandon the whole cell: the post-loop cleanup below runs and
                # the outer loop moves on. Never score a rep against a rig that
                # did not come up the way the cell declared.
                break
            fi
        fi

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

        # $PROBE_JOIN_EXTRA is intentionally UNQUOTED: it is either empty or the
        # two words "--natpmp-gateway <ip>", both rig-controlled, never user input.
        ns s8-hB env HOME="$WORK/hB" XDG_DATA_HOME="$WORK/hB" \
            "$PROBE" --role join --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
            --code-file "$CODEF" --timeout-ms 45000 $PROBE_JOIN_EXTRA \
            > "$WORK/join_$TAG.json" 2> "$WORK/join_$TAG.err"
        JRC=$?

        # The HOST has no wall-clock budget by design -- it waits in HOST_WAITING
        # as long as the room code is on screen (direct_p2p.c:3235-3239). So on a
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
        case "$STATUS" in
            RIG_ERROR)   rig_errors=$((rig_errors+1)) ;;
            DID_NOT_RUN) not_run=$((not_run+1)) ;;
            *)           scored=$((scored+1)) ;;
        esac
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
    # natns.sh down deliberately does not stop the port-map mock (it is left
    # byte-for-byte alone for other lanes), so it must be stopped here or the
    # daemon lingers holding UDP 5351 and poisons the next cell.
    [ "$NATPMP_JOINER" = 1 ] && "$NATNS" portmap B down >/dev/null 2>&1
    "$NATNS" down >/dev/null 2>&1
done
done

echo "matrix done: $connected/$total attempts connected -> $OUT" >&2
echo "  scored=$scored rig_errors=$rig_errors did_not_run=$not_run" >&2

# ---------------------------------------------------------------------------
# EXIT CODE. This script used to fall off the end and exit 0 unconditionally --
# including when the topology failed to come up in EVERY cell and not one packet
# was ever sent. A run that measured nothing must not be reportable as a run.
#
#   0  every attempt was scored (CONNECTED / NOT_CONNECTED / TIMEOUT)
#   4  at least one attempt was scored, but some rows are rig errors or
#      did-not-run -- the JSONL is contaminated and summarize.py will refuse it
#   3  VACUOUS: nothing was scored at all
# ---------------------------------------------------------------------------
if [ "$scored" -eq 0 ]; then
    echo "run_matrix.sh: VACUOUS RUN -- zero attempts were scored." >&2
    exit 3
fi
if [ "$rig_errors" -gt 0 ] || [ "$not_run" -gt 0 ]; then
    echo "run_matrix.sh: run is CONTAMINATED ($rig_errors rig errors, $not_run did-not-run)." >&2
    exit 4
fi
exit 0
