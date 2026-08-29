#!/usr/bin/env bash
# run_all.sh -- the full S8 experiment set, in one detached pass.
#
# Order matters: non-vacuity FIRST. If the harness cannot be shown to connect
# where it should and fail where it should, the matrix numbers below are not
# evidence of anything and there is no point collecting them.
#
# Usage: run_all.sh --probe /path/to/p2p_probe [--out DIR] [--reps N]
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROBE=""; OUT="/tmp/s8results"; REPS=3
while [ $# -gt 0 ]; do
    case "$1" in
        --probe) PROBE="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --reps) REPS="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -x "$PROBE" ] || { echo "run_all.sh: --probe must be executable" >&2; exit 2; }
mkdir -p "$OUT"

echo "################ 0. NON-VACUITY ################"
"$HERE/nonvacuity.sh" --probe "$PROBE" --work "$OUT/nonvac" 2>&1 | tee "$OUT/nonvacuity.txt"
NV=${PIPESTATUS[0]}
echo "nonvacuity rc=$NV"

echo
echo "################ 1. BASELINE MATRIX (no impairment) ################"
"$HERE/run_matrix.sh" --probe "$PROBE" --reps "$REPS" --label baseline \
    --out "$OUT/baseline.jsonl" 2>&1 | tail -60

echo
echo "################ 2. DELIVER LOSS = 100% toward the HOST ################"
# The server's unsolicited DELIVER push is a bare socket.send with no retransmit
# (rendezvous-server.js:719). Dropping it forces the host to learn the peer only
# from the reply to its OWN next REGISTER -- a full register interval later
# (direct_p2p.c:2664). This is the mechanism that manufactures the start skew
# behind the residual split-brain band.
"$HERE/run_matrix.sh" --probe "$PROBE" --reps "$REPS" --label deliverloss100 \
    --types "port-restricted" --deliver-loss 100 \
    --out "$OUT/deliverloss100.jsonl" 2>&1 | tail -30

echo
echo "################ 3. DELIVER LOSS = 50% ################"
"$HERE/run_matrix.sh" --probe "$PROBE" --reps "$REPS" --label deliverloss50 \
    --types "port-restricted" --deliver-loss 50 \
    --out "$OUT/deliverloss50.jsonl" 2>&1 | tail -30

echo
echo "################ 4. HIGH ONE-WAY DELAY (asymmetric) ################"
# G >= 2d is the split-brain convergence condition, G = RACE_PUNCH_SETTLE_MS =
# 600 ms (direct_p2p.c:1348). d > 300 ms opens the band. 400/250 ms one-way is
# past that threshold on the A->B leg.
"$HERE/run_matrix.sh" --probe "$PROBE" --reps "$REPS" --label owd_asym \
    --types "port-restricted symmetric" --owd-a 400 --owd-b 250 \
    --out "$OUT/owd_asym.jsonl" 2>&1 | tail -40

echo
echo "################ 5. DELIVER LOSS + HIGH DELAY (band hunt) ################"
"$HERE/run_matrix.sh" --probe "$PROBE" --reps "$REPS" --label band_hunt \
    --types "port-restricted" --owd-a 400 --owd-b 400 --deliver-loss 100 \
    --out "$OUT/band_hunt.jsonl" 2>&1 | tail -30

echo
echo "################ 6. UNIFORM PACKET LOSS 10% ################"
"$HERE/run_matrix.sh" --probe "$PROBE" --reps "$REPS" --label loss10 \
    --types "port-restricted symmetric" --loss 10 \
    --out "$OUT/loss10.jsonl" 2>&1 | tail -40

echo
echo "################ SUMMARY ################"
python3 "$HERE/summarize.py" "$OUT"/*.jsonl 2>&1 | tee "$OUT/summary.md"
echo "RUN_ALL_DONE nonvacuity_rc=$NV"
