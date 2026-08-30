#!/usr/bin/env bash
# mapping_poison.sh -- isolate ONE mechanism behind the port-restricted row.
#
# QUESTION. A port-restricted NAT is supposed to DISCARD an inbound datagram it
# has no mapping for, and that discard must not change anything else. Does it?
#
# METHOD. host = port-restricted, joiner = FULLCONE. The fullcone joiner admits a
# datagram from any source port, so it can REPORT the source port the host's
# punch actually carried -- which is the quantity in question and which no
# port-restricted peer could ever observe (it would have discarded it). The host
# binds 7000 and its server-observed external port is 7000, so an
# endpoint-independent mapping must put 7000 on the punch as well.
#
# ORDER MATTERS and is the cascade's: the joiner punches the host FIRST (race
# leg 0, direct_p2p.c:1734 -- armed from the room code), and only afterwards does
# the host punch back (direct_p2p.c:2068, leg 1, armed by the rendezvous DELIVER).
#
# ARMS.
#   none       baseline -- the rule set as natns.sh had it for task-42.
#   noicmp     drop only the ICMP port-unreachable the NAT box emits for the
#              refused datagram. Separates "the ICMP error did it" from
#              "the conntrack entry did it": the entry is still created here.
#   inputdrop  drop the refused datagram in filter INPUT. That chain runs at
#              priority NF_IP_PRI_FILTER (0), BEFORE nf_conntrack_confirm at
#              LOCAL_IN priority NF_IP_PRI_CONNTRACK_CONFIRM (INT_MAX), so no
#              conntrack entry is created at all -- which is what a real
#              NAT/firewall does with unsolicited WAN input.
#
# Usage: mapping_poison.sh [none|noicmp|inputdrop]
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
NATNS="$HERE/natns.sh"
PM="$HERE/rig/punch_mech.py"
MODE="${1:-none}"
WORK="${WORK:-/tmp/s8poison}"
SRV_IP=203.0.113.100

ns() { sudo -n ip netns exec "$1" "${@:2}"; }
cleanup() { sudo -n pkill -f "$PM" 2>/dev/null; "$NATNS" down >/dev/null 2>&1; }
trap cleanup EXIT

mkdir -p "$WORK"
"$NATNS" up port-restricted fullcone >/dev/null 2>&1 || { echo "up failed"; exit 2; }

# natns.sh may already install the INPUT drop (that is the task-102 fix). Take
# the arm's rule set from a known state so every arm means what it says.
ns s8-nA iptables -F INPUT
ns s8-nB iptables -F INPUT
ns s8-nA iptables -F OUTPUT
case "$MODE" in
  none) ;;
  noicmp)
    ns s8-nA iptables -A OUTPUT -o nAo -p icmp --icmp-type port-unreachable -j DROP ;;
  inputdrop)
    ns s8-nA iptables -A INPUT -i nAo -p udp -j DROP ;;
  *) echo "mode must be none|noicmp|inputdrop" >&2; exit 2 ;;
esac

ns s8-srv python3 "$PM" observer --bind "${SRV_IP}:19500" >"$WORK/obs.log" 2>&1 &
sleep 0.5
HE="$WORK/he"; JE="$WORK/je"; rm -f "$HE" "$JE"

ns s8-hA python3 "$PM" peer --name host --bind-port 7000 --srv "${SRV_IP}:19500" \
    --ext-out "$HE" --peer-ext-file "$JE" --start-delay-ms 1500 --duration-ms 3000 \
    >"$WORK/host.json" 2>"$WORK/host.err" &
HP=$!
ns s8-hB python3 "$PM" peer --name join --bind-port 0 --srv "${SRV_IP}:19500" \
    --ext-out "$JE" --peer-ext-file "$HE" --start-delay-ms 0 --duration-ms 4500 \
    >"$WORK/join.json" 2>"$WORK/join.err"
wait $HP 2>/dev/null

python3 - "$MODE" "$WORK/join.json" "$WORK/host.json" <<'PY'
import json, sys
mode, jp, hp = sys.argv[1], sys.argv[2], sys.argv[3]
j = json.loads(open(jp).read().strip().splitlines()[-1])
h = json.loads(open(hp).read().strip().splitlines()[-1])
tgt = j.get("target")                      # the host's server-observed endpoint
src = j.get("first_punch_src")             # where the host's punch actually came from
print(f"ARM={mode}")
print(f"  host external endpoint as the SERVER saw it (= the room code): {h.get('ext')}")
print(f"  joiner was told to punch:                                     {tgt}")
print(f"  the host's punch actually ARRIVED from:                       {src}")
if src is None:
    print("  VERDICT: the joiner heard nothing at all")
else:
    same = src.rsplit(":", 1)[1] == str(tgt).rsplit(":", 1)[1]
    print("  VERDICT: mapping is ENDPOINT-INDEPENDENT (port preserved)" if same
          else "  VERDICT: MAPPING STOLEN -- the refused inbound punch took the port")
PY
