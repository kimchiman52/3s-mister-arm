/*
 * direct_p2p.c — Step 7 of docs/plan-stun-direct-p2p.md.
 *
 * Direct-P2P orchestrator. See direct_p2p.h for the public contract and
 * the state-machine rationale.
 *
 * Upstream reference (pattern, not line-by-line):
 *   /tmp/3sxtra/src/port/sdl/netplay/sdl_netplay_ui.cpp:696-749 (thread fns),
 *   :1141-1209 (punch-done handler + socket ownership transfer),
 *   :1213-1240 (UPnP-done handler). Our ordering is inverted per user
 *   locked decision #2: UPnP first, STUN fallback.
 *
 * Key invariants:
 *   - All SDL3_net / miniupnpc blocking calls run on the worker thread.
 *   - State transitions published via SDL_AtomicInt; main thread only
 *     reads that atomic and inspects s_work (a struct populated by the
 *     worker before it publishes DIRECT_P2P_HOST_WAITING or a terminal
 *     state). The worker never touches s_work after it publishes a
 *     terminal state.
 *   - DirectP2P_Tick never blocks. HOST_WAITING receive polling is a
 *     single non-blocking NET_ReceiveDatagram probe per call.
 *   - The STUN socket lives in s_work.stun.socket until handoff; after
 *     Netplay_SetStunSocket(sock) we null s_work.stun.socket so cancel
 *     and session teardown don't double-close it.
 *   - The UPnP mapping lives in s_upnp_mapping and is released by
 *     direct_p2p_on_teardown() fired from the netplay.c session exit
 *     path (registered via Netplay_SetSessionTeardownCallback in
 *     DirectP2P_Init). Cancel before handoff releases directly.
 */

#include "netplay/direct_p2p.h"
#include "netplay/rendezvous.h"

/* This whole translation unit is netplay-only. The CMake glob excludes
 * direct_p2p.c from NETPLAY=OFF builds, and direct_p2p.h provides
 * header-local no-op inlines for that config. We still wrap the body
 * in #ifdef ENABLE_NETPLAY so a misconfigured include from a
 * non-netplay toolchain compiles cleanly to an empty TU rather than
 * bleeding symbol references. */
#ifdef ENABLE_NETPLAY

#include "netplay/netplay.h"
#include "netplay/room_code.h"
#include "netplay/stun.h"
#include "netplay/upnp.h"
#include "port/config/config.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* Default UDP port used when neither the --direct-p2p-handoff file nor
 * CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT specifies one. Chosen to sit
 * adjacent to RetroArch's netplay port (55435) in the IANA dynamic
 * range: shared "retro netplay" neighborhood for firewall/docs, no
 * collision with registered services, +3 offset as a Third-Strike
 * mnemonic. A concrete port (rather than 0 = OS-ephemeral) is required
 * for UPnP port-forwarding to work — router error 716
 * (WildCardNotPermittedInExtPort) means most routers reject wildcard
 * external-port requests. */
#define NETPLAY_DIRECT_P2P_DEFAULT_PORT 55438

/* --- module state ------------------------------------------------------ */

/* Role enum lives in direct_p2p.h so direct_p2p_overlay.c can branch on
 * DirectP2P_GetRole without depending on the file-local Work struct. */

typedef struct {
    Role role;

    /* Host-side preferred local port (0 => OS-assigned). */
    int preferred_port;

    /* Join-side decoded peer tuple. peer_local_port was removed when
     * the room code shrank to 11 chars (see room_code.h). Hairpin
     * fallback now relies on the router's NAT-loopback behavior
     * rather than rewriting to 127.0.0.1:peer_local_port. */
    char peer_code[64];
    char peer_ip[64];
    uint16_t peer_public_port;

    /* Populated by worker on STUN success. The socket here is the one
     * that must eventually hand off to netplay.c via
     * Netplay_SetStunSocket. */
    StunResult stun;

    /* Host-side published room code — valid while state ==
     * DIRECT_P2P_HOST_WAITING. */
    char host_code[ROOM_CODE_BUF_LEN];

    /* Host-side: the public port actually encoded in host_code (UPnP
     * external port when the mapping succeeded, else the STUN-observed
     * port). The rendezvous session key MUST be derived from this
     * advertised tuple — the joiner derives its key from the decoded
     * room code, so a host deriving from the raw STUN port instead
     * would land in a different rendezvous slot whenever the UPnP
     * external port differs from the STUN-observed port (non-port-
     * preserving NAT). Set by host_thread_fn before HOST_WAITING is
     * published; only rewritten on the main thread by the STUN-rebind
     * drift handler (which joins the rendezvous thread first). */
    uint16_t advertised_port;

    /* Parsed rendezvous endpoint cache; populated in BeginHost/BeginJoin
     * once 5b/5c land. Holds the result of parsing the signal-url config
     * value (host:port). Both fields zero in 5a — no callers yet. */
    char     signal_host[64];
    uint16_t signal_port;
} Work;

static SDL_AtomicInt s_state = { DIRECT_P2P_IDLE };
static SDL_AtomicInt s_cancel = { 0 };

/* s_work is touched by the worker while !terminal_state and only read
 * by the main thread once it observes a state transition that implies
 * the worker has finished producing the relevant fields (HOST_WAITING
 * implies stun / host_code are populated; HANDOFF / FAILED_* implies
 * the worker has exited the relevant phase). */
static Work s_work;

static SDL_Thread* s_thread = NULL;

/* Bilateral hole-punch fallback: separate cancel atomics per phase so
 * cancelling one phase doesn't accidentally signal another. Thread
 * handles are mutually exclusive on the user-action axis (host vs.
 * join) but coexist in time on the host path during the
 * FALLBACK_BILATERAL_PUNCH phase. Both stay NULL in 5a — no spawns
 * happen until 5b/5c. Cancel and teardown still join all three. */
static SDL_AtomicInt s_rendezvous_cancel = { 0 };
static SDL_AtomicInt s_bilateral_punch_cancel = { 0 };
static SDL_Thread* s_rendezvous_thread = NULL;
static SDL_Thread* s_bilateral_punch_thread = NULL;

/* Bilateral-punch worker -> Tick handoff signal. The worker thread cannot
 * call do_handoff (which invokes Netplay_SetParams / Netplay_SetRemotePort
 * / Netplay_SetStunSocket — main-thread-only). Instead, on punch SUCCESS
 * the worker writes peer_ip/peer_public_port back to s_work, sets this
 * flag, and exits. Tick's FALLBACK_BILATERAL_PUNCH case observes the flag
 * on the next frame, joins the worker, and runs do_handoff inline. */
static SDL_AtomicInt s_bilateral_handoff_pending = { 0 };

/* Review M1: host-side bilateral-punch FAILURE signal, mirror of the
 * handoff-pending flag above. The worker must not park the host in a
 * terminal state — a single stale or hostile REGISTER on our session
 * key (anyone who saw the room code) would then kill the room for the
 * entire hosting period. Instead the worker raises this flag and
 * exits; Tick joins it and returns the host to HOST_WAITING (respawning
 * the rendezvous loop) up to HOST_BILATERAL_MAX_FAILURES times per
 * hosting session, after which it parks FAILED_BILATERAL so a genuinely
 * unreachable pairing still surfaces. The count is main-thread only.
 * The JOINER's failure disposition is deliberately unchanged. */
static SDL_AtomicInt s_bilateral_failed = { 0 };
static int s_bilateral_fail_count = 0;
#define HOST_BILATERAL_MAX_FAILURES 5

/* Set by the worker right before it publishes a FAILED_* or
 * HOST_WAITING state so the status-text reader sees the fresh message
 * without a race. Writes from worker -> readers from main. Char buffer
 * is bounded; a torn read shows at worst a truncated but NUL-terminated
 * string because we always write the NUL first if we shorten. */
static char s_status[128] = { 0 };

/* UPnP mapping owned by the orchestrator until session teardown fires
 * direct_p2p_on_teardown(). "active == true" gates the RemoveMapping
 * call so re-entering Init doesn't double-release. */
static UpnpMapping s_upnp_mapping = { 0 };

/* R-1: latched by DirectP2P_NotifySessionRejected (game thread) when the
 * post-handoff MIST handshake rejects the peer. Consumed by
 * direct_p2p_on_teardown, which then parks in FAILED_HANDSHAKE instead
 * of IDLE so the overlay keeps showing the reject reason. Main-thread
 * only (notify, teardown, and Cancel all run on the game thread). */
static bool s_handshake_reject_latched = false;

/* S1 host liveness: STUN rebind keepalive bookkeeping. Main-thread only
 * (written/read exclusively from Tick's HOST_WAITING branch and the
 * BeginHost/BeginJoin/Cancel/teardown resets, all on the game thread).
 * last_ms == 0
 * means "not armed yet" — the first keepalive fires one interval after
 * HOST_WAITING is first ticked. txid_valid gates response parsing so a
 * stale or spoofed binding response without a matching in-flight
 * transaction is ignored. */
static uint64_t s_stun_keepalive_last_ms = 0;
static uint8_t s_rebind_txid[12] = { 0 };
static bool s_rebind_txid_valid = false;

/* Init guard so DirectP2P_Init is idempotent. */
static bool s_init_done = false;

/* --- internal helpers -------------------------------------------------- */

static void set_state(DirectP2PState s) {
    SDL_SetAtomicInt(&s_state, (int)s);
}

static DirectP2PState get_state(void) {
    return (DirectP2PState)SDL_GetAtomicInt(&s_state);
}

static void set_status(const char* msg) {
    if (msg == NULL) {
        s_status[0] = '\0';
        return;
    }
    /* snprintf writes NUL last; to avoid torn mid-string reads from the
     * main thread, zero the buffer first. Single-byte atomicity is the
     * only guarantee we need because DirectP2P_GetStatusText copies out
     * of the buffer eagerly. */
    size_t len = strlen(msg);
    if (len >= sizeof(s_status)) len = sizeof(s_status) - 1;
    memset(s_status, 0, sizeof(s_status));
    memcpy(s_status, msg, len);
}

static bool cancel_requested(void) {
    return SDL_GetAtomicInt(&s_cancel) != 0;
}

/* Returns true when ip is in a private/loopback/link-local IPv4 range.
 * Used by the bilateral-punch fallback (5b/5c) to short-circuit
 * rendezvous when the peer turns out to be on the same LAN — direct
 * STUN-discovered punch is the right path there, not the public
 * rendezvous server. */
static bool direct_p2p_is_lan_peer(const char* ip) {
    if (!ip || !*ip) return false;
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) return false;
    uint32_t a = ntohl(addr.s_addr);
    /* 127.0.0.0/8 */
    if ((a & 0xFF000000u) == 0x7F000000u) return true;
    /* 10.0.0.0/8 */
    if ((a & 0xFF000000u) == 0x0A000000u) return true;
    /* 172.16.0.0/12 */
    if ((a & 0xFFF00000u) == 0xAC100000u) return true;
    /* 192.168.0.0/16 */
    if ((a & 0xFFFF0000u) == 0xC0A80000u) return true;
    /* 169.254.0.0/16 (link-local) */
    if ((a & 0xFFFF0000u) == 0xA9FE0000u) return true;
    return false;
}

/* Equality check that normalizes both inputs through inet_pton so two
 * dotted-quad strings that differ only in formatting (leading zeros,
 * embedded whitespace handling, IPv4-mapped-v6 like "::ffff:1.2.3.4")
 * still compare equal. Returns false if either input fails to parse as
 * IPv4. Used by the hairpin gate in 5b/5c. */
static bool direct_p2p_ip_eq_normalized(const char* a, const char* b) {
    if (!a || !b) return false;
    struct in_addr aa, bb;
    if (inet_pton(AF_INET, a, &aa) != 1) return false;
    if (inet_pton(AF_INET, b, &bb) != 1) return false;
    return aa.s_addr == bb.s_addr;
}

/*
 * Production send wrapper. Lives in direct_p2p.c (not rendezvous.c) so
 * rendezvous.c stays SDL_net-pure. Step 6 wraps this in a function-pointer
 * seam so the bilateral-punch unit tests can interpose without touching
 * the network. See NETPLAY_TEST_HOOKS block below.
 */
static bool Rendezvous_Send(NET_DatagramSocket* sock, NET_Address* target,
                            uint16_t target_port, const uint8_t* pkt,
                            size_t pkt_len) {
    if (!sock || !target || !pkt || pkt_len == 0 || pkt_len > INT_MAX) return false;
    return NET_SendDatagram(sock, target, target_port, pkt, (int)pkt_len);
}

/*
 * NETPLAY_TEST_HOOKS — function-pointer seam (Step 6 of
 * docs/plan-bilateral-hole-punch.md).
 *
 * Production builds (without -DNETPLAY_TEST_HOOKS) keep the call sites as
 * direct calls, so the resulting object code is byte-identical to
 * pre-Step-6. Test builds (-DNETPLAY_TEST_HOOKS) replace the direct calls
 * with indirect calls through s_stun_hole_punch_impl /
 * s_rendezvous_send_impl, which test_bilateral_punch.c can override via
 * the DirectP2P_TestHook_Set* setters.
 *
 * Signatures must match the productions exactly:
 *   bool Stun_HolePunch(StunResult*, char*, uint16_t*, int, SDL_AtomicInt*);
 *   bool Rendezvous_Send(NET_DatagramSocket*, NET_Address*, uint16_t,
 *                        const uint8_t*, size_t);
 */
#ifdef NETPLAY_TEST_HOOKS
/* Public typedefs are declared in direct_p2p.h. The local function
 * pointers default to the production functions; test_bilateral_punch.c
 * overrides them via DirectP2P_TestHook_Set*. */
static DirectP2P_StunHolePunch_fn  s_stun_hole_punch_impl  = Stun_HolePunch;
static DirectP2P_RendezvousSend_fn s_rendezvous_send_impl  = Rendezvous_Send;

#define STUN_HOLE_PUNCH(stun, peer_ip, peer_port, duration_ms, cancel) \
    s_stun_hole_punch_impl((stun), (peer_ip), (peer_port), (duration_ms), (cancel))
#define RENDEZVOUS_SEND(sock, target, target_port, pkt, pkt_len) \
    s_rendezvous_send_impl((sock), (target), (target_port), (pkt), (pkt_len))

void DirectP2P_TestHook_SetStunHolePunch(DirectP2P_StunHolePunch_fn fn) {
    s_stun_hole_punch_impl = (fn != NULL) ? fn : Stun_HolePunch;
}
void DirectP2P_TestHook_SetRendezvousSend(DirectP2P_RendezvousSend_fn fn) {
    s_rendezvous_send_impl = (fn != NULL) ? fn : Rendezvous_Send;
}
bool DirectP2P_TestHook_IsLanPeer(const char* ip) {
    return direct_p2p_is_lan_peer(ip);
}
#else
#define STUN_HOLE_PUNCH(stun, peer_ip, peer_port, duration_ms, cancel) \
    Stun_HolePunch((stun), (peer_ip), (peer_port), (duration_ms), (cancel))
#define RENDEZVOUS_SEND(sock, target, target_port, pkt, pkt_len) \
    Rendezvous_Send((sock), (target), (target_port), (pkt), (pkt_len))
#endif /* NETPLAY_TEST_HOOKS */

/* --- rendezvous send queue (SPSC ring) --------------------------------- */

/* Per plan §Decision 3: the rendezvous worker thread does not call
 * NET_SendDatagram directly. It enqueues (NET_RefAddress'd target, payload)
 * tuples here and the main thread drains the queue from DirectP2P_Tick's
 * HOST_WAITING branch, calling Rendezvous_Send (and NET_UnrefAddress).
 * This keeps the STUN socket main-thread-owned during HOST_WAITING — the
 * same socket the magic-byte gate in host_tick_receive reads from. The
 * single-producer / single-consumer invariant is enforced by the
 * lifecycle: only host_rendezvous_thread_fn produces, only Tick consumes.
 *
 * Ring math: 8 slots; head moves on consume, tail moves on produce; a
 * full ring is (tail - head) == capacity; an empty ring is
 * (tail - head) == 0. We rely on uint32_t wraparound for the difference
 * — distance is always < capacity by construction.
 */
#define REND_Q_CAPACITY 8u

typedef struct {
    NET_Address* target;       /* NET_RefAddress'd by producer; consumer Unrefs after send */
    uint16_t     target_port;  /* host order, matches NET_SendDatagram */
    uint8_t      payload[28];  /* REGISTER or POLL packet */
    uint8_t      payload_len;  /* always 28 in practice; future-proof */
} RendezvousSendSlot;

static RendezvousSendSlot s_rendezvous_send_q[REND_Q_CAPACITY];
static SDL_AtomicInt s_q_head = { 0 };  /* consumer-write (drain) */
static SDL_AtomicInt s_q_tail = { 0 };  /* producer-write (enqueue) */
static SDL_AtomicInt s_q_drops = { 0 }; /* producer-incremented; logged once per session */
static SDL_AtomicInt s_q_drops_logged = { 0 };

/* Producer: returns false on full-ring (drop). Caller should already have
 * NET_RefAddress'd `target`; on overflow, the caller is responsible for
 * NET_UnrefAddress'ing it (we do NOT auto-unref here, so producer stays
 * trivially side-effect-free on its own Refcount). */
static bool rend_q_enqueue(NET_Address* target, uint16_t target_port,
                           const uint8_t* payload, uint8_t payload_len) {
    int head = SDL_GetAtomicInt(&s_q_head);
    int tail = SDL_GetAtomicInt(&s_q_tail);
    /* SPSC invariant guarantees |tail-head| <= REND_Q_CAPACITY, so a plain
     * unsigned subtraction (with natural wraparound) yields the depth
     * without needing a mask. */
    int depth = (int)((unsigned)(tail - head));
    if (depth >= (int)REND_Q_CAPACITY) {
        int drops = SDL_AddAtomicInt(&s_q_drops, 1) + 1;
        if (SDL_CompareAndSwapAtomicInt(&s_q_drops_logged, 0, 1)) {
            SDL_Log("[direct_p2p] WARNING: rendezvous send queue overflowed "
                    "(drops=%d). Main-thread drain may be stalled.", drops);
        }
        return false;
    }
    RendezvousSendSlot* slot = &s_rendezvous_send_q[(unsigned)tail % REND_Q_CAPACITY];
    slot->target = target;
    slot->target_port = target_port;
    if (payload_len > sizeof(slot->payload)) payload_len = (uint8_t)sizeof(slot->payload);
    memcpy(slot->payload, payload, payload_len);
    slot->payload_len = payload_len;
    /* Publish: bump tail AFTER slot is fully written. */
    SDL_SetAtomicInt(&s_q_tail, tail + 1);
    return true;
}

/* Consumer: drain up to `max_slots` items. Sends each via Rendezvous_Send
 * and Unrefs the target. Called from DirectP2P_Tick (main thread). */
static void rend_q_drain(NET_DatagramSocket* sock, int max_slots) {
    if (sock == NULL) {
        /* No socket — but slots may have been enqueued. Still drain to
         * release the NET_Address refs; just skip the send. */
    }
    for (int i = 0; i < max_slots; ++i) {
        int head = SDL_GetAtomicInt(&s_q_head);
        int tail = SDL_GetAtomicInt(&s_q_tail);
        if (head == tail) return;  /* empty */
        RendezvousSendSlot* slot = &s_rendezvous_send_q[(unsigned)head % REND_Q_CAPACITY];
        NET_Address* target = slot->target;
        uint16_t target_port = slot->target_port;
        uint8_t payload[28];
        memcpy(payload, slot->payload, sizeof(payload));
        uint8_t payload_len = slot->payload_len;
        /* Consume: bump head BEFORE we use the slot's pointers (post-bump
         * the producer may overwrite the slot — that's fine, we already
         * copied the payload and saved target/target_port locally). */
        SDL_SetAtomicInt(&s_q_head, head + 1);
        if (sock != NULL && target != NULL) {
            (void)RENDEZVOUS_SEND(sock, target, target_port, payload, payload_len);
        }
        if (target != NULL) {
            NET_UnrefAddress(target);
        }
    }
}

/* Drain the queue and Unref any pending targets without sending. Used at
 * teardown / cancel so we don't leak NET_Address refs. */
static void rend_q_purge(void) {
    rend_q_drain(NULL, (int)REND_Q_CAPACITY);
}

/* NOTE on CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS: the current
 * Stun_Discover API has its own hard-coded 2s-per-server internal
 * budget (see src/netplay/stun.c:281-284) and does not accept a
 * caller-supplied timeout. Honoring the config key from here would
 * require a new parameter on Stun_Discover — out of scope for Step 7.
 * Left read-through for Step 12's integration tests. */

/* --- worker thread ----------------------------------------------------- */

/* --- UPnP worker thread (wraps Upnp_AddMapping with a wall-clock
 * timeout so a slow/broken router doesn't stall the orchestrator).
 *
 * miniupnpc's SSDP discover has its own ~2s cap but a misbehaving
 * router can still keep us blocked for longer than we want to wait on
 * a game's "Host Game" button. Budget the whole UPnP attempt at
 * ~3 seconds and fall through to STUN-only if the router takes too
 * long. On timeout we detach the side thread; if its Upnp_AddMapping
 * later succeeds the mapping will still be registered on the router
 * but we never publish it — it expires naturally when its lease runs
 * out (miniupnpc requests a 1-hour lease by default) or when the
 * router reboots. Accepting that small leak is cheaper than forcing
 * a clean kill on a blocking socket call. */

typedef struct {
    /* Inputs populated before SDL_CreateThread; never mutated after. */
    uint16_t internal_port;
    uint16_t preferred_external;

    /* Output populated by the side thread on completion. Consumer reads
     * these two fields only if SDL_GetThreadState returned
     * SDL_THREAD_COMPLETE within the budget. */
    UpnpMapping result;
    bool ok;
} UpnpJob;

static int SDLCALL upnp_worker_fn(void* data) {
    UpnpJob* job = (UpnpJob*)data;
    memset(&job->result, 0, sizeof(job->result));
    job->ok = Upnp_AddMapping(&job->result,
                              job->internal_port,
                              job->preferred_external != 0 ? job->preferred_external
                                                           : job->internal_port,
                              "UDP");
    return 0;
}

/* Attempt UPnP mapping. Returns true on success (s_upnp_mapping.active
 * is set). Returns false on user-disabled, miniupnpc unavailable,
 * router rejection, or wall-clock timeout. See UpnpJob comment for the
 * timeout rationale. */
static bool try_upnp(uint16_t internal_port, uint16_t preferred_external) {
    /* Known caveat: libminiupnpc 2.2.1 upnpDiscover() segfaults on MiSTer
     * (Buildroot 2021.02.4, glibc 2.31) when a Realtek 8821cu USB WiFi
     * adapter is connected alongside eth0 — validated 2026-04-22.
     * eth0-only is safe. Users running WiFi can set
     * CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP=1 to skip this path. */
    if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_UPNP)) {
        return false;
    }
    set_status("Preparing...");

    /* Wrap Upnp_AddMapping with a wall-clock budget. miniupnpc's internal
     * SSDP discovery alone can consume up to UPNP_DISCOVER_TIMEOUT_MS
     * (see upnp.c); GetValidIGD + GetExternalIP + AddPortMapping add
     * another 1–3 s on slow routers. 6 s is comfortable headroom without
     * making the Host Game click feel stuck. If it times out we detach
     * the thread (may register the mapping belatedly — harmless, expires
     * on its own lease) and fall back to STUN.
     *
     * Heap-allocated job struct: the detached thread must not reference
     * our stack, and the leak on timeout is bounded by host-game attempts
     * per session. */
    UpnpJob* job = (UpnpJob*)SDL_calloc(1, sizeof(UpnpJob));
    if (job == NULL) {
        return false;
    }
    job->internal_port = internal_port;
    job->preferred_external = preferred_external;

    SDL_Thread* t = SDL_CreateThread(upnp_worker_fn, "UpnpProbe", job);
    if (t == NULL) {
        SDL_free(job);
        return false;
    }

    const uint64_t deadline_ms = SDL_GetTicks() + 6000;
    while (SDL_GetThreadState(t) != SDL_THREAD_COMPLETE) {
        if (SDL_GetTicks() >= deadline_ms) {
            SDL_Log("[direct_p2p] WARNING: UPnP mapping attempt timed out after 6s; falling back to STUN.");
            /* Detach and leak the job struct — the side thread owns it
             * until it exits, at which point the OS reclaims everything.
             * Any mapping it eventually registers will expire on its
             * own lease. */
            SDL_DetachThread(t);
            return false;
        }
        SDL_Delay(20);
    }

    /* Thread completed within budget. Join to reclaim resources. */
    SDL_WaitThread(t, NULL);
    bool ok = job->ok;
    if (ok) {
        s_upnp_mapping = job->result;
    }
    SDL_free(job);
    return ok;
}

/* --- S1 host liveness: UPnP lease renewal ------------------------------ */

/* The UPnP mapping is requested with a 1-hour lease (UPNP_LEASE_DURATION
 * "3600" in upnp.c) and was never renewed — a host relying on UPnP lost
 * its mapping exactly one hour in, potentially MID-SESSION (the mapping
 * is what carries the peer's traffic to us). Renew at half-lease while
 * the mapping is in use, including into the active netplay session:
 * main.c now calls DirectP2P_Tick from the session branch too, and the
 * renewal runs on a short-lived side thread (miniupnpc HTTP to the
 * router), so the netplay hot path and the GekkoNet-owned socket are
 * never touched — UPnP renewal is router-side HTTP, not socket I/O.
 *
 * Threading: the renewal thread reuses upnp_worker_fn/UpnpJob. It only
 * runs in states where host_thread_fn has finished writing
 * s_upnp_mapping (see upnp_renew_tick's caller gate), and teardown /
 * Cancel join it BEFORE calling Upnp_RemoveMapping, so miniupnpc's
 * cached-IGD statics are never used concurrently. */
#define UPNP_RENEW_INTERVAL_MS (30u * 60u * 1000u) /* half the 3600 s lease */
#define UPNP_RENEW_RETRY_MS (5u * 60u * 1000u)     /* failed renew: retry sooner */

static SDL_Thread* s_upnp_renew_thread = NULL;
static UpnpJob* s_upnp_renew_job = NULL;
static uint64_t s_upnp_next_renew_ms = 0;

/* Reap the renewal thread with a BOUNDED wait (review H3 — mirrors
 * try_upnp's deadline+detach pattern). An unbounded SDL_WaitThread here
 * was a main-thread hang: the worker runs Upnp_AddMapping = two blocking
 * router HTTP transactions, and if the router is dead/rebooting —
 * exactly when a renewal is likely to be in flight — a TCP connect can
 * block for the OS SYN timeout (~75 s macOS, ~2 min Linux), freezing
 * the game thread inside teardown/Cancel.
 *
 * Returns true when the thread was joined (or none was running), i.e.
 * miniupnpc is quiescent and the caller may safely make further
 * miniupnpc calls (Upnp_RemoveMapping). Returns false when the thread
 * had to be DETACHED: the caller must then SKIP Upnp_RemoveMapping —
 * (a) it would race the still-running worker on miniupnpc's cached-IGD
 * statics, and (b) it would block the main thread on the same dead
 * router anyway. The un-removed mapping expires on its own 1-hour
 * lease (same accepted leak as try_upnp's timeout path). On detach the
 * heap job struct is intentionally leaked to the worker — it is the
 * thread's sole owner from that point, so the detached thread can
 * never touch freed state (it references only the job and miniupnpc
 * statics, never s_work). */
#define UPNP_RENEW_JOIN_BUDGET_MS 2000
static bool upnp_renew_join_and_discard(void) {
    bool joined = true;
    if (s_upnp_renew_thread != NULL) {
        const uint64_t deadline_ms = SDL_GetTicks() + UPNP_RENEW_JOIN_BUDGET_MS;
        while (SDL_GetThreadState(s_upnp_renew_thread) != SDL_THREAD_COMPLETE &&
               SDL_GetTicks() < deadline_ms) {
            SDL_Delay(10);
        }
        if (SDL_GetThreadState(s_upnp_renew_thread) == SDL_THREAD_COMPLETE) {
            SDL_WaitThread(s_upnp_renew_thread, NULL);
        } else {
            SDL_Log("[direct_p2p] WARNING: UPnP renewal thread unresponsive after %u ms "
                    "(router down?) — detaching; router-side mapping removal skipped, "
                    "the lease expires on its own.",
                    (unsigned)UPNP_RENEW_JOIN_BUDGET_MS);
            SDL_DetachThread(s_upnp_renew_thread);
            /* Ownership of the job transfers to the detached worker. */
            s_upnp_renew_job = NULL;
            joined = false;
        }
        s_upnp_renew_thread = NULL;
    }
    if (s_upnp_renew_job != NULL) {
        SDL_free(s_upnp_renew_job);
        s_upnp_renew_job = NULL;
    }
    s_upnp_next_renew_ms = 0;
    return joined;
}

/* Main-thread, non-blocking. Called once per frame from DirectP2P_Tick
 * while the mapping is in use. Polls a completed renewal thread, or
 * spawns one when the half-lease deadline passes. */
static void upnp_renew_tick(void) {
    if (s_upnp_renew_thread != NULL) {
        if (SDL_GetThreadState(s_upnp_renew_thread) != SDL_THREAD_COMPLETE) {
            return; /* renewal in flight */
        }
        SDL_WaitThread(s_upnp_renew_thread, NULL);
        s_upnp_renew_thread = NULL;
        uint64_t now = SDL_GetTicks();
        if (s_upnp_renew_job != NULL && s_upnp_renew_job->ok) {
            s_upnp_mapping = s_upnp_renew_job->result;
            s_upnp_next_renew_ms = now + UPNP_RENEW_INTERVAL_MS;
            SDL_Log("[direct_p2p] UPnP lease renewed (external %u -> internal %u); next renewal in %u min",
                    s_upnp_mapping.external_port, s_upnp_mapping.internal_port,
                    UPNP_RENEW_INTERVAL_MS / 60000u);
        } else {
            s_upnp_next_renew_ms = now + UPNP_RENEW_RETRY_MS;
            SDL_Log("[direct_p2p] WARNING: UPnP lease renewal failed; retrying in %u min "
                    "(mapping expires at the end of its current lease)",
                    UPNP_RENEW_RETRY_MS / 60000u);
        }
        if (s_upnp_renew_job != NULL) {
            SDL_free(s_upnp_renew_job);
            s_upnp_renew_job = NULL;
        }
        return;
    }

    if (!s_upnp_mapping.active) {
        return;
    }
    uint64_t now = SDL_GetTicks();
    if (s_upnp_next_renew_ms == 0) {
        /* First sighting of an active mapping — arm the half-lease timer.
         * (Lazily armed here rather than in the worker so the deadline
         * bookkeeping stays main-thread-only.) */
        s_upnp_next_renew_ms = now + UPNP_RENEW_INTERVAL_MS;
        return;
    }
    if (now < s_upnp_next_renew_ms) {
        return;
    }

    UpnpJob* job = (UpnpJob*)SDL_calloc(1, sizeof(UpnpJob));
    if (job == NULL) {
        s_upnp_next_renew_ms = now + UPNP_RENEW_RETRY_MS;
        return;
    }
    job->internal_port = s_upnp_mapping.internal_port;
    job->preferred_external = s_upnp_mapping.external_port;
    s_upnp_renew_thread = SDL_CreateThread(upnp_worker_fn, "UpnpRenew", job);
    if (s_upnp_renew_thread == NULL) {
        SDL_free(job);
        s_upnp_next_renew_ms = now + UPNP_RENEW_RETRY_MS;
        SDL_Log("[direct_p2p] WARNING: failed to spawn UPnP renewal thread");
        return;
    }
    s_upnp_renew_job = job;
    SDL_Log("[direct_p2p] UPnP lease renewal started (external port %u)",
            s_upnp_mapping.external_port);
}

/* Convert public_ip string to network-byte-order uint32_t for room-code
 * encoding. Returns 0 on failure (still a valid IP, but 0.0.0.0 is not a
 * routable public addr so we also bail the caller). */
static uint32_t ipv4_str_to_be(const char* ip) {
    struct in_addr in = { 0 };
    if (inet_pton(AF_INET, ip, &in) != 1) {
        return 0;
    }
    /* inet_pton writes network byte order into in.s_addr. */
    return (uint32_t)in.s_addr;
}

/* --- bilateral fallback: host-side threads ----------------------------- */

/* Resolve `host` to a NET_Address with a 100ms-bounded poll, mirroring
 * stun.c:272-275. Caller must NET_UnrefAddress on success. Returns NULL
 * on resolve failure. */
static NET_Address* resolve_with_short_poll(const char* host) {
    NET_Address* addr = NET_ResolveHostname(host);
    if (!addr) return NULL;
    int wait_attempts = 0;
    while (NET_GetAddressStatus(addr) == NET_WAITING && wait_attempts < 100) {
        SDL_Delay(1);
        wait_attempts++;
    }
    if (NET_GetAddressStatus(addr) != NET_SUCCESS) {
        NET_UnrefAddress(addr);
        return NULL;
    }
    return addr;
}

/* Host rendezvous worker: parses signal-url, resolves once, then loops
 * sending REGISTER via the s_rendezvous_send_q for the ENTIRE duration
 * of HOST_WAITING (docs/plan-netplay-connection.md S1 "host liveness").
 *
 * The pre-S1 version resent for an 8 s budget and then exited
 * permanently, so (a) the rendezvous server forgot the session at its
 * TTL and (b) the host's NAT mapping decayed at the router's UDP idle
 * timeout — while the overlay kept displaying the room code. Real
 * usage shares the code out-of-band over minutes, so the host must
 * stay registered for as long as it is advertising.
 *
 * Cadence: CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS (default
 * 5000 ms, floor 1000 ms — the server rate-limits at 10 pkts/s/IP).
 * Each REGISTER also refreshes the host->server NAT mapping.
 *
 * Exit conditions (all prompt, <= ~50 ms latency via the inner sleep):
 *   - s_rendezvous_cancel set (DELIVER arrived / Cancel / teardown);
 *   - state leaves HOST_WAITING (direct-punch handoff sets HANDOFF
 *     without raising s_rendezvous_cancel — the state check covers it,
 *     so the loop cannot keep REGISTERing into an active session).
 * Spawn stays gated behind CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL
 * (see host_thread_fn step 5), which remains the kill switch for this
 * whole path. */
static int SDLCALL host_rendezvous_thread_fn(void* data) {
    (void)data;

    /* 1) Parse signal URL from config. */
    const char* signal_url = Config_GetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL);
    if (signal_url == NULL || signal_url[0] == '\0') {
        SDL_Log("[direct_p2p] rendezvous: no signal URL configured; fallback disabled");
        return 0;
    }
    char host[64] = { 0 };
    uint16_t port = 0;
    if (!Rendezvous_ParseSignalUrl(signal_url, host, &port)) {
        SDL_Log("[direct_p2p] rendezvous: malformed signal URL '%s'; fallback disabled",
                signal_url);
        return 0;
    }
    SDL_strlcpy(s_work.signal_host, host, sizeof(s_work.signal_host));
    s_work.signal_port = port;

    /* 2) Resolve hostname once at thread start (100ms-bounded poll —
     * mirror stun.c:272-275). Per the plan, resolve failure exits
     * immediately; the 8-second budget is for peer-pairing, not DNS. */
    NET_Address* signal_addr = resolve_with_short_poll(host);
    if (!signal_addr) {
        SDL_Log("[direct_p2p] rendezvous: failed to resolve %s; fallback disabled", host);
        return 0;
    }

    /* 3) Derive session key from OUR ADVERTISED endpoint — the exact
     * tuple the room code encodes (advertised_port is the UPnP external
     * port when the mapping succeeded, else the STUN port). The joiner
     * derives its key from the decoded room code, so both sides only
     * land in the same rendezvous slot when the host hashes the same
     * pair. Pre-S1 this hashed the raw STUN port, which diverges from
     * the room code whenever UPnP maps a different external port. */
    uint8_t session_key[16];
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, session_key)) {
        SDL_Log("[direct_p2p] rendezvous: failed to derive session key");
        NET_UnrefAddress(signal_addr);
        return 0;
    }

    /* 4) Build REGISTER once — payload is constant across resends. */
    uint8_t register_pkt[28];
    if (!Rendezvous_BuildRegister(s_work.stun.public_port, session_key, register_pkt)) {
        SDL_Log("[direct_p2p] rendezvous: failed to build REGISTER packet");
        NET_UnrefAddress(signal_addr);
        return 0;
    }

    /* 5) Persistent resend loop (S1). First REGISTER goes out
     * immediately, then one every interval_ms until cancel or the state
     * leaves HOST_WAITING. There is deliberately NO wall-clock budget:
     * the host must stay registered (and keep its NAT mapping warm) for
     * as long as the room code is on screen. Cancel-flag and state
     * checks happen every 50 ms inner tick, so exit latency on
     * handoff / cancel / teardown is ~50 ms. */
    int interval_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS);
    if (interval_ms <= 0) interval_ms = 5000;
    if (interval_ms < 1000) interval_ms = 1000; /* server limits 10 pkts/s/IP */
    uint32_t last_send = 0;
    for (;;) {
        if (SDL_GetAtomicInt(&s_rendezvous_cancel)) {
            /* Main thread received DELIVER (or shutdown). Exit cleanly. */
            break;
        }
        if (get_state() != DIRECT_P2P_HOST_WAITING) {
            /* Direct-punch handoff / failure / cancel — we are no longer
             * advertising, so stop re-REGISTERing. This is the exit path
             * for the direct-punch success case, which never raises
             * s_rendezvous_cancel. */
            break;
        }
        uint32_t now = SDL_GetTicks();
        if (last_send == 0 || (now - last_send) >= (uint32_t)interval_ms) {
            /* Producer must Ref before enqueue; consumer Unrefs after
             * send. On enqueue failure (queue full) we Unref ourselves. */
            NET_Address* ref = NET_RefAddress(signal_addr);
            if (ref) {
                if (!rend_q_enqueue(ref, port, register_pkt, sizeof(register_pkt))) {
                    NET_UnrefAddress(ref);
                }
            }
            last_send = now;
        }
        SDL_Delay(50);
    }

    NET_UnrefAddress(signal_addr);
    return 0;
}

/* Host bilateral-punch worker: takes a stack-local copy of peer_ip /
 * peer_port (mirror :418-420 / :493-495 patterns), runs Stun_HolePunch
 * with the configured bilateral budget, and on success writes the
 * (possibly-updated) endpoint BACK to s_work BEFORE transitioning to
 * HANDOFF (mirror :518-521 -> :531). On failure: FAILED_BILATERAL.
 *
 * §Decision 3: while this thread runs, the STUN socket is exclusively
 * read/written by Stun_HolePunch on this thread. The main-thread Tick
 * MUST NOT call host_tick_receive or drain s_rendezvous_send_q during
 * FALLBACK_BILATERAL_PUNCH — see DirectP2P_Tick's case for that state. */
static int SDLCALL host_bilateral_punch_thread_fn(void* data) {
    (void)data;

    /* Stack-locals: Stun_HolePunch overwrites *peer_ip / *peer_port at
     * stun.c:438-442 with the post-NAT-translation source endpoint. The
     * main thread reads s_work.peer_ip in do_handoff; passing &s_work...
     * directly would race. */
    char peer_ip[64];
    SDL_strlcpy(peer_ip, s_work.peer_ip, sizeof(peer_ip));
    uint16_t peer_port = s_work.peer_public_port;

    int budget_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS);
    if (budget_ms <= 0) budget_ms = 3000;

    set_status("Connecting...");
    SDL_Log("[direct_p2p] entering FALLBACK_BILATERAL_PUNCH peer=%s:%u (budget=%dms)",
            peer_ip, (unsigned)peer_port, budget_ms);

    bool punched = STUN_HOLE_PUNCH(&s_work.stun, peer_ip, &peer_port,
                                   budget_ms, &s_bilateral_punch_cancel);
    if (SDL_GetAtomicInt(&s_bilateral_punch_cancel) || cancel_requested()) {
        /* Cancelled: leave state untouched if a terminal state has already
         * been published by another path; otherwise drop to IDLE-ish via
         * Cancel's own teardown. The DirectP2P_Cancel path joins this
         * thread before tearing down s_work, so we just exit. */
        return 0;
    }
    if (!punched) {
        /* Review M1: do NOT park terminal from here. Raise the failure
         * flag and let Tick (main thread) decide: back to HOST_WAITING
         * with the rendezvous loop respawned, or FAILED_BILATERAL once
         * the per-session retry budget is spent. State stays
         * FALLBACK_BILATERAL_PUNCH until Tick acts. */
        SDL_SetAtomicInt(&s_bilateral_failed, 1);
        SDL_Log("[direct_p2p] bilateral punch FAILED — deferring disposition to Tick");
        return 0;
    }

    /* Writeback BEFORE signaling — main-thread do_handoff reads
     * s_work.peer_ip/peer_public_port and we want the post-punch values. */
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = peer_port;

    set_status("Connecting...");
    /* Cannot set_state(DIRECT_P2P_HANDOFF) here: do_handoff calls
     * Netplay_SetParams / Netplay_SetRemotePort / Netplay_SetStunSocket
     * which are main-thread-only. Stay in FALLBACK_BILATERAL_PUNCH and
     * raise s_bilateral_handoff_pending so the next Tick observes us,
     * joins this thread, and runs do_handoff on the main thread. */
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 1);
    SDL_Log("[direct_p2p] bilateral punch SUCCESS - handoff pending");
    return 0;
}

/* Host worker: UPnP (optional) -> STUN discover -> publish room code ->
 * transition to HOST_WAITING. The main-thread Tick then owns the wait
 * for the first inbound datagram. */
static int SDLCALL host_thread_fn(void* data) {
    (void)data;

    /* Let the main thread finish its first render frame before hitting
     * SDL_net. Calling NET_Init / NET_ResolveHostname on a worker thread
     * while the main thread is still completing game init (ppg, sound,
     * memcard) races and segfaults inside NET_ResolveHostname on MiSTer
     * (glibc 2.31, ARM). 200ms is empirically enough to get past all
     * the initialize_game() work that follows defer_direct_p2p_handoff. */
    SDL_Delay(200);

    const uint16_t local_port = (uint16_t)s_work.preferred_port;

    /* 1) UPnP probe. Non-fatal if it fails — we fall through to STUN.
     * Request external == internal so the router doesn't have to invent
     * an external port (some firmware rejects wildcards, error 716). */
    set_state(DIRECT_P2P_UPNP_PROBE);
    bool upnp_ok = try_upnp(local_port, local_port);
    if (cancel_requested()) {
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (upnp_ok) {
        SDL_Log("[direct_p2p] UPnP mapping OK (external %u -> internal %u)",
                s_upnp_mapping.external_port,
                s_upnp_mapping.internal_port);
    } else {
        SDL_Log("[direct_p2p] UPnP unavailable or refused; falling back to STUN.");
    }

    /* 2) STUN discover. We always need a public IP + port for the room
     * code, even if UPnP succeeded (the room code encodes the public
     * tuple; the peer needs to know where to send). Stun_Discover
     * itself assumes NET_Init has already run — ensure it here on the
     * worker thread because this may be the first netplay path any
     * session takes. NET_Init is idempotent. */
    NET_Init();
    set_state(DIRECT_P2P_STUN_DISCOVER);
    set_status("Preparing...");
    bool stun_ok = Stun_Discover(&s_work.stun, local_port);
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!stun_ok) {
        set_status("Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

    /* S1 CGNAT gate: on carrier-grade NAT (or any double NAT) the inner
     * router happily grants a mapping whose external IP is a private /
     * CGN (100.64/10) address, while STUN reports the true public IP.
     * The room code is built from the STUN IP + the chosen port, so
     * advertising the UPnP port would pair the STUN IP with a port that
     * only exists on the INNER router — a wrong (ip, port) pair that
     * silently kills the direct path. If the two external IPs disagree,
     * drop the mapping and advertise the STUN-observed endpoint; the
     * punch/bilateral paths carry it from there. (ip_eq_normalized
     * returns false on unparseable/IPv6 strings, which conservatively
     * lands in the mismatch branch.) */
    if (upnp_ok &&
        !direct_p2p_ip_eq_normalized(s_upnp_mapping.external_ip, s_work.stun.public_ip)) {
        SDL_Log("[direct_p2p] CGNAT detected: UPnP external IP %s != STUN public IP %s "
                "— ignoring the UPnP mapping and advertising the STUN endpoint instead",
                s_upnp_mapping.external_ip[0] ? s_upnp_mapping.external_ip : "(empty)",
                s_work.stun.public_ip);
        /* Release the useless mapping now (worker thread — same thread
         * class try_upnp used; no concurrent miniupnpc user exists in
         * UPNP_PROBE/STUN_DISCOVER states). Also stops the S1 lease
         * renewal from ever arming for it. */
        Upnp_RemoveMapping(&s_upnp_mapping);
        memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
        upnp_ok = false;
    }

    /* 3) Build room code from discovered endpoint. If UPnP succeeded we
     * prefer its external port over the STUN-observed port — symmetric
     * NATs translate per-destination, so the STUN port is only valid
     * for STUN servers; the UPnP mapping is stable for any peer. The
     * room code no longer carries the host's LAN local_port; see
     * room_code.h for the rationale. */
    uint16_t pub_port = upnp_ok ? s_upnp_mapping.external_port : s_work.stun.public_port;
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    if (ip_be == 0 ||
        !RoomCode_Encode(ip_be, pub_port, s_work.host_code)) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }
    /* Record the advertised tuple's port — the rendezvous session key is
     * derived from (public_ip, advertised_port) on both roles (S1). Must
     * be visible before HOST_WAITING publishes (the rendezvous thread
     * spawns after and reads it). */
    s_work.advertised_port = pub_port;

    /* 4) Publish — Tick() will now drain the STUN socket non-blockingly
     * until a peer punches through. The room code renders on overlay
     * line 2 via DirectP2P_GetHostCode(); this status goes on line 3.
     *
     * Cancel-flag race fix: clear s_rendezvous_cancel and
     * s_bilateral_punch_cancel BEFORE publishing HOST_WAITING. Once Tick
     * sees HOST_WAITING it can call host_tick_receive -> try_handle_deliver
     * which raises s_rendezvous_cancel as its DELIVER signal. If we
     * cleared after publishing, that signal could be clobbered by us
     * here. Clear-before-publish ensures any subsequent set-by-DELIVER
     * survives. */
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);

    set_status("Waiting for player 2...");
    set_state(DIRECT_P2P_HOST_WAITING);
    SDL_Log("[direct_p2p] HOST_WAITING published. Code=%s public=%s:%u (via %s)",
            s_work.host_code, s_work.stun.public_ip, (unsigned)pub_port,
            upnp_ok ? "UPnP" : "STUN");

    /* 5) Bilateral fallback: spawn the rendezvous-resender thread unless
     * the kill switch is set. The thread enqueues REGISTER packets onto
     * s_rendezvous_send_q; the main thread drains them in Tick's
     * HOST_WAITING branch. Per §Decision 5 the host stays in
     * HOST_WAITING during this phase — there's no host-side
     * FALLBACK_SIGNALING transition.
     *
     * Note: s_rendezvous_cancel was cleared above (before HOST_WAITING
     * publish) to avoid clobbering a try_handle_deliver-set value. */
    if (!Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
        s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                               "DirectP2PRendezvous", NULL);
        if (s_rendezvous_thread == NULL) {
            SDL_Log("[direct_p2p] WARNING: failed to spawn rendezvous thread; "
                    "fallback disabled this session");
        }
    }
    return 0;
}

/* Join worker: STUN discover -> hairpin-detect -> Stun_HolePunch -> hand
 * off. The final socket transfer into netplay.c happens on the main
 * thread inside Tick() once this worker publishes DIRECT_P2P_HANDOFF;
 * the worker arranges s_work so Tick has everything it needs. */
static int SDLCALL join_thread_fn(void* data) {
    (void)data;

    /* See host_thread_fn for the rationale on this pre-NET_Init delay. */
    SDL_Delay(200);

    /* Idempotent NET_Init — Stun_Discover assumes we've called it. */
    NET_Init();
    set_state(DIRECT_P2P_STUN_DISCOVER);
    set_status("Preparing...");
    bool stun_ok = Stun_Discover(&s_work.stun, 0);
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!stun_ok) {
        set_status("Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

    /* Hairpin detect: peer public IP == our public IP => same LAN.
     * The room code used to carry the peer's LAN local_port so we
     * could rewrite the target to 127.0.0.1:peer_local_port; since
     * that field was dropped (see room_code.h) we now rely on the
     * router's NAT-loopback support. If the router forwards the UDP
     * mapping back to itself on a same-LAN send, the hole-punch
     * completes normally using the STUN-discovered public address.
     * If the router does NOT support NAT loopback, the punch times
     * out and the existing "Cannot reach peer" / FAILED_SYMMETRIC
     * path reports it — same failure the user would get from any
     * other hairpin-broken network. */
    if (s_work.peer_ip[0] != '\0' && s_work.stun.public_ip[0] != '\0' &&
        strcmp(s_work.peer_ip, s_work.stun.public_ip) == 0) {
        SDL_Log("[direct_p2p] NAT hairpin detected — router must support "
                "NAT loopback. Proceeding with public address %s:%u.",
                s_work.peer_ip, (unsigned)s_work.peer_public_port);
    }

    /* Hole-punch. 2.5s window per upstream. Stun_HolePunch updates the
     * peer_ip/peer_port in place with the translated endpoint the host
     * actually reaches us from. */
    set_state(DIRECT_P2P_JOIN_PUNCHING);
    set_status("Connecting...");
    char punch_peer_ip[64];
    SDL_strlcpy(punch_peer_ip, s_work.peer_ip, sizeof(punch_peer_ip));
    uint16_t punch_peer_port = s_work.peer_public_port;
    bool punched = STUN_HOLE_PUNCH(&s_work.stun, punch_peer_ip, &punch_peer_port,
                                   2500, &s_cancel);
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!punched) {
        /* Direct punch failed. Three-way bypass before attempting the
         * bilateral rendezvous fallback:
         *   (a) kill switch — user disabled bilateral fallback;
         *   (b) LAN peer — we should have reached a private-subnet peer
         *       directly, public rendezvous is the wrong path;
         *   (c) hairpin — peer's public IP equals our own, meaning a
         *       same-LAN router that lacks NAT loopback. The bilateral
         *       punch would loopback through the same broken router and
         *       fail identically (Hard Requirement 3(c)).
         * Each bypass takes the legacy FAILED_SYMMETRIC path with an
         * appropriate status. The original "Possible Symmetric NAT"
         * string is preserved for the kill-switch / LAN cases since the
         * underlying diagnosis is unchanged. */
        if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_SYMMETRIC);
            return 0;
        }
        if (direct_p2p_is_lan_peer(s_work.peer_ip)) {
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_SYMMETRIC);
            return 0;
        }
        if (s_work.stun.public_ip[0] != '\0' &&
            direct_p2p_ip_eq_normalized(s_work.peer_ip, s_work.stun.public_ip)) {
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_SYMMETRIC);
            return 0;
        }

        /* Bilateral fallback (joiner side). Per §Decision 4, the joiner
         * runs the rendezvous REGISTER/POLL loop INLINE in this worker
         * thread instead of spawning a producer/queue split — its STUN
         * socket has only one reader/writer (this thread), so direct
         * Rendezvous_Send calls are race-free.
         *
         * Flow: FALLBACK_SIGNALING -> resolve signal URL -> derive
         * session key -> resend REGISTER every 500ms while reading non-
         * blocking, watching for a 32+ byte '3SXR' DELIVER. On valid
         * DELIVER -> FALLBACK_BILATERAL_PUNCH -> Stun_HolePunch (bilateral
         * budget) -> HANDOFF or FAILED_BILATERAL. */
        set_status("Connecting...");
        set_state(DIRECT_P2P_FALLBACK_SIGNALING);

        /* 1) Parse signal URL. */
        const char* signal_url = Config_GetString(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_URL);
        if (signal_url == NULL || signal_url[0] == '\0') {
            SDL_Log("[direct_p2p] joiner fallback: no signal URL configured");
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }
        char signal_host[64] = { 0 };
        uint16_t signal_port = 0;
        if (!Rendezvous_ParseSignalUrl(signal_url, signal_host, &signal_port)) {
            SDL_Log("[direct_p2p] joiner fallback: malformed signal URL '%s'", signal_url);
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }

        /* 2) Resolve hostname (100ms-bounded poll, mirror stun.c:272-275). */
        NET_Address* signal_addr = resolve_with_short_poll(signal_host);
        if (!signal_addr) {
            SDL_Log("[direct_p2p] joiner fallback: failed to resolve %s", signal_host);
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }

        /* 3) Derive session key from the HOST's public endpoint, decoded
         * from the room code into peer_ip / peer_public_port. The host
         * registered with the rendezvous using the same hash, and the
         * server pairs entries by literal session_key equality — so both
         * sides MUST hash the host's tuple to wind up in the same slot. */
        uint8_t session_key[16];
        uint32_t host_ip_be = ipv4_str_to_be(s_work.peer_ip);
        if (!Rendezvous_DeriveSessionKey(host_ip_be, s_work.peer_public_port, session_key)) {
            SDL_Log("[direct_p2p] joiner fallback: failed to derive session key");
            NET_UnrefAddress(signal_addr);
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }

        /* 4) Build REGISTER once — payload is constant across resends. */
        uint8_t register_pkt[28];
        if (!Rendezvous_BuildRegister(s_work.stun.public_port, session_key, register_pkt)) {
            SDL_Log("[direct_p2p] joiner fallback: failed to build REGISTER packet");
            NET_UnrefAddress(signal_addr);
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }

        /* 5) Inline REGISTER/POLL + DELIVER receive loop. 500ms send
         * cadence; 8s default budget (configurable). 50ms inner sleep so
         * the cancel check has ~50ms granularity. Direct Rendezvous_Send
         * is safe here because this thread is the sole reader/writer of
         * the STUN socket. */
        int signal_budget_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS);
        if (signal_budget_ms <= 0) signal_budget_ms = 8000;
        int bilateral_budget_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS);
        if (bilateral_budget_ms <= 0) bilateral_budget_ms = 3000;

        char fb_peer_ip[64] = { 0 };
        uint16_t fb_peer_port = 0;
        bool got_deliver = false;

        const uint32_t signal_start = SDL_GetTicks();
        uint32_t last_send = 0;
        while ((int)(SDL_GetTicks() - signal_start) < signal_budget_ms) {
            if (cancel_requested()) {
                NET_UnrefAddress(signal_addr);
                Stun_CloseSocket(&s_work.stun);
                set_status("Cancelled.");
                set_state(DIRECT_P2P_IDLE);
                return 0;
            }

            uint32_t now = SDL_GetTicks();
            if (last_send == 0 || (now - last_send) >= 500u) {
                (void)RENDEZVOUS_SEND(s_work.stun.socket, signal_addr,
                                      signal_port, register_pkt, sizeof(register_pkt));
                last_send = now;
            }

            /* Non-blocking receive — mirror host_tick_receive's call
             * convention. NET_ReceiveDatagram returns success/failure as
             * bool; a successful call with no datagram available leaves
             * dgram == NULL. */
            NET_Datagram* dgram = NULL;
            if (NET_ReceiveDatagram(s_work.stun.socket, &dgram) && dgram != NULL) {
                if (dgram->buflen >= 32 &&
                    dgram->buf[0] == 0x33 && dgram->buf[1] == 0x53 &&
                    dgram->buf[2] == 0x58 && dgram->buf[3] == 0x52) {
                    char parsed_ip[64] = { 0 };
                    uint16_t parsed_port = 0;
                    if (Rendezvous_ParseDeliver(dgram->buf, dgram->buflen,
                                                session_key, parsed_ip, &parsed_port) &&
                        parsed_ip[0] != '\0' && parsed_port != 0) {
                        SDL_strlcpy(fb_peer_ip, parsed_ip, sizeof(fb_peer_ip));
                        fb_peer_port = parsed_port;
                        got_deliver = true;
                        SDL_Log("[direct_p2p] joiner DELIVER received peer=%s:%u",
                                fb_peer_ip, (unsigned)fb_peer_port);
                    }
                }
                /* Non-DELIVER packets (stale punch echoes, garbage) are
                 * dropped — no echo here; this loop is rendezvous-only. */
                NET_DestroyDatagram(dgram);
                if (got_deliver) break;
            }

            SDL_Delay(50);
        }

        NET_UnrefAddress(signal_addr);

        if (!got_deliver) {
            SDL_Log("[direct_p2p] joiner fallback: signal budget expired without DELIVER");
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }

        /* 6) Bilateral hole-punch with a stack-local copy of the peer
         * endpoint (Stun_HolePunch overwrites *peer_ip / *peer_port with
         * the post-NAT-translation source on success — mirrors the
         * direct-path locals at :788-790). */
        char bp_peer_ip[64];
        SDL_strlcpy(bp_peer_ip, fb_peer_ip, sizeof(bp_peer_ip));
        uint16_t bp_peer_port = fb_peer_port;

        set_status("Connecting...");
        set_state(DIRECT_P2P_FALLBACK_BILATERAL_PUNCH);
        SDL_Log("[direct_p2p] joiner entering FALLBACK_BILATERAL_PUNCH peer=%s:%u (budget=%dms)",
                bp_peer_ip, (unsigned)bp_peer_port, bilateral_budget_ms);

        bool bilateral_punched = STUN_HOLE_PUNCH(&s_work.stun, bp_peer_ip,
                                                 &bp_peer_port,
                                                 bilateral_budget_ms, &s_cancel);
        if (cancel_requested()) {
            Stun_CloseSocket(&s_work.stun);
            set_status("Cancelled.");
            set_state(DIRECT_P2P_IDLE);
            return 0;
        }
        if (!bilateral_punched) {
            Stun_CloseSocket(&s_work.stun);
            set_status("Could not connect. Try a different network.");
            set_state(DIRECT_P2P_FAILED_BILATERAL);
            return 0;
        }

        /* 7) Bilateral punch succeeded. Writeback BEFORE set_state(HANDOFF)
         * — Tick reads s_work.peer_ip/peer_public_port in join_tick_handoff,
         * so the post-punch translated endpoint must be visible by the
         * time HANDOFF is observed. Persist the room code and fall through
         * to the existing HANDOFF publish below. */
        SDL_strlcpy(s_work.peer_ip, bp_peer_ip, sizeof(s_work.peer_ip));
        s_work.peer_public_port = bp_peer_port;

        if (s_work.peer_code[0] != '\0') {
            Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE, s_work.peer_code);
        }

        set_status("Connected!");
        set_state(DIRECT_P2P_HANDOFF);
        return 0;
    }

    /* Stash the post-punch translated endpoint so Tick can feed it to
     * Netplay_SetParams. The old "hairpin bypass" block that rewrote
     * the STUN socket to 127.0.0.1 is gone now that the room code no
     * longer carries peer_local_port — hairpin traffic flows through
     * the router's NAT loopback using the STUN-discovered public
     * endpoint (or fails with the generic FAILED_SYMMETRIC path). */
    SDL_strlcpy(s_work.peer_ip, punch_peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = punch_peer_port;

    /* Persist the code the user just joined with. */
    if (s_work.peer_code[0] != '\0') {
        Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE, s_work.peer_code);
    }

    /* Worker is done; Tick will observe HANDOFF and drive the
     * netplay.c handoff on the main thread. */
    set_status("Connected!");
    set_state(DIRECT_P2P_HANDOFF);
    return 0;
}

/* --- main-thread handoff ----------------------------------------------- */

/* Called from session teardown (via Netplay_SetSessionTeardownCallback).
 * Releases the UPnP mapping while the STUN socket is still bound so
 * miniupnpc can cleanly deregister. Safe to call when no mapping is
 * active. */
static void direct_p2p_on_teardown(void) {
    /* Signal any still-running worker(s) to exit, then join. The natural-
     * success exit path returns from the worker without nulling the
     * handle (5a switched the spawn sites away from SDL_DetachThread),
     * so teardown owns the join responsibility for both the orchestrator
     * worker and the new (5b/5c) rendezvous / bilateral-punch threads. */
    SDL_SetAtomicInt(&s_cancel, 1);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 1);
    if (s_thread)                  { SDL_WaitThread(s_thread,                  NULL); s_thread                  = NULL; }
    if (s_rendezvous_thread)       { SDL_WaitThread(s_rendezvous_thread,       NULL); s_rendezvous_thread       = NULL; }
    if (s_bilateral_punch_thread)  { SDL_WaitThread(s_bilateral_punch_thread,  NULL); s_bilateral_punch_thread  = NULL; }

    /* Producer threads are joined; release any pending NET_Address refs
     * still in the rendezvous send queue (consumer never gets to drain
     * after teardown). */
    rend_q_purge();

    /* S1: reap any in-flight UPnP lease renewal BEFORE RemoveMapping so
     * miniupnpc's cached-IGD statics are never used concurrently. Bounded
     * (review H3); on detach-timeout the router-side removal is skipped —
     * see upnp_renew_join_and_discard. */
    bool upnp_quiescent = upnp_renew_join_and_discard();

    if (s_upnp_mapping.active) {
        if (upnp_quiescent) {
            Upnp_RemoveMapping(&s_upnp_mapping);
        }
        memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    }
    /* Note: we do NOT destroy s_work.stun.socket here — ownership has
     * been transferred to netplay.c via Netplay_SetStunSocket and
     * netplay.c will destroy it immediately after this callback
     * returns. */
    s_work.stun.socket = NULL;
    /* S1: the ref'd STUN server address is still ours (do_handoff already
     * released it on the handoff path; this covers non-handoff teardowns). */
    Stun_ReleaseServerAddr(&s_work.stun);
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    /* Reset state so the next BeginHost/BeginJoin starts clean.
     * R-1 exception: when the MIST handshake rejected the session, park
     * in FAILED_HANDSHAKE instead so the overlay keeps ERROR + the
     * reason (set_status by NotifySessionRejected) on screen after the
     * soft reset — an IDLE state would hide the overlay and the player
     * would see a silent drop with no explanation. Terminal like the
     * other FAILED_* states: cleared by DirectP2P_Cancel or process
     * restart (the MiSTer OSD retry path re-execs anyway). */
    if (s_handshake_reject_latched) {
        s_handshake_reject_latched = false;
        set_state(DIRECT_P2P_FAILED_HANDSHAKE);
    } else {
        set_state(DIRECT_P2P_IDLE);
    }
}

/* Finish the handoff on the main thread. Called from Tick when the
 * worker publishes DIRECT_P2P_HANDOFF (Join path) or when Tick itself
 * receives the first inbound datagram (Host path). */
static void do_handoff(int player, const char* peer_ip, uint16_t peer_port) {
    SDL_Log("[direct_p2p] Handoff to netplay: player=%d peer=%s:%u", player, peer_ip, (unsigned)peer_port);
    /* Netplay_SetParams wires remote_ip, local_port and the default
     * remote_port from (player, ip). The STUN socket we hand off below is a
     * plain datagram socket with no connected-peer state, so GekkoNet sends
     * go to whatever remote_ip:remote_port configure_gekko stringifies
     * (netplay.c:525). For direct-P2P over the internet the real peer
     * endpoint is the STUN-translated peer_port from the hole-punch, not
     * SetParams' hardcoded 50000. Override remote_port here so outbound
     * Gekko frames reach the actual punched endpoint instead of oblivion. */
    Netplay_SetParams(player, peer_ip);
    Netplay_SetRemotePort(peer_port);
    Netplay_SetStunSocket(s_work.stun.socket);
    /* Ownership transferred — prevent direct_p2p_on_teardown or a
     * subsequent Cancel from double-closing. */
    s_work.stun.socket = NULL;
    /* S1: keepalives stop at handoff (GekkoNet traffic keeps the mapping
     * warm from here) — drop the ref'd STUN server address now. */
    Stun_ReleaseServerAddr(&s_work.stun);
    s_rebind_txid_valid = false;
    /* netplay_nav owns the Netplay_BeginDirectP2P() call now. Netplay_
     * SetParams just populated remote_ip; nav's NAV_WAIT_ORCHESTRATOR
     * was gating on Netplay_IsRemoteIpSet() and will advance to
     * NAV_START_NETPLAY on its next tick. If menu-nav completes first
     * (fast path on host, orchestrator still hole-punching on joiner)
     * nav simply stays in NAV_WAIT_ORCHESTRATOR until we land here. */
    SDL_Log("[direct_p2p] handoff complete — nav state machine will start netplay session");
}

/* --- S1 host liveness: STUN rebind keepalive + drift detection --------- */

/* Called from Tick's HOST_WAITING branch (main thread — the socket's
 * sole I/O actor in that phase). Every CFG_KEY_NETPLAY_DIRECT_P2P_STUN_
 * KEEPALIVE_MS (default 20 s; <= 0 disables) re-issues a STUN Binding
 * Request on the shared socket toward the server that answered
 * Stun_Discover. Two effects:
 *   (a) the outbound datagram refreshes the host's NAT mapping — for a
 *       non-UPnP host this is the very mapping the room code
 *       advertises, which otherwise decays at the router's UDP idle
 *       timeout while the host sits waiting;
 *   (b) the response (handled in host_handle_stun_rebind via
 *       host_tick_receive's STUN gate) reveals whether the NAT rebound
 *       the mapping — i.e. whether the displayed room code went stale. */
static void host_stun_keepalive_tick(void) {
    int interval_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_KEEPALIVE_MS);
    if (interval_ms <= 0) return; /* disabled */
    if (interval_ms < 5000) interval_ms = 5000; /* don't spam public STUN */
    uint64_t now = SDL_GetTicks();
    if (s_stun_keepalive_last_ms == 0) {
        /* Arm on first HOST_WAITING tick; Stun_Discover just probed, so
         * the first keepalive is due one full interval from now. */
        s_stun_keepalive_last_ms = now;
        return;
    }
    if (now - s_stun_keepalive_last_ms < (uint64_t)interval_ms) return;
    s_stun_keepalive_last_ms = now;
    if (Stun_SendKeepalive(&s_work.stun, s_rebind_txid)) {
        s_rebind_txid_valid = true;
    }
}

/* Handle a STUN Binding Success Response received on the host socket
 * during HOST_WAITING (main thread). If the mapped endpoint matches the
 * last known public endpoint the NAT mapping is stable — nothing to do.
 * If it DIFFERS, the NAT rebound and the displayed room code is stale.
 *
 * Chosen behavior (documented in docs/plan-netplay-connection.md S1):
 * re-encode and display the NEW code, plus an explicit status line
 * ("Network changed! Share the NEW code."). Rationale: silently
 * continuing would leave a code on screen that can never connect; the
 * least surprising outcome for a user staring at a code they already
 * shared is a visible, explained code change — the old code was already
 * dead the moment the NAT rebound, so there is nothing to preserve.
 *
 * Ordering: the rendezvous thread derives its session key from
 * (public_ip, advertised_port), so it is cancelled and JOINED before
 * s_work is mutated, then respawned under the new key. Join latency is
 * ~50 ms (the thread's inner tick) — acceptable for a rare event on a
 * menu screen. */
static void host_handle_stun_rebind(const uint8_t* buf, int len) {
    if (!s_rebind_txid_valid) {
        return; /* no keepalive in flight — stale/foreign response */
    }
    char ip[64] = { 0 };
    uint16_t port = 0;
    if (!Stun_ParseBindingResponse(buf, len, s_rebind_txid, ip, sizeof(ip), &port)) {
        return; /* wrong transaction / malformed — ignore */
    }
    s_rebind_txid_valid = false;

    if (port == s_work.stun.public_port &&
        direct_p2p_ip_eq_normalized(ip, s_work.stun.public_ip)) {
        return; /* mapping stable — keepalive did its NAT-refresh job */
    }

    SDL_Log("[direct_p2p] STUN rebind drift: public endpoint changed "
            "%s:%u -> %s:%u — displayed room code is stale",
            s_work.stun.public_ip, (unsigned)s_work.stun.public_port,
            ip, (unsigned)port);

    /* Stop the rendezvous re-REGISTER loop before mutating the fields it
     * reads (public_ip / advertised_port). */
    if (s_rendezvous_thread != NULL) {
        SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
        SDL_WaitThread(s_rendezvous_thread, NULL);
        s_rendezvous_thread = NULL;
    }

    SDL_strlcpy(s_work.stun.public_ip, ip, sizeof(s_work.stun.public_ip));
    s_work.stun.public_port = port;

    /* Recompute the advertised tuple. A live UPnP mapping pins the
     * advertised PORT (the router forwards it regardless of the STUN
     * mapping), so only the IP component can drift there; without UPnP
     * both components track the STUN-observed endpoint. */
    uint16_t new_adv_port = s_upnp_mapping.active ? s_upnp_mapping.external_port : port;
    uint32_t ip_be = ipv4_str_to_be(ip);
    char new_code[ROOM_CODE_BUF_LEN];
    if (ip_be != 0 && RoomCode_Encode(ip_be, new_adv_port, new_code) &&
        strcmp(new_code, s_work.host_code) != 0) {
        SDL_Log("[direct_p2p] room code re-encoded: %s -> %s",
                s_work.host_code, new_code);
        SDL_strlcpy(s_work.host_code, new_code, sizeof(s_work.host_code));
        set_status("Network changed! Share the NEW code.");
    }
    s_work.advertised_port = new_adv_port;

    /* Respawn the rendezvous loop under the new session key (same kill
     * switch as the original spawn in host_thread_fn step 5). */
    if (!Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
        SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
        s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                               "DirectP2PRendezvous", NULL);
        if (s_rendezvous_thread == NULL) {
            SDL_Log("[direct_p2p] WARNING: failed to respawn rendezvous thread "
                    "after endpoint drift; fallback disabled this session");
        }
    }
}

/*
 * DELIVER handler — invoked from host_tick_receive after the magic-byte
 * gate. Parses the packet against our session key (derived from our own
 * public endpoint, same as the rendezvous thread). On a valid DELIVER
 * with a non-zero peer endpoint, transitions out of HOST_WAITING per
 * the bilateral fallback flow:
 *   - LAN peer => stay HOST_WAITING (we should have seen a direct punch
 *     already; rendezvous via public infra is the wrong path).
 *   - Hairpin (peer public IP == our public IP) => FAILED_SYMMETRIC; the
 *     router that broke the direct punch will also break the bilateral
 *     punch (Hard Requirement 3(c)).
 *   - Otherwise => stash peer endpoint, signal rendezvous-cancel,
 *     transition to FALLBACK_BILATERAL_PUNCH and spawn the
 *     bilateral-punch worker thread.
 *
 * Returns true iff the packet was a recognized DELIVER (whether or not
 * we acted on it) so the caller knows it consumed the datagram.
 */
static bool try_handle_deliver(const uint8_t* pkt, int len) {
    /* Only honor DELIVER while we're still in HOST_WAITING. After we've
     * transitioned to FALLBACK_BILATERAL_PUNCH (or any terminal) a stale
     * DELIVER from the server is a no-op. */
    if (get_state() != DIRECT_P2P_HOST_WAITING) {
        return true;
    }

    /* Derive the session key from our own ADVERTISED endpoint — the same
     * derivation the rendezvous thread does and the joiner does (from the
     * decoded room code). See the advertised_port field comment. */
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    uint8_t session_key[16];
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, session_key)) {
        SDL_Log("[direct_p2p] DELIVER drop: failed to derive session key");
        return true;
    }

    char peer_ip[64] = { 0 };
    uint16_t peer_port = 0;
    if (!Rendezvous_ParseDeliver(pkt, len, session_key, peer_ip, &peer_port)) {
        /* Malformed, wrong session, or "peer not yet registered" sentinel —
         * any of which the rendezvous thread will retry past. No action. */
        return true;
    }

    SDL_Log("[direct_p2p] DELIVER received peer=%s:%u", peer_ip, (unsigned)peer_port);

    /* Self-DELIVER gate (review H1): a DELIVER whose peer IP equals our
     * OWN public IP is our own stale registration echoed back, not a
     * peer. Scenario: host presses Host, cancels, re-hosts within the
     * server TTL. With UPnP the external port is pinned so the session
     * key is unchanged; if the new socket's NAT mapping toward the
     * server picked a different source port, the server sees stale slot
     * A + empty slot B, makes us B, and its reply DELIVER carries our
     * own old endpoint. Pre-fix this hit the terminal hairpin gate
     * below and killed the re-hosted room with FAILED_SYMMETRIC for the
     * whole 10-minute TTL.
     *
     * A LEGITIMATE joiner can never produce a same-IP DELIVER: the
     * joiner's own hairpin bypass (join_thread_fn, the
     * ip_eq_normalized(peer_ip, own public_ip) check before
     * FALLBACK_SIGNALING) fails it with FAILED_SYMMETRIC before it ever
     * REGISTERs with the rendezvous server. So every same-IP DELIVER is
     * either our own stale slot (any old NAT port — we cannot know
     * which, so we ignore ALL same-IP ports, not just advertised_port)
     * or a spoofed REGISTER; in both cases the right move is to keep
     * waiting, and to keep the rendezvous resender ALIVE so our
     * re-REGISTERs eventually refresh/reclaim the slot server-side.
     * This subsumes the old terminal hairpin gate, whose only reachable
     * firing case was exactly this self-DELIVER poison. */
    if (direct_p2p_ip_eq_normalized(peer_ip, s_work.stun.public_ip)) {
        SDL_Log("[direct_p2p] DELIVER carries our own IP (%s:%u, advertised port %u) "
                "— stale self-registration or spoof; ignoring and staying HOST_WAITING.",
                peer_ip, (unsigned)peer_port, (unsigned)s_work.advertised_port);
        return true;
    }

    /* Stop the rendezvous resender — we have what we need from the server. */
    SDL_SetAtomicInt(&s_rendezvous_cancel, 1);

    /* LAN bypass: if the rendezvous-delivered peer is on a private
     * subnet, we should have seen a direct punch already. Something is
     * weird (firewall blocking the LAN path? mismatched configs?). Stay
     * in HOST_WAITING and let the user retry / the direct path catch up. */
    if (direct_p2p_is_lan_peer(peer_ip)) {
        SDL_Log("[direct_p2p] DELIVER peer is LAN (%s); staying HOST_WAITING — "
                "direct path should already have completed.", peer_ip);
        return true;
    }

    /* Stash the peer endpoint so the bilateral-punch thread can copy
     * stack-locals from it. The thread overwrites these on success
     * before transitioning to HANDOFF. */
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = peer_port;

    /* Transition + spawn the bilateral-punch worker. The thread itself
     * publishes HANDOFF / FAILED_BILATERAL when it completes. */
    set_state(DIRECT_P2P_FALLBACK_BILATERAL_PUNCH);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    /* Natural-success guard: if a previous bilateral-punch worker exited
     * without being detached, join it now before clobbering the handle. */
    if (s_bilateral_punch_thread != NULL) {
        SDL_WaitThread(s_bilateral_punch_thread, NULL);
        s_bilateral_punch_thread = NULL;
    }
    s_bilateral_punch_thread = SDL_CreateThread(host_bilateral_punch_thread_fn,
                                                "DirectP2PBilateral", NULL);
    if (s_bilateral_punch_thread == NULL) {
        SDL_Log("[direct_p2p] failed to spawn bilateral-punch thread");
        set_status("Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_BILATERAL);
    }
    return true;
}

/* Host-side per-frame drain. Returns true once a valid inbound datagram
 * has been received and the handoff was executed. The first inbound
 * packet on a direct-P2P Host socket is the joiner's Stun_HolePunch
 * probe ("3SX_PUNCH" string, mirroring the upstream convention). We
 * echo one packet back to the source so the joiner's Stun_HolePunch
 * loop sees a response and returns success — otherwise the joiner
 * would time out and flag FAILED_SYMMETRIC even though connectivity is
 * established. */
static bool host_tick_receive(void) {
    if (s_work.stun.socket == NULL) {
        return false;
    }
    NET_Datagram* dgram = NULL;
    if (!NET_ReceiveDatagram(s_work.stun.socket, &dgram) || dgram == NULL) {
        return false;
    }
    /* Rendezvous DELIVER: 32+ bytes starting with '3SXR' magic
     * (0x33535852 BE). Hosts only ever receive DELIVER (server -> host);
     * REGISTER and POLL are client -> server only. Gate this BEFORE the
     * peer-endpoint capture and echo below so a DELIVER from the
     * rendezvous server is not mistaken for a peer punch probe. */
    if (dgram->buflen >= 32 &&
        dgram->buf[0] == 0x33 && dgram->buf[1] == 0x53 &&
        dgram->buf[2] == 0x58 && dgram->buf[3] == 0x52) {
        try_handle_deliver(dgram->buf, dgram->buflen);
        NET_DestroyDatagram(dgram);
        return true;
    }
    /* S1: STUN Binding Responses (keepalive replies, or a late duplicate
     * from a slower server probed during Stun_Discover) must NOT fall
     * through to the peer-endpoint capture below — pre-S1, a straggler
     * STUN response arriving in HOST_WAITING would be captured as "the
     * peer" and handed off to a STUN server's endpoint. Gate them here
     * and route keepalive replies to the drift detector. */
    if (Stun_IsBindingResponse(dgram->buf, dgram->buflen)) {
        host_handle_stun_rebind(dgram->buf, dgram->buflen);
        NET_DestroyDatagram(dgram);
        return false; /* not a peer — keep waiting */
    }
    /* Capture the source endpoint — this is who we'll talk to. The
     * socket's internal "connected peer" state gets set when
     * configure_gekko wraps it into SDLNetAdapter; for the orchestrator
     * the source-IP:port from the first packet is the peer's public
     * endpoint (post-NAT translation, which is what we need for sends). */
    char src_ip[64];
    SDL_strlcpy(src_ip, NET_GetAddressString(dgram->addr), sizeof(src_ip));
    uint16_t src_port = dgram->port;

    /* Echo the packet back — Stun_HolePunch on the joiner accepts any
     * byte-identical response payload from the expected peer. Sending
     * the same buffer the peer sent is the simplest way to satisfy
     * that check without exposing "3SX_PUNCH" here. */
    (void)NET_SendDatagram(s_work.stun.socket, dgram->addr, src_port,
                           dgram->buf, dgram->buflen);
    NET_DestroyDatagram(dgram);

    /* Stash for debug-logging; the handoff itself only needs ip. */
    SDL_strlcpy(s_work.peer_ip, src_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = src_port;

    SDL_Log("[direct_p2p] Host received first inbound from %s:%u", src_ip, src_port);
    set_status("Connected!");
    set_state(DIRECT_P2P_HANDOFF);

    /* Host is player 1 (player_number = 0). */
    do_handoff(1, src_ip, src_port);
    return true;
}

/* Join-side tick: the worker already completed punch and published
 * HANDOFF; Tick executes the main-thread handoff. */
static void join_tick_handoff(void) {
    /* Join is player 2. */
    do_handoff(2, s_work.peer_ip, s_work.peer_public_port);
}

/* --- public API -------------------------------------------------------- */

void DirectP2P_Init(void) {
    if (s_init_done) return;
    SDL_SetAtomicInt(&s_state, (int)DIRECT_P2P_IDLE);
    SDL_SetAtomicInt(&s_cancel, 0);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
    SDL_SetAtomicInt(&s_bilateral_failed, 0);
    s_bilateral_fail_count = 0; /* M1: fresh retry budget per hosting session */
    SDL_SetAtomicInt(&s_q_head, 0);
    SDL_SetAtomicInt(&s_q_tail, 0);
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    memset(s_rendezvous_send_q, 0, sizeof(s_rendezvous_send_q));
    memset(&s_work, 0, sizeof(s_work));
    memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    s_status[0] = '\0';
    Netplay_SetSessionTeardownCallback(direct_p2p_on_teardown);
    s_init_done = true;
}

void DirectP2P_BeginHost(int preferred_port) {
    DirectP2P_Init();
    if (get_state() != DIRECT_P2P_IDLE) {
        return;
    }
    /* Natural-success exit guard: if a previous worker returned without
     * being detached (5a switched from detach to wait), join its handle
     * before clobbering it. Safe when s_thread is NULL. */
    if (s_thread != NULL) {
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }

    SDL_SetAtomicInt(&s_cancel, 0);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
    SDL_SetAtomicInt(&s_bilateral_failed, 0);
    s_bilateral_fail_count = 0; /* M1: fresh retry budget per hosting session */
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    /* R-1: a reject latched during a session whose teardown never ran the
     * callback (e.g. LAN CLI session with no orchestrator) must not leak
     * into this fresh session's teardown. */
    s_handshake_reject_latched = false;
    Stun_ReleaseServerAddr(&s_work.stun); /* belt-and-braces before memset */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    memset(&s_work, 0, sizeof(s_work));
    s_work.role = ROLE_HOST;
    s_work.preferred_port = preferred_port;
    /* HOST_PORT config key semantics: >0 wins over the parameter when
     * the parameter is 0 (wrapper passes the config-derived value in
     * directly; this is a belt-and-braces fallback for callers that
     * forgot). If neither the parameter nor the config key specifies a
     * port, fall back to NETPLAY_DIRECT_P2P_DEFAULT_PORT — UPnP needs a
     * concrete external port to map, so 0 (OS-ephemeral) isn't viable. */
    if (preferred_port == 0) {
        int cfg_port = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_HOST_PORT);
        if (cfg_port > 0 && cfg_port <= 65535) {
            s_work.preferred_port = cfg_port;
        } else {
            s_work.preferred_port = NETPLAY_DIRECT_P2P_DEFAULT_PORT;
        }
    }
    s_thread = SDL_CreateThread(host_thread_fn, "DirectP2PHost", NULL);
    if (s_thread == NULL) {
        set_status("Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return;
    }
    /* Worker handle retained — DirectP2P_Cancel and direct_p2p_on_teardown
     * SDL_WaitThread it. Natural-success exit is joined on next
     * BeginHost/BeginJoin via the guard above, or on teardown. */
}

void DirectP2P_BeginJoin(const char* peer_code) {
    DirectP2P_Init();
    if (peer_code == NULL || peer_code[0] == '\0') return;
    if (get_state() != DIRECT_P2P_IDLE) return;

    /* Decode before spawning the thread: user-input errors should
     * surface immediately, not after a STUN round-trip. */
    uint32_t ip_be = 0;
    uint16_t pub_port = 0;
    if (!RoomCode_Decode(peer_code, &ip_be, &pub_port)) {
        set_status("Invalid room code.");
        set_state(DIRECT_P2P_FAILED_PUNCH);
        return;
    }

    struct in_addr in;
    in.s_addr = ip_be;
    char peer_ip[64] = { 0 };
    if (inet_ntop(AF_INET, &in, peer_ip, sizeof(peer_ip)) == NULL) {
        set_status("Invalid room code.");
        set_state(DIRECT_P2P_FAILED_PUNCH);
        return;
    }

    /* Natural-success exit guard: see BeginHost for the rationale. */
    if (s_thread != NULL) {
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }

    SDL_SetAtomicInt(&s_cancel, 0);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
    SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
    SDL_SetAtomicInt(&s_bilateral_failed, 0);
    s_bilateral_fail_count = 0; /* M1: fresh retry budget per hosting session */
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    s_handshake_reject_latched = false; /* R-1: see BeginHost */
    Stun_ReleaseServerAddr(&s_work.stun); /* belt-and-braces before memset */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    memset(&s_work, 0, sizeof(s_work));
    s_work.role = ROLE_JOIN;
    SDL_strlcpy(s_work.peer_code, peer_code, sizeof(s_work.peer_code));
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = pub_port;

    s_thread = SDL_CreateThread(join_thread_fn, "DirectP2PJoin", NULL);
    if (s_thread == NULL) {
        set_status("Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return;
    }
    /* Worker handle retained — see BeginHost. */
}

void DirectP2P_Cancel(void) {
    DirectP2PState st = get_state();
    if (st == DIRECT_P2P_IDLE || st == DIRECT_P2P_HANDOFF) return;

    SDL_SetAtomicInt(&s_cancel, 1);
    SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
    SDL_SetAtomicInt(&s_bilateral_punch_cancel, 1);

    /* Wait for any in-flight worker(s) to observe the cancel flag and
     * exit. Replaces the prior spin-on-state loop — joining the actual
     * thread handle eliminates the race where the worker writes s_work
     * after the memset below. Each worker honors its cancel flag at the
     * next loop iteration; worst-case blocking is bounded by the
     * longest cancel-check interval among the active workers (~2s for
     * Stun_Discover, ~10ms inside Stun_HolePunch). */
    if (s_thread)                  { SDL_WaitThread(s_thread,                  NULL); s_thread                  = NULL; }
    if (s_rendezvous_thread)       { SDL_WaitThread(s_rendezvous_thread,       NULL); s_rendezvous_thread       = NULL; }
    if (s_bilateral_punch_thread)  { SDL_WaitThread(s_bilateral_punch_thread,  NULL); s_bilateral_punch_thread  = NULL; }

    /* Producer threads are joined; release any pending NET_Address refs
     * still in the rendezvous send queue. */
    rend_q_purge();

    /* Tear down any retained resources: the worker may already have
     * populated s_work.stun and then observed cancel after publishing
     * HOST_WAITING. */
    if (s_work.stun.socket != NULL) {
        NET_DestroyDatagramSocket(s_work.stun.socket);
        s_work.stun.socket = NULL;
    }
    Stun_ReleaseServerAddr(&s_work.stun); /* S1: drop keepalive target ref */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    /* S1: reap any in-flight UPnP lease renewal before RemoveMapping —
     * bounded, and RemoveMapping is skipped when the worker had to be
     * detached (review H3; see upnp_renew_join_and_discard). */
    if (upnp_renew_join_and_discard()) {
        if (s_upnp_mapping.active) {
            Upnp_RemoveMapping(&s_upnp_mapping);
        }
    }
    memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    memset(&s_work, 0, sizeof(s_work));
    set_status("");
    s_handshake_reject_latched = false; /* R-1: drop any pending reject latch */
    set_state(DIRECT_P2P_IDLE);
}

/* R-1 — see direct_p2p.h. Records the reject reason for the overlay and
 * latches the teardown redirect to FAILED_HANDSHAKE. */
void DirectP2P_NotifySessionRejected(const char* reason) {
    set_status((reason != NULL && reason[0] != '\0')
                   ? reason
                   : "Connection rejected.");
    s_handshake_reject_latched = true;
    SDL_Log("[direct_p2p] session rejected by MIST handshake: %s",
            reason ? reason : "(no reason)");
}

void DirectP2P_Tick(void) {
    DirectP2PState st = get_state();

    /* S1: UPnP lease renewal — only in states where host_thread_fn has
     * finished writing s_upnp_mapping (it publishes HOST_WAITING after)
     * and the mapping is potentially carrying traffic. HANDOFF covers
     * the active netplay session: main.c ticks us from the session
     * branch too, so a mapping that outlives the 1-hour lease keeps
     * getting renewed mid-session. Never runs during UPNP_PROBE /
     * STUN_DISCOVER, where the worker thread still owns the mapping. */
    if (st == DIRECT_P2P_HOST_WAITING || st == DIRECT_P2P_FALLBACK_SIGNALING ||
        st == DIRECT_P2P_FALLBACK_BILATERAL_PUNCH || st == DIRECT_P2P_HANDOFF) {
        upnp_renew_tick();
    }

    switch (st) {
    case DIRECT_P2P_IDLE:
    case DIRECT_P2P_UPNP_PROBE:
    case DIRECT_P2P_STUN_DISCOVER:
    case DIRECT_P2P_JOIN_PUNCHING:
        /* Worker is active; nothing for main thread to do. */
        return;

    case DIRECT_P2P_HOST_WAITING:
        /* Drain the rendezvous send queue first (up to 4 slots per tick
         * per §Decision 3) so REGISTER/POLL packets enqueued by the
         * rendezvous worker reach the wire from the main thread (the
         * STUN socket's sole I/O actor during this phase). Then poll for
         * inbound — peer punch OR rendezvous DELIVER. */
        rend_q_drain(s_work.stun.socket, 4);
        /* S1: periodic STUN rebind keepalive (mapping refresh + drift
         * probe). Send-only and non-blocking; the response comes back
         * through host_tick_receive's STUN gate. */
        host_stun_keepalive_tick();
        host_tick_receive();
        return;

    case DIRECT_P2P_HANDOFF:
        /* Three entry points land us here:
         *   - Host direct path: host_tick_receive() already executed
         *     the handoff inline (main thread) and left state at
         *     HANDOFF. The netplay session is now running; nothing
         *     more to do. Leave state at HANDOFF; teardown resets it
         *     via direct_p2p_on_teardown.
         *   - Host bilateral fallback: the bilateral worker writes
         *     peer endpoint back to s_work and raises
         *     s_bilateral_handoff_pending while staying in
         *     FALLBACK_BILATERAL_PUNCH. Tick's
         *     FALLBACK_BILATERAL_PUNCH case (below) runs do_handoff
         *     and publishes HANDOFF — by the time we land here the
         *     handoff has already executed.
         *   - Join path: the worker published HANDOFF but the actual
         *     Netplay_BeginDirectP2P call has to happen on the main
         *     thread (Netplay internals are not thread-safe). Detect
         *     the join side by role and drive the handoff once.
         */
        if (s_work.role == ROLE_JOIN && s_work.stun.socket != NULL) {
            join_tick_handoff();
        }
        return;

    case DIRECT_P2P_FAILED_SYMMETRIC:
    case DIRECT_P2P_FAILED_STUN:
    case DIRECT_P2P_FAILED_PUNCH:
        /* Terminal failure; no auto-retry. The status-rendering
         * overlay (Step 8) is responsible for showing the reason. The
         * caller (menu) will eventually issue DirectP2P_Cancel to
         * return to IDLE. */
        return;

    case DIRECT_P2P_FALLBACK_SIGNALING:
        /* Joiner inlines the rendezvous loop into its worker thread, so
         * there's nothing for Tick to drive on the joiner side. The
         * host's REGISTER/POLL phase happens while state is still
         * HOST_WAITING; this case is unreachable in 5a (no transitions
         * to it yet) and remains a no-op skeleton until 5b/5c. */
        return;

    case DIRECT_P2P_FALLBACK_BILATERAL_PUNCH:
        /* The bilateral-punch worker thread (host_bilateral_punch_thread_fn)
         * exclusively owns s_work.stun.socket for reads/writes via
         * Stun_HolePunch during this phase. Per §Decision 3, the main
         * thread MUST NOT call host_tick_receive (concurrent
         * NET_ReceiveDatagram on the same socket is a race) and MUST NOT
         * drain the rendezvous queue (rendezvous resends are stopped by
         * the s_rendezvous_cancel signal raised in try_handle_deliver).
         *
         * On punch SUCCESS the worker raises s_bilateral_handoff_pending
         * (after writing peer_ip/peer_public_port back to s_work) and
         * exits without changing state — do_handoff invokes
         * Netplay_SetParams / Netplay_SetRemotePort / Netplay_SetStunSocket
         * which are main-thread-only. We pick up that signal here, join
         * the worker so its handle is reclaimed and s_work reads are
         * race-free, then run do_handoff inline. do_handoff itself does
         * NOT publish HANDOFF (mirroring the direct-path convention at
         * host_tick_receive:1014/1017), so we set_state HANDOFF after.
         *
         * On punch FAILURE the worker publishes FAILED_BILATERAL itself
         * and we never observe the pending flag — that path is handled
         * by the FAILED_BILATERAL terminal case. */
        if (SDL_GetAtomicInt(&s_bilateral_handoff_pending) != 0) {
            SDL_SetAtomicInt(&s_bilateral_handoff_pending, 0);
            if (s_bilateral_punch_thread != NULL) {
                SDL_WaitThread(s_bilateral_punch_thread, NULL);
                s_bilateral_punch_thread = NULL;
            }
            set_state(DIRECT_P2P_HANDOFF);
            /* Host is player 1 (player_number = 0). */
            do_handoff(1, s_work.peer_ip, s_work.peer_public_port);
            return;
        }
        /* Review M1: host-side punch FAILURE. Pre-fix the worker parked
         * terminal FAILED_BILATERAL and the host stopped advertising —
         * so one stale/hostile REGISTER against our key (anyone who saw
         * the room code, or an aborted joiner's leftover slot) killed
         * the room for the whole hosting period. Return to HOST_WAITING
         * and respawn the rendezvous loop (bounded retries so a
         * repeated attacker cannot spin the host in 3 s punch cycles
         * forever); the joiner's own failure handling is untouched. */
        if (SDL_GetAtomicInt(&s_bilateral_failed) != 0 &&
            s_work.role == ROLE_HOST) {
            SDL_SetAtomicInt(&s_bilateral_failed, 0);
            if (s_bilateral_punch_thread != NULL) {
                SDL_WaitThread(s_bilateral_punch_thread, NULL);
                s_bilateral_punch_thread = NULL;
            }
            s_bilateral_fail_count++;
            if (s_bilateral_fail_count >= HOST_BILATERAL_MAX_FAILURES) {
                SDL_Log("[direct_p2p] bilateral punch failed %d times this session — "
                        "parking FAILED_BILATERAL", s_bilateral_fail_count);
                set_status("Could not connect. Try a different network.");
                set_state(DIRECT_P2P_FAILED_BILATERAL);
                return;
            }
            SDL_Log("[direct_p2p] bilateral punch failure %d/%d — returning to "
                    "HOST_WAITING and resuming rendezvous advertising",
                    s_bilateral_fail_count, HOST_BILATERAL_MAX_FAILURES);
            /* The rendezvous thread exited when try_handle_deliver raised
             * s_rendezvous_cancel (and the state left HOST_WAITING); its
             * handle is still ours to reclaim before respawning. */
            if (s_rendezvous_thread != NULL) {
                SDL_WaitThread(s_rendezvous_thread, NULL);
                s_rendezvous_thread = NULL;
            }
            set_status("Waiting for player 2...");
            /* Clear the cancel flag BEFORE publishing HOST_WAITING so a
             * DELIVER processed on a later tick can re-raise it without
             * being clobbered (same ordering as host_thread_fn step 4). */
            SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
            SDL_SetAtomicInt(&s_bilateral_punch_cancel, 0);
            set_state(DIRECT_P2P_HOST_WAITING);
            if (!Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
                s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                                       "DirectP2PRendezvous", NULL);
                if (s_rendezvous_thread == NULL) {
                    SDL_Log("[direct_p2p] WARNING: failed to respawn rendezvous thread "
                            "after bilateral failure; direct punch still possible");
                }
            }
        }
        return;

    case DIRECT_P2P_FAILED_BILATERAL:
        /* Terminal — bilateral fallback exhausted. No work; the menu
         * will issue DirectP2P_Cancel to return to IDLE. */
        return;

    case DIRECT_P2P_FAILED_HANDSHAKE:
        /* Terminal — R-1 MIST handshake rejected the peer post-handoff.
         * Overlay shows ERROR + the reason; DirectP2P_Cancel (or the
         * OSD retry's process re-exec) returns to IDLE. */
        return;
    }
}

DirectP2PState DirectP2P_GetState(void) {
    return get_state();
}

Role DirectP2P_GetRole(void) {
    /* Snapshot value — set once per BeginHost/BeginJoin and not mutated
     * thereafter. No atomic needed; a stale read across the BeginHost ->
     * worker transition is at worst one frame of overlay miscategorization. */
    return s_work.role;
}

const char* DirectP2P_GetHostCode(void) {
    if (get_state() != DIRECT_P2P_HOST_WAITING) return "";
    return s_work.host_code;
}

const char* DirectP2P_GetStatusText(void) {
    return s_status;
}

#endif /* ENABLE_NETPLAY */
