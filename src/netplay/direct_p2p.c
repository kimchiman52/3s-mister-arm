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

typedef enum {
    ROLE_NONE = 0,
    ROLE_HOST,
    ROLE_JOIN,
} Role;

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

static int SDLCALL upnp_worker_fn(void* data) __attribute__((unused));
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
    set_status("Opening port forward via UPnP...");

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
    set_status("Discovering public endpoint...");
    bool stun_ok = Stun_Discover(&s_work.stun, local_port);
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!stun_ok) {
        set_status("STUN discovery failed.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
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
        set_status("Failed to encode room code.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

    /* 4) Publish — Tick() will now drain the STUN socket non-blockingly
     * until a peer punches through. The room code renders on overlay
     * line 2 via DirectP2P_GetHostCode(); this status goes on line 3. */
    set_status("Waiting for peer...");
    set_state(DIRECT_P2P_HOST_WAITING);
    SDL_Log("[direct_p2p] HOST_WAITING published. Code=%s public=%s:%u (via %s)",
            s_work.host_code, s_work.stun.public_ip, (unsigned)pub_port,
            upnp_ok ? "UPnP" : "STUN");
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
    set_status("Discovering public endpoint...");
    bool stun_ok = Stun_Discover(&s_work.stun, 0);
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!stun_ok) {
        set_status("STUN discovery failed.");
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
    set_status("Connecting to peer...");
    char punch_peer_ip[64];
    SDL_strlcpy(punch_peer_ip, s_work.peer_ip, sizeof(punch_peer_ip));
    uint16_t punch_peer_port = s_work.peer_public_port;
    bool punched = Stun_HolePunch(&s_work.stun, punch_peer_ip, &punch_peer_port,
                                  2500, &s_cancel);
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!punched) {
        Stun_CloseSocket(&s_work.stun);
        /* If we got here via STUN (UPnP not tried on Join side), a
         * punch timeout most plausibly means Symmetric NAT on one of
         * the peers. No TURN fallback (locked decision #3). */
        set_status("Cannot reach peer. Possible Symmetric NAT.");
        set_state(DIRECT_P2P_FAILED_SYMMETRIC);
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
    set_status("Connected. Starting session...");
    set_state(DIRECT_P2P_HANDOFF);
    return 0;
}

/* --- main-thread handoff ----------------------------------------------- */

/* Called from session teardown (via Netplay_SetSessionTeardownCallback).
 * Releases the UPnP mapping while the STUN socket is still bound so
 * miniupnpc can cleanly deregister. Safe to call when no mapping is
 * active. */
static void direct_p2p_on_teardown(void) {
    if (s_upnp_mapping.active) {
        Upnp_RemoveMapping(&s_upnp_mapping);
        memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    }
    /* Note: we do NOT destroy s_work.stun.socket here — ownership has
     * been transferred to netplay.c via Netplay_SetStunSocket and
     * netplay.c will destroy it immediately after this callback
     * returns. */
    s_work.stun.socket = NULL;
    /* Reset state so the next BeginHost/BeginJoin starts clean. */
    set_state(DIRECT_P2P_IDLE);
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
    /* netplay_nav owns the Netplay_BeginDirectP2P() call now. Netplay_
     * SetParams just populated remote_ip; nav's NAV_WAIT_ORCHESTRATOR
     * was gating on Netplay_IsRemoteIpSet() and will advance to
     * NAV_START_NETPLAY on its next tick. If menu-nav completes first
     * (fast path on host, orchestrator still hole-punching on joiner)
     * nav simply stays in NAV_WAIT_ORCHESTRATOR until we land here. */
    SDL_Log("[direct_p2p] handoff complete — nav state machine will start netplay session");
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
    set_status("Connected. Starting session...");
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
    SDL_SetAtomicInt(&s_cancel, 0);
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
        set_status("Failed to start Host thread.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return;
    }
    SDL_DetachThread(s_thread);
    s_thread = NULL; /* detached — worker owns its own lifetime */
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

    SDL_SetAtomicInt(&s_cancel, 0);
    memset(&s_work, 0, sizeof(s_work));
    s_work.role = ROLE_JOIN;
    SDL_strlcpy(s_work.peer_code, peer_code, sizeof(s_work.peer_code));
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = pub_port;

    s_thread = SDL_CreateThread(join_thread_fn, "DirectP2PJoin", NULL);
    if (s_thread == NULL) {
        set_status("Failed to start Join thread.");
        set_state(DIRECT_P2P_FAILED_STUN);
        return;
    }
    SDL_DetachThread(s_thread);
    s_thread = NULL;
}

void DirectP2P_Cancel(void) {
    DirectP2PState st = get_state();
    if (st == DIRECT_P2P_IDLE || st == DIRECT_P2P_HANDOFF) return;

    SDL_SetAtomicInt(&s_cancel, 1);

    /* Short grace period for the worker to observe the flag. In the
     * worst case the worker is inside a Stun_Discover resolve and has
     * to let its 2s timeout expire; we don't block the game loop on
     * that — future Tick() calls will see the IDLE transition when it
     * lands. */
    for (int i = 0; i < 50 && get_state() != DIRECT_P2P_IDLE; i++) {
        SDL_Delay(10);
    }

    /* Tear down any retained resources: the worker may already have
     * populated s_work.stun and then observed cancel after publishing
     * HOST_WAITING. */
    if (s_work.stun.socket != NULL) {
        NET_DestroyDatagramSocket(s_work.stun.socket);
        s_work.stun.socket = NULL;
    }
    if (s_upnp_mapping.active) {
        Upnp_RemoveMapping(&s_upnp_mapping);
        memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
    }
    memset(&s_work, 0, sizeof(s_work));
    set_status("");
    set_state(DIRECT_P2P_IDLE);
}

void DirectP2P_Tick(void) {
    DirectP2PState st = get_state();
    switch (st) {
    case DIRECT_P2P_IDLE:
    case DIRECT_P2P_UPNP_PROBE:
    case DIRECT_P2P_STUN_DISCOVER:
    case DIRECT_P2P_JOIN_PUNCHING:
        /* Worker is active; nothing for main thread to do. */
        return;

    case DIRECT_P2P_HOST_WAITING:
        /* Drain the STUN socket non-blockingly until a peer arrives. */
        host_tick_receive();
        return;

    case DIRECT_P2P_HANDOFF:
        /* Two entry points land us here:
         *   - Host path: host_tick_receive() already executed the
         *     handoff and left state at HANDOFF. The netplay session
         *     is now running; nothing more to do. Leave state at
         *     HANDOFF; teardown resets it via direct_p2p_on_teardown.
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
    }
}

DirectP2PState DirectP2P_GetState(void) {
    return get_state();
}

const char* DirectP2P_GetHostCode(void) {
    if (get_state() != DIRECT_P2P_HOST_WAITING) return "";
    return s_work.host_code;
}

const char* DirectP2P_GetStatusText(void) {
    return s_status;
}

#endif /* ENABLE_NETPLAY */
