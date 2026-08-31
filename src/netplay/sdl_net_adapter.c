#include "netplay/sdl_net_adapter.h"

#include "netplay/late_punch.h"
#include "netplay/rendezvous.h"
#include "netplay/stun.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define MAX_NETWORK_RESULTS 128

static NET_DatagramSocket* adapter_sock = NULL;
static GekkoNetAdapter adapter;
static GekkoNetResult* results[MAX_NETWORK_RESULTS];
static int result_count = 0;

static NET_Address* cached_remote = NULL;
static Uint16 cached_port = 0;

// === Task #119: peer-endpoint relearn, at the adapter seam ===
//
// GekkoNet registers the remote actor ONCE, by "ip:port" string, at
// configure time (netplay.c configure_gekko), and its inbound handlers
// match by a raw byte-compare of that string — NetAddress::Equals is a
// size check + memcmp in the vendored library's backend (upstream
// GekkoLib source @ 7be848c; only build/include + build/lib ship here) —
// with no relearn API on the public surface (gekkonet.h). When the peer
// legitimately moves — the S2 joiner auto-retry punches back in from a
// FRESH socket, i.e. a new NAT mapping — GekkoNet would send to a dead
// endpoint and ignore the live one forever. Rather than patch the
// vendored library (pinned @ 7be848c), the adapter translates:
//
//   send:    the registered (canonical) address string keeps resolving
//            to wherever the authenticated peer actually is now;
//   receive: datagrams from the current actual endpoint are PRESENTED
//            to GekkoNet under the canonical string it registered.
//
// GekkoNet never observes the move. The ONLY writer of the actual
// endpoint is netplay.c's relearn application, which acts exclusively
// on LatePunch's token-authenticated, same-IP-restricted verdict
// (late_punch.h) — an unauthenticated source can shift nothing here.
// Until SDLNetAdapter_RetargetPeer first diverges from the canonical
// endpoint, every path below is byte-for-byte the pre-#119 behaviour.
static char s_canonical_peer[64] = { 0 };
static char s_actual_peer[64] = { 0 };
static bool s_retarget_active = false;

void SDLNetAdapter_SetCanonicalPeer(const char* ip, Uint16 port) {
    if (ip == NULL || ip[0] == '\0') {
        s_canonical_peer[0] = '\0';
        s_actual_peer[0] = '\0';
        s_retarget_active = false;
        return;
    }
    // %d, not %u: must be byte-identical to the strings this file and
    // configure_gekko build ("%s:%d" / "%s:%hu" agree for 0..65535).
    SDL_snprintf(s_canonical_peer, sizeof(s_canonical_peer), "%s:%d", ip, (int)port);
    SDL_strlcpy(s_actual_peer, s_canonical_peer, sizeof(s_actual_peer));
    s_retarget_active = false;
}

void SDLNetAdapter_RetargetPeer(const char* ip, Uint16 port) {
    if (ip == NULL || ip[0] == '\0' || port == 0) {
        return;
    }
    SDL_snprintf(s_actual_peer, sizeof(s_actual_peer), "%s:%d", ip, (int)port);
    s_retarget_active =
        (SDL_strcmp(s_actual_peer, s_canonical_peer) != 0) &&
        s_canonical_peer[0] != '\0';
    // Force send_data's lazy resolver to re-resolve toward the actual
    // endpoint on the next outbound packet.
    if (cached_remote != NULL) {
        NET_UnrefAddress(cached_remote);
        cached_remote = NULL;
    }
    cached_port = 0;
    SDL_Log("[sdl_net_adapter] retargeted peer sends to %s (canonical %s%s)",
            s_actual_peer, s_canonical_peer,
            s_retarget_active ? "" : " — identical, passthrough");
}

// === Tier-1 netplay diag — Item 3: per-packet-type counters ===
// Indexed by (PacketType byte & 7). 8 slots cover the 7 enumerated types
// in net.h:28-36 (Inputs=1, SpectatorInputs=2, InputAck=3, SyncRequest=4,
// SyncResponse=5, SessionHealth=6, NetworkHealth=7) plus slot 0 for
// malformed packets. Single-thread access (the netplay thread runs both
// send_data and receive_data inside gekko_network_poll), so plain non-atomic
// counters are correct.
static uint64_t s_pkt_tx[8] = { 0 };
static uint64_t s_pkt_rx[8] = { 0 };

// === Tier-1 netplay diag — Item 4: per-packet ring buffer ===
// 512 slots * 24 bytes = 12 KB statically allocated. dir: 1=tx, 2=rx.
// type holds the raw PacketType byte (0..7 used in practice).
typedef struct PacketRingEntry {
    uint64_t utc_ms;
    uint8_t  dir;
    uint8_t  type;
    uint16_t len;
    uint32_t peer_ip;
    uint16_t peer_port;
    uint16_t pad;
} PacketRingEntry;

#define PACKET_RING_SLOTS 512
#define PACKET_RING_MASK  (PACKET_RING_SLOTS - 1)

static PacketRingEntry s_packet_ring[PACKET_RING_SLOTS];
static uint32_t s_packet_ring_idx = 0;     // Write cursor; mask-wraps.
static uint64_t s_packet_ring_count = 0;   // Total writes (for "valid slot" math).

// === Task #149-review P-2(a): receive-side drop accounting ===
// Both guards below (empty datagram, foreign source) drop before the
// existing s_pkt_rx / packet-ring accounting runs, so a wrong filter would
// otherwise present as zero received traffic — indistinguishable from "no
// packets arrived", the single most common NAT failure mode. Two counters,
// not one: they are different diagnoses. Zero-length is a malformed-wire
// artifact (any peer, benign); foreign-source is the one that would matter
// during a field failure — it is the signal that something is addressing
// this socket that is not the registered peer. Same convention as
// s_pkt_tx/s_pkt_rx above: plain non-atomic counters, single-thread access,
// incremented on the hot per-packet path (a single ++ is cheap enough not
// to warrant rate-limited logging there); surfaced once per dump in
// SDLNetAdapter_DumpPacketRing below.
static uint64_t s_drop_zero_len = 0;
static uint64_t s_drop_foreign_source = 0;

// Returns current UTC time in milliseconds with full sub-second precision.
// Uses CLOCK_REALTIME (vDSO-fast on Linux, no syscall) so packet-ring
// timestamps cross-correlate exactly with the netplay watchdog and
// session-start log lines (which now share this epoch). Hot-path safe.
static uint64_t now_utc_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

// Parse a "x.x.x.x:port" string into a packed IPv4 + port. Returns 0,0 on
// any parse failure — the ring entry just shows zeros, no crash. Used at
// peer-change time only (NOT on the hot per-packet path); see the cached
// values below.
static void parse_addr_str(const char* s, uint32_t* out_ip, uint16_t* out_port) {
    *out_ip = 0;
    *out_port = 0;
    if (s == NULL) return;
    unsigned a = 0, b = 0, c = 0, d = 0, port = 0;
    if (SDL_sscanf(s, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5) {
        *out_ip = ((uint32_t)(a & 0xff) << 24) |
                  ((uint32_t)(b & 0xff) << 16) |
                  ((uint32_t)(c & 0xff) << 8)  |
                   (uint32_t)(d & 0xff);
        *out_port = (uint16_t)port;
    }
}

// === Tier-1 netplay diag — Item 4: hot-path peer-address cache ===
// In a steady-state session there is exactly one peer; the address string
// is identical packet after packet. Cache the last-seen string + its
// parsed (ip, port) so the per-packet path skips SDL_sscanf when the peer
// hasn't changed. P-1.3 fix: removes the per-packet SDL_sscanf cost from
// the hot path while preserving the same ring contents.
static char     s_cached_peer_str[64] = { 0 };
static uint32_t s_cached_peer_ip = 0;
static uint16_t s_cached_peer_port = 0;

static inline void cached_parse_addr(const char* s, uint32_t* out_ip, uint16_t* out_port) {
    if (s != NULL && SDL_strcmp(s, s_cached_peer_str) == 0) {
        *out_ip   = s_cached_peer_ip;
        *out_port = s_cached_peer_port;
        return;
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    parse_addr_str(s, &ip, &port);
    if (s != NULL) {
        SDL_strlcpy(s_cached_peer_str, s, sizeof(s_cached_peer_str));
    } else {
        s_cached_peer_str[0] = '\0';
    }
    s_cached_peer_ip = ip;
    s_cached_peer_port = port;
    *out_ip = ip;
    *out_port = port;
}

static void ring_record(uint64_t utc_ms,
                        uint8_t dir, uint8_t type, uint16_t len,
                        uint32_t peer_ip, uint16_t peer_port) {
    PacketRingEntry* e = &s_packet_ring[s_packet_ring_idx & PACKET_RING_MASK];
    e->utc_ms   = utc_ms;
    e->dir      = dir;
    e->type     = type;
    e->len      = len;
    e->peer_ip  = peer_ip;
    e->peer_port = peer_port;
    e->pad      = 0;
    s_packet_ring_idx = s_packet_ring_idx + 1;
    s_packet_ring_count++;
}

void SDLNetAdapter_SnapshotCounters(uint64_t tx[8], uint64_t rx[8]) {
    if (tx != NULL) {
        SDL_memcpy(tx, s_pkt_tx, sizeof(s_pkt_tx));
    }
    if (rx != NULL) {
        SDL_memcpy(rx, s_pkt_rx, sizeof(s_pkt_rx));
    }
}

void SDLNetAdapter_DumpPacketRing(uint64_t utc_ms,
                                  const char* role,
                                  uint32_t session_uuid) {
    const char* pref = Paths_GetPrefPath();
    if (pref == NULL || pref[0] == '\0') {
        return;
    }

    char states_dir[512];
    SDL_snprintf(states_dir, sizeof(states_dir), "%sstates", pref);
    SDL_CreateDirectory(states_dir);

    char path[640];
    SDL_snprintf(path, sizeof(path),
                 "%s/netplay_packet_ring_%llu_%s.txt",
                 states_dir,
                 (unsigned long long)utc_ms,
                 role ? role : "unknown");

    FILE* f = fopen(path, "w");
    if (f == NULL) {
        SDL_Log("[netplay] ERROR: could not open %s for writing", path);
        return;
    }

    fprintf(f, "=== NETPLAY PACKET RING DUMP ===\n");
    fprintf(f, "session_uuid=0x%08x  role=%s  utc_ms=%llu  ring_slots=%d\n",
            session_uuid,
            role ? role : "unknown",
            (unsigned long long)utc_ms,
            PACKET_RING_SLOTS);
    fprintf(f, "total_writes=%llu  cumulative_tx=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu  "
               "cumulative_rx=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
            (unsigned long long)s_packet_ring_count,
            (unsigned long long)s_pkt_tx[0], (unsigned long long)s_pkt_tx[1],
            (unsigned long long)s_pkt_tx[2], (unsigned long long)s_pkt_tx[3],
            (unsigned long long)s_pkt_tx[4], (unsigned long long)s_pkt_tx[5],
            (unsigned long long)s_pkt_tx[6], (unsigned long long)s_pkt_tx[7],
            (unsigned long long)s_pkt_rx[0], (unsigned long long)s_pkt_rx[1],
            (unsigned long long)s_pkt_rx[2], (unsigned long long)s_pkt_rx[3],
            (unsigned long long)s_pkt_rx[4], (unsigned long long)s_pkt_rx[5],
            (unsigned long long)s_pkt_rx[6], (unsigned long long)s_pkt_rx[7]);
    fprintf(f, "type legend: 1=Inputs 2=SpectatorInputs 3=InputAck 4=SyncRequest "
               "5=SyncResponse 6=SessionHealth 7=NetworkHealth\n");
    fprintf(f, "drops: zero_len=%llu  foreign_source=%llu\n",
            (unsigned long long)s_drop_zero_len,
            (unsigned long long)s_drop_foreign_source);
    fprintf(f, "%-15s  %-3s  %-4s  %-6s  %-22s\n",
            "utc_ms", "dir", "type", "len", "peer");

    // Walk newest-to-oldest from the slot just written.
    const uint64_t valid = (s_packet_ring_count < PACKET_RING_SLOTS)
        ? s_packet_ring_count
        : PACKET_RING_SLOTS;
    for (uint64_t i = 0; i < valid; i++) {
        // s_packet_ring_idx is the *next* write slot, so the most recent
        // entry sits at (idx - 1) mod N.
        uint32_t slot = (s_packet_ring_idx + PACKET_RING_SLOTS - 1 - (uint32_t)i) & PACKET_RING_MASK;
        const PacketRingEntry* e = &s_packet_ring[slot];
        char peer[32];
        SDL_snprintf(peer, sizeof(peer), "%u.%u.%u.%u:%u",
                     (e->peer_ip >> 24) & 0xff,
                     (e->peer_ip >> 16) & 0xff,
                     (e->peer_ip >> 8) & 0xff,
                     e->peer_ip & 0xff,
                     (unsigned)e->peer_port);
        fprintf(f, "%15llu  %-3s  %-4u  %-6u  %-22s\n",
                (unsigned long long)e->utc_ms,
                (e->dir == 1) ? "tx" : (e->dir == 2 ? "rx" : "??"),
                (unsigned)e->type,
                (unsigned)e->len,
                peer);
    }

    fclose(f);
    SDL_Log("[netplay sess=%08x] wrote packet ring (%llu entries) to %s",
            session_uuid, (unsigned long long)valid, path);
}

static void send_data(GekkoNetAddress* addr, const char* data, int length) {
    if (adapter_sock == NULL) {
        return;
    }

    if (cached_remote == NULL) {
        char ip[64];
        int port = 0;
        // Task #119: after a relearn, the canonical string GekkoNet hands
        // us still names the endpoint it registered at configure time; the
        // authenticated peer now lives at s_actual_peer. Resolve toward
        // where the peer IS, not where it WAS. Single-peer assumption
        // unchanged (see the cached_remote comment above).
        const char* target = s_retarget_active
                                 ? s_actual_peer
                                 : (const char*)addr->data;
        SDL_sscanf(target, "%63[^:]:%d", ip, &port);
        cached_remote = NET_ResolveHostname(ip);
        cached_port = (Uint16)port;
    }

    // Tier-1 netplay diag — Item 3 + Item 4: account for every outbound
    // packet, regardless of resolver state. Counters and the ring run
    // before the actual send so the data[0] type byte is the source of
    // truth. If the address-resolver is mid-resolve and we skip the send,
    // we still record the attempt — that's the signal we want.
    // P-1.3 fix: cached_parse_addr skips SDL_sscanf when the peer string
    // is unchanged (the steady-state case for a 2-peer session).
    if (data != NULL && length > 0) {
        const uint8_t type_byte = (uint8_t)data[0];
        s_pkt_tx[type_byte & 7]++;
        uint32_t ip = 0;
        uint16_t port = 0;
        if (addr != NULL && addr->data != NULL) {
            cached_parse_addr((const char*)addr->data, &ip, &port);
        }
        ring_record(now_utc_ms(), /*dir=tx*/ 1, type_byte, (uint16_t)length, ip, port);
    }

    switch (NET_GetAddressStatus(cached_remote)) {
    case NET_SUCCESS:
        NET_SendDatagram(adapter_sock, cached_remote, cached_port, data, length);
        break;
    case NET_FAILURE:
        NET_UnrefAddress(cached_remote);
        cached_remote = NULL;
        break;
    case NET_WAITING: // still resolving, skip — GekkoNet will retransmit
        break;
    }
}

static GekkoNetResult** receive_data(int* length) {
    result_count = 0;

    if (adapter_sock == NULL) {
        *length = 0;
        return results;
    }

    NET_Datagram* dgram = NULL;

    while (result_count < MAX_NETWORK_RESULTS && NET_ReceiveDatagram(adapter_sock, &dgram) && dgram) {
        // Task #149: an empty datagram is droppable by inspection — no
        // GekkoNet packet is zero bytes (its serialized MsgHeader alone
        // is not empty), and anyone can send one. Before this guard it
        // fell through every classifier below (they all require buf !=
        // NULL / buflen > 0 to MATCH, not to pass) and reached the
        // result block, handing GekkoNet SDL_malloc(0) with
        // data_len = 0. Drop it before anything else looks at it.
        if (dgram->buf == NULL || dgram->buflen <= 0) {
            s_drop_zero_len++;
            NET_DestroyDatagram(dgram);
            dgram = NULL;
            continue;
        }
        // S1 review L1: a STUN Binding Response straggler (a keepalive
        // reply from HOST_WAITING arriving after the socket was handed
        // off to GekkoNet) is not a GekkoNet packet — drop it before it
        // pollutes the type counters or reaches the rollback engine.
        if (dgram->buf != NULL &&
            Stun_IsBindingResponse(dgram->buf, dgram->buflen)) {
            NET_DestroyDatagram(dgram);
            dgram = NULL;
            continue;
        }
        // The same class of straggler, from the rendezvous rung: a
        // DELIVER or CHALLENGE still in flight when do_handoff transfers
        // the socket would otherwise land here. Its first byte is 0x33
        // ('3'), and 0x33 & 7 == 3, so it would be miscounted as an
        // InputAck in the diag counters and then handed to GekkoNet as a
        // packet of type 51 — outside the 1..7 PacketType range
        // (third_party/GekkoNet/build/include/net.h:28-36). Dropping
        // every '3SXR' frame here is exact rather than heuristic: a
        // GekkoNet packet's type byte is 1..7 by construction and can
        // never be 0x33, so this can only ever catch our own control
        // traffic, and it does not depend on GekkoNet's unknown-type
        // handling.
        //
        // Review LOW-1: the test is the MAGIC ALONE, not
        // Rendezvous_FrameType() != 0. FrameType returns 0 for any
        // version != REND_VERSION (rendezvous.c), so a non-v2 '3SXR'
        // frame used to pass straight through this guard and be
        // miscounted as an InputAck — the exact bug the guard exists to
        // close. Unreachable today (this build only emits v2), but the
        // comment above says "every '3SXR' frame" and now that is true.
        if (dgram->buf != NULL &&
            Rendezvous_HasMagic((const uint8_t*)dgram->buf, dgram->buflen)) {
            NET_DestroyDatagram(dgram);
            dgram = NULL;
            continue;
        }
        const char* ip_str = NET_GetAddressString(dgram->addr);

        // Task #119: punch traffic belongs to the late-punch layer, never
        // to GekkoNet. Armed, LatePunch_HandleDatagram answers / relearns /
        // counts by its own rules (late_punch.c); armed or not, a
        // punch-prefixed datagram is DROPPED here — "3SX_PUNCH" starts
        // with 0x33, outside the 1..7 PacketType range, so this can only
        // ever catch our own control traffic (the same argument as the
        // '3SXR' drop above, which cannot match a punch: the prefixes
        // diverge at byte 3, '_' vs 'R'). Pre-#119 a stray punch keepalive
        // reached GekkoNet as a type-51 packet it had to ignore; now the
        // peer's pre-session keepalive stream never reaches it at all.
        if (dgram->buf != NULL &&
            Stun_HasPunchPrefix(dgram->buf, dgram->buflen)) {
            (void)LatePunch_HandleDatagram(dgram->buf, dgram->buflen,
                                           ip_str, dgram->port);
            NET_DestroyDatagram(dgram);
            dgram = NULL;
            continue;
        }

        char addr_str[64];

        SDL_snprintf(addr_str, sizeof(addr_str), "%s:%d", ip_str, (int)dgram->port);

        // Task #149: source filter. Everything that survives the guards
        // above is headed into GekkoNet's deserializer — a vendored
        // binary we cannot audit at review time (#146 hardened its
        // packet handling, but filtering here keeps arbitrary hosts'
        // bytes out of it entirely). Accept only the canonical peer
        // (configure_gekko's registration) and the actual peer (moved
        // only by SDLNetAdapter_RetargetPeer, whose sole caller acts on
        // LatePunch's token-authenticated, same-IP-restricted relearn —
        // late_punch.c).
        //
        // This drops nothing GekkoNet would have usefully accepted: its
        // inbound handlers match the presented address string byte-for-
        // byte against the registered one (see the relearn block at the
        // top of this file), and the #119 translation below only ever
        // maps actual -> canonical. A mid-session NAT rebind already
        // kills the session today — LatePunch_Disarm() runs at
        // GekkoSessionStarted, so no relearn path exists once RUNNING —
        // and a pre-session rebind re-punches through the LatePunch path
        // above, which this filter sits after.
        //
        // It also closes something GekkoNet does NOT ignore: a foreign
        // datagram carrying the (guessable u16) session magic drives
        // GekkoNet's own foreign-source reaction. Once a player reaches
        // Connected, OnSyncResponse's Connected-arm
        // (GekkoLib/src/backend.cpp @ 7be848c) increments should_send
        // with no player->address.Equals(addr) check, then replies with
        // SendSyncResponse(&addr, ...) to whatever address the packet
        // arrived from — HandleData builds that addr straight from the
        // adapter-supplied res->addr.data, unfiltered pre-#149. So
        // GekkoNet would have *responded* to a foreign source there, not
        // ignored it; this filter keeps that datagram from ever reaching
        // ParsePacket. Fall open while no canonical peer is registered
        // (cleared registration).
        if (s_canonical_peer[0] != '\0' &&
            SDL_strcmp(addr_str, s_canonical_peer) != 0 &&
            SDL_strcmp(addr_str, s_actual_peer) != 0) {
            s_drop_foreign_source++;
            NET_DestroyDatagram(dgram);
            dgram = NULL;
            continue;
        }

        // Task #119: present datagrams from the relearned peer endpoint
        // under the canonical string GekkoNet registered — its handlers
        // match by byte-compare and would otherwise ignore the live peer
        // (see the relearn block at the top of this file). Counters/ring
        // below then log the canonical endpoint too, which keeps one
        // peer identity per session in the diag stream.
        if (s_retarget_active && SDL_strcmp(addr_str, s_actual_peer) == 0) {
            SDL_strlcpy(addr_str, s_canonical_peer, sizeof(addr_str));
        }

        // Tier-1 netplay diag — Item 3 + Item 4: account for every received
        // packet before we copy it into the result list. dgram->buflen is
        // the wire size; dgram->buf[0] is the GekkoNet PacketType byte.
        // P-1.3 fix: cached_parse_addr skips SDL_sscanf when the peer
        // string matches the previous packet's (the common case).
        if (dgram->buf != NULL && dgram->buflen > 0) {
            const uint8_t type_byte = (uint8_t)dgram->buf[0];
            s_pkt_rx[type_byte & 7]++;
            uint32_t ip = 0;
            uint16_t port = 0;
            cached_parse_addr(addr_str, &ip, &port);
            ring_record(now_utc_ms(), /*dir=rx*/ 2, type_byte, (uint16_t)dgram->buflen, ip, port);
        }

        GekkoNetResult* res = SDL_malloc(sizeof(GekkoNetResult));
        size_t addr_len = SDL_strlen(addr_str);

        res->addr.data = SDL_malloc(addr_len + 1);
        SDL_strlcpy((char*)res->addr.data, addr_str, addr_len + 1);
        res->addr.size = (unsigned int)addr_len;

        res->data = SDL_malloc(dgram->buflen);
        SDL_memcpy(res->data, dgram->buf, dgram->buflen);
        res->data_len = (unsigned int)dgram->buflen;

        results[result_count++] = res;
        NET_DestroyDatagram(dgram);
        dgram = NULL;
    }

    *length = result_count;
    return results;
}

static void free_data(void* ptr) {
    SDL_free(ptr);
}

GekkoNetAdapter* SDLNetAdapter_Create(struct NET_DatagramSocket* sock) {
    adapter_sock = sock;
    adapter.send_data = send_data;
    adapter.receive_data = receive_data;
    adapter.free_data = free_data;
    return &adapter;
}

void SDLNetAdapter_Destroy() {
    adapter_sock = NULL;
    if (cached_remote != NULL) {
        NET_UnrefAddress(cached_remote);
        cached_remote = NULL;
    }
    cached_port = 0;
    // Task #119: a stale retarget must not survive into the next session
    // (whose canonical peer is a fresh configure_gekko registration).
    s_canonical_peer[0] = '\0';
    s_actual_peer[0] = '\0';
    s_retarget_active = false;
}
