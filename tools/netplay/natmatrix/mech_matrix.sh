#!/usr/bin/env bash
# mech_matrix.sh -- run the WIRE-LEVEL punch mechanism (rig/punch_mech.py) across
# the same NAT matrix run_matrix.sh uses, with no SDL and no cascade.
#
# Why this exists: run_matrix.sh reports whether a pairing CONNECTED. When a cell
# fails it cannot say whether the datagram was refused by the emulated NAT or
# discarded by the state machine. This one models only the datagrams, in the
# cascade's order (see rig/punch_mech.py header), so a failure here is a NAT
# property and a failure only there is a code property.
#
# Usage: mech_matrix.sh [--types "..."] [--reps N] [--host-delay-ms N]
#                       [--duration-ms N] [--out FILE]
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NATNS="$HERE/natns.sh"

TYPES="fullcone addr-restricted port-restricted symmetric"
REPS=3
HOST_DELAY_MS=1000
DURATION_MS=5000
OUT="/tmp/s8mech/results.jsonl"

while [ $# -gt 0 ]; do
    case "$1" in
        --types) TYPES="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        --host-delay-ms) HOST_DELAY_MS="$2"; shift 2 ;;
        --duration-ms) DURATION_MS="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

WORK="$(dirname "$OUT")"; mkdir -p "$WORK"; : > "$OUT"
SRV_IP=203.0.113.100
PM="$HERE/rig/punch_mech.py"

ns() { sudo -n ip netns exec "$1" "${@:2}"; }
stop_obs() { sudo -n pkill -f "$PM" 2>/dev/null; sleep 0.2; }
cleanup() { stop_obs; "$NATNS" down >/dev/null 2>&1; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# EXIT-CODE ACCOUNTING.
#
# This script used to fall off the end of the file: the last statement was an
# `echo`, so it exited with THAT echo's status -- 0, unconditionally, whatever
# the per-rep rcs had been. run_all.sh:24-31 and the tail of run_matrix.sh both
# grew rc propagation after the same defect; this driver never did, so every
# mech matrix it has ever reported was green by construction, including a run
# in which the topology failed to come up in every cell and not one datagram
# was ever sent.
#
# What a rep's rc MEANS here (rig/punch_mech.py: run_peer returns at the three
# points below). Getting this wrong in the other direction would be just as bad
# -- a non-zero rep rc is usually a FINDING, not a failure:
#
#   0   the peer's punches were heard                      -> a finding
#   10  they were not                                      -> ALSO a finding.
#       symmetric x symmetric is EXPECTED to return 10; a driver that treated
#       10 as failure could never report the negative half of the grid.
#   20  no server reply, or the peer never published an endpoint
#                                                          -> NOT a finding.
#       The trial did not run: the rig failed, not the NAT.
#   anything else (python traceback, signal, kill) -> rig error.
#
# So the propagated verdict is about whether the run MEASURED anything, exactly
# as run_matrix.sh's tail is, and with the same three exit codes.
scored=0; rig_errors=0; not_run=0

# rep_class <rc> <last-json-line> -> SCORED | RIG_ERROR | DID_NOT_RUN
#
# punch_mech.py sets "ran": True (run_peer, before any return) and prints the
# object on every return path including the two rc-20 ones, so an ABSENT "ran"
# means the process died before it could conclude -- that is DID_NOT_RUN, and
# it is not scoreable. Both separator spellings are accepted so that a future
# json.dumps(separators=...) cannot silently turn every rep into DID_NOT_RUN.
rep_class() {
    case "$2" in
        *'"ran": true'*|*'"ran":true'*) ;;
        *) echo DID_NOT_RUN; return ;;
    esac
    case "$1" in
        0|10) echo SCORED ;;
        *)    echo RIG_ERROR ;;
    esac
}

for A in $TYPES; do
for B in $TYPES; do
    echo "=== mech A(host)=$A B(join)=$B ===" >&2
    if ! "$NATNS" up "$A" "$B" >"$WORK/up.log" 2>&1; then
        # A cell whose topology never came up produces no JSONL rows at all, so
        # without this counter it is invisible to both the reader and the exit
        # code. Count it, print the reason, and move on.
        echo "  UP_FAILED" >&2
        sed -n '1,20p' "$WORK/up.log" >&2
        rig_errors=$((rig_errors+1))
        continue
    fi

    # Measure what the rig actually emulates, exactly as run_matrix.sh does.
    ns s8-srv python3 "$HERE/rig/nat_classify.py" observer \
        --ip1 "$SRV_IP" --ip2 203.0.113.101 --ip3 203.0.113.102 \
        --p1 19401 --p2 19402 >"$WORK/cobs.log" 2>&1 &
    COBS=$!
    sleep 1
    MA=$(ns s8-hA timeout 40 python3 "$HERE/rig/nat_classify.py" prober --json \
         --ip1 "$SRV_IP" --ip2 203.0.113.101 --ip3 203.0.113.102 --p1 19401 --p2 19402 \
         2>/dev/null | tail -1 | sed -n 's/.*"nat_type": *"\([^"]*\)".*/\1/p')
    MB=$(ns s8-hB timeout 40 python3 "$HERE/rig/nat_classify.py" prober --json \
         --ip1 "$SRV_IP" --ip2 203.0.113.101 --ip3 203.0.113.102 --p1 19401 --p2 19402 \
         2>/dev/null | tail -1 | sed -n 's/.*"nat_type": *"\([^"]*\)".*/\1/p')
    # See the note in run_matrix.sh: killing the backgrounded `sudo ip netns
    # exec` reaps only the wrapper, leaving the observer alive on 19401/19402.
    # And never `wait` on it -- the python grandchild still owns the redirected
    # fd, so `wait` blocks forever. Kill the wrapper, pkill the child by its
    # full lane-private path, move on.
    kill $COBS 2>/dev/null
    sudo -n pkill -f "$HERE/rig/nat_classify.py" 2>/dev/null
    sleep 0.3
    MA="${MA:-unmeasured}"; MB="${MB:-unmeasured}"
    echo "  declared A=$A B=$B | measured A=$MA B=$MB" >&2

    for rep in $(seq 1 "$REPS"); do
        stop_obs
        # Independent trials: see the same note in run_matrix.sh. The
        # classification pass and every earlier rep leave live conntrack and
        # xt_recent state behind, and that state changes the answer.
        "$NATNS" up "$A" "$B" >>"$WORK/up.log" 2>&1
        ns s8-srv python3 "$PM" observer --bind "${SRV_IP}:19500" \
            >"$WORK/obs_${A}_${B}_${rep}.log" 2>&1 &
        sleep 0.5
        HE="$WORK/hostext_${A}_${B}_${rep}"; JE="$WORK/joinext_${A}_${B}_${rep}"
        rm -f "$HE" "$JE"

        # HOST binds the fixed game port; JOINER binds ephemeral. The joiner
        # passes bind_port to STUN_DISCOVER (direct_p2p.c:3822) and every retry
        # re-runs it with local_port 0, so it binds a fresh OS-assigned port
        # (direct_p2p.c:4197).
        ns s8-hA python3 "$PM" peer --name host --bind-port 7000 \
            --srv "${SRV_IP}:19500" --ext-out "$HE" --peer-ext-file "$JE" \
            --start-delay-ms "$HOST_DELAY_MS" --duration-ms "$DURATION_MS" \
            >"$WORK/host_${A}_${B}_${rep}.json" 2>"$WORK/host_${A}_${B}_${rep}.err" &
        HP=$!
        ns s8-hB python3 "$PM" peer --name join --bind-port 0 \
            --srv "${SRV_IP}:19500" --ext-out "$JE" --peer-ext-file "$HE" \
            --start-delay-ms 0 --duration-ms "$((HOST_DELAY_MS + DURATION_MS))" \
            >"$WORK/join_${A}_${B}_${rep}.json" 2>"$WORK/join_${A}_${B}_${rep}.err"
        JRC=$?
        wait $HP; HRC=$?

        HJ=$(tail -1 "$WORK/host_${A}_${B}_${rep}.json" 2>/dev/null)
        JJ=$(tail -1 "$WORK/join_${A}_${B}_${rep}.json" 2>/dev/null)

        # Both sides must have concluded for the rep to be evidence: the host
        # and the joiner are each other's only source of punches, so a rep in
        # which either end failed to run says nothing about the NAT.
        HC=$(rep_class "$HRC" "$HJ"); JC=$(rep_class "$JRC" "$JJ")
        case "$HC$JC" in
            *DID_NOT_RUN*) STATUS=DID_NOT_RUN; not_run=$((not_run+1)) ;;
            *RIG_ERROR*)   STATUS=RIG_ERROR;   rig_errors=$((rig_errors+1)) ;;
            *)             STATUS=SCORED;      scored=$((scored+1)) ;;
        esac

        echo "  rep $rep: $STATUS (host_rc=$HRC join_rc=$JRC)" >&2
        printf '{"natA":"%s","natB":"%s","measA":"%s","measB":"%s","rep":%d,' \
            "$A" "$B" "$MA" "$MB" "$rep" >> "$OUT"
        printf '"status":"%s","host_rc":%s,"join_rc":%s,"host":%s,"join":%s}\n' \
            "$STATUS" "$HRC" "$JRC" "${HJ:-null}" "${JJ:-null}" >> "$OUT"
    done
    stop_obs
    "$NATNS" down >/dev/null 2>&1
done
done
echo "mech matrix done -> $OUT" >&2
echo "  scored=$scored rig_errors=$rig_errors did_not_run=$not_run" >&2

# ---------------------------------------------------------------------------
# EXIT CODE. Deliberately the same vocabulary as run_matrix.sh's tail, because
# these two drivers are read side by side and a reader should not have to learn
# two.
#
#   0  every rep ran on both sides and returned a finding (0 or 10)
#   4  at least one rep was scored, but some reps are rig errors or did-not-run
#      -- the JSONL is contaminated and the grid is not a clean grid
#   3  VACUOUS: nothing was scored at all
#
# NOTE: `trap cleanup EXIT` above does not disturb this. A bash EXIT trap that
# does not itself call `exit` leaves the script's status alone.
# ---------------------------------------------------------------------------
if [ "$scored" -eq 0 ]; then
    echo "mech_matrix.sh: VACUOUS RUN -- zero reps produced a finding." >&2
    exit 3
fi
if [ "$rig_errors" -gt 0 ] || [ "$not_run" -gt 0 ]; then
    echo "mech_matrix.sh: run is CONTAMINATED ($rig_errors rig errors, $not_run did-not-run)." >&2
    exit 4
fi
exit 0
