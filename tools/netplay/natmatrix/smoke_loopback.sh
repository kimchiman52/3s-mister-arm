#!/usr/bin/env bash
# Loopback smoke test for p2p_probe -- NO namespaces, NO NAT.
#
# Purpose: prove the instrument works (probe drives the real cascade, the STUN
# mock answers, the real rendezvous-server.js pairs the two peers, and the two
# probes reach HANDOFF) before any of it is judged inside a netns. If this does
# not connect, nothing in the NAT matrix means anything.
#
# This is the POSITIVE control for the whole harness.
#
# Usage: smoke_loopback.sh <path-to-p2p_probe> [workdir]
set -uo pipefail

PROBE="${1:?usage: smoke_loopback.sh <p2p_probe> [workdir]}"
WORK="${2:-/tmp/s8smoke}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

rm -rf "$WORK"; mkdir -p "$WORK/A" "$WORK/B"

cleanup() {
    [ -n "${STUN_PID:-}" ] && kill "$STUN_PID" 2>/dev/null
    [ -n "${RDV_PID:-}" ]  && kill "$RDV_PID"  2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

python3 "$HERE/rig/stun_mock.py" --bind 127.0.0.1:19302 --bind 127.0.0.1:19303 \
        > "$WORK/stun.log" 2>&1 &
STUN_PID=$!
node "$REPO/tools/rendezvous-server/rendezvous-server.js" 34780 \
        > "$WORK/rdv.log" 2>&1 &
RDV_PID=$!
sleep 1

STUN="127.0.0.1:19302,127.0.0.1:19303"
SIGNAL="udp://127.0.0.1:34780"
CODEF="$WORK/code.txt"

# HOME/XDG steer SDL_GetPrefPath so the two probes keep separate config files.
HOME="$WORK/A" XDG_DATA_HOME="$WORK/A" "$PROBE" --role host --port 7001 \
    --stun "$STUN" --signal "$SIGNAL" --code-file "$CODEF" \
    --timeout-ms 45000 > "$WORK/host.json" 2> "$WORK/host.err" &
HOST_PID=$!

sleep 2
HOME="$WORK/B" XDG_DATA_HOME="$WORK/B" "$PROBE" --role join --port 7002 \
    --stun "$STUN" --signal "$SIGNAL" --code-file "$CODEF" \
    --timeout-ms 45000 > "$WORK/join.json" 2> "$WORK/join.err"
JOIN_RC=$?
wait $HOST_PID; HOST_RC=$?

echo "--- host (rc=$HOST_RC) ---"; cat "$WORK/host.json"
echo "--- join (rc=$JOIN_RC) ---"; cat "$WORK/join.json"

if [ "$HOST_RC" = 0 ] && [ "$JOIN_RC" = 0 ]; then
    echo "SMOKE: PASS (both peers reached HANDOFF)"
    exit 0
fi
echo "SMOKE: FAIL (host rc=$HOST_RC join rc=$JOIN_RC)"
echo "--- host.err ---"; tail -20 "$WORK/host.err"
echo "--- join.err ---"; tail -20 "$WORK/join.err"
echo "--- rdv.log ---";  tail -20 "$WORK/rdv.log"
echo "--- stun.log ---"; tail -5  "$WORK/stun.log"
exit 1
