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
#   natns.sh portmap <A|B|both> <up|down> [natpmp_mock.py args...]
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
# "none" = an OPEN host with no gateway at all. Such a host must be addressed
# PUBLICLY, not out of RFC1918: the production client refuses to punch a peer
# whose rendezvous-DELIVERed endpoint is private
# (direct_p2p_is_lan_peer, src/netplay/direct_p2p.c:838; the host-side refusal
# is at :4774-4777, "DELIVER peer is LAN (%s); staying HOST_WAITING"). With the
# open side on 10.x.0.2 the guard fires and every <natted-host> x none cell
# reads 0/N -- which measures the rig's ADDRESSING CHOICE, not the product.
# Measured before this change: port-restricted x none 0/3 and symmetric x none
# 0/3, both with the host log line above and the joiner never punched back.
# 198.51.100.0/24 (TEST-NET-2) and 192.0.2.0/24 (TEST-NET-1) are public,
# non-RFC1918, and distinct from the 203.0.113.0/24 (TEST-NET-3) WAN.
OPEN_A="198.51.100"; OPEN_B="192.0.2"
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

    # -----------------------------------------------------------------------
    # A NAT DISCARDS the inbound datagram its filtering rule refuses. These NAT
    # namespaces are real Linux hosts with a default-ACCEPT filter INPUT policy,
    # and the namespace OWNS $ext -- so WITHOUT this rule a refused datagram is
    # not discarded, it is delivered to the NAT box's own IP stack, and
    # conntrack CONFIRMS it: nf_conntrack_confirm sits at LOCAL_IN priority
    # NF_IP_PRI_CONNTRACK_CONFIRM (INT_MAX), i.e. AFTER the filter INPUT chain.
    # Nothing forwards the packet, but the entry is now real and [UNREPLIED].
    #
    # That phantom entry's ORIGINAL tuple is (peerExt:pport -> ourExt:GAME_PORT)
    # -- bit-for-bit the REPLY tuple the inner host's own outbound punch to that
    # same peer needs. nf_nat_used_tuple() therefore refuses to preserve the
    # source port, and our punch leaves from a DIFFERENT external port. The peer
    # punched ourExt:GAME_PORT, so a port-restricted peer discards the answer and
    # neither side ever hears the other.
    #
    # In other words: without this rule the rig loses endpoint-independent
    # mapping for exactly the endpoint being punched, purely because that
    # endpoint punched us first -- behaviour no NAT has. It turned the whole
    # port-restricted row of the matrix into a false negative (task-102,
    # rediscovered as task #126 because task-102's fix was never merged).
    # A real NAT/firewall drops unsolicited WAN input in the INPUT chain, which
    # runs BEFORE the confirm hook, so no entry is ever created.
    #
    # THIS IS THE INBOUND PATH FOR port-restricted AND symmetric, which install
    # no DNAT of their own. It is not "drop everything": a datagram from an
    # endpoint the inner host HAS sent to matches the conntrack entry that
    # outbound flow created, is reverse-NAT'd in nat PREROUTING and routed
    # onward, so it traverses FORWARD and never reaches INPUT. That is exactly
    # RFC 4787 address-and-port-dependent filtering. Only the UNSOLICITED
    # datagram -- the one a real port-restricted NAT drops -- lands here. Same
    # for the fullcone / addr-restricted DNAT paths, which rewrite the
    # destination in PREROUTING before the routing decision.
    #
    # Scoped to $wif (the WAN side) so the LAN-facing NAT-PMP mock on
    # ${lan}.1:5351 is untouched -- see portmap() below.
    ipns "$ns" iptables -A INPUT -i "$wif" -p udp -j DROP

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
        # The JOINER passes bind_port to STUN_DISCOVER
        # (src/netplay/direct_p2p.c:3822) and that is 0 on every first attempt,
        # so it gets an OS-assigned ephemeral port; a fixed-port map would
        # silently fail to emulate full cone on the joiner side and would
        # corrupt that column.
        #
        # Task #121 note: bind_port is no longer ALWAYS 0. A joiner whose
        # background port-map probe returned a mapping binds that mapping's
        # internal port on its retry. The blanket DNAT above is unaffected --
        # it forwards every inbound UDP port to the single inner host, so it
        # emulates full cone for whichever port the joiner ends up on. The
        # reason this rule is portless is now stronger, not weaker.
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

    # An un-NATted side keeps its own source address on the WAN, so that address
    # has to be one a real peer could route to and one the client will accept as
    # a peer. See the OPEN_A/OPEN_B note at the top of this file.
    local lanA="$LAN_A" lanB="$LAN_B"
    [ "$typeA" = none ] && lanA="$OPEN_A"
    [ "$typeB" = none ] && lanB="$OPEN_B"

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
    ipns "$HA" ip addr add "${lanA}.2/24" dev hA0
    ipns "$HA" ip link set hA0 up
    ipns "$HA" ip route add default via "${lanA}.1"

    # NAT A
    ipns "$NA" ip addr add "${lanA}.1/24" dev nAi
    ipns "$NA" ip addr add "${EXT_A}/24" dev nAo
    ipns "$NA" ip link set nAi up; ipns "$NA" ip link set nAo up

    # Host B
    ipns "$HB" ip addr add "${lanB}.2/24" dev hB0
    ipns "$HB" ip link set hB0 up
    ipns "$HB" ip route add default via "${lanB}.1"

    # NAT B
    ipns "$NB" ip addr add "${lanB}.1/24" dev nBi
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
    # no NAT its packets keep their own source address, so the WAN needs a way
    # back. With a NAT installed nothing on the WAN ever addresses that prefix,
    # so these routes are inert -- they only make the "none" cell work.
    ipns "$SRV" ip route add "${lanA}.0/24" via "$EXT_A"
    ipns "$SRV" ip route add "${lanB}.0/24" via "$EXT_B"
    ipns "$NA"  ip route add "${lanB}.0/24" via "$EXT_B"
    ipns "$NB"  ip route add "${lanA}.0/24" via "$EXT_A"

    apply_nat "$NA" nAo "$EXT_A" "${lanA}.2" "$typeA"
    apply_nat "$NB" nBo "$EXT_B" "${lanB}.2" "$typeB"
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
# rendezvous-server.js:1252 is a bare socket.send with NO retransmit, so when it is
# lost the host learns the peer endpoint only from the reply to its OWN next
# REGISTER -- a full register-interval later (direct_p2p.c:3242).
#
# The drop is installed at the RECEIVING side's NAT ingress (mangle PREROUTING on
# its WAN interface), NOT in the server's OUTPUT chain. That distinction matters:
# a local OUTPUT DROP returns EPERM to the sender, so node's socket.send would
# raise and the server would behave differently from reality. Dropping in transit
# is what actually happens on the internet -- the server's send succeeds and the
# datagram simply never arrives.
deliverloss() { # deliverloss <pct 0-100> <A|B|both>
    local pct="$1" who="${2:-A}"
    ipns "$NA" iptables -t mangle -F PREROUTING 2>/dev/null || true
    ipns "$NB" iptables -t mangle -F PREROUTING 2>/dev/null || true
    [ "$pct" = "0" ] && return 0
    local prob
    prob=$(awk -v p="$pct" 'BEGIN{printf "%.6f", p/100.0}')
    _drop_at() { # _drop_at <ns> <wan-if>
        ipns "$1" iptables -t mangle -A PREROUTING -i "$2" -p udp -s "$SRV_IP" \
            -m string --algo bm --hex-string '|335358520202|' --from 28 --to 34 \
            -m statistic --mode random --probability "$prob" -j DROP
    }
    case "$who" in
      A) _drop_at "$NA" nAo ;;
      B) _drop_at "$NB" nBo ;;
      both) _drop_at "$NA" nAo; _drop_at "$NB" nBo ;;
      *) echo "natns.sh deliverloss: who must be A|B|both" >&2; return 2 ;;
    esac
}

# ---------------------------------------------------------------------------
# NAT-PMP / PCP gateway mock (rig/natpmp_mock.py), one per NAT namespace.
#
# Task #121 gives the JOINER a port-mapping probe. The netns had no gateway at
# all, which is why probe/p2p_probe.c:143-145 disables UPnP and NAT-PMP in every
# cell's config. This starts a gateway that speaks the real protocol on
# 10.x.0.1:5351 and, on a granted mapping, installs static DNAT/SNAT rules at
# the HEAD of the nat chains -- so the mapping actually overrides the dynamic
# rules apply_nat() installed above, including symmetric's
# MASQUERADE --random-fully.
#
# Point the production client at it with Natpmp_TestHook_SetGateway(<lan>.1,
# 5351) (src/netplay/natpmp.h:345); that hook is consulted at natpmp.c:757-763,
# BEFORE the test-build refusal to read the real default route at :764-785.
#
# TEARDOWN. `natns.sh down` is deliberately left untouched (other lanes depend
# on it byte-for-byte), so it does NOT stop these daemons. Call
# `natns.sh portmap both down` before `natns.sh down`, or the python process
# lingers -- harmlessly, in a namespace that no longer exists, but it lingers.
#
# RIG NOTE, measured while building this, and FIXED in apply_nat() as of task
# #126. A NAT namespace OWNS its WAN address, and for symmetric/port-restricted
# (which install no inbound DNAT) a datagram aimed at ext_ip:P used to be
# delivered LOCALLY to the NAT namespace instead of being dropped. That created
# a conntrack entry occupying ext_ip:P, and nf_nat then refused to hand that same
# port to the inner host's own outbound flow -- so a peer punching at an UNMAPPED
# port on the far side pushed that side's port-preserving SNAT off its expected
# external port. It was pre-existing behaviour of apply_nat(), not of this
# daemon, and it invalidated any test that assumes "port-restricted side keeps
# its internal port on the outside".
#
# apply_nat() now installs `-A INPUT -i $wif -p udp -j DROP` in every NAT
# namespace, which runs before the conntrack confirm hook, so the phantom entry
# is never created and the confound is gone. Giving the far side a mapping
# (portmap both up) is therefore no longer needed to work around it -- it is only
# needed when the far side genuinely should have a port mapping.
#
# Killing a backgrounded `sudo ip netns exec ...` reaps only the sudo wrapper;
# the python child survives and keeps UDP 5351 bound (same hazard run_matrix.sh
# documents for the STUN/rendezvous mocks). So `down` also pkills the child by
# its full, lane-private command line -- never an unscoped pkill.
PORTMAP_MOCK="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/rig/natpmp_mock.py"
PORTMAP_RUNDIR="${PORTMAP_RUNDIR:-/tmp/s8-portmap}"

portmap() { # portmap <A|B|both> <up|down> [extra natpmp_mock.py args...]
    local side="$1" act="$2"; shift 2 || true
    if [ "$side" = "both" ]; then
        portmap A "$act" "$@"; portmap B "$act" "$@"; return 0
    fi
    local ns wif ext inner lan
    case "$side" in
      # These are the NATTED prefixes on purpose. A side declared `none` has no
      # NAT and is addressed out of OPEN_A/OPEN_B instead, so a port mapping on
      # it would be meaningless -- there is nothing to map through. `portmap`
      # is undefined for a `none` side and run_matrix.sh's JOINER_LAN_GW
      # (10.2.0.1) assumes side B is natted, which every --natpmp-joiner cell is.
      A) ns="$NA"; wif=nAo; ext="$EXT_A"; inner="${LAN_A}.2"; lan="${LAN_A}.1" ;;
      B) ns="$NB"; wif=nBo; ext="$EXT_B"; inner="${LAN_B}.2"; lan="${LAN_B}.1" ;;
      *) echo "natns.sh portmap: side must be A|B|both" >&2; return 2 ;;
    esac
    mkdir -p "$PORTMAP_RUNDIR"
    local log="$PORTMAP_RUNDIR/$side.log" pidf="$PORTMAP_RUNDIR/$side.pid"
    # The --listen address is unique per side, so this pattern matches this
    # side's daemon and nothing else in the VM.
    local pat="natpmp_mock.py --listen $lan"

    case "$act" in
      up)
        portmap "$side" down >/dev/null 2>&1 || true
        : > "$log"
        $SUDO ip netns exec "$ns" python3 "$PORTMAP_MOCK" \
            --listen "$lan" --external-ip "$ext" --inner-ip "$inner" \
            --wan-if "$wif" "$@" >>"$log" 2>&1 &
        echo $! > "$pidf"
        # The daemon prints "ready on <ip>:5351 ..." once its socket is bound.
        # Waiting on that rather than on a fixed sleep means a slow VM cannot
        # produce a cell whose first NAT-PMP request hit a closed port.
        local i
        for i in $(seq 1 100); do
            grep -q 'ready on' "$log" 2>/dev/null && return 0
            kill -0 "$(cat "$pidf")" 2>/dev/null || break
            sleep 0.1
        done
        echo "natns.sh portmap $side up: mock did not become ready; log:" >&2
        cat "$log" >&2
        return 1
        ;;
      down)
        if [ -f "$pidf" ]; then
            kill "$(cat "$pidf")" 2>/dev/null || true
            rm -f "$pidf"
        fi
        $SUDO pkill -f "$pat" 2>/dev/null || true
        return 0
        ;;
      *) echo "natns.sh portmap: action must be up|down" >&2; return 2 ;;
    esac
}

case "${1:-}" in
  up)    shift; up "${1:?natA type}" "${2:?natB type}" ;;
  down)  down ;;
  netem) shift; netem "$@" ;;
  portmap) shift; portmap "${1:?side A|B|both}" "${2:?up|down}" "${@:3}" ;;
  deliverloss) shift; deliverloss "$@" ;;
  exec)  shift; ns="$1"; shift
         case "$ns" in
           hA) ns="$HA";; hB) ns="$HB";; srv) ns="$SRV";;
           nA) ns="$NA";; nB) ns="$NB";; wan) ns="$WAN";;
         esac
         exec $SUDO ip netns exec "$ns" "$@" ;;
  *) sed -n '2,31p' "$0"; exit 2 ;;
esac
