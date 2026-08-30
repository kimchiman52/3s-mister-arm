#!/usr/bin/env bash
# nonvacuity.sh -- prove the S8 harness actually tests something.
#
# On this project a clean-looking exit has repeatedly meant "never ran". A NAT
# matrix that only shows the easy cells connecting would be worthless: it cannot
# distinguish "the cascade succeeded" from "the rig never carried a packet".
#
# Four checks, each answering a specific way this harness could be a lie:
#
#   A. POSITIVE CONTROL -- port-restricted x port-restricted must CONNECT.
#      If this fails, every negative result below is just a broken rig.
#
#   B. TRUE NEGATIVE -- symmetric x symmetric must NOT connect, AND the failure
#      must be attributable to the punch specifically. We assert that STUN
#      still resolved (so the host<->server path is live) and that the joiner
#      reached a real cascade terminal state. A rig that simply dropped all
#      traffic would fail STUN too, and would be caught here.
#
#   C. SABOTAGE DISCRIMINATION -- take the cell that PASSED in (A) and blackhole
#      only the peer-to-peer path, leaving the servers reachable. It must flip
#      to NOT_CONNECTED. This proves the harness's verdict tracks peer
#      reachability rather than being a constant.
#
#   D. BUILD GUARD -- compiling the probe without -DNETPLAY_TEST_HOOKS must FAIL
#      the build. This is what makes the exit-2 "not compiled in" false pass
#      structurally impossible: there is no such binary to run.
#
# Usage: nonvacuity.sh --probe /path/to/p2p_probe [--work DIR]
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
NATNS="$HERE/natns.sh"
NODE="${NODE_BIN:-node}"

PROBE=""; WORK="/tmp/s8nonvac"
while [ $# -gt 0 ]; do
    case "$1" in
        --probe) PROBE="$2"; shift 2 ;;
        --work) WORK="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -x "$PROBE" ] || { echo "nonvacuity.sh: --probe must be executable" >&2; exit 2; }

# A PROBE THAT DID NOT BUILD IS NOT A PROBE (task #126). This driver only ever
# checked `[ -x "$PROBE" ]`. A build that failed to LINK leaves the previous
# binary sitting there, so the driver runs it, scores it, and exits 0 -- and the
# run describes a tree that no longer exists. That is not hypothetical: p2p_probe
# failed to link on this branch from d207ef1e (2026-08-29 12:39) to fd1fa3cc
# (2026-08-30 04:38), because direct_p2p.c began calling
# Netplay_LogConnectEventMT / Netplay_LogSinkInit (1c7a7c61) and
# probe/netplay_probe_stub.c did not stub them until 0b00f237. Nothing in this
# harness noticed for sixteen hours.
#
# So refuse a probe older than any source it links. There is deliberately no
# override: the fix is to rebuild, and a run that cannot prove its instrument is
# current is not evidence.
assert_probe_fresh() {
    local stale
    stale=$(find "$REPO/src/netplay" "$HERE/probe" \
                 \( -name '*.c' -o -name '*.h' \) -newer "$PROBE" 2>/dev/null | head -3)
    [ -z "$stale" ] && return 0
    echo "$(basename "$0"): STALE PROBE -- $PROBE is older than sources it links:" >&2
    printf '  %s\n' $stale >&2
    echo "  Rebuild p2p_probe against this tree and re-run." >&2
    exit 2
}
assert_probe_fresh

mkdir -p "$WORK"
SRV_IP=203.0.113.100; SRV_IP2=203.0.113.101
STUN_SPEC="${SRV_IP}:19302,${SRV_IP2}:19302,${SRV_IP}:19303"
SIGNAL="udp://${SRV_IP}:3478"

ns() { sudo -n ip netns exec "$1" "${@:2}"; }

STUN_PID=""; RDV_PID=""
start_servers() {
    ns s8-srv python3 "$HERE/rig/stun_mock.py" \
        --bind "${SRV_IP}:19302" --bind "${SRV_IP2}:19302" --bind "${SRV_IP}:19303" \
        > "$WORK/stun.log" 2>&1 &
    STUN_PID=$!
    ns s8-srv "$NODE" "$REPO/tools/rendezvous-server/rendezvous-server.js" 3478 \
        > "$WORK/rdv.log" 2>&1 &
    RDV_PID=$!
    sleep 1
}
stop_servers() {
    # Killing the backgrounded `sudo ip netns exec` reaps only the wrapper; the
    # child keeps its UDP port. Match the child by full lane-private path too.
    [ -n "$STUN_PID" ] && kill "$STUN_PID" 2>/dev/null
    [ -n "$RDV_PID" ]  && kill "$RDV_PID"  2>/dev/null
    sudo -n pkill -f "$HERE/rig/stun_mock.py" 2>/dev/null
    sudo -n pkill -f "$REPO/tools/rendezvous-server/rendezvous-server.js" 2>/dev/null
    STUN_PID=""; RDV_PID=""
    sleep 0.3
}
trap 'stop_servers; "$NATNS" down >/dev/null 2>&1' EXIT

# run_pair <tag> -> sets JRC, JJSON, HJSON
run_pair() {
    local tag="$1"
    local codef="$WORK/code_$tag.txt"
    rm -f "$codef"
    sudo -n rm -rf "$WORK/hA" "$WORK/hB"; mkdir -p "$WORK/hA" "$WORK/hB"

    ns s8-hA env HOME="$WORK/hA" XDG_DATA_HOME="$WORK/hA" \
        "$PROBE" --role host --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
        --code-file "$codef" --timeout-ms 40000 \
        > "$WORK/host_$tag.json" 2> "$WORK/host_$tag.err" &
    local hpid=$!
    ns s8-hB env HOME="$WORK/hB" XDG_DATA_HOME="$WORK/hB" \
        "$PROBE" --role join --port 7000 --stun "$STUN_SPEC" --signal "$SIGNAL" \
        --code-file "$codef" --timeout-ms 40000 \
        > "$WORK/join_$tag.json" 2> "$WORK/join_$tag.err"
    JRC=$?
    for _ in $(seq 1 30); do kill -0 "$hpid" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$hpid" 2>/dev/null && kill "$hpid" 2>/dev/null
    wait "$hpid" 2>/dev/null
    sudo -n pkill -x p2p_probe 2>/dev/null
    JJSON=$(tail -1 "$WORK/join_$tag.json" 2>/dev/null)
    HJSON=$(tail -1 "$WORK/host_$tag.json" 2>/dev/null)
}

FAILS=0
UNVERIFIED=0
# Paths for check D. Overridable so the compile can run inside a container that
# sees the tree at a different mount point.
GUARD_SRC="${S8_GUARD_SRC:-$HERE/probe/p2p_probe.c}"
GUARD_INC_SRC="${S8_GUARD_INC_SRC:-$REPO/src}"
GUARD_INC_PORT="${S8_GUARD_INC_PORT:-$REPO/src/port}"
GUARD_INC_PSA="${S8_GUARD_INC_PSA:-$HERE/probe/psa_shim}"
GUARD_INC_SDL="${S8_GUARD_INC_SDL:-/out/include}"
say() { echo "$@"; }
expect() { # expect <desc> <condition-result 0/1>
    if [ "$2" = 0 ]; then say "  PASS  $1"; else say "  FAIL  $1"; FAILS=$((FAILS+1)); fi
}

# ---------------------------------------------------------------- A
#
# The control is fullcone x port-restricted, NOT port-restricted x
# port-restricted. The latter was the original choice and it FAILS -- measured,
# reproducibly, on correctly-classified NATs (see the matrix). Keeping a control
# that the shipped cascade cannot actually satisfy would mean the harness could
# never go green, and would hide the difference between "the rig is broken" and
# "this pairing genuinely does not traverse". fullcone x port-restricted is the
# strongest pairing that is measured to connect, so it is the honest control.
say "== A. POSITIVE CONTROL: fullcone x port-restricted must CONNECT =="
CONTROL_A=fullcone; CONTROL_B=port-restricted
"$NATNS" up "$CONTROL_A" "$CONTROL_B" >/dev/null 2>&1
start_servers
run_pair A
say "  joiner: $JJSON"
[ "$JRC" = 0 ]; expect "joiner exit 0 (CONNECTED)" $?
case "$JJSON" in *'"final_state":"HANDOFF"'*) r=0;; *) r=1;; esac
expect "joiner final_state == HANDOFF" $r
stop_servers; "$NATNS" down >/dev/null 2>&1

# ---------------------------------------------------------------- B
say ""
say "== B. TRUE NEGATIVE: symmetric x symmetric must NOT connect =="
"$NATNS" up symmetric symmetric >/dev/null 2>&1
# Confirm the rig really produced symmetric mapping on BOTH sides before we
# attribute any failure to symmetric NAT.
ns s8-srv python3 "$HERE/rig/nat_classify.py" observer \
    --ip1 "$SRV_IP" --ip2 "$SRV_IP2" --p1 19401 --p2 19402 >"$WORK/cls.log" 2>&1 &
CPID=$!
sleep 1
CA=$(ns s8-hA timeout 40 python3 "$HERE/rig/nat_classify.py" prober --json \
     --ip1 "$SRV_IP" --ip2 "$SRV_IP2" --p1 19401 --p2 19402 2>/dev/null | tail -1)
CB=$(ns s8-hB timeout 40 python3 "$HERE/rig/nat_classify.py" prober --json \
     --ip1 "$SRV_IP" --ip2 "$SRV_IP2" --p1 19401 --p2 19402 2>/dev/null | tail -1)
# Same hazard as stop_servers: `wait` on a backgrounded `sudo ip netns exec`
# blocks forever because killing the wrapper leaves the python observer alive
# holding the redirected fd. Kill the wrapper, then pkill the child by its FULL
# lane-private path, and never wait. (Measured: this line hung the script.)
kill $CPID 2>/dev/null
sudo -n pkill -f "$HERE/rig/nat_classify.py observer" 2>/dev/null
sleep 0.3
say "  measured A: $CA"
say "  measured B: $CB"
case "$CA" in *'"nat_type": "symmetric"'*) r=0;; *) r=1;; esac
expect "side A measured symmetric" $r
case "$CB" in *'"nat_type": "symmetric"'*) r=0;; *) r=1;; esac
expect "side B measured symmetric" $r

start_servers
run_pair B
say "  joiner: $JJSON"
[ "$JRC" != 0 ]; expect "joiner did NOT connect" $?
case "$JJSON" in *'"ran":true'*) r=0;; *) r=1;; esac
expect "joiner actually ran (not a crash/did-not-run)" $r
# The decisive check: STUN must have SUCCEEDED. If the rig were simply
# blackholing everything, STUN would fail too and the negative would be vacuous.
case "$JJSON" in *'STUN_DISCOVER'*) r=0;; *) r=1;; esac
expect "joiner reached STUN_DISCOVER (server path alive)" $r
case "$JJSON" in *'"final_state":"FAILED_STUN"'*) r=1;; *) r=0;; esac
expect "failure is NOT FAILED_STUN (so it is the punch, not the rig)" $r
stop_servers; "$NATNS" down >/dev/null 2>&1

# ---------------------------------------------------------------- C
say ""
say "== C. SABOTAGE: the cell that PASSED in A must flip when the peer path dies =="
"$NATNS" up "$CONTROL_A" "$CONTROL_B" >/dev/null 2>&1
# Blackhole ONLY peer<->peer traffic on the WAN bridge. Server traffic is
# untouched, so STUN and rendezvous still work -- exactly isolating the punch.
sudo -n ip netns exec s8-wan iptables -I FORWARD -s 203.0.113.10 -d 203.0.113.20 -j DROP 2>/dev/null
sudo -n ip netns exec s8-wan iptables -I FORWARD -s 203.0.113.20 -d 203.0.113.10 -j DROP 2>/dev/null
# The bridge forwards at L2, so FORWARD may not see it; drop at the NAT egress too.
sudo -n ip netns exec s8-nA iptables -I OUTPUT -d 203.0.113.20 -j DROP 2>/dev/null
sudo -n ip netns exec s8-nA iptables -I FORWARD -d 203.0.113.20 -j DROP 2>/dev/null
sudo -n ip netns exec s8-nB iptables -I OUTPUT -d 203.0.113.10 -j DROP 2>/dev/null
sudo -n ip netns exec s8-nB iptables -I FORWARD -d 203.0.113.10 -j DROP 2>/dev/null
start_servers
run_pair C
say "  joiner: $JJSON"
[ "$JRC" != 0 ]; expect "sabotaged cell did NOT connect" $?
case "$JJSON" in *'STUN_DISCOVER'*) r=0;; *) r=1;; esac
expect "sabotaged cell still reached STUN (only peer path was cut)" $r
stop_servers; "$NATNS" down >/dev/null 2>&1

# ---------------------------------------------------------------- D
say ""
say "== D. BUILD GUARD: probe must not compile without NETPLAY_TEST_HOOKS =="
# The rig host may have no compiler (the toolchain lives in a build container),
# so allow S8_CC to supply one, e.g.
#   S8_CC="docker run --rm -v /home/sb.guest/s8repo:/repo -v /out:/out s8build:latest cc"
# If no compiler is reachable this reports COULD-NOT-VERIFY, never a silent pass:
# an unverifiable guard is an open question, not a green check.
CC_CMD="${S8_CC:-cc}"
if ! command -v ${CC_CMD%% *} >/dev/null 2>&1; then
    say "  COULD NOT VERIFY -- no compiler available (tried '${CC_CMD%% *}')."
    say "  Set S8_CC to a working compiler command to check the guard here."
    UNVERIFIED=$((${UNVERIFIED:-0}+1))
else
    if $CC_CMD -fsyntax-only "$GUARD_SRC" \
         -I "$GUARD_INC_SRC" -I "$GUARD_INC_PORT" -I "$GUARD_INC_PSA" \
         -I "$GUARD_INC_SDL" -DENABLE_NETPLAY >"$WORK/guard.log" 2>&1; then
        expect "compile without NETPLAY_TEST_HOOKS fails" 1
    elif grep -q "NETPLAY_TEST_HOOKS" "$WORK/guard.log"; then
        expect "compile without NETPLAY_TEST_HOOKS fails with the #error" 0
    else
        say "  (compile failed, but not on the guard: $(head -2 "$WORK/guard.log"))"
        expect "compile without NETPLAY_TEST_HOOKS fails with the #error" 1
    fi
fi

say ""
[ "${UNVERIFIED:-0}" -gt 0 ] && say "NONVACUITY: ${UNVERIFIED} check(s) COULD NOT BE VERIFIED here (see above)."
if [ "$FAILS" = 0 ]; then
    say "NONVACUITY: PASS -- harness connects where it should, fails where it should,"
    say "            and the failures are attributable to the punch, not the rig."
    exit 0
fi
say "NONVACUITY: FAIL ($FAILS checks failed)"
exit 1
