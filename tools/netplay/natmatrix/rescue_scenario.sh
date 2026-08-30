#!/usr/bin/env bash
# rescue_scenario.sh -- task #119: prove, on the netns rig, that a case which
# ends in FAILED_HANDSHAKE without the late-punch layer instead CONNECTS LATE
# with it.
#
# THE INJECTED CONDITION. A transient one-way block of punch traffic toward
# the JOINER: an iptables string-match on the 17-byte authenticated punch
# payload ("3SX_PUNCH" + token) in the joiner-side NAT namespace's FORWARD
# chain, lifted mid-run. This manufactures, deterministically, the asymmetric
# path that docs/plan-netplay-connection.md §8.10 concedes no settle window
# can close: the joiner's punches traverse (the host confirms, settles, hands
# off to the session), the host's answers do not (the joiner's race exhausts).
# That is the one-sided handoff. Pre-#119 it is terminal on both sides --
# the host waits out CONNECT_TIMEOUT_CONNECTING_MS against a peer that tore
# down and parks in FAILED_HANDSHAKE; the joiner's S2 retry punches into a
# void and parks in FAILED_BILATERAL. With #119, the retry's punches (from a
# FRESH socket, i.e. a new source port) are answered by the connected host's
# late-punch layer once the block lifts, the host RELEARNS the moved port,
# and both sides reach GekkoSessionStarted before the deadline.
#
# WHY THIS TOPOLOGY IS TRUSTWORTHY. fullcone x fullcone: natns.sh's fullcone
# is a blanket, PORTLESS inbound DNAT (natns.sh apply_nat), so both sides have
# a working inbound path for every port -- including the joiner retry's fresh
# ephemeral port. This deliberately avoids the port-restricted/symmetric rows,
# whose zeros are rig artifacts (#126: natns.sh has no inbound path there, so
# punches die in the NAT namespace and nothing can be attributed). The NAT
# type is NOT the variable under test -- the split brain is a timing/asymmetry
# failure -- and a topology in which inbound provably works is exactly what
# isolates it.
#
# ANTI-VACUITY (the #121 lesson: a run that measured nothing must not be
# reportable as a run). Three phases, each a differential guard for the next:
#   CONTROL   no block, late-punch OFF  -> both sides must CONNECT + sync.
#             Proves the topology, servers and session phase work at all.
#             Fails => RIG (exit 5), nothing else is trustworthy.
#   BASELINE  block + lift, late-punch OFF -> host must park FAILED_HANDSHAKE
#             (session DEADLINE), joiner must park FAILED_BILATERAL.
#             Anything else => the injection did not produce the split brain
#             and the rescue phase would prove nothing: VACUOUS (exit 3).
#   RESCUE    identical network schedule, late-punch ON -> both sides must
#             reach SessionStarted, with >= 1 relearn applied on the host.
#             Fails => RED (exit 1).
# All three refuse to score any probe output lacking "ran":true (exit 4).
#
# Usage: rescue_scenario.sh --probe /path/to/p2p_probe [--out DIR]
#        [--lift-s N]   seconds after joiner launch to lift the punch block
#                       (default 11: inside the joiner's attempt-2 punch
#                        window at default budgets -- attempt 1 ends ~6.5 s
#                        after its start, attempt 2 punches for 5 s from
#                        ~7.5 s; the window tolerates several seconds of
#                        code-file/STUN skew either way)
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
NATNS="$HERE/natns.sh"
NODE="${NODE_BIN:-node}"

PROBE=""
OUT="/tmp/t119rescue"
LIFT_S=11

while [ $# -gt 0 ]; do
    case "$1" in
        --probe) PROBE="$2"; shift 2 ;;
        --out)   OUT="$2"; shift 2 ;;
        --lift-s) LIFT_S="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -x "$PROBE" ] || { echo "rescue_scenario: --probe must be an executable p2p_probe" >&2; exit 2; }
mkdir -p "$OUT"

SRV_IP=203.0.113.100
SRV_IP2=203.0.113.101
STUN_SPEC="${SRV_IP}:19302,${SRV_IP2}:19302,${SRV_IP}:19303"
SIGNAL="udp://${SRV_IP}:3478"

ns() { sudo -n ip netns exec "$1" "${@:2}"; }

STUN_PID=""; RDV_PID=""
stop_servers() {
    [ -n "$STUN_PID" ] && kill "$STUN_PID" 2>/dev/null
    [ -n "$RDV_PID" ]  && kill "$RDV_PID"  2>/dev/null
    sudo -n pkill -f "$HERE/rig/stun_mock.py" 2>/dev/null
    sudo -n pkill -f "$REPO/tools/rendezvous-server/rendezvous-server.js" 2>/dev/null
    STUN_PID=""; RDV_PID=""
    sleep 0.3
}
kill_probes() { sudo -n pkill -x p2p_probe 2>/dev/null; }

# The punch block: FORWARD chain of the joiner's NAT namespace, matching the
# punch payload prefix on datagrams headed to the joiner's LAN side (-o nBi).
# Payload-match, not port-match, so ONLY punch traffic is asymmetric -- the
# rendezvous/STUN/GekkoNet flows are untouched in both directions.
block_up()   { "$NATNS" exec nB iptables -I FORWARD -o nBi -m string --algo bm --string "3SX_PUNCH" -j DROP; }
block_down() { "$NATNS" exec nB iptables -D FORWARD -o nBi -m string --algo bm --string "3SX_PUNCH" -j DROP 2>/dev/null || true; }

cleanup_all() {
    stop_servers; kill_probes
    "$NATNS" down >/dev/null 2>&1
    return 0
}
trap cleanup_all EXIT

json_field() { # json_field <file> <key>  -> string value or empty
    sed -n 's/.*"'"$2"'":"\{0,1\}\([^",}]*\)"\{0,1\}[,}].*/\1/p' "$1" 2>/dev/null | head -1
}

run_phase() { # run_phase <name> <late_punch 0|1> <block 0|1>
    local name="$1" lp="$2" block="$3"
    echo "=== phase $name (late-punch=$lp block=$block lift=${LIFT_S}s) ===" >&2

    stop_servers; kill_probes
    block_down
    ns s8-srv python3 "$HERE/rig/stun_mock.py" \
        --bind "${SRV_IP}:19302" --bind "${SRV_IP2}:19302" --bind "${SRV_IP}:19303" \
        > "$OUT/stun_$name.log" 2>&1 &
    STUN_PID=$!
    ns s8-srv "$NODE" "$REPO/tools/rendezvous-server/rendezvous-server.js" 3478 \
        > "$OUT/rdv_$name.log" 2>&1 &
    RDV_PID=$!
    sleep 1

    [ "$block" = 1 ] && block_up

    local codef="$OUT/code_$name.txt"
    rm -f "$codef"
    sudo -n rm -rf "$OUT/hA" "$OUT/hB"; mkdir -p "$OUT/hA" "$OUT/hB"

    ns s8-hA env HOME="$OUT/hA" XDG_DATA_HOME="$OUT/hA" \
        "$PROBE" --role host --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
        --code-file "$codef" --timeout-ms 60000 \
        --session --late-punch "$lp" \
        > "$OUT/host_$name.json" 2> "$OUT/host_$name.err" &
    local hpid=$!

    ns s8-hB env HOME="$OUT/hB" XDG_DATA_HOME="$OUT/hB" \
        "$PROBE" --role join --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
        --code-file "$codef" --timeout-ms 60000 \
        --session --late-punch "$lp" \
        > "$OUT/join_$name.json" 2> "$OUT/join_$name.err" &
    local jpid=$!

    if [ "$block" = 1 ]; then
        ( sleep "$LIFT_S"; block_down; echo "[rescue_scenario] punch block lifted at +${LIFT_S}s" >&2 ) &
    fi

    wait "$jpid"; JRC=$?
    # The host self-terminates in --session mode (session started + grace, or
    # the CONNECTING deadline); allow it the deadline plus slack.
    local waited=0
    while kill -0 "$hpid" 2>/dev/null && [ "$waited" -lt 400 ]; do sleep 0.1; waited=$((waited+1)); done
    if kill -0 "$hpid" 2>/dev/null; then kill "$hpid" 2>/dev/null; kill_probes; HRC=99
    else wait "$hpid"; HRC=$?; fi
    block_down

    HJSON="$OUT/host_$name.json"; JJSON="$OUT/join_$name.json"
    grep -q '"ran":true' "$HJSON" || { echo "phase $name: host probe did not run" >&2; return 40; }
    grep -q '"ran":true' "$JJSON" || { echo "phase $name: join probe did not run" >&2; return 40; }

    echo "  host: rc=$HRC session=$(json_field "$HJSON" session) final=$(json_field "$HJSON" final_state) relearns=$(json_field "$HJSON" relearns) ms_to_session=$(json_field "$HJSON" ms_to_session)" >&2
    echo "  join: rc=$JRC session=$(json_field "$JJSON" session) final=$(json_field "$JJSON" final_state) relearns=$(json_field "$JJSON" relearns) ms_to_session=$(json_field "$JJSON" ms_to_session)" >&2
    return 0
}

# ---- topology --------------------------------------------------------------
# fullcone x fullcone (see the header for why). One topology for all three
# phases: the phases must differ ONLY in the block schedule and the
# late-punch flag, or the differential proves nothing.
if ! "$NATNS" up fullcone fullcone > "$OUT/up.log" 2>&1; then
    echo "RESULT: RIG -- topology up failed:" >&2
    sed -n '1,20p' "$OUT/up.log" >&2
    exit 5
fi

# ---- CONTROL --------------------------------------------------------------
run_phase control 0 0 || exit 4
if [ "$(json_field "$OUT/host_control.json" session)" != "STARTED" ] || \
   [ "$(json_field "$OUT/join_control.json" session)" != "STARTED" ]; then
    echo "RESULT: RIG -- control phase (no block) did not sync; topology/servers/session"
    echo "phase are broken and neither later phase can be trusted."
    exit 5
fi

# ---- BASELINE -------------------------------------------------------------
run_phase baseline 0 1 || exit 4
BH_FINAL=$(json_field "$OUT/host_baseline.json" final_state)
BH_SESS=$(json_field "$OUT/host_baseline.json" session)
BJ_FINAL=$(json_field "$OUT/join_baseline.json" final_state)
if [ "$BH_FINAL" != "FAILED_HANDSHAKE" ] || [ "$BH_SESS" != "DEADLINE" ]; then
    echo "RESULT: VACUOUS -- baseline host ended $BH_FINAL/$BH_SESS, not the"
    echo "FAILED_HANDSHAKE split brain this scenario exists to reproduce. The"
    echo "injection missed (lift too early/late?); a rescue phase would prove nothing."
    exit 3
fi
if [ "$BJ_FINAL" != "FAILED_BILATERAL" ] && [ "$BJ_FINAL" != "FAILED_SYMMETRIC" ]; then
    echo "RESULT: VACUOUS -- baseline joiner ended $BJ_FINAL (expected a terminal"
    echo "FAILED_* park). The one-sidedness did not hold."
    exit 3
fi

# ---- RESCUE ---------------------------------------------------------------
run_phase rescue 1 1 || exit 4
RH_SESS=$(json_field "$OUT/host_rescue.json" session)
RJ_SESS=$(json_field "$OUT/join_rescue.json" session)
RH_RELEARN=$(json_field "$OUT/host_rescue.json" relearns)
if [ "$RH_SESS" != "STARTED" ] || [ "$RJ_SESS" != "STARTED" ]; then
    echo "RESULT: RED -- identical network schedule, late-punch ON, and the pair"
    echo "still did not sync (host=$RH_SESS join=$RJ_SESS). The rescue failed."
    exit 1
fi
if [ "${RH_RELEARN:-0}" -lt 1 ]; then
    echo "RESULT: RED -- the pair synced but the host applied no relearn; the"
    echo "rescue did not go through the mechanism under test (did the joiner"
    echo "reuse its port?). Not accepting a pass that bypassed the mechanism."
    exit 1
fi

echo "RESULT: GREEN"
echo "  baseline: host FAILED_HANDSHAKE (session DEADLINE), joiner $BJ_FINAL -- the split brain."
echo "  rescue:   host relearned x$RH_RELEARN and synced in $(json_field "$OUT/host_rescue.json" ms_to_session) ms;"
echo "            joiner synced in $(json_field "$OUT/join_rescue.json" ms_to_session) ms. Same schedule, one variable."
exit 0
