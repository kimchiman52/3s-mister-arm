#!/usr/bin/env bash
# natns.sh -- Linux network-namespace topology for the S8 NAT traversal matrix.
#
# Topology (all namespaces prefixed s8- so teardown is lane-private):
#
#   [s8-hA 10.1.0.2]--hA0/nAi--[s8-nA]--nAo/wA--+
#                                                |
#                                        [s8-wan br0]--wS/srv0--[s8-srv 203.0.113.100]
#                                                |
#   [s8-hB 10.2.0.2]--hB0/nBi--[s8-nB]--nBo/wB--+
#
#   WAN side:  s8-nA = 203.0.113.10, s8-nB = 203.0.113.20, s8-srv = 203.0.113.100
#
# The two hosts are on overlapping-purpose private LANs behind independent NATs and
# can only ever address each other by the NAT's public WAN address -- exactly the
# real topology. There is no route between 10.1.0.0/24 and 10.2.0.0/24.
#
# Usage:
#   natns.sh up   <natA_type> <natB_type>
#   natns.sh down
#   natns.sh netem <side:A|B|both> <delay_ms> <jitter_ms> <loss_pct>   # asymmetric-capable
#   natns.sh exec  <hA|hB|srv|nA|nB|wan> <cmd...>
#
# NAT types: fullcone | addr-restricted | port-restricted | symmetric | none
# NOTE: the *declared* type is only the rule set installed. The *actual* emulated
# behaviour is measured separately by rig/nat_classify.py -- never trust the label.

set -euo pipefail

NS_PREFIX="s8"
HA="${NS_PREFIX}-hA"; NA="${NS_PREFIX}-nA"
HB="${NS_PREFIX}-hB"; NB="${NS_PREFIX}-nB"
WAN="${NS_PREFIX}-wan"; SRV="${NS_PREFIX}-srv"
ALL_NS=("$HA" "$NA" "$HB" "$NB" "$WAN" "$SRV")

LAN_A="10.1.0"; LAN_B="10.2.0"
WAN_NET="203.0.113"
EXT_A="${WAN_NET}.10"; EXT_B="${WAN_NET}.20"
SRV_IP="${WAN_NET}.100"; SRV_IP2="${WAN_NET}.101"; SRV_IP3="${WAN_NET}.102"

# The single UDP port the game binds on each host. Used by the fullcone static map.
GAME_PORT="${GAME_PORT:-7000}"

SUDO="sudo -n"

ipns() { local ns="$1"; shift; $SUDO ip netns exec "$ns" "$@"; }

down() {
    for ns in "${ALL_NS[@]}"; do
        $SUDO ip netns del "$ns" 2>/dev/null || true
    done
    # veths live inside the netns and die with them; sweep any strays in root ns.
    for l in hA0 nAi nAo wA hB0 nBi nBo wB wS srv0; do
        $SUDO ip link del "$l" 2>/dev/null || true
    done
    return 0
}

mk_veth() { # mk_veth <ifA> <nsA> <ifB> <nsB>
    $SUDO ip link add "$1" type veth peer name "$3"
    $SUDO ip link set "$1" netns "$2"
    $SUDO ip link set "$3" netns "$4"
}

# ---------------------------------------------------------------------------
# NAT rule sets. All operate on the NAT namespace's WAN-side interface ($wif),
# translating for the single internal host $inner.
#
# Terminology (RFC 4787): mapping behaviour = how the external port is chosen;
# filtering behaviour = which inbound sources are admitted.
#   fullcone         EIM + endpoint-independent filtering
#   addr-restricted  EIM + address-dependent filtering
#   port-restricted  EIM + address-and-port-dependent filtering  (conntrack default)
#   symmetric        endpoint-DEPENDENT mapping (new port per destination)
# ---------------------------------------------------------------------------
apply_nat() { # apply_nat <ns> <wif> <ext_ip> <inner_ip> <type>
    local ns="$1" wif="$2" ext="$3" inner="$4" type="$5"

    ipns "$ns" sysctl -qw net.ipv4.ip_forward=1
    # Keep UDP conntrack entries alive long enough to span the cascade's timers
    # (the rendezvous REGISTER interval alone is 5 s).
    ipns "$ns" sysctl -qw net.netfilter.nf_conntrack_udp_timeout=120 2>/dev/null || true
    ipns "$ns" sysctl -qw net.netfilter.nf_conntrack_udp_timeout_stream=180 2>/dev/null || true

    case "$type" in
      none)
        # Pure router, no translation. Control cell: proves the rig itself is not
        # what breaks a pairing.
        ipns "$ns" iptables -t nat -A POSTROUTING -o "$wif" -j ACCEPT
        ;;

      symmetric)
        # --random-fully forces a fresh, randomly-chosen external port for every
        # distinct destination tuple => endpoint-dependent mapping.
        ipns "$ns" iptables -t nat -A POSTROUTING -o "$wif" -p udp \
             -j MASQUERADE --random-fully
        ipns "$ns" iptables -t nat -A POSTROUTING -o "$wif" ! -p udp -j MASQUERADE
        ;;

      port-restricted)
        # Plain SNAT to the WAN address. nf_nat preserves the source port when it
        # is free => EIM. conntrack admits inbound only from the exact addr+port
        # already contacted => address-and-port-dependent filtering.
        ipns "$ns" iptables -t nat -A POSTROUTING -o "$wif" -j SNAT --to-source "$ext"
        ;;

      addr-restricted)
        # EIM as above, plus: remember every destination ADDRESS we have sent to
        # (xt_recent list "s8sent"), and admit any inbound packet from a remembered
        # address regardless of its source PORT => address-dependent filtering.
        ipns "$ns" iptables -t nat -A POSTROUTING -o "$wif" -j SNAT --to-source "$ext"
        ipns "$ns" iptables -t mangle -A POSTROUTING -o "$wif" -p udp \
             -m recent --set --name s8sent --rdest
        ipns "$ns" iptables -t nat -A PREROUTING -i "$wif" -p udp \
             -m recent --rcheck --seconds 120 --name s8sent --rsource \
             -j DNAT --to-destination "$inner"
        ;;

      fullcone)
        # EIM via port-preserving SNAT, plus a blanket inbound DNAT to the single
        # host behind this NAT. Every inbound UDP datagram is forwarded in with its
        # destination port intact, from ANY source => endpoint-independent
        # filtering.
        #
        # Deliberately NOT keyed to a fixed port: only the HOST binds a chosen port.
        # The JOINER binds local_port 0 and gets an OS-assigned ephemeral port
        # (src/netplay/direct_p2p.c:3226), so a fixed-port map would silently fail
        # to emulate full cone on the joiner side and would corrupt that column.
        ipns "$ns" iptables -t nat -A POSTROUTING -o "$wif" -j SNAT --to-source "$ext"
        ipns "$ns" iptables -t nat -A PREROUTING  -i "$wif" -p udp \
             -j DNAT --to-destination "$inner"
        ;;

      *) echo "natns.sh: unknown NAT type '$type'" >&2; return 2 ;;
    esac
}

up() {
    local typeA="$1" typeB="$2"
    down

    for ns in "${ALL_NS[@]}"; do $SUDO ip netns add "$ns"; done
    for ns in "${ALL_NS[@]}"; do ipns "$ns" ip link set lo up; done

    mk_veth hA0 "$HA" nAi "$NA"
    mk_veth nAo "$NA" wA  "$WAN"
    mk_veth hB0 "$HB" nBi "$NB"
    mk_veth nBo "$NB" wB  "$WAN"
    mk_veth srv0 "$SRV" wS "$WAN"

    # WAN backbone bridge
    ipns "$WAN" ip link add br0 type bridge
    ipns "$WAN" ip link set br0 up
    for l in wA wB wS; do
        ipns "$WAN" ip link set "$l" master br0
        ipns "$WAN" ip link set "$l" up
    done

    # Host A
    ipns "$HA" ip addr add "${LAN_A}.2/24" dev hA0
    ipns "$HA" ip link set hA0 up
    ipns "$HA" ip route add default via "${LAN_A}.1"

    # NAT A
    ipns "$NA" ip addr add "${LAN_A}.1/24" dev nAi
    ipns "$NA" ip addr add "${EXT_A}/24" dev nAo
    ipns "$NA" ip link set nAi up; ipns "$NA" ip link set nAo up

    # Host B
    ipns "$HB" ip addr add "${LAN_B}.2/24" dev hB0
    ipns "$HB" ip link set hB0 up
    ipns "$HB" ip route add default via "${LAN_B}.1"

    # NAT B
    ipns "$NB" ip addr add "${LAN_B}.1/24" dev nBi
    ipns "$NB" ip addr add "${EXT_B}/24" dev nBo
    ipns "$NB" ip link set nBi up; ipns "$NB" ip link set nBo up

    # Server (STUN + rendezvous) sits on the open WAN, no NAT. It carries a second
    # address so the NAT classifier can distinguish address-dependent from
    # port-dependent mapping/filtering (RFC 4787 needs two distinct server IPs).
    ipns "$SRV" ip addr add "${SRV_IP}/24" dev srv0
    ipns "$SRV" ip addr add "${SRV_IP2}/24" dev srv0
    # SRV_IP3 is reflection-only: the classifier never sends TO it, so an
    # address-restricted NAT has no reason to admit it. Without a third address
    # the filtering test is contaminated by the mapping test and every
    # address-restricted cell mismeasures as full cone.
    ipns "$SRV" ip addr add "${SRV_IP3}/24" dev srv0
    ipns "$SRV" ip link set srv0 up

    # Return routes for the un-NATted ("none" = open host) case. When a side runs
    # no NAT its packets keep their private source address, so the WAN needs a way
    # back. With a NAT installed nothing on the WAN ever addresses the private
    # prefix, so these routes are inert -- they only make the "none" cell work.
    ipns "$SRV" ip route add "${LAN_A}.0/24" via "$EXT_A"
    ipns "$SRV" ip route add "${LAN_B}.0/24" via "$EXT_B"
    ipns "$NA"  ip route add "${LAN_B}.0/24" via "$EXT_B"
    ipns "$NB"  ip route add "${LAN_A}.0/24" via "$EXT_A"

    apply_nat "$NA" nAo "$EXT_A" "${LAN_A}.2" "$typeA"
    apply_nat "$NB" nBo "$EXT_B" "${LAN_B}.2" "$typeB"
}

# netem is installed on the NAT WAN-side egress, so delay/loss applies to the
# public path only -- the LAN legs stay clean, as in reality. Applying to one
# side only produces a genuinely asymmetric one-way delay.
netem() { # netem <A|B|both> <delay_ms> <jitter_ms> <loss_pct>
    local side="$1" d="$2" j="$3" loss="$4"
    _one() {
        local ns="$1" dev="$2"
        ipns "$ns" tc qdisc del dev "$dev" root 2>/dev/null || true
        [ "$d" = "0" ] && [ "$loss" = "0" ] && return 0
        ipns "$ns" tc qdisc add dev "$dev" root netem \
            delay "${d}ms" "${j}ms" loss "${loss}%"
    }
    case "$side" in
      A) _one "$NA" nAo ;;
      B) _one "$NB" nBo ;;
      both) _one "$NA" nAo; _one "$NB" nBo ;;
      *) echo "natns.sh netem: side must be A|B|both" >&2; return 2 ;;
    esac
}

# Drop rendezvous DELIVER frames leaving the server, with a probability.
#
# A DELIVER is uniquely identifiable on the wire: bytes [0..3] are the '3SXR'
# magic 0x33535852 (rendezvous.c:21), byte [4] is version 2 (rendezvous.c:28) and
# byte [5] is REND_TYPE_DELIVER = 2 (rendezvous.c:30). So the 6-byte prefix
# 33 53 58 52 02 02 matches DELIVER and nothing else.
#
# This models the real mechanism: the server's unsolicited push to the host at
# rendezvous-server.js:719 is a bare socket.send with NO retransmit, so when it is
# lost the host learns the peer endpoint only from the reply to its OWN next
# REGISTER -- a full register-interval later (direct_p2p.c:2543).
deliverloss() { # deliverloss <pct 0-100> <A|B|both>
    local pct="$1" who="${2:-A}"
    ipns "$SRV" iptables -F OUTPUT 2>/dev/null || true
    [ "$pct" = "0" ] && return 0
    local prob
    prob=$(awk -v p="$pct" 'BEGIN{printf "%.6f", p/100.0}')
    _drop_to() {
        ipns "$SRV" iptables -A OUTPUT -p udp -d "$1" \
            -m string --algo bm --hex-string '|335358520202|' --from 28 --to 34 \
            -m statistic --mode random --probability "$prob" -j DROP
    }
    case "$who" in
      A) _drop_to "$EXT_A" ;;
      B) _drop_to "$EXT_B" ;;
      both) _drop_to "$EXT_A"; _drop_to "$EXT_B" ;;
      *) echo "natns.sh deliverloss: who must be A|B|both" >&2; return 2 ;;
    esac
}

case "${1:-}" in
  up)    shift; up "${1:?natA type}" "${2:?natB type}" ;;
  down)  down ;;
  netem) shift; netem "$@" ;;
  deliverloss) shift; deliverloss "$@" ;;
  exec)  shift; ns="$1"; shift
         case "$ns" in
           hA) ns="$HA";; hB) ns="$HB";; srv) ns="$SRV";;
           nA) ns="$NA";; nB) ns="$NB";; wan) ns="$WAN";;
         esac
         exec $SUDO ip netns exec "$ns" "$@" ;;
  *) sed -n '2,30p' "$0"; exit 2 ;;
esac
